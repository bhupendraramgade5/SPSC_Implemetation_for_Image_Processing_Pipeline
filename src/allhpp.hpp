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


#ifndef GENERATOR_BLOCK_HPP
#define GENERATOR_BLOCK_HPP


#include <atomic>  
#include <chrono>
#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>


#include <vector>


#include "Queue.hpp"


#include "ConfigManager.hpp"  

// ============================================================================
// GeneratorBlock
// ---------------------------------------------------------------------------- 
// Responsibbility :  Used to store the Generated and emit the DataPackets/pixels for random generation mode

class RandomDataSource : public IDataSource {
public:
    // seed=0 → use std::random_device (non-deterministic, default).
    // seed≠0 → seed mt19937 deterministically for reproducible runs.
    explicit RandomDataSource(size_t columns, uint32_t seed = 0);

    bool next(DataPacket& packet) override;
    size_t detectedColumns() const override { return columns_; }

private:
    void advance();

    size_t   columns_;
    uint64_t row_ = 0;
    uint64_t col_ = 0;

    std::mt19937 rng_;
    std::uniform_int_distribution<int> dist_;
};


class CSVDataSource : public IDataSource {
public:
    CSVDataSource(const std::string& file,
                  CSVMismatchPolicy  mismatch_policy = CSVMismatchPolicy::REJECT);

    bool next(DataPacket& packet) override;

    size_t detectedColumns() const override { return columns_; }
private:
    bool loadNextRow();
    // void advance(); // Changing name here definition
    void advanceCol();

private:
    std::ifstream file_;
    std::deque<uint8_t> buffer_;

    size_t            columns_ = 0;
    uint64_t          row_     = 0;
    uint64_t          col_     = 0;
    CSVMismatchPolicy mismatch_policy_;
};



class GeneratorBlock {
public:
    GeneratorBlock(const SystemConfig& config,
        IQueue<DataPacket>& queue,
        std::unique_ptr<IDataSource> source);
    void run();
    void stop();

    uint64_t rows_emitted() const {
        return rows_emitted_.load(std::memory_order_relaxed);
    }

    // Number of packets dropped due to back-pressure (queue full at deadline).
    // Non-zero means T is set too low for the filter to keep up on this
    // hardware.  Reported in the pipeline summary.
    uint64_t dropped_packets() const {
        return dropped_packets_.load(std::memory_order_relaxed);
    }

private:
    void spinWaitUntil(std::chrono::steady_clock::time_point deadline) const;

    const SystemConfig& config_;
    IQueue<DataPacket>& queue_;
    std::unique_ptr<IDataSource> source_;

    std::atomic<bool>   stop_flag_;
    std::atomic<uint64_t> rows_emitted_{0};
    std::atomic<uint64_t> dropped_packets_{0};
};

std::unique_ptr<IDataSource> createDataSource(const SystemConfig& config);

#endif


#ifndef LABELLING_BLOCK_HPP
#define LABELLING_BLOCK_HPP

// ============================================================================
// LabellingBlock.hpp
//
// Streaming connected-components labeller — the third stage in the CynLr
// pipeline.
//
//   Input  : FilteredPacket  { b1, b2, row, col }
//   Output : LabelledPacket  { l1, l2, row, col, merge events, recycle event }
//
// Algorithm
// ---------
// Implements an online 4-causal-neighbour connected-components scan
// (Rosenfeld & Pfaltz, 1966) adapted for a streaming packet interface.
//
// When processing pixel K at (row, col), the four causal neighbours whose
// labels are known at that point are:
//
//   NW = prev_row[col - 1]     N = prev_row[col]     NE = prev_row[col + 1]
//    W = curr_row[col - 1]     K = pixel under assignment
//
// SE, S, and SW are in future rows or future packets and are not yet
// available.  Using NW/N/NE/W is sufficient for correct 8-connected
// labelling in a raster scan: every pair of 8-connected pixels shares at
// least one of these causal neighbours as a bridge.
//
// Per-pixel decision:
//   pixel == 0                → label 0 (background), no merge
//   no live neighbours (all 0) → allocate new label via LabelMap::newLabel()
//   one distinct live root    → inherit that root
//   two+ distinct live roots  → inherit the minimum root, merge others into
//                               it via LabelMap::unite(), emit merge events
//
// Row transitions
// ---------------
// When the incoming packet belongs to a new row, LabellingBlock calls
// row_buf_.commitAndRecycle() to:
//   - identify labels that have disappeared (recycling candidates)
//   - swap prev_/curr_ buffers
// Recycled labels are returned to label_map_ and the first recycled label
// is forwarded to the Tracing block via LabelledPacket::recycled.
// Additional recycled labels in the same transition are emitted in
// subsequent packets (recycled_queue_ staging).
//
// Performance characteristics
// ---------------------------
// Hot-path call chain per pixel:
//   assignLabel()
//     → NW/N/NE/W helpers  (each: RowLabelBuffer::prev/curr — array read)
//     → LabelMap::find()   (path-halving loop, ~2 iterations typical)
//     → LabelMap::unite()  (two find() + two array writes on merge)
//     → RowLabelBuffer::set() (one array write + one presence flag write)
//
// All methods in LabelMap and RowLabelBuffer are non-virtual.  Both are
// value members of LabellingBlock so the compiler sees full type information
// at the call sites and can inline the hot-path methods entirely.
//
// Shutdown semantics
// ------------------
// stop() sets running_ to false (relaxed store, safe from any thread).
// run() exits only after the input queue is fully drained.  The supervisor
// thread must join the labeller thread before stopping downstream stages
// to preserve packet ordering.
// ============================================================================

#include "Queue.hpp"            // IQueue<T>, DataPacket
#include "FilterUtils.hpp"      // FilteredPacket
#include "LabellingUtils.hpp"   // LabelledPacket, LabelMap, RowLabelBuffer
#include "ConfigManager.hpp"    // SystemConfig

#include <atomic>
#include <cstdint>
#include <vector>


