#ifndef FILTER_BLOCK_HPP
#define FILTER_BLOCK_HPP

#include "Queue.hpp"           // IQueue<T>, DataPacket
#include "FilterUtils.hpp"  // FilteredPacket
#include "ConfigManager.hpp"   // SystemConfig

#include <vector>
#include <memory>
#include <atomic>

#ifdef CYNLR_PERF_BUILD
#include <chrono>

inline uint64_t read_clock() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}
#endif

class FilterBlock {
public:
    FilterBlock(const SystemConfig&           config,
                IQueue<DataPacket>&           in_queue,
                IQueue<FilteredPacket>&       out_queue,
                uint8_t                   threshold,
                BoundaryPolicy                policy = BoundaryPolicy::REPLICATE);
    void run();
    void stop();
private:
    bool processSample(uint8_t value, uint64_t row, uint64_t col);
    void flushRowEnd(uint8_t edge_value, uint64_t row, uint64_t last_col);
    float dotProduct() const;
    void emitIfReady();

private:
    const SystemConfig&           config_;
    IQueue<DataPacket>&           in_queue_;
    IQueue<FilteredPacket>&       out_queue_;
    //std::unique_ptr<IThresholder> thresholder_;
	 float threshold_; 
    BoundaryPolicy                policy_;

    SlidingWindow                 window_;
    const size_t                  half_width_;   // kernel.size() / 2

    // ---- per-packet output staging ----
    struct PendingOutput {
        bool    has_b1 = false;
        bool    has_b2 = false;
        uint8_t b1     = 0;
        uint8_t b2     = 0;
        uint64_t row   = 0;
        uint64_t col   = 0;   // col of b1
        #ifdef CYNLR_PERF_BUILD
            uint64_t t1    = 0;
            uint64_t t2    = 0;
        #endif
    };
    PendingOutput pending_;

    std::atomic<bool> running_{true};
};

#endif // FILTER_BLOCK_HPP