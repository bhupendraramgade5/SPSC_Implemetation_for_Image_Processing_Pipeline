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

#include "ConfigManager.hpp"
#include "Queue.hpp"
#include "GeneratorBlock.hpp"
#include "FilterBlock.hpp"      // FilterBlock (pulls in FilterUtils.hpp)

//static constexpr uint64_t RUN_DURATION_MS = 200;   
static void printConfig(const SystemConfig& cfg) {
    std::cout << "========================================\n";
    std::cout << " CynLr Pipeline — Phase 1 + 2\n";
    std::cout << "========================================\n";
    std::cout << " Mode        : "
              << (cfg.mode == Mode::CSV ? "CSV" : "Random") << "\n";
    if (cfg.mode == Mode::CSV)
        std::cout << " Input file  : " << cfg.input_file << "\n";
    std::cout << " Columns (m) : " << cfg.columns       << "\n";
    std::cout << " Cycle (T)   : " << cfg.cycle_time_ns << " ns\n";
    std::cout << " Threshold   : " << static_cast<int>(cfg.threshold) << "\n";
    std::cout << " Kernel size : " << cfg.kernel.size() << "\n";
    std::cout << "========================================\n\n";
}

static void runGenerator(GeneratorBlock& gen, std::atomic<bool>& generator_done) {
    gen.run();
    generator_done.store(true, std::memory_order_release);
    std::cout << "[Generator] Done.\n";
}
static void runFilter(FilterBlock& filter, const std::atomic<bool>& generator_done) {
    filter.run();   // exits when stop() is called and queue is empty
    std::cout << "[Filter]    Done.\n";
}
int main(int argc, char** argv) {

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
	SimpleQueue<DataPacket>     gen_to_filter;   // Generator → Filter
    SimpleQueue<FilteredPacket> filter_output;   // Filter → (next block / sink)



    // Generator: owns its data source (CSV or random), injected via factory
    auto data_source   = createDataSource(config);
    GeneratorBlock generator(config, gen_to_filter, std::move(data_source));

    // Filter: thresholder injected; BoundaryPolicy defaults to REPLICATE
    auto thresholder = std::make_unique<BinaryThresholder>(config.threshold);
    FilterBlock filter(config,
                       gen_to_filter,
                       filter_output,
                       std::move(thresholder));
 
    // 4. Launch threads
    std::atomic<bool> generator_done{false};

    std::thread gen_thread(runGenerator,
                           std::ref(generator),
                           std::ref(generator_done));

    std::thread filter_thread(runFilter,
                              std::ref(filter),
                              std::cref(generator_done));
 
    gen_thread.join();
    // 5. Consumer loop  (main thread)
    while (true) {
        DataPacket probe;
        // Peek: if queue is empty, filter has caught up
        // SimpleQueue::pop is non-destructive-peek-free, so we rely on the
        // generator_done flag + a short sleep to give the filter time to drain.
        std::this_thread::sleep_for(
            std::chrono::nanoseconds(config.cycle_time_ns * 2));

        // If filter thread is still running and queue could still have data,
        // give it one more cycle.  After 2*T of silence post-generator, stop.
        break;
    }
    filter.stop();
    filter_thread.join();

    // -------------------------------------------------------------------------
    // 6. Consume and report output (sink — placeholder for next pipeline stage)
    // -------------------------------------------------------------------------
    size_t total_output = 0;
    size_t ones         = 0;
    size_t zeros        = 0;

    FilteredPacket fp;
    while (filter_output.pop(fp)) {
        ++total_output;
        if (fp.b1 == 1) ++ones; else ++zeros;
        if (fp.b2 == 1) ++ones; else ++zeros;
    }

    std::cout << "\n========================================\n";
    std::cout << " Pipeline Summary\n";
    std::cout << "========================================\n";
    std::cout << " Output packets : " << total_output          << "\n";
    std::cout << " Output pixels  : " << total_output * 2      << "\n";
    std::cout << " Ones  (1)      : " << ones                  << "\n";
    std::cout << " Zeros (0)      : " << zeros                 << "\n";
    std::cout << "========================================\n";

    return EXIT_SUCCESS;
}