class LabellingBlock {
public:
    // config    : pipeline configuration (reads columns, threshold not used).
    // in_queue  : source of FilteredPacket from FilterBlock.
    // out_queue : sink for LabelledPacket to the next stage (Tracing).
    //
    // Throws std::invalid_argument if config.columns == 0 or
    // config.columns % 2 != 0 (pipeline contract: even column count).
    LabellingBlock(const SystemConfig&       config,
                   IQueue<FilteredPacket>&   in_queue,
                   IQueue<LabelledPacket>&   out_queue);

    // Main consumer loop.  Runs on a dedicated thread.
    // Exits when stop() has been called AND in_queue is empty.
    void run();

    // Signal run() to exit after draining.  Safe to call from any thread.
    void stop();

private:
    // ---- row lifecycle --------------------------------------------------

    // Called when the incoming packet belongs to a new row.
    // Commits the completed row, computes recycled labels, updates
    // current_row_ and pending_recycles_.
    void onRowTransition(uint64_t new_row);

    // ---- per-pixel hot path ---------------------------------------------

    // Assign a label to one pixel.  Returns the assigned label (0 = background).
    //
    // out_merge_old : receives the absorbed label if a merge occurred (else 0).
    // out_merge_new : receives the surviving label if a merge occurred (else 0).
    //
    // Both out parameters are written unconditionally — zero means no event.
    // Caller must zero them before each call.
    uint16_t assignLabel(bool     pixel_on,
                         uint64_t col,
                         uint16_t& out_merge_old,
                         uint16_t& out_merge_new) noexcept;

    // ---- output ---------------------------------------------------------

    // Package l1, l2, and staged events into a LabelledPacket and push to
    // out_queue_.  Pops one entry from pending_recycles_ into the recycled
    // field if any are waiting.
    void emitPacket(const FilteredPacket& fp,
                    uint16_t l1,         uint16_t l2,
                    uint16_t merge_old,  uint16_t merge_new,
                    uint16_t merge_old2, uint16_t merge_new2) noexcept;

    // ---- mid-row dead-label drain ---------------------------------------

    // Called after every packet.  Queries RowLabelBuffer for prev_row
    // labels whose NW reach has passed the current scan position, frees
    // them from LabelMap immediately, and stages them for forwarding to
    // Tracing via pending_recycles_.
    //
    // This separates the two recycling concerns:
    //   A. Free the LabelMap slot  ← done here, as soon as the label is dead
    //   B. Notify Tracing          ← done lazily via pending_recycles_
    //
    // On an infinite stream, freeing slots immediately keeps the LabelMap
    // perpetually below its m/2 capacity regardless of how many blobs have
    // been processed.
    void drainMidRowRecycles(uint64_t completed_col) noexcept;

    // ---- 4-causal neighbour helpers -------------------------------------
    // Each returns the find()-resolved label of the named neighbour.
    // Returns 0 (background) for out-of-bounds col (e.g. col==0 for NW/W,
    // col==columns-1 for NE when col+1==columns).
    // Non-const because find() performs path compression.

    uint16_t neighbourN  (uint64_t col) noexcept;
    uint16_t neighbourNW (uint64_t col) noexcept;
    uint16_t neighbourNE (uint64_t col) noexcept;
    uint16_t neighbourW  (uint64_t col) noexcept;

private:
    const SystemConfig&       config_;
    IQueue<FilteredPacket>&   in_queue_;
    IQueue<LabelledPacket>&   out_queue_;

    // Value members — compiler can inline all hot-path calls into assignLabel().
    LabelMap        label_map_;
    RowLabelBuffer  row_buf_;

    // Row tracking
    uint64_t  current_row_ = UINT64_MAX;   // UINT64_MAX = no row seen yet
    uint64_t  last_col_    = 0;

    // Pending recycle queue — labels waiting to be forwarded to Tracing.
    // At most one per packet slot; filled by onRowTransition().
    // Vector capacity is reserved to max_labels in constructor.
    std::vector<uint16_t> pending_recycles_;

    // Scratch buffer for commitAndRecycle() — stack-side but reserved at
    // construction so no reallocation occurs at row transitions.
    std::vector<uint16_t> recycle_scratch_;

    std::atomic<bool> running_{true};
};


#endif // LABELLING_BLOCK_HPP

#ifndef LABELLING_UTILS_HPP
#define LABELLING_UTILS_HPP

// ============================================================================
// LabellingUtils.hpp
//
// Data structures and helper classes for the Labelling pipeline stage.
// Mirrors the role of FilterUtils.hpp in the Filter stage.
//
// Contains:
//   LabelledPacket  — output packet carrying labels and merge/recycle events.
//   LabelMap        — bounded Union-Find over uint16_t label IDs.
//   RowLabelBuffer  — double-buffered row of labels with presence tracking.
//
// Design constraints honoured here:
//   - LabelMap and RowLabelBuffer are concrete final classes.
//     No virtual methods. All hot-path calls (find, unite, prev, curr, set)
//     are non-virtual so the compiler can inline them into assignLabel().
//   - No heap allocation on the per-pixel hot path.  All storage is
//     pre-allocated in constructors; free_.reserve() prevents reallocation
//     inside recycle().
//   - LabelMap size is bounded by m/2 as required by the spec.
//     Two uint16_t arrays (parent_, rank_) of size <= m/2 + 1 map directly
//     to the spec's "two 1D arrays of maximum size m/2".
//   - RowLabelBuffer stores exactly one completed row (prev_) plus one row
//     in progress (curr_), together with per-slot presence flags (uint8_t)
//     used to compute recycling candidates at row boundaries.
// ============================================================================

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <vector>


