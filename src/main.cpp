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
 
#include "ConfigManager.hpp"
#include "GeneratorBlock.hpp"
#include "Queue.hpp"

static constexpr uint64_t RUN_DURATION_MS = 200;   

int main(int argc, char** argv) {

    // 1. Load configuration
    SystemConfig config{};
    try {
        config = ConfigManager::load(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << "[main] Configuration error: " << ex.what() << '\n';
        return EXIT_FAILURE;
    }
 
    std::cout << "[main] Config loaded:\n"
              << "       columns (m) = " << config.columns        << '\n'
              << "       cycle_ns (T)= " << config.cycle_time_ns  << '\n'
              << "       threshold   = " << static_cast<int>(config.threshold) << '\n'
              << "       mode        = "
              << (config.mode == Mode::CSV ? "CSV" : "RANDOM")    << '\n';
 

    // 2. Construct the inter-block queue
    //    PipelineQueue = SPSCQueue<DataPacket, 64>  (lock-free, production path)
    PipelineQueue queue;


    // 3. Create data source via factory
    std::unique_ptr<IDataSource> source;
    try {
        source = createDataSource(config);
    } catch (const std::exception& ex) {
        std::cerr << "[main] Data source error: " << ex.what() << '\n';
        return EXIT_FAILURE;
    }

    // 4. Construct and launch GeneratorBlock on a producer thread
    GeneratorBlock generator(config, queue, std::move(source));
 
    std::thread producer_thread([&generator]() {
        generator.run();
    });
 
    // 5. Consumer loop  (main thread)
    //    TODO — Phase 2: replace this stub with FilterBlock::run().
    //    For now: drain the queue, count packets, print a sample, then stop.
	
    uint64_t packets_consumed = 0;
    const auto run_until = std::chrono::steady_clock::now()
                         + std::chrono::milliseconds(RUN_DURATION_MS);
 
    while (std::chrono::steady_clock::now() < run_until) {
        DataPacket pkt{};
        if (queue.pop(pkt)) {
            ++packets_consumed;
 
            // Print the first 5 packets so output is human-verifiable.
            if (packets_consumed <= 5) {
                std::cout << "[consumer] pkt #" << packets_consumed
                          << "  row=" << pkt.row
                          << "  col=" << pkt.col
                          << "  v1="  << static_cast<int>(pkt.v1)
                          << "  v2="  << static_cast<int>(pkt.v2)
                          << '\n';
            }
        }
        // Yield rather than hard-spin — the consumer is not timing-critical here.
        std::this_thread::yield();
    }

    // 6. Signal producer to stop and wait for it to finish
    generator.stop();
    producer_thread.join();
 
    std::cout << "[main] Run complete. Packets consumed: "
              << packets_consumed << '\n';
 
    return EXIT_SUCCESS;
}