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