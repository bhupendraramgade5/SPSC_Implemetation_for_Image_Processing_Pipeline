#include "FilterBlock.hpp"  // pulls in FilterUtils.hpp, Queue.hpp, ConfigManager.hpp

#include <chrono>
#include <thread>
#include <stdexcept>

#if defined(_MSC_VER)
    #include <immintrin.h>
#endif


FilterBlock::FilterBlock(const SystemConfig&           config,
                         IQueue<DataPacket>&           in_queue,
                         IQueue<FilteredPacket>&       out_queue,
                         uint8_t                   threshold,
                         BoundaryPolicy                policy)
    : config_(config)
    , in_queue_(in_queue)
    , out_queue_(out_queue)
    , threshold_(static_cast<float>(threshold))
    , policy_(policy)
    , window_(config.kernel.size())           // ring buffer sized from kernel
    , half_width_(config.kernel.size() / 2)
{
    if (config.kernel.empty())
        throw std::invalid_argument("FilterBlock: kernel must not be empty");
    if (config.kernel.size() % 2 == 0)
        throw std::invalid_argument("FilterBlock: kernel size must be odd");
}
void FilterBlock::stop() {
    running_.store(false, std::memory_order_relaxed);
}

void FilterBlock::run() {
    uint64_t current_row  = UINT64_MAX;  // sentinel: no row seen yet
    uint8_t  last_value   = 0;           // last real sample (for REPLICATE)
    uint64_t last_col     = 0;           // last real col in a row

    DataPacket packet;

    while (true) {

        // Try to get the next packet; spin-yield if queue is temporarily empty
        if (!in_queue_.pop(packet)) {
            if (!running_.load(std::memory_order_relaxed)) break;
#if defined(_MSC_VER)
            _mm_pause();
#elif defined(__GNUC__) || defined(__clang__)
            __builtin_ia32_pause();
#endif
            continue;
        }

        if (packet.row != current_row) {

            // Flush end of previous row (skip on very first packet)
            if (current_row != UINT64_MAX) {
                flushRowEnd(last_value, current_row, last_col);
            }

            current_row = packet.row;
            window_.reset();
            pending_ = PendingOutput{};

            uint8_t left_edge = packet.v1;
            for (size_t i = 0; i < half_width_; ++i) {
                uint8_t pad = applyLeft(policy_, left_edge, half_width_ - i);
                window_.push(pad, current_row, 0);
            }
        }
        processSample(packet.v1, packet.row, packet.col);
        emitIfReady();

        processSample(packet.v2, packet.row, packet.col + 1);
        emitIfReady();

        last_value = packet.v2;
        last_col   = packet.col + 1;
    }
    if (current_row != UINT64_MAX) {
        flushRowEnd(last_value, current_row, last_col);
    }
}
bool FilterBlock::processSample(uint8_t value, uint64_t row, uint64_t col) {
    window_.push(value, row, col);

    if (!window_.is_full()) return false;

    const WindowSlot& centre = window_.centre();

    float filtered = dotProduct();
    
    // ✓ OPTIMIZATION 1: Direct comparison, no virtual dispatch
    uint8_t binary = (filtered >= threshold_) ? uint8_t{1} : uint8_t{0};

    if (!pending_.has_b1) {
        pending_.b1     = binary;
        pending_.row    = centre.row;
        pending_.col    = centre.col;
        pending_.has_b1 = true;
        #ifdef CYNLR_PERF_BUILD
            pending_.t1 = read_clock();
        #endif
    } else {
        // b1 was already stored; this is b2
        pending_.b2     = binary;
        pending_.has_b2 = true;
        #ifdef CYNLR_PERF_BUILD
            pending_.t2 = read_clock();
        #endif
    }

    return true;
}
void FilterBlock::flushRowEnd(uint8_t edge_value,
                               uint64_t row,
                               uint64_t last_col)
{
    for (size_t i = 0; i < half_width_; ++i) {
        uint8_t pad = applyRight(policy_, edge_value, i + 1);
        // Virtual col indices continue beyond last_col for position tagging
        processSample(pad, row, last_col + 1 + i);
        emitIfReady();
    }

    if (pending_.has_b1 && !pending_.has_b2) {
        FilteredPacket fp;
        fp.b1  = pending_.b1;
        fp.b2  = 0;             // no second pixel — padding artifact
        fp.row = pending_.row;
        fp.col = pending_.col;
        out_queue_.push(fp);
        pending_ = PendingOutput{};
    }
}
float FilterBlock::dotProduct() const {
    const auto& kernel = config_.kernel;
    const size_t n     = kernel.size();

    // ✓ OPTIMIZATION 2A: Fast path for 9-tap (spec kernel)
    // Manual unroll: no loop, better instruction pipelining
    if (n == 9) {
        return static_cast<float>(window_.at(0).value) * kernel[0]
             + static_cast<float>(window_.at(1).value) * kernel[1]
             + static_cast<float>(window_.at(2).value) * kernel[2]
             + static_cast<float>(window_.at(3).value) * kernel[3]
             + static_cast<float>(window_.at(4).value) * kernel[4]
             + static_cast<float>(window_.at(5).value) * kernel[5]
             + static_cast<float>(window_.at(6).value) * kernel[6]
             + static_cast<float>(window_.at(7).value) * kernel[7]
             + static_cast<float>(window_.at(8).value) * kernel[8];
    }

    // ✓ OPTIMIZATION 2B: Fallback for other kernel sizes (unlikely)
    // Generic loop: still fast, maintains flexibility
    float sum = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        sum += static_cast<float>(window_.at(i).value) * kernel[i];
    }
    return sum;
}
void FilterBlock::emitIfReady() {
    if (!pending_.has_b1 || !pending_.has_b2) return;

    FilteredPacket fp;
    fp.b1  = pending_.b1;
    fp.b2  = pending_.b2;
    fp.row = pending_.row;
    fp.col = pending_.col;
    #ifdef CYNLR_PERF_BUILD
        fp.t1 = pending_.t1;
        fp.t2 = pending_.t2;
    #endif

    out_queue_.push(fp);
    pending_ = PendingOutput{};
}