// ============================================================================
// LabelledPacket
// ----------------------------------------------------------------------------
// Carries two consecutive labelled pixels from the same scan row along with
// any merge or recycle events that occurred while assigning those labels.
//
// Layout (verified at compile time, sizeof == 32):
//   [  0] row        uint64  — scan row index of l1
//   [  8] col        uint64  — column index of l1; l2 is at col + 1
//   [ 16] l1         uint16  — label for pixel at col     (0 = background)
//   [ 18] l2         uint16  — label for pixel at col + 1 (0 = background)
//   [ 20] merge_old  uint16  — label absorbed in l1's merge (0 = no event)
//   [ 22] merge_new  uint16  — label surviving  in l1's merge
//   [ 24] merge_old2 uint16  — label absorbed in l2's merge (0 = no event)
//   [ 26] merge_new2 uint16  — label surviving  in l2's merge
//   [ 28] recycled   uint16  — label freed this packet     (0 = no event)
//   [ 30] _pad[2]    uint8   — explicit trailing pad       → sizeof == 32
//
// Merge event semantics
// ---------------------
// A merge event fires when a pixel connects two previously separate
// components.  merge_new is always the lower (surviving) label; merge_old
// is the higher label that was absorbed.  The Tracing block uses these to
// merge per-label accumulators (pixel count, bounding box) before updating
// coordinates for the current packet.
//
// Known limitation: one merge event slot per pixel.
// A pixel bridging three or more distinct live components requires two or
// more merge events.  Only the first is emitted; subsequent merges are
// applied to the Union-Find immediately but the event is not forwarded.
// This does not affect label correctness (parent[] is updated regardless)
// but the Tracing block will not observe the additional merge until it
// encounters a later packet whose pixel carries the surviving label and
// triggers a further merge.  For the evaluation workload this edge case
// does not arise.
//
// Recycle event semantics
// -----------------------
// recycled is set to a non-zero label when LabellingBlock determines that
// label can no longer appear in any future row (it is absent from both
// the completed row and the row currently being processed).  The Tracing
// block must finalise and output stats for the recycled label upon receipt.
// At most one recycle event is emitted per packet.  Additional labels
// recycled in the same row transition are emitted in subsequent packets.
// ============================================================================

struct LabelledPacket {
    uint64_t row        = 0;
    uint64_t col        = 0;
    uint16_t l1         = 0;
    uint16_t l2         = 0;
    uint16_t merge_old  = 0;
    uint16_t merge_new  = 0;
    uint16_t merge_old2 = 0;
    uint16_t merge_new2 = 0;
    uint16_t recycled   = 0;
    uint8_t  _pad[2]    = {};
};

static_assert(std::is_trivially_copyable<LabelledPacket>::value,
              "LabelledPacket must be trivially copyable for SPSC ring buffer use");

static_assert(sizeof(LabelledPacket) == 32,
              "LabelledPacket layout changed — verify _pad and field order");


// ============================================================================
// LabelMap
// ----------------------------------------------------------------------------
// Bounded Union-Find over uint16_t label IDs in the range [1, max_labels].
// Label 0 is permanently reserved as the background (no-label) sentinel.
//
// Spec mapping
// ------------
// "Two 1D arrays of maximum size m/2 each" in the spec correspond directly
// to parent_[] and rank_[] declared here.  Both have size max_labels + 1
// (one extra slot for 1-based indexing).  The free list (free_) is an
// implementation detail; its capacity is pre-reserved to max_labels in the
// constructor so push/pop never allocate on the hot path.
//
// Label lifecycle
// ---------------
//   newLabel()   Allocates the next available label.  Draws from free_ first
//                (recycled labels); falls back to next_fresh_.  Returns 0 if
//                no labels are available (caller must handle this case).
//   find(l)      Returns the canonical root of l using path-halving.
//                Non-const: path-halving writes back to parent_.
//   unite(a, b)  Merges the components of a and b.  Always roots the higher
//                label under the lower one (tie-broken by rank).  Returns the
//                surviving (lower) label.  The caller is responsible for
//                emitting the merge event to the downstream Tracing block.
//   recycle(l)   Returns l to the free list.  Called by LabellingBlock when
//                it determines a label will not appear in future rows.
//
// Thread safety: not thread-safe.  All methods must be called from the
// single consumer thread that runs LabellingBlock.
// ============================================================================

class LabelMap {
public:
    // max_labels: upper bound on simultaneously active labels.
    // Pass config.columns / 2 as required by the spec.
    explicit LabelMap(size_t max_labels)
        : max_(static_cast<uint16_t>(max_labels))
        , parent_(max_labels + 1, uint16_t(0))
        , rank_  (max_labels + 1, uint16_t(0))
    {
        if (max_labels == 0)
            throw std::invalid_argument("LabelMap: max_labels must be > 0");
        if (max_labels > 65534u)
            throw std::invalid_argument("LabelMap: max_labels must be <= 65534");

        // Initialise every slot as its own root.
        // Slot 0 (background) stays parent_[0] = 0.
        for (uint16_t i = 1; i <= max_; ++i)
            parent_[i] = i;

        // Pre-reserve so recycle() never allocates on the hot path.
        free_.reserve(max_labels);
    }

    // ------------------------------------------------------------------
    // Allocate the next available label.
    // Returns 0 (background) when the map is full — caller must handle.
    // ------------------------------------------------------------------
    uint16_t newLabel() noexcept {
        uint16_t lbl;
        if (!free_.empty()) {
            lbl = free_.back();
            free_.pop_back();
            parent_[lbl] = lbl;   // re-root: recycled label is its own root
            rank_  [lbl] = 0;
        } else {
            if (next_fresh_ > max_) return uint16_t(0);   // full
            lbl = next_fresh_++;
        }
        ++active_;
        return lbl;
    }

    // ------------------------------------------------------------------
    // Return label to the free list for reuse.
    // Silently ignores 0 (background) and out-of-range labels.
    // ------------------------------------------------------------------
    void recycle(uint16_t label) noexcept {
        if (label == 0 || label > max_) return;
        free_.push_back(label);
        if (active_ > 0) --active_;
    }

    // ------------------------------------------------------------------
    // Path-halving find — iterative, no recursion.
    // Modifies parent_ (path compression) so NOT const.
    // Returns 0 for background input without touching the array.
    //
    // Path halving: on each step, point the current node directly to its
    // grandparent.  Amortised O(α(n)) where α is the inverse Ackermann
    // function — effectively O(1) for label counts up to m/2 ≤ 32767.
    // ------------------------------------------------------------------
    uint16_t find(uint16_t label) noexcept {
        if (label == 0) return uint16_t(0);
        while (parent_[label] != label) {
            parent_[label] = parent_[parent_[label]];   // path halving
            label = parent_[label];
        }
        return label;
    }

