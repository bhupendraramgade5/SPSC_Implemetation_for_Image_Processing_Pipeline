#include "FilterBlock.hpp"  // pulls in FilterUtils.hpp, Queue.hpp, ConfigManager.hpp

#include <chrono>
#include <thread>
#include <stdexcept>

#if defined(_MSC_VER)
    #include <immintrin.h>
#endif


// ============================================================================
// FilterBlock — constructor
// ----------------------------------------------------------------------------
// Validates the kernel (non-empty, odd size) and initialises the SlidingWindow
// to kernel.size() slots.  half_width_ = kernel.size() / 2 is the number of
// padding elements added to each side of a row before convolution begins.
//
// Throws std::invalid_argument on bad kernel — fail fast at construction so
// the pipeline never starts in an invalid state.
// ============================================================================

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


// ============================================================================
// FilterBlock::stop
// ----------------------------------------------------------------------------
// Sets running_ to false.  Safe to call from any thread.
// run() checks running_ only when the input queue is empty, so shutdown
// latency is bounded by one spin-yield cycle (nanoseconds), not one full T.
// ============================================================================

void FilterBlock::stop() {
    running_.store(false, std::memory_order_relaxed);
}


// ============================================================================
// FilterBlock::run
// ----------------------------------------------------------------------------
// Main consumer loop.  Runs on a dedicated thread until stop() is called
// and the input queue is fully drained.
//
// Per-packet flow:
//   1. Pop a DataPacket from in_queue_.  Spin-yield if empty.
//   2. On row transition: flush the previous row's right-padding, reset the
//      window, apply left-padding for the new row.
//   3. Process v1 and v2 individually through processSample().
//   4. After each processSample(), call emitIfReady() to push a
//      FilteredPacket to out_queue_ whenever a complete pair (b1, b2) exists.
//   5. Track last_value and last_col for the right-pad flush at row end.
//
// Exit condition:
//   stop() sets running_ = false.  The loop only exits on a failed pop when
//   running_ is false — this ensures all packets already in the queue are
//   processed before shutdown (drain-then-exit semantics).
// ============================================================================

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


// ============================================================================
// FilterBlock::processSample
// ----------------------------------------------------------------------------
// Pushes one raw pixel into the sliding window.  Once the window is full,
// computes the dot product, thresholds to binary, and stages the result in
// pending_.
//
// Staging logic (PendingOutput):
//   First call after a reset or emit fills pending_.b1 and sets has_b1.
//   Second call fills pending_.b2 and sets has_b2.
//   emitIfReady() then pushes the complete pair to out_queue_.
//
// Returns true when a binary value was produced (window full), false while
// the window is still filling (first half_width_ pixels of each row).
//
// CYNLR_PERF_BUILD: records a steady_clock timestamp at each staging point
// so inter-pixel gaps can be measured in the pipeline summary.
// ============================================================================

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


// ============================================================================
// FilterBlock::flushRowEnd
// ----------------------------------------------------------------------------
// Called once at the end of each scan row to process the right-padding region.
//
// The convolution window requires half_width_ future pixels beyond the last
// real pixel.  These are synthetic — generated by applyRight() according to
// the BoundaryPolicy — and pushed through processSample() exactly like real
// pixels so the centre pixel of each padded window position is correctly
// convolved.
//
// After padding, if pending_ holds an unpaired b1 (odd number of output
// pixels in the row), a final packet is emitted with b2=0 to preserve the
// row's output count.  This is a known padding artifact, documented here so
// downstream consumers know b2=0 at a row boundary may not be a real pixel.
// ============================================================================

void FilterBlock::flushRowEnd(uint8_t  edge_value,
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


// ============================================================================
// FilterBlock::dotProduct
// ----------------------------------------------------------------------------
// Computes the weighted sum of the sliding window values against the kernel.
//
// Fast path (n == 9):
//   Fully unrolled — nine independent multiply-accumulate operations with no
//   loop counter, branch, or increment.  The compiler (MSVC /O2, GCC -O2)
//   can schedule these as a dependency chain or auto-vectorise with SSE/AVX.
//
// Fallback (n != 9):
//   Generic scalar loop for non-standard kernel sizes.  Maintains flexibility
//   for future kernel configurations without penalising the common 9-tap case.
// ============================================================================

float FilterBlock::dotProduct() const {
    const auto& kernel = config_.kernel;
    const size_t n     = kernel.size();

    // Fast path: 9-tap unrolled.
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

    // Generic fallback for other kernel sizes.
    float sum = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        sum += static_cast<float>(window_.at(i).value) * kernel[i];
    }
    return sum;
}


// ============================================================================
// FilterBlock::emitIfReady
// ----------------------------------------------------------------------------
// Pushes pending_ to out_queue_ when both b1 and b2 are staged, then resets
// pending_ for the next pair.
//
// Called unconditionally after every processSample() — the has_b1/has_b2
// check is a branch-predictor-friendly early exit when the pair is incomplete,
// so the cost is negligible on the non-emitting path.
// ============================================================================

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


// ============================================================================
// LinearFilter — constructor
// ----------------------------------------------------------------------------
// Same validation as FilterBlock — non-empty kernel, odd size.
// policy_ is taken directly from cfg.boundary_policy so the single-threaded
// pipeline honours the same config file setting as the threaded pipeline.
// ============================================================================

