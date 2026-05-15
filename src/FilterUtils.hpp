#ifndef FILTER_UTILS_HPP
#define FILTER_UTILS_HPP


#include "ConfigManager.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>
struct FilteredPacket {
    uint8_t  b1  = 0;
    uint8_t  b2  = 0;
    uint8_t  flags = 0;    // bit 0: FP_FLAG_B2_IS_PAD
    uint8_t  _pad  = 0;    // explicit pad byte; compiler adds 4 more before row
    // 4 bytes of implicit compiler padding here (aligns row to offset 8)
    uint64_t row   = 0;
    uint64_t col   = 0;
#ifdef CYNLR_PERF_BUILD
    uint64_t t1    = 0;     // staging timestamp for b1 (nanoseconds)
    uint64_t t2    = 0;     // staging timestamp for b2 (nanoseconds)
#endif
};

// Bit definitions for FilteredPacket::flags
static constexpr uint8_t FP_FLAG_B2_IS_PAD = 0x01;  // b2 is row-end padding, not a real pixel

// Layout: b1(1)+b2(1)+flags(1)+_pad(1)+implicit_pad(4) = 8 bytes header,
//         then row(8)+col(8) → sizeof == 24.
#ifdef CYNLR_PERF_BUILD
static_assert(sizeof(FilteredPacket) == 40,
    "FilteredPacket (PERF build) layout changed -- "
    "expected 24-byte base + 16 bytes for t1/t2 = 40 bytes total. "
    "Update this assert and the layout comment above if the change is intentional.");
#else
static_assert(sizeof(FilteredPacket) == 24,
    "FilteredPacket (non-PERF build) layout changed -- "
    "expected b1+b2+flags+_pad (4) + implicit_pad (4) + row+col (16) = 24 bytes. "
    "Update this assert and the layout comment above if the change is intentional.");
#endif


// ============================================================================
// applyLeft / applyRight
// ----------------------------------------------------------------------------
// Boundary padding helpers used by FilterBlock and InlineLinearFilter.
// ============================================================================

inline uint8_t applyLeft(BoundaryPolicy policy,
                         uint8_t        edge,
                         size_t       /*offset*/) {
    switch (policy) {
        case BoundaryPolicy::REPLICATE: return edge;
        case BoundaryPolicy::ZERO_PAD:  return 0;
        default:
            throw std::runtime_error("applyLeft: unknown BoundaryPolicy");
    }
}

inline uint8_t applyRight(BoundaryPolicy policy,
                          uint8_t        edge,
                          size_t       /*offset*/) {
    switch (policy) {
        case BoundaryPolicy::REPLICATE: return edge;
        case BoundaryPolicy::ZERO_PAD:  return 0;
        default:
            throw std::runtime_error("applyRight: unknown BoundaryPolicy");
    }
}
struct WindowSlot {
    uint8_t  value = 0;
    uint64_t row   = 0;
    uint64_t col   = 0;
};
class SlidingWindow {
public:
    explicit SlidingWindow(size_t capacity)
        : capacity_(capacity)
        , buffer_(capacity)
        , head_(0)
        , filled_(0)
    {
        if (capacity == 0)
            throw std::invalid_argument("SlidingWindow: capacity must be > 0");
    }
    void push(uint8_t value, uint64_t row, uint64_t col) {
        buffer_[head_] = WindowSlot{value, row, col};
        head_ = (head_ + 1) % capacity_;
        if (filled_ < capacity_) ++filled_;
    }
    const WindowSlot& at(size_t logical_index) const {
        return buffer_[(head_ + logical_index) % capacity_];
    }
    const WindowSlot& centre() const { return at(capacity_ / 2); }

    void reset() {
        std::fill(buffer_.begin(), buffer_.end(), WindowSlot{});
        head_   = 0;
        filled_ = 0;
    }

    size_t capacity() const { return capacity_; }
    size_t filled()   const { return filled_;   }
    bool   is_full()  const { return filled_ == capacity_; }

private:
    size_t                  capacity_;
    std::vector<WindowSlot> buffer_;
    size_t                  head_;     // index of next write slot
    size_t                  filled_;   // number of valid samples present
};

class IThresholder {
public:
    virtual uint8_t apply(float filtered_value) const = 0;
    virtual ~IThresholder() = default;
};

class BinaryThresholder : public IThresholder {
public:
    explicit BinaryThresholder(uint8_t threshold_value)
        : threshold_(static_cast<float>(threshold_value)) {}

    uint8_t apply(float filtered_value) const override {
        return (filtered_value >= threshold_) ? uint8_t{1} : uint8_t{0};
    }

    void set_threshold(uint8_t new_tv) {
        threshold_ = static_cast<float>(new_tv);
    }

private:
    float threshold_;
};

#endif // FILTER_UTILS_HPP