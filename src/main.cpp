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

#include "ConfigManager.hpp"
#include "Queue.hpp"
#include "GeneratorBlock.hpp"
#include "FilterBlock.hpp"      // FilterBlock (pulls in FilterUtils.hpp)

//static constexpr uint64_t RUN_DURATION_MS = 200;   
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
    std::cout << "========================================\n";
    std::cout << " CynLr Pipeline\n";
    std::cout << "========================================\n";
    std::cout << " Mode             : " << cfg.mode << "\n";
    if (cfg.mode == Mode::CSV) {
        std::cout << " Input file       : " << cfg.input_file        << "\n";
        std::cout << " CSV mismatch     : " << cfg.csv_mismatch_policy << "\n";
    }
    std::cout << " Columns (m)      : " << cfg.columns             << "\n";
    std::cout << " Cycle (T)        : " << cfg.cycle_time_ns       << " ns\n";
    std::cout << " Threshold        : " << static_cast<int>(cfg.threshold) << "\n";
    std::cout << " Kernel size      : " << cfg.kernel.size()       << "\n";
    std::cout << " Boundary policy  : " << cfg.boundary_policy     << "\n";

    std::cout << " Duration         : ";
    if (cfg.run_duration_ms == 0) std::cout << "unlimited\n";
    else                          std::cout << cfg.run_duration_ms << " ms\n";

    std::cout << " Max rows         : ";
    if (cfg.max_rows == 0) std::cout << "unlimited\n";
    else                   std::cout << cfg.max_rows << "\n";

    std::cout << "========================================\n\n";
}

// Thread entry points
static void runGenerator(GeneratorBlock& gen, std::atomic<bool>& generator_done) {
    gen.run();
    generator_done.store(true, std::memory_order_release);
    std::cout << "[Generator] Done.\n";
}
static void runFilter(FilterBlock& filter) {
    filter.run();   // exits when stop() is called and queue is empty
    std::cout << "[Filter]    Done.\n";
}
// shouldTerminate
static bool shouldTerminate(const SystemConfig&                          cfg,
                            const std::chrono::steady_clock::time_point& deadline,
                            const std::atomic<bool>&                     generator_done)
{
    if (generator_done.load(std::memory_order_acquire))          return true;
    if (g_shutdown_requested.load(std::memory_order_relaxed))    return true;
    if (cfg.run_duration_ms > 0 &&
        std::chrono::steady_clock::now() >= deadline)            return true;
    return false;
}
int main(int argc, char** argv) {

    installSignalHandlers();
    // 1. Load configuration
    // -------------------------------------------------------------------------
    SystemConfig config;
    try {
        config = ConfigManager::load(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << "[ERROR] Config load failed: " << ex.what() << "\n";
        return EXIT_FAILURE;
    }

    printConfig(config);

    // 2. Construct the inter-block queue
    //    PipelineQueue = SPSCQueue<DataPacket, 64>  (lock-free, production path)
    //PipelineQueue queue;
    const std::size_t queue_capacity = config.columns / 2;  // packets per row
    DynamicSPSCQueue<DataPacket>    gen_to_filter(queue_capacity);
    SimpleQueue<FilteredPacket> filter_output;   // Filter → (next block / sink)



    // Generator: owns its data source (CSV or random), injected via factory
    auto data_source   = createDataSource(config);
    GeneratorBlock generator(config, gen_to_filter, std::move(data_source));

    // Filter: thresholder injected; BoundaryPolicy defaults to REPLICATE
    auto thresholder = std::make_unique<BinaryThresholder>(config.threshold);
    FilterBlock filter(config,
                       gen_to_filter,
                       filter_output,
                       std::move(thresholder),
                       config.boundary_policy);   // ← from config
 
    // 4. Launch threads
    std::atomic<bool> generator_done{false};

    std::thread gen_thread(runGenerator,
                           std::ref(generator),
                           std::ref(generator_done));

    std::thread filter_thread(runFilter, std::ref(filter));
                              //std::ref(filter));
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(config.run_duration_ms);
    while (!shouldTerminate(config, deadline, generator_done)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    generator.stop();
    gen_thread.join();
    // 5. Consumer loop  (main thread)
    // while (true) {
    while (!gen_to_filter.empty()) {
        std::this_thread::yield();
    }

    //std::this_thread::sleep_for(std::chrono::nanoseconds(config.cycle_time_ns * 2));
    //    break;}
    filter.stop();
    filter_thread.join();

    // -------------------------------------------------------------------------
    // 6. Consume and report output (sink — placeholder for next pipeline stage)
    // -------------------------------------------------------------------------
    size_t total_packets = 0;
    size_t ones          = 0;
    size_t zeros         = 0;

    FilteredPacket fp;
    while (filter_output.pop(fp)) {
        ++total_packets;
        if (fp.b1 == 1) ++ones; else ++zeros;
        if (fp.b2 == 1) ++ones; else ++zeros;
    }

    // 8. Summary
    std::cout << "\n========================================\n";
    std::cout << " Pipeline Summary\n";
    std::cout << "========================================\n";
    std::cout << " Rows generated   : " << generator.rows_emitted() << "\n";
    std::cout << " Packets dropped  : " << generator.dropped_packets();
    if (generator.dropped_packets() > 0)
        std::cout << "  ← T too low for filter throughput on this hardware";
    std::cout << "\n";
    std::cout << " Output packets   : " << total_packets            << "\n";
    std::cout << " Output pixels    : " << total_packets * 2        << "\n";
    std::cout << " Ones  (1)        : " << ones                     << "\n";
    std::cout << " Zeros (0)        : " << zeros                    << "\n";
    const std::size_t actual_cap = gen_to_filter.capacity();
    const std::size_t peak       = gen_to_filter.peak_occupancy();
    std::cout << " Queue capacity   : " << actual_cap
              << " packets (next pow2 above m/2=" << queue_capacity << ")\n";
    std::cout << " Peak queue depth : " << peak
              << " / " << queue_capacity << " (m/2 limit)\n";
    std::cout << " Memory OK        : "
              << (peak <= queue_capacity ? "YES" : "NO") << "\n";
    std::cout << " Shutdown cause   : ";
    if (g_shutdown_requested.load())
        std::cout << "signal (SIGINT/SIGTERM)\n";
    else if (config.run_duration_ms > 0 &&
             std::chrono::steady_clock::now() >= deadline)
        std::cout << "duration limit reached\n";
    else
        std::cout << "natural / max_rows\n";
    std::cout << "========================================\n";

    return EXIT_SUCCESS;
}