LinearFilter::LinearFilter(const SystemConfig& cfg)
    : cfg_(cfg)
    , threshold_(static_cast<float>(cfg.threshold))
    , window_(cfg.kernel.size())
    , half_width_(cfg.kernel.size() / 2)
    , policy_(cfg.boundary_policy)
{
    if (cfg.kernel.empty())
        throw std::invalid_argument("LinearFilter: empty kernel");
    if (cfg.kernel.size() % 2 == 0)
        throw std::invalid_argument("LinearFilter: kernel size must be odd");
}


// ============================================================================
// LinearFilter::beginRow
// ----------------------------------------------------------------------------
// Resets all per-row state and applies left-padding before the first real
// pixel of a new scan row is processed.
//
// Must be called exactly once per row transition, before the first
// processSample() call for that row.  Calling it mid-row would corrupt the
// window and produce incorrect convolution results for remaining pixels.
// ============================================================================

void LinearFilter::beginRow(uint8_t left_edge, uint64_t row) {
    current_row_ = row;
    window_.reset();
    pending_ = PendingOutput{};

    for (size_t i = 0; i < half_width_; ++i) {
        uint8_t pad = applyLeft(policy_, left_edge, half_width_ - i);
        window_.push(pad, row, 0);
    }
}


// ============================================================================
// LinearFilter::processSample
// ----------------------------------------------------------------------------
// Single-threaded equivalent of FilterBlock::processSample().
//
// Key difference from FilterBlock:
//   Returns the completed FilteredPacket via output parameter fp rather than
//   staging into out_queue_.  The caller (main_linear.cpp) handles writing
//   fp to the OutputWriter and recording timestamps — keeping this class free
//   of I/O and timing concerns and making it trivially unit-testable.
//
// Returns true when a complete pair (b1, b2) is ready in fp.
// Returns false while the window is filling or only b1 is staged.
// ============================================================================

bool LinearFilter::processSample(uint8_t        value,
                                  uint64_t       row,
                                  uint64_t       col,
                                  FilteredPacket& fp)
{
    window_.push(value, row, col);

    if (!window_.is_full()) return false;

    const WindowSlot& c       = window_.centre();
    const float       filtered = dotProduct();
    const uint8_t     binary   = (filtered >= threshold_) ? uint8_t{1} : uint8_t{0};

    if (!pending_.has_b1) {
        pending_.b1     = binary;
        pending_.row    = c.row;
        pending_.col    = c.col;
        pending_.has_b1 = true;
        return false;
    }

    // Both b1 and b2 ready — fill output packet and reset staging.
    fp.b1  = pending_.b1;
    fp.b2  = binary;
    fp.row = pending_.row;
    fp.col = pending_.col;
    pending_ = PendingOutput{};
    return true;
}


// ============================================================================
// LinearFilter::flush
// ----------------------------------------------------------------------------
// Right-pads the end of the current row and drains any staged unpaired pixel.
// Appends all resulting FilteredPackets to the out vector.
//
// Uses a vector instead of a queue so the caller can iterate over flush
// results in the same loop where it records timestamps and writes output —
// keeping the single-threaded loop structure explicit and easy to profile.
// Mirrors FilterBlock::flushRowEnd() in logic, differs only in output sink.
// ============================================================================

void LinearFilter::flush(uint8_t                       edge,
                          uint64_t                      row,
                          uint64_t                      last_col,
                          std::vector<FilteredPacket>&  out)
{
    for (size_t i = 0; i < half_width_; ++i) {
        uint8_t pad = applyRight(policy_, edge, i + 1);
        FilteredPacket fp;
        if (processSample(pad, row, last_col + 1 + i, fp))
            out.push_back(fp);
    }

    // Push any leftover unpaired pixel.
    if (pending_.has_b1 && !pending_.has_b2) {
        FilteredPacket fp;
        fp.b1  = pending_.b1;
        fp.b2  = 0;
        fp.row = pending_.row;
        fp.col = pending_.col;
        out.push_back(fp);
        pending_ = PendingOutput{};
    }
}


// ============================================================================
// LinearFilter::dotProduct
// ----------------------------------------------------------------------------
// Identical logic to FilterBlock::dotProduct().
// Kept as a separate method rather than a shared free function so the compiler
// can inline it independently into each class's hot path without cross-unit
// visibility constraints affecting the optimisation boundary.
// ============================================================================

float LinearFilter::dotProduct() const {
    const auto&  k = cfg_.kernel;
    const size_t n = k.size();

    // Fast path: 9-tap unrolled
    if (n == 9) {
        return static_cast<float>(window_.at(0).value) * k[0]
             + static_cast<float>(window_.at(1).value) * k[1]
             + static_cast<float>(window_.at(2).value) * k[2]
             + static_cast<float>(window_.at(3).value) * k[3]
             + static_cast<float>(window_.at(4).value) * k[4]
             + static_cast<float>(window_.at(5).value) * k[5]
             + static_cast<float>(window_.at(6).value) * k[6]
             + static_cast<float>(window_.at(7).value) * k[7]
             + static_cast<float>(window_.at(8).value) * k[8];
    }

    float sum = 0.0f;
    for (size_t i = 0; i < n; ++i)
        sum += static_cast<float>(window_.at(i).value) * k[i];
    return sum;
}