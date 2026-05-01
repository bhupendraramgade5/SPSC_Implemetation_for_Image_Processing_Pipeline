

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

// ============================================================================
// DataPacket
// ----------------------------------------------------------------------------
// Carries two consecutive raw pixel values (v1, v2) from the same scan row,
// together with their grid coordinates (row, col).
// col always refers to v1; v2 is implicitly at col+1.
// Must remain trivially copyable — SPSCQueue and DynamicSPSCQueue copy
// items via raw assignment and the static_assert below enforces this.
// ============================================================================

struct DataPacket {
    uint8_t  v1  = 0;
    uint8_t  v2  = 0;
    uint64_t row = 0;
    uint64_t col = 0;
};

static_assert(std::is_trivially_copyable<DataPacket>::value,
              "DataPacket must be trivially copyable for safe ring-buffer use");
			  
// ============================================================================
// IDataSource
// ----------------------------------------------------------------------------
// Abstract source of DataPackets.
// Responsibility : decouple the GeneratorBlock from the origin of pixel data.
// Implementations: RandomDataSource  — infinite RNG pixel stream
//                  CSVDataSource     — finite stream read from a .csv file
//
// Contract:
//   next(packet) : fills packet and returns true while data is available.
//   Returns false (and leaves packet unchanged) when the source is exhausted.
//   Once false is returned it must stay false on all subsequent calls.
// ============================================================================

class IDataSource {
public:
    virtual bool next(DataPacket& packet) = 0;
    virtual ~IDataSource() = default;
};
// ============================================================================
// IQueue<T>
// ----------------------------------------------------------------------------
// Abstract non-blocking queue interface used for inter-block communication.
// Responsibility : decouple producers from consumers and allow the queue
//                  implementation to be swapped (mutex vs lock-free) without
//                  changing any block code.
//Functions : 
//   push() — returns true if the item was accepted, false if the queue is full.
//   pop()  — returns true and fills item if an element was available,
//             false if the queue was empty (non-blocking).
//   empty()— non-blocking snapshot; may be stale by the time caller acts on it.
// Implementations in this file:
//   SimpleQueue<T>       — mutex-protected std::queue, unbounded, for tests
//   SPSCQueue<T,N>       — lock-free ring buffer, compile-time capacity
//   DynamicSPSCQueue<T>  — lock-free ring buffer, runtime capacity
// ============================================================================
template <typename T>
class IQueue {
public:
    virtual bool push(const T& item) = 0;
    virtual bool pop(T& item) = 0;

    // Non-blocking check.
    virtual bool empty() const = 0;

    virtual ~IQueue() = default;
};


// SimpleQueue<T>
// ----------------------------------------------------------------------------
// Mutex-protected wrapper around std::queue<T>.
//
// Responsibility : provide a correct, easy-to-reason-about queue for use in
//                  unit tests and single-threaded harnesses where lock-free
//                  performance is not required.
//
// Characteristics:
//   - Unbounded — push() always returns true (never back-pressures).
//   - Thread-safe via std::mutex for both push and pop.
//   - NOT suitable for the pipeline hot path — mutex acquisition
//     cost (~20-50 ns) violates the <100 ns per-pixel budget at high rates.
//
// Use when: writing unit tests, draining queues after pipeline shutdown,
//           or feeding known test vectors into FilterBlock synchronously.
// ============================================================================

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


