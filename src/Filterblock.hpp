#ifndef FILTER_BLOCK_HPP
#define FILTER_BLOCK_HPP

#include "Queue.hpp"           // IQueue<T>, DataPacket
#include "FilteredPacket.hpp"  // FilteredPacket
#include "ConfigManager.hpp"   // SystemConfig
#include "SlidingWindow.hpp"   // SlidingWindow
#include "Thresholder.hpp"     // IThresholder
#include "BoundaryPolicy.hpp"  // BoundaryPolicy

#include <vector>
#include <memory>
#include <atomic>

class FilterBlock {
public:
    FilterBlock(const SystemConfig&           config,
                IQueue<DataPacket>&           in_queue,
                IQueue<FilteredPacket>&       out_queue,
                std::unique_ptr<IThresholder> thresholder,
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
    std::unique_ptr<IThresholder> thresholder_;
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
    };
    PendingOutput pending_;

    std::atomic<bool> running_{true};
};

#endif // FILTER_BLOCK_HPP