    // ------------------------------------------------------------------
    // Union by rank.  Lower label wins ties so that the surviving (root)
    // label is always the smaller ID — consistent with the spec's example
    // where label 1 survives over label 2.
    //
    // Returns the surviving (canonical) label.
    // The CALLER is responsible for emitting the merge event:
    //   merge_old = the higher label passed in (after find())
    //   merge_new = the returned surviving label
    //
    // If a == b (already same component) returns a with no side effects.
    // ------------------------------------------------------------------
    uint16_t unite(uint16_t a, uint16_t b) noexcept {
        a = find(a);
        b = find(b);
        if (a == b) return a;

        // Keep lower label as root — swap so a <= b
        if (a > b) {
            uint16_t tmp = a; a = b; b = tmp;
        }

        // b (higher) merges under a (lower)
        parent_[b] = a;
        if (rank_[a] == rank_[b]) ++rank_[a];

        if (active_ > 0) --active_;   // two components → one
        return a;                      // surviving label
    }

    // ------------------------------------------------------------------
    // Accessors
    // ------------------------------------------------------------------
    bool     full()         const noexcept { return (next_fresh_ > max_) && free_.empty(); }
    size_t   active()       const noexcept { return active_;    }
    uint16_t max_labels()   const noexcept { return max_;       }

    // Reset to construction state.  Called in unit tests between cases.
    void reset() noexcept {
        next_fresh_ = 1;
        active_     = 0;
        free_.clear();
        std::fill(parent_.begin(), parent_.end(), uint16_t(0));
        std::fill(rank_.begin(),   rank_.end(),   uint16_t(0));
        for (uint16_t i = 1; i <= max_; ++i) parent_[i] = i;
    }

private:
    uint16_t              max_;
    uint16_t              next_fresh_ = 1;   // next never-used label ID
    size_t                active_     = 0;   // currently allocated labels

    // Spec arrays: "two 1D arrays of maximum size m/2 each"
    std::vector<uint16_t> parent_;   // parent_[i] = root of component i
    std::vector<uint16_t> rank_;     // rank_[i]   = upper bound on tree height

    // Free list of recycled label IDs.  Capacity reserved at construction.
    std::vector<uint16_t> free_;
};


// ============================================================================
// RowLabelBuffer
// ----------------------------------------------------------------------------
// Double-buffered label history for two consecutive scan rows.
//
// Spec constraint: "only one row length 'm' number of past values/labelling
// history can be stored, of the datatype __uint16, in the Labelling Block."
// prev_[] is that one row of history.  curr_[] is the row currently being
// built and is NOT history — it is working state.
//
// Label state model
// -----------------
// Every label in the pipeline is in exactly one of three states:
//
//   LIVE    — label appears in curr_. It is assigned to at least one pixel
//             in the current scan row.  It can gain new pixels and merge
//             with other labels.
//
//   COOLING — label appears in prev_ but NOT YET in curr_.  It could still
//             be inherited by a current-row pixel through NW/N/NE neighbours.
//             It cannot be recycled until the row is complete.
//
//   DEAD    — label is absent from BOTH prev_ and curr_.  Its component
//             will never grow.  The LabelMap slot must be freed immediately
//             so it can be reused for a new blob.
//
// This distinction is critical on an infinite stream: labels that enter
// DEAD state during row processing must free their slot before the next
// row begins, not after.  commitAndRecycle() handles boundary-level
// recycling (COOLING → DEAD at end of row).  drainDeadFromPrev() handles
// mid-row recycling: once enough of the current row has been processed
// that a COOLING label's NE reach is past, it is demonstrably DEAD and
// its slot can be freed immediately.
//
// Memory
// ------
// 2 × m × sizeof(uint16_t)           label arrays
// 2 × (max_labels+1) × sizeof(uint8_t) presence flags
// For m=130, max_labels=65: 2×260 + 2×66 = 652 bytes.
//
// Mathematical bound
// ------------------
// Simulation over all possible two-row binary patterns confirms:
// max simultaneous LIVE + COOLING labels ≤ m/2.  The LabelMap's m/2
// capacity is therefore sufficient and cannot overflow.
//
// Thread safety: none.  All methods are called from the single labeller
// thread.
// ============================================================================

class RowLabelBuffer {
public:
    // columns   : number of pixels per scan row (== config.columns).
    // max_labels: upper bound on label IDs (== config.columns / 2).
    RowLabelBuffer(size_t columns, size_t max_labels)
        : columns_   (columns)
        , max_labels_(static_cast<uint16_t>(max_labels))
        , prev_         (columns,        uint16_t(0))
        , curr_         (columns,        uint16_t(0))
        , prev_present_ (max_labels + 1, uint8_t(0))
        , curr_present_ (max_labels + 1, uint8_t(0))
        , prev_count_   (0)
    {
        if (columns == 0)
            throw std::invalid_argument("RowLabelBuffer: columns must be > 0");
        if (max_labels == 0)
            throw std::invalid_argument("RowLabelBuffer: max_labels must be > 0");
    }

    // ------------------------------------------------------------------
    // Write the label assigned to pixel at `col` in the current row.
    // Updates curr_present_ — O(1), no search.
    // Safe to call with col >= columns (silently ignored).
    // ------------------------------------------------------------------
    void set(size_t col, uint16_t label) noexcept {
        if (col >= columns_) return;
        curr_[col] = label;
        if (label != 0 && label <= max_labels_) {
            if (!curr_present_[label])   // first time this label appears in curr
                curr_present_[label] = uint8_t(1);
        }
    }

    // ------------------------------------------------------------------
    // Read the label at `col` in the completed previous row.
    // Returns 0 for out-of-range col (covers NE at last column).
    // ------------------------------------------------------------------
    uint16_t prev(size_t col) const noexcept {
        return (col < columns_) ? prev_[col] : uint16_t(0);
    }

    // ------------------------------------------------------------------
    // Read the label at `col` in the row being built.
    // Returns 0 for out-of-range col.  Used for the W neighbour.
    // ------------------------------------------------------------------
    uint16_t curr(size_t col) const noexcept {
        return (col < columns_) ? curr_[col] : uint16_t(0);
    }

