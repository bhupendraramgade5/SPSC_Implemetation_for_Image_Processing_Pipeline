#ifndef FILTER_UTILS_HPP
#define FILTER_UTILS_HPP

#include "ConfigManager.hpp"
#include <cstdint>
#include <vector>
#include <stdexcept>
#include <algorithm>
struct FilteredPacket {
    uint8_t  b1  = 0;
    uint8_t  b2  = 0;
    uint64_t row = 0;
    uint64_t col = 0;
};

inline uint8_t applyLeft(BoundaryPolicy policy, uint8_t edge, size_t /*offset*/) {
    switch (policy) {
        case BoundaryPolicy::REPLICATE: return edge;
        case BoundaryPolicy::ZERO_PAD:  return 0;
        default:
            throw std::runtime_error("BoundaryPolicy: unknown policy");
    }
}
inline uint8_t applyRight(BoundaryPolicy policy, uint8_t edge, size_t /*offset*/) {
    switch (policy) {
        case BoundaryPolicy::REPLICATE: return edge;
        case BoundaryPolicy::ZERO_PAD:  return 0;
        default:
            throw std::runtime_error("BoundaryPolicy: unknown policy");
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