// ============================================================================
// SPSCQueue<T, CAPACITY>
// ----------------------------------------------------------------------------
// Lock-free Single-Producer Single-Consumer ring buffer with compile-time
// capacity.
//
// Responsibility : provide the lowest-latency inter-block channel for the
//                   pipeline where exactly one thread writes and
//                  exactly one thread reads.
//
// Design:
//   - Power-of-two CAPACITY enforced by static_assert; index masking replaces
//     modulo (single AND instruction on the hot path).
//   - head_ and tail_ are on separate cache lines (alignas(64)) to eliminate
//     false sharing between the producer and consumer cores.
//   - Acquire/release memory ordering on the index stores/loads — no fences,
//     no locks, no CAS on the data path.
//   - push() returns false (back-pressure) when the buffer is full rather
//     than blocking; the caller decides whether to spin, drop, or wait.
//
// Constraints:
//   - T must be trivially copyable (enforced by static_assert).
//   - CAPACITY must be >= 2 and a power of two (enforced by static_assert).
//   - Exactly ONE producer thread and ONE consumer thread — using from
//     multiple producers or consumers is undefined behaviour.
//
// Use when: connecting GeneratorBlock → FilterBlock in the threaded pipeline.
// ============================================================================

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
        if ((next_head - tail_.load(std::memory_order_acquire)) > MASK)
            return false;
        buffer_[head & MASK] = item;

        head_.store(next_head, std::memory_order_release);
        return true;
    }

    bool pop(T& item) override {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;
        item = buffer_[tail & MASK];

        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool empty() const override {
        return tail_.load(std::memory_order_relaxed)
            == head_.load(std::memory_order_acquire);
    }

    std::size_t size() const {
        return head_.load(std::memory_order_relaxed)
             - tail_.load(std::memory_order_relaxed);
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


// ============================================================================
// DynamicSPSCQueue<T>
// ----------------------------------------------------------------------------
// Lock-free Single-Producer Single-Consumer ring buffer with runtime-
// configurable capacity.
//
// Responsibility : same as SPSCQueue but sized from a runtime value (e.g.
//                  m/2 columns) so the queue depth scales with the scan width
//                  without recompiling.  Used in main.cpp where the column
//                  count comes from the config file.
//
// Additional features over SPSCQueue:
//   - logical_max_capacity : a second, softer limit smaller than the ring
//     buffer's physical capacity.  push() returns false when occupancy exceeds
//     this value even if ring slots are available.  Used to enforce the memory
//     budget constraint (queue depth <= m) stated in the spec.
//   - peak_occupancy_      : atomic high-water mark updated on every push via
//     a CAS loop.  Reported in the pipeline summary so the user can verify
//     the memory requirement is met at runtime.
//   - reset_peak()         : resets the high-water mark; useful between
//     measurement windows.
//
// Constraints:
//   - Same SPSC threading contract as SPSCQueue.
//   - T must be trivially copyable.
//   - capacity_hint is rounded up to the next power of two internally.
//
// Use when: connecting GeneratorBlock → FilterBlock in main.cpp where column
//           count is not known at compile time.
// ============================================================================

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
    explicit DynamicSPSCQueue(std::size_t capacity_hint, std::size_t logical_max_capacity = 0)
        : capacity_(nextPow2(std::max(capacity_hint, std::size_t{2})))
        , mask_(capacity_ - 1)
        , buffer_(capacity_)
        , head_(0)
        , tail_(0)
        , peak_occupancy_(0)
        ,logical_max_capacity_(logical_max_capacity > 0 ? logical_max_capacity : capacity_hint)
    {}

    bool push(const T& item) override {
        const std::size_t head      = head_.load(std::memory_order_relaxed);
        const std::size_t next_head = head + 1;
        const std::size_t tail      = tail_.load(std::memory_order_acquire);

        // Check if ring buffer would be full
        if ((next_head - tail) > mask_)
            return false;   // ring buffer full

        // Check if logical capacity would be exceeded
        const std::size_t occupancy = next_head - tail;
        if (logical_max_capacity_ > 0 && occupancy > logical_max_capacity_)
            return false;   // back-pressure: queue at logical limit

        buffer_[head & mask_] = item;
        head_.store(next_head, std::memory_order_release);

        std::size_t prev = peak_occupancy_.load(std::memory_order_relaxed);
        while (occupancy > prev) {
            if (peak_occupancy_.compare_exchange_weak(
                    prev, occupancy, std::memory_order_relaxed)) {
                break;  // successfully stored new peak
            }
            // prev updated by CAS on failure; loop re-checks condition
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
        return head_.load(std::memory_order_relaxed)
             - tail_.load(std::memory_order_relaxed);
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
        if (n <= 1) return 2;
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
    const std::size_t logical_max_capacity_;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif


// ============================================================================
// PipelineQueue
// ----------------------------------------------------------------------------
// Convenience alias for the production inter-block queue.
// Fixed at 64 slots — large enough to absorb one full scan row at m=128
// (64 packets × 2 pixels) without back-pressure under normal timing.
// Used in test harnesses that need a realistic queue type without knowing
// the runtime column count.
// ============================================================================

using PipelineQueue = SPSCQueue<DataPacket, 64>;

#endif // SIMPLE_QUEUE_HPP