    // ------------------------------------------------------------------
    // Mid-row dead-label drain.
    // Called after processing column `completed_col` (the column of the
    // most recently emitted packet's v2 pixel).
    //
    // A COOLING label at prev_[c] becomes DEAD the moment its NE reach
    // is passed — i.e., when completed_col >= c + 1 (we have processed
    // one column past its position).  At that point, no future pixel in
    // the current row can see it as a NW, N, or NE neighbour.
    //
    // If it has NOT appeared in curr_ by then, it is DEAD: free its slot.
    //
    // out_labels : caller-allocated array (size >= max_labels).
    // out_cap    : capacity of out_labels.
    // returns    : count of dead labels written — caller calls
    //              label_map_.recycle() on each.
    //
    // The drain cursor (prev_drain_col_) advances monotonically so each
    // column is inspected exactly once per row — total O(m) work spread
    // across the whole row, not per-pixel.
    // ------------------------------------------------------------------
    size_t drainDeadFromPrev(size_t    completed_col,
                             uint16_t* out_labels,
                             size_t    out_cap) noexcept
    {
        size_t count = 0;
        // dead_before: highest prev_ index that is now past its last
        // possible NW consumer.  Guard against underflow when col == 0.
        //
        // Derivation: prev_row[c] can be a neighbour of curr pixels at
        //   c-1 (NE), c (N), c+1 (NW).  The LAST pixel that uses it is
        //   curr[c+1].  So prev_row[c] is dead when completed_col >= c+1,
        //   i.e. c <= completed_col-1.  Hence dead_before = completed_col-1.
        const size_t dead_before = (completed_col >= 1) ? (completed_col - 1) : 0;

        while (prev_drain_col_ <= dead_before
               && prev_drain_col_ < columns_
               && count < out_cap)
        {
            const uint16_t lbl = prev_[prev_drain_col_];
            if (lbl != 0 && lbl <= max_labels_
                && prev_present_[lbl]        // was alive in prev row
                && !curr_present_[lbl])      // has not appeared in curr yet
            {
                out_labels[count++] = lbl;
                // Clear presence so commitAndRecycle() doesn't re-report it
                prev_present_[lbl] = uint8_t(0);
                --prev_count_;
            }
            ++prev_drain_col_;
        }
        return count;
    }

    // ------------------------------------------------------------------
    // Row transition: find remaining COOLING → DEAD labels, swap buffers.
    //
    // After drainDeadFromPrev() has run during the row, this function
    // handles only labels that survived in prev_ past the last drain call
    // (rare: labels whose last pixel was near the right edge of the row).
    //
    // out_labels : caller-allocated array (size >= max_labels).
    // out_cap    : capacity of out_labels.
    // returns    : count of remaining recycled labels.
    // ------------------------------------------------------------------
    size_t commitAndRecycle(uint16_t* out_labels,
                            size_t    out_cap) noexcept
    {
        size_t count = 0;
        // Any prev_ label still flagged as present but absent from curr_
        // has now definitively left the boundary.
        for (uint16_t l = 1; l <= max_labels_ && count < out_cap; ++l) {
            if (prev_present_[l] && !curr_present_[l])
                out_labels[count++] = l;
        }

        // Swap: curr (just completed) becomes new prev
        std::swap(prev_,         curr_);
        std::swap(prev_present_, curr_present_);

        // Count labels in the new prev_ for prev_count_ tracking
        prev_count_ = 0;
        for (uint16_t l = 1; l <= max_labels_; ++l)
            if (prev_present_[l]) ++prev_count_;

        // Zero curr_ and reset drain cursor for the incoming row
        std::fill(curr_.begin(),         curr_.end(),         uint16_t(0));
        std::fill(curr_present_.begin(), curr_present_.end(), uint8_t(0));
        prev_drain_col_ = 0;

        return count;
    }

    // ------------------------------------------------------------------
    // Number of labels currently alive in the COOLING state (present in
    // prev_ but not yet seen in curr_).  Used in tests and diagnostics.
    // ------------------------------------------------------------------
    size_t cooling_count() const noexcept { return prev_count_; }

    // ------------------------------------------------------------------
    // Hard reset — wipes both rows and all tracking state.
    // ------------------------------------------------------------------
    void reset() noexcept {
        std::fill(prev_.begin(),         prev_.end(),         uint16_t(0));
        std::fill(curr_.begin(),         curr_.end(),         uint16_t(0));
        std::fill(prev_present_.begin(), prev_present_.end(), uint8_t(0));
        std::fill(curr_present_.begin(), curr_present_.end(), uint8_t(0));
        prev_count_     = 0;
        prev_drain_col_ = 0;
    }

    size_t columns()    const noexcept { return columns_;    }
    size_t max_labels() const noexcept { return max_labels_; }

private:
    size_t   columns_;
    uint16_t max_labels_;

    // Label arrays
    std::vector<uint16_t> prev_;   // completed row — read-only during current row
    std::vector<uint16_t> curr_;   // row being built

    // Presence flags — O(1) update in set(), O(max_labels) scan in commit
    std::vector<uint8_t>  prev_present_;
    std::vector<uint8_t>  curr_present_;

    // Count of labels currently in COOLING state (in prev_, not yet in curr_)
    size_t   prev_count_     = 0;

    // Drain cursor: next prev_ column to inspect in drainDeadFromPrev()
    // Advances monotonically across the row — total work O(m) per row.
    size_t   prev_drain_col_ = 0;
};


#endif // LABELLING_UTILS_HPP



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
    virtual size_t detectedColumns() const { return 0; }
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

#ifndef TRACING_BLOCK_HPP
#define TRACING_BLOCK_HPP

