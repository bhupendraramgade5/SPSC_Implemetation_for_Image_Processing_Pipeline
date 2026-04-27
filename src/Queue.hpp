

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

private:
    std::queue<T> queue_;
    // std::mutex mutex_;
	mutable std::mutex mutex_;
};



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
=

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

    char pad_[CACHE_LINE - sizeof(std::atomic<std::size_t>)];

    alignas(CACHE_LINE) std::atomic<std::size_t> tail_;

    T buffer_[CAPACITY];
};

using PipelineQueue = SPSCQueue<DataPacket, 64>;
#endif // SIMPLE_QUEUE_HPP

