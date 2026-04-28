

#ifndef QUEUE_HPP
#define QUEUE_HPP

#include <atomic>       
#include <array>        
#include <cstddef>      
#include <cstdint>      
#include <mutex>        
#include <queue>        
#include <stdexcept>    
#include <type_traits>  
#include <vector>

struct DataPacket {
    uint8_t  v1  = 0;
    uint8_t  v2  = 0;
    uint64_t row = 0;
    uint64_t col = 0;
};

static_assert(std::is_trivially_copyable<DataPacket>::value,
              "DataPacket must be trivially copyable for safe ring-buffer use");
			  
/*
struct DataPacket {
    uint8_t v1;
    uint8_t v2;
    uint64_t row;
    uint64_t col;
};
*/

class IDataSource {
public:
    virtual bool next(DataPacket& packet) = 0;
    virtual ~IDataSource() = default;
};
template <typename T>
class IQueue {
public:
    virtual bool push(const T& item) = 0;
    virtual bool pop(T& item) = 0;

    // Non-blocking check.
    virtual bool empty() const = 0;

    virtual ~IQueue() = default;
};



template <typename T>
class SimpleQueue : public IQueue<T> {
public:
    bool push(const T& item) override {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(item);
        return true;
    }

    bool pop(T& item) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        item = queue_.front();
        queue_.pop();
        return true;
    }

    bool empty() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    std::queue<T> queue_;
    // std::mutex mutex_;
	mutable std::mutex mutex_;
};



#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4324)  
#endif

template <typename T, std::size_t CAPACITY>
class SPSCQueue : public IQueue<T> {

    static_assert(std::is_trivially_copyable<T>::value,
                  "SPSCQueue<T>: T must be trivially copyable");

    static_assert(CAPACITY >= 2,
                  "SPSCQueue: CAPACITY must be at least 2");

    static_assert((CAPACITY & (CAPACITY - 1)) == 0,
                  "SPSCQueue: CAPACITY must be a power of two");

    static constexpr std::size_t MASK = CAPACITY - 1;
    static constexpr std::size_t CACHE_LINE = 64;
public:
    SPSCQueue() : head_(0), tail_(0) {}
    bool push(const T& item) override {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next_head = head + 1;
        if ((next_head - tail_.load(std::memory_order_acquire)) > MASK) {
            return false;   
        }

        buffer_[head & MASK] = item;

        head_.store(next_head, std::memory_order_release);
        return true;
    }

    bool pop(T& item) override {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_.load(std::memory_order_acquire)) {
            return false; 
        }

        item = buffer_[tail & MASK];

        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool empty() const override {
        return tail_.load(std::memory_order_relaxed)
            == head_.load(std::memory_order_acquire);
    }

    std::size_t size() const {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        return h - t;
    }

private:
    alignas(CACHE_LINE) std::atomic<std::size_t> head_;

    //char pad_[CACHE_LINE - sizeof(std::atomic<std::size_t>)];

    alignas(CACHE_LINE) std::atomic<std::size_t> tail_;

    T buffer_[CAPACITY];
};

#ifdef _MSC_VER
#pragma warning(pop)    // restore warning state — C4324 re-enabled after this point
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4324)
#endif

template <typename T>
class DynamicSPSCQueue : public IQueue<T> {

    static_assert(std::is_trivially_copyable<T>::value,
                  "DynamicSPSCQueue<T>: T must be trivially copyable");

    static constexpr std::size_t CACHE_LINE = 64;

public:
    explicit DynamicSPSCQueue(std::size_t capacity_hint)
        : capacity_(nextPow2(std::max(capacity_hint, std::size_t{2})))
        , mask_(capacity_ - 1)
        , buffer_(capacity_)
        , head_(0)
        , tail_(0)
        , peak_occupancy_(0)
    {}

    bool push(const T& item) override {
        const std::size_t head      = head_.load(std::memory_order_relaxed);
        const std::size_t next_head = head + 1;

        if ((next_head - tail_.load(std::memory_order_acquire)) > mask_)
            return false;   // full — caller must retry or handle back-pressure

        buffer_[head & mask_] = item;
        head_.store(next_head, std::memory_order_release);
        const std::size_t current =
            next_head - tail_.load(std::memory_order_relaxed);

        std::size_t prev = peak_occupancy_.load(std::memory_order_relaxed);
        while (current > prev &&
               !peak_occupancy_.compare_exchange_weak(
                   prev, current, std::memory_order_relaxed)) {
            // prev is updated by CAS on failure — loop converges immediately.
        }

        return true;
    }

    bool pop(T& item) override {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;   // empty
        item = buffer_[tail & mask_];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool empty() const override {
        return tail_.load(std::memory_order_relaxed)
            == head_.load(std::memory_order_acquire);
    }

    std::size_t size() const {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        return h - t;
    }
    std::size_t capacity() const { return capacity_; }
    std::size_t peak_occupancy() const {
        return peak_occupancy_.load(std::memory_order_relaxed);
    }

    void reset_peak() {
        peak_occupancy_.store(0, std::memory_order_relaxed);
    }

private:
    static std::size_t nextPow2(std::size_t n) {
        if (n == 0) return 1;
        --n;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        n |= n >> 32;
        return n + 1;
    }
    const std::size_t  capacity_;
    const std::size_t  mask_;
    std::vector<T>     buffer_;   // heap; data_ pointer permanently L1-resident
    alignas(CACHE_LINE) std::atomic<std::size_t> head_;
    alignas(CACHE_LINE) std::atomic<std::size_t> tail_;
    alignas(CACHE_LINE) std::atomic<std::size_t> peak_occupancy_;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
using PipelineQueue = SPSCQueue<DataPacket, 64>;

#endif // SIMPLE_QUEUE_HPP