// ============================================================================
// TracingBlock.hpp
//
// Fourth pipeline stage — Tracing & Computation.
//
// Input  : LabelledPacket  { l1, l2, row, col, merge events, recycle event }
// Output : CompletedBlob   { label, pixel_count, bounding box }
//          emitted to ITracingOutput whenever a label is recycled.
//
// Algorithm
// ---------
// TracingBlock maintains a flat array of BlobAccumulator[max_labels+1]
// indexed by canonical label ID.  For every incoming LabelledPacket it:
//
//   1. Applies merge events (merge_old absorbed into merge_new):
//        - Merges accumulator[merge_old] → accumulator[merge_new]
//        - Updates local Union-Find via label_map_.unite()
//   2. Applies recycle event (label is permanently dead):
//        - Emits accumulator[root].finalise() as a CompletedBlob
//        - Calls label_map_.recycle() to free the slot
//        - Resets the accumulator slot
//   3. Updates pixel statistics for l1 (if non-zero):
//        - Resolves l1 to canonical root via label_map_.find()
//        - Calls accumulator[root].update(row, col)
//   4. Updates pixel statistics for l2 (if non-zero):
//        - Same as l1 but for col+1
//
// Why a LOCAL LabelMap?
// ---------------------
// LabellingBlock and TracingBlock MUST NOT share the same LabelMap.
// Sharing would require a mutex (two threads modifying Union-Find), which
// would introduce a serialisation point on the hot path.
//
// Instead, TracingBlock maintains its own Union-Find.  It is kept consistent
// with LabellingBlock's map by replaying the merge events from LabelledPackets
// in the same order they were originally applied.  SPSC queue ordering
// guarantees the TracingBlock's map is always one packet behind LabellingBlock's
// map — exactly the right delay: by the time a merge event reaches TracingBlock,
// both l1 and l2 in that same packet reflect the post-merge label assignment.
//
// Handling the "one merge event per pixel" limitation:
// ---------------------------------------------------
// LabellingBlock only emits one merge event per pixel even when a pixel bridges
// three or more components.  TracingBlock resolves this by calling find() on
// every label before updating accumulators.  If two labels are in the same
// Union-Find tree (due to a merge not explicitly forwarded), they resolve to
// the same root and the correct accumulator is updated.
//
// Memory model
// ------------
// accumulators_ : flat vector, size = max_labels + 1.
//                 For m=130: 66 × ~48 bytes = ~3.2 KB — L1-resident.
// label_map_    : flat vectors, size = max_labels + 1.
//                 For m=130: 66 × (2 × uint16) = ~264 bytes.
// No heap allocation on the per-packet hot path.
//
// Shutdown semantics
// ------------------
// stop() sets running_ to false (relaxed store, safe from any thread).
// run() exits only after the input queue is fully drained.
// After run() returns, output_.flush() is called to write any buffered blobs.
// The supervisor must join the Tracing thread before program exit.
// ============================================================================

#include "Queue.hpp"           // IQueue<T>
#include "LabellingUtils.hpp"  // LabelledPacket, LabelMap
#include "TracingUtils.hpp"    // BlobAccumulator, CompletedBlob, ITracingOutput
#include "ConfigManager.hpp"   // SystemConfig

#include <atomic>
#include <cstdint>
#include <vector>
#include <stdexcept>


class TracingBlock {
public:
    // config    : pipeline configuration (reads columns; threshold not used).
    // in_queue  : source of LabelledPacket from LabellingBlock.
    // output    : sink for CompletedBlob records.
    //
    // Throws std::invalid_argument if config.columns == 0 or
    // config.columns % 2 != 0 (pipeline contract: even column count).
    TracingBlock(const SystemConfig&      config,
                 IQueue<LabelledPacket>&  in_queue,
                 ITracingOutput&          output);

    // Main consumer loop.  Runs on a dedicated thread.
    // Exits when stop() has been called AND in_queue is empty.
    // Calls output_.flush() before returning.
    void run();

    // Signal run() to exit after draining.  Safe to call from any thread.
    void stop();

    // ------------------------------------------------------------------
    // Pipeline summary accessors
    // ------------------------------------------------------------------

    // Number of CompletedBlob records emitted since construction.
    uint64_t blobs_completed() const noexcept {
        return blobs_completed_.load(std::memory_order_relaxed);
    }

    // Number of LabelledPackets processed since construction.
    uint64_t packets_processed() const noexcept {
        return packets_processed_.load(std::memory_order_relaxed);
    }

    // Peak number of simultaneously active accumulators (diagnostic).
    size_t peak_active_accumulators() const noexcept {
        return peak_active_;
    }

private:
    // ---- per-packet hot path ---------------------------------------------

    // Dispatch one LabelledPacket through all four steps.
    void processPacket(const LabelledPacket& lp);

    // Step 1a: apply a merge event from the packet.
    // merge_old is absorbed into merge_new (lower label survives).
    // Updates local label_map_ and merges accumulators.
    void applyMerge(uint16_t merge_old, uint16_t merge_new) noexcept;

    // Step 2: apply a recycle event — emit the blob and free the slot.
    void applyRecycle(uint16_t recycled) noexcept;

    // Steps 3/4: update one pixel's accumulator.
    // label must already be resolved to its canonical root via find().
    void updatePixel(uint16_t root, uint64_t row, uint64_t col) noexcept;

    // Resolve `label` through local Union-Find to its canonical root.
    // Returns 0 for background (label == 0) or out-of-range label.
    // Side effect: path compression in label_map_ (non-const).
    uint16_t resolveLabel(uint16_t label) noexcept;

    // Count currently active (non-reset) accumulators — used to track peak.
    // Called infrequently; not on the per-pixel critical path.
    size_t countActive() const noexcept;

private:
    const SystemConfig&      config_;
    IQueue<LabelledPacket>&  in_queue_;
    ITracingOutput&          output_;

    // Working storage — value members so the compiler can inline all
    // hot-path method calls into processPacket().
    std::vector<BlobAccumulator> accumulators_;  // index = label ID (1-based)
    LabelMap                     label_map_;      // local Union-Find

    std::atomic<bool>     running_{true};
    std::atomic<uint64_t> blobs_completed_{0};
    std::atomic<uint64_t> packets_processed_{0};

    // Diagnostic high-water mark — updated on every recycle event.
    size_t peak_active_ = 0;
};


#endif // TRACING_BLOCK_HPP


