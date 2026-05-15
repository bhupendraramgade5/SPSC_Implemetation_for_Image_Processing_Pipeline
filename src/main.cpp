// CynLr.cpp : Defines the entry point for the application.
//

// #include <iostream>

// using namespace std;

// int main()
// {
// 	cout << "Hello CMake." << endl;
// 	return 0;
// }


#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <memory>
#include <chrono>
#include <iomanip>
#include <csignal>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "ConfigManager.hpp"
#include "Queue.hpp"
#include "GeneratorBlock.hpp"
#include "Filterblock.hpp"
#include "LabellingBlock.hpp"
#include "TracingBlock.hpp"     // Phase 4
#include "TracingUtils.hpp"     // Phase 4 — makeTracingOutput, ITracingOutput
#include "OutputWriter.hpp"
#include "PerfTest.hpp"

// ---------------------------------------------------------------------------
// Global shutdown flag — set by SIGINT / SIGTERM
// ---------------------------------------------------------------------------
namespace {
    std::atomic<bool> g_shutdown_requested{false};
}
extern "C" void signalHandler(int /*signum*/) {
    g_shutdown_requested.store(true, std::memory_order_relaxed);
}

static void installSignalHandlers() {
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);
}
static void printConfig(const SystemConfig& cfg) {
    std::cout << "========================================\n"
              << " CynLr Pipeline  (threaded, 4 stages)\n"
              << "========================================\n"
              << " Mode             : " << cfg.mode             << "\n";
    if (cfg.mode == Mode::CSV)
        std::cout << " Input file       : " << cfg.input_file        << "\n"
                  << " CSV mismatch     : " << cfg.csv_mismatch_policy << "\n";
    std::cout << " Columns (m)      : " << cfg.columns             << "\n"
              << " Cycle (T)        : " << cfg.cycle_time_ns       << " ns\n"
              << " Threshold        : " << static_cast<int>(cfg.threshold) << "\n"
              << " Kernel size      : " << cfg.kernel.size()       << "\n"
              << " Boundary policy  : " << cfg.boundary_policy     << "\n"
              << " Write output     : " << (cfg.write_output ? "yes" : "no") << "\n";
    if (cfg.write_output)
        std::cout << " Output file      : " << cfg.output_file << "\n"
                  << " Blob file        : " << cfg.output_file << "_blobs.csv\n";
    std::cout << " Duration         : ";
    if (cfg.run_duration_ms == 0) std::cout << "unlimited\n";
    else                          std::cout << cfg.run_duration_ms << " ms\n";

    std::cout << " Max rows         : ";
    if (cfg.max_rows == 0) std::cout << "unlimited\n";
    else                   std::cout << cfg.max_rows << "\n";

    std::cout << "========================================\n\n";
}

// Thread entry points
// ---------------------------------------------------------------------------
static void runGenerator(GeneratorBlock& gen, std::atomic<bool>& done) {
    gen.run();
    done.store(true, std::memory_order_release);
    std::cout << "[Generator]  Done.\n";
}
static void runFilter(FilterBlock& filter) {
    filter.run();
    std::cout << "[Filter]     Done.\n";
}

static void runLabeller(LabellingBlock& labeller) {
    labeller.run();
    std::cout << "[Labeller]   Done.\n";
}

static void runTracer(TracingBlock& tracer) {   // Phase 4
    tracer.run();
    std::cout << "[Tracer]     Done.\n";
}

// ---------------------------------------------------------------------------
// Termination predicate
// ---------------------------------------------------------------------------
static bool shouldTerminate(
        const SystemConfig&                          cfg,
        const std::chrono::steady_clock::time_point& deadline,
        const std::atomic<bool>&                     gen_done)
{
    if (gen_done.load(std::memory_order_acquire))        return true;
    if (g_shutdown_requested.load(std::memory_order_relaxed)) return true;
    if (cfg.run_duration_ms > 0 &&
        std::chrono::steady_clock::now() >= deadline)            return true;
    return false;
}