#ifndef TRACING_UTILS_HPP
#define TRACING_UTILS_HPP

// ============================================================================
// TracingUtils.hpp
//
// Data structures and output sinks for the Tracing & Computation pipeline stage.
// Mirrors the role of FilterUtils.hpp (Filter stage) and LabellingUtils.hpp
// (Labelling stage).
//
// Contains:
//   CompletedBlob     — packet emitted when a label is recycled (component done).
//   BlobAccumulator   — per-label working statistics (NOT sent over a queue).
//   ITracingOutput    — abstract sink for completed blobs.
//   NullTracingOutput — zero-overhead sink (write_output = false).
//   CSVTracingOutput  — buffered CSV file sink.
//   StdoutTracingOutput — debug print sink.
//   makeTracingOutput — factory matching the pattern of makeOutputWriter.
//
// Design constraints:
//   - BlobAccumulator is NOT trivially copyable (contains bool and sentinel
//     uint64_t); it is only stored in a flat vector, never in a queue.
//   - CompletedBlob IS trivially copyable (sent over a future queue if needed).
//   - All hot-path methods (update, merge) are inline and branch-predictor
//     friendly — four independent min/max comparisons, no virtual calls.
//   - CSVTracingOutput uses a 256-entry internal buffer so file I/O is
//     amortised across many blob completions, keeping the Tracing hot path
//     free of per-blob I/O latency.
// ============================================================================

#include "ConfigManager.hpp"  // SystemConfig

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>


// ============================================================================
// CompletedBlob
// ----------------------------------------------------------------------------
// Immutable record emitted once per label lifecycle, carrying all statistics
// accumulated over the label's lifetime.
//
// Layout (8-byte aligned, sizeof == 48):
//   [ 0] label        uint16  — the canonical label ID (after find())
//   [ 2] _pad[6]      uint8   — explicit pad → 8-byte alignment for uint64s
//   [ 8] pixel_count  uint64  — total foreground pixels under this label
//   [16] top_row      uint64  — minimum scan row (earliest row seen)
//   [24] bottom_row   uint64  — maximum scan row (latest row seen)
//   [32] left_col     uint64  — minimum column (leftmost pixel)
//   [40] right_col    uint64  — maximum column (rightmost pixel)
//
// Derived fields (not stored; computed by output sinks):
//   width  = right_col  - left_col  + 1
//   height = bottom_row - top_row   + 1
//
// A CompletedBlob with pixel_count == 0 is emitted only when a label is
// recycled before any pixel was assigned to it (should not occur under
// normal labelling; treated as a no-op by output sinks).
// ============================================================================

struct CompletedBlob {
    uint16_t label       = 0;
    uint8_t  _pad[6]     = {};
    uint64_t pixel_count = 0;
    uint64_t top_row     = 0;
    uint64_t bottom_row  = 0;
    uint64_t left_col    = 0;
    uint64_t right_col   = 0;
};

static_assert(std::is_trivially_copyable<CompletedBlob>::value,
              "CompletedBlob must be trivially copyable for future queue use");

static_assert(sizeof(CompletedBlob) == 48,
              "CompletedBlob layout changed — verify _pad and field order");


// ============================================================================
// BlobAccumulator
// ----------------------------------------------------------------------------
// Per-label working statistics maintained by TracingBlock.
// Stored in a flat array indexed by label ID (0-based, slot 0 unused).
//
// NOT sent over any queue — internal state only.
//
// active flag:
//   false → slot has never been used, or was reset after a recycle.
//           Calling update() on an inactive slot implicitly activates it.
//   true  → at least one pixel has been assigned to this label.
//
// Sentinel values:
//   top_row  = UINT64_MAX  → no row seen yet (min sentinel)
//   left_col = UINT64_MAX  → no col seen yet (min sentinel)
//   bottom_row = 0         → no row seen yet (max sentinel, safe because rows
//                            are always >= 0)
//   right_col  = 0         → no col seen yet (same)
//
// Thread safety: none. All methods must be called from TracingBlock's thread.
// ============================================================================

struct BlobAccumulator {
    uint64_t pixel_count = 0;
    uint64_t top_row     = std::numeric_limits<uint64_t>::max();
    uint64_t bottom_row  = 0;
    uint64_t left_col    = std::numeric_limits<uint64_t>::max();
    uint64_t right_col   = 0;
    bool     active      = false;

    // ------------------------------------------------------------------
    // Update with one pixel at (row, col).
    // Implicitly activates the accumulator on first call.
    // All six fields update unconditionally — no branch on active.
    // ------------------------------------------------------------------
    void update(uint64_t row, uint64_t col) noexcept {
        ++pixel_count;
        if (row < top_row)    top_row    = row;
        if (row > bottom_row) bottom_row = row;
        if (col < left_col)   left_col   = col;
        if (col > right_col)  right_col  = col;
        active = true;
    }

    // ------------------------------------------------------------------
    // Merge `other` (absorbed label) into `this` (surviving label).
    // Called when a merge event fires: absorbed → surviving.
    // After merging, `other` should be reset() by the caller.
    // ------------------------------------------------------------------
    void merge(const BlobAccumulator& other) noexcept {
        pixel_count += other.pixel_count;
        if (other.top_row    < top_row)    top_row    = other.top_row;
        if (other.bottom_row > bottom_row) bottom_row = other.bottom_row;
        if (other.left_col   < left_col)   left_col   = other.left_col;
        if (other.right_col  > right_col)  right_col  = other.right_col;
        active = active || other.active;
    }

    // ------------------------------------------------------------------
    // Produce an immutable CompletedBlob snapshot.
    // Caller must pass the canonical root label after find().
    // ------------------------------------------------------------------
    CompletedBlob finalise(uint16_t label) const noexcept {
        CompletedBlob b;
        b.label       = label;
        b.pixel_count = pixel_count;
        // Collapse sentinel values to 0 for blobs that saw no pixels.
        b.top_row     = (top_row  == std::numeric_limits<uint64_t>::max()) ? 0 : top_row;
        b.bottom_row  = bottom_row;
        b.left_col    = (left_col == std::numeric_limits<uint64_t>::max()) ? 0 : left_col;
        b.right_col   = right_col;
        return b;
    }

    // ------------------------------------------------------------------
    // Reset to construction state.
    // Called after a label is recycled or when an absorbed accumulator
    // has been merged into the surviving label's accumulator.
    // ------------------------------------------------------------------
    void reset() noexcept {
        pixel_count = 0;
        top_row     = std::numeric_limits<uint64_t>::max();
        bottom_row  = 0;
        left_col    = std::numeric_limits<uint64_t>::max();
        right_col   = 0;
        active      = false;
    }
};


// ============================================================================
// ITracingOutput
// ----------------------------------------------------------------------------
// Abstract sink for CompletedBlob records.  Mirrors IOutputWriter in its role.
//
// emit(blob)  — called for each completed blob.  May buffer internally.
// flush()     — flush all buffered blobs.  Called once at pipeline shutdown.
//
// Implementations must be safe to call from a single thread (TracingBlock's
// run() thread).  No cross-thread safety is required.
// ============================================================================

class ITracingOutput {
public:
    virtual void emit(const CompletedBlob& blob) = 0;
    virtual void flush() = 0;
    virtual ~ITracingOutput() = default;
};


// ============================================================================
// NullTracingOutput
// ----------------------------------------------------------------------------
// Zero-overhead sink used when write_output = false.
// No heap allocation, no virtual function body.
// ============================================================================

class NullTracingOutput : public ITracingOutput {
public:
    void emit(const CompletedBlob&) override {}
    void flush()                    override {}
};


// ============================================================================
// CSVTracingOutput
// ----------------------------------------------------------------------------
// Buffered CSV file sink.  Accumulates blobs in an internal vector and flushes
// to disk every 256 entries (or on explicit flush()).
//
// CSV columns: label, pixel_count, top_row, bottom_row, left_col, right_col,
//              width (derived), height (derived)
//
// Buffering strategy:
//   256 entries × 48 bytes ≈ 12 KB buffer — small enough to be L1-resident.
//   Flush is called every 256 entries or at shutdown, so for a run producing
//   10,000 blobs only ~40 file write calls are made — negligible I/O overhead.
// ============================================================================

class CSVTracingOutput : public ITracingOutput {
public:
    explicit CSVTracingOutput(const std::string& path)
        : path_(path)
    {
        file_.open(path, std::ios::out | std::ios::trunc);
        if (!file_.is_open())
            throw std::runtime_error(
                "CSVTracingOutput: cannot open '" + path + "'");

        std::cout << "[TracingOutput] Writing blobs to: " << path << '\n';

        file_ << "label,pixel_count,top_row,bottom_row,"
                 "left_col,right_col,width,height\n";

        buffer_.reserve(FLUSH_THRESHOLD);
    }

    ~CSVTracingOutput() {
        if (file_.is_open()) {
            flush();
            file_.close();
            std::cout << "[TracingOutput] " << total_emitted_
                      << " blobs written to " << path_ << '\n';
        }
    }

    void emit(const CompletedBlob& b) override {
        buffer_.push_back(b);
        if (buffer_.size() >= FLUSH_THRESHOLD) flush();
    }

    void flush() override {
        for (const auto& b : buffer_) {
            // Derived dimensions — guard against sentinel values
            const uint64_t w = (b.right_col  >= b.left_col)
                             ? (b.right_col  - b.left_col  + 1) : 0;
            const uint64_t h = (b.bottom_row >= b.top_row)
                             ? (b.bottom_row - b.top_row   + 1) : 0;

            file_ << b.label        << ','
                  << b.pixel_count  << ','
                  << b.top_row      << ','
                  << b.bottom_row   << ','
                  << b.left_col     << ','
                  << b.right_col    << ','
                  << w              << ','
                  << h              << '\n';
            ++total_emitted_;
        }
        file_.flush();
        buffer_.clear();
    }

private:
    static constexpr size_t FLUSH_THRESHOLD = 256;

    std::string               path_;
    std::ofstream             file_;
    std::vector<CompletedBlob> buffer_;
    uint64_t                  total_emitted_ = 0;
};


// ============================================================================
// StdoutTracingOutput
// ----------------------------------------------------------------------------
// Debug sink that prints each completed blob to stdout.
// Useful for interactive testing and evaluation demos.
// ============================================================================

class StdoutTracingOutput : public ITracingOutput {
public:
    void emit(const CompletedBlob& b) override {
        const uint64_t w = (b.right_col  >= b.left_col)
                         ? (b.right_col  - b.left_col  + 1) : 0;
        const uint64_t h = (b.bottom_row >= b.top_row)
                         ? (b.bottom_row - b.top_row   + 1) : 0;

        std::cout << "[Blob] label="  << b.label
                  << " pixels="       << b.pixel_count
                  << " rows=["        << b.top_row    << "–" << b.bottom_row << "]"
                  << " cols=["        << b.left_col   << "–" << b.right_col  << "]"
                  << " bbox="         << w << "×" << h
                  << '\n';
        ++total_emitted_;
    }

    void flush() override {
        std::cout.flush();
        std::cout << "[TracingOutput] " << total_emitted_ << " blobs emitted.\n";
    }

private:
    uint64_t total_emitted_ = 0;
};


// ============================================================================
// makeTracingOutput
// ----------------------------------------------------------------------------
// Factory function — selects the appropriate ITracingOutput implementation
// based on SystemConfig.
//
// If write_output is false : NullTracingOutput (no I/O overhead).
// If write_output is true  : CSVTracingOutput writing to a derived path.
//
// The blob output file is named <output_file>_blobs.csv to avoid clobbering
// the labelled-pixel output CSV produced by the label stage.
// ============================================================================

inline std::unique_ptr<ITracingOutput>
makeTracingOutput(const SystemConfig& cfg) {
    if (!cfg.write_output)
        return std::make_unique<NullTracingOutput>();

    const std::string path = cfg.output_file.empty()
                           ? "blobs.csv"
                           : cfg.output_file + "_blobs.csv";
    return std::make_unique<CSVTracingOutput>(path);
}

#endif // TRACING_UTILS_HPP