// ---------------------------------------------------------------------------
// Drain helper — spin-yield until a queue reports empty.
// Accepts the race documented in DESIGN_DOCUMENT.md: checks emptiness, not
// stage completion.  Correct for the evaluation workload; a production system
// would use stage-level drain signals.
// ---------------------------------------------------------------------------
template<typename Q>
static void drainQueue(Q& q) {
    while (!q.empty())
        std::this_thread::yield();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {

    SetProcessAffinityMask(GetCurrentProcess(), 3);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    
    installSignalHandlers();

    // -------------------------------------------------------------------------
    // 1. Load configuration
    // -------------------------------------------------------------------------
    SystemConfig config;
    std::cout<<"Loading config..."<<std::endl;
    try {
        config = ConfigManager::load(argc, argv);

    } catch (const std::exception& ex) {
        std::cerr << "[ERROR] Config: " << ex.what() << "\n";
        return EXIT_FAILURE;
    }

    printConfig(config);

    // ---- 2. Build queues -----------------------------------------------
    // All inter-stage hot-path queues: DynamicSPSCQueue, depth = m/2.
    // Output sink: SimpleQueue (Zone-A/B boundary — mutex acceptable here).

    const std::size_t queue_depth = config.columns / 2;

    DynamicSPSCQueue<DataPacket>     gen_to_filter   (queue_depth, queue_depth);
    DynamicSPSCQueue<FilteredPacket> filter_to_label (queue_depth, queue_depth);
    DynamicSPSCQueue<LabelledPacket> label_to_tracing(queue_depth, queue_depth); // Phase 4



    // Generator: owns its data source (CSV or random), injected via factory
    auto data_source   = createDataSource(config);
    
    if (config.mode == Mode::CSV && config.columns == 0) {
        config.columns = data_source->detectedColumns();
        std::cout << "[Main] CSV auto-detected columns: " << config.columns << "\n";
        if (config.columns < 2)
            throw std::runtime_error("CSV file has fewer than 2 columns");
        if (config.columns % 2 != 0) {
            std::cout << "[Main] Warning: odd column count — truncating to "
                      << (config.columns - 1) << "\n";
            --config.columns;
        }
    }

    // -------------------------------------------------------------------------
    // 4. Construct pipeline blocks
    // -------------------------------------------------------------------------
    GeneratorBlock generator(config,
                             gen_to_filter,
                             std::move(data_source));

    FilterBlock filter(config,
                       gen_to_filter,
                       filter_to_label,
                       config.threshold,
                       config.boundary_policy);

    LabellingBlock labeller(config,
                            filter_to_label,
                            label_to_tracing);          // Phase 4: new downstream queue

    // Phase 4: tracing output sink selected from config (null / CSV)
    auto tracing_output = makeTracingOutput(config);

    TracingBlock tracer(config,                         // Phase 4
                        label_to_tracing,
                        *tracing_output);

    // -------------------------------------------------------------------------
    // 5. Launch threads
    // -------------------------------------------------------------------------
    std::atomic<bool> gen_done{false};

    std::thread gen_thread   (runGenerator, std::ref(generator), std::ref(gen_done));
    std::thread filter_thread(runFilter,    std::ref(filter));
    std::thread label_thread (runLabeller,  std::ref(labeller));
    std::thread trace_thread (runTracer,    std::ref(tracer));   // Phase 4

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(
                              config.run_duration_ms > 0
                                  ? config.run_duration_ms
                                  : UINT64_MAX / 2);

    // Supervisor loop — sleeps 10 ms per iteration
    while (!shouldTerminate(config, deadline, gen_done))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    generator.stop();
    gen_thread.join();

    drainQueue(gen_to_filter);      // wait until filter has consumed all packets
    filter.stop();
    filter_thread.join();

    drainQueue(filter_to_label);    // wait until labeller has consumed all packets
    labeller.stop();
    label_thread.join();

    drainQueue(label_to_tracing);    // Phase 4: drain before stopping tracer
    tracer.stop();
    trace_thread.join();             // Phase 4: join tracer thread

    // tracing_output destructor / flush() is called when unique_ptr goes out
    // of scope at end of main() — all buffered blobs are written to disk here.

    // ---- 9. Pipeline summary -------------------------------------------
    const std::size_t gen_filt_peak  = gen_to_filter.peak_occupancy();
    const std::size_t filt_lab_peak  = filter_to_label.peak_occupancy();
    const std::size_t lab_trac_peak  = label_to_tracing.peak_occupancy(); // Phase 4

    const bool memory_ok =
        (gen_filt_peak  <= queue_depth) &&
        (filt_lab_peak  <= queue_depth) &&
        (lab_trac_peak  <= queue_depth);                                   // Phase 4

    std::cout << "\n========================================\n"
              << " Pipeline Summary  (4 stages)\n"
              << "========================================\n"
              << " Rows generated       : " << generator.rows_emitted()  << "\n"
              << " Packets dropped      : " << generator.dropped_packets();
    if (generator.dropped_packets() > 0)
        std::cout << "  <- T too tight; increase T or reduce m";
    std::cout << "\n"
              << "\n"
              << " Tracing packets      : " << tracer.packets_processed()        << "\n"
              << " Blobs completed      : " << tracer.blobs_completed()          << "\n"
              << " Peak active labels   : " << tracer.peak_active_accumulators()
              <<   " / " << queue_depth << " (m/2)\n"
              << "\n"
              << " Queue gen→filter     : peak=" << gen_filt_peak
              <<   " / " << queue_depth << " (m/2 limit)"
              <<   "  cap=" << gen_to_filter.capacity()    << "\n"
              << " Queue filter→label   : peak=" << filt_lab_peak
              <<   " / " << queue_depth << " (m/2 limit)"
              <<   "  cap=" << filter_to_label.capacity()  << "\n"
              << " Queue label→tracing  : peak=" << lab_trac_peak           // Phase 4
              <<   " / " << queue_depth << " (m/2 limit)"
              <<   "  cap=" << label_to_tracing.capacity() << "\n"
              << " Memory OK (all)      : " << (memory_ok ? "YES" : "NO")  << "\n"
              << "\n"
              << " Shutdown cause       : ";

    if (g_shutdown_requested.load())
        std::cout << "signal (SIGINT/SIGTERM)\n";
    else if (config.run_duration_ms > 0 &&
             std::chrono::steady_clock::now() >= deadline)
        std::cout << "duration limit reached\n";
    else
        std::cout << "source exhausted / max_rows\n";

    std::cout << "========================================\n";

    return EXIT_SUCCESS;
}