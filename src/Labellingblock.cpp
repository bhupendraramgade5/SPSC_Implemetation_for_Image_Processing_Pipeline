// ============================================================================
// LabellingBlock.cpp
//
// Implementation of the streaming connected-components labeller.
// See LabellingBlock.hpp for the full design description.
// ============================================================================

#include "LabellingBlock.hpp"

#include <stdexcept>
#include <algorithm>

#if defined(_MSC_VER)
    #include <immintrin.h>
#endif


// ============================================================================
// Constructor
// ----------------------------------------------------------------------------
// Validates the pipeline contract (even, non-zero column count), then
// constructs LabelMap and RowLabelBuffer with the correct sizes derived from
// config.columns.
//
// max_labels = columns / 2
//   In the worst case (alternating 1s and 0s in a row), every other pixel
//   is an isolated foreground pixel.  m pixels → at most m/2 active labels.
//   This matches the spec: "two 1D arrays of maximum size m/2 each".
//
// pending_recycles_ and recycle_scratch_ are both reserved to max_labels
// entries here so neither vector reallocates at row transitions.
// ============================================================================

LabellingBlock::LabellingBlock(const SystemConfig&      config,
                               IQueue<FilteredPacket>&  in_queue,
                               IQueue<LabelledPacket>&  out_queue)
    : config_    (config)
    , in_queue_  (in_queue)
    , out_queue_ (out_queue)
    , label_map_ (config.columns / 2)
    , row_buf_   (config.columns, config.columns / 2)
{
    if (config.columns == 0)
        throw std::invalid_argument(
            "LabellingBlock: config.columns must be > 0");
    if (config.columns % 2 != 0)
        throw std::invalid_argument(
            "LabellingBlock: config.columns must be even "
            "(pipeline emits 2 pixels per packet)");

    const size_t max_labels = config.columns / 2;
    pending_recycles_.reserve(max_labels);
    recycle_scratch_.resize(max_labels, uint16_t(0));
}


// ============================================================================
// stop
// ----------------------------------------------------------------------------
// Relaxed store — correctness is guaranteed by the SPSC queue's acquire/
// release ordering on the data path.  run() checks running_ only when the
// queue reports empty, by which point all in-flight packets have already
// been processed.
// ============================================================================

void LabellingBlock::stop() {
    running_.store(false, std::memory_order_relaxed);
}


// ============================================================================
// run
// ----------------------------------------------------------------------------
// Main consumer loop.  Mirrors FilterBlock::run() in structure.
//
// Per-iteration flow:
//   1. Pop a FilteredPacket.  Spin-yield if queue is empty.
//   2. On row transition: commit the completed row, compute recycled labels,
//      update current_row_.
//   3. Process fp.b1 → l1 via assignLabel(), capturing any merge event.
//   4. Process fp.b2 → l2 via assignLabel(), capturing any merge event.
//   5. Emit one LabelledPacket containing l1, l2, and both merge events.
//
// Exit: running_ == false AND in_queue_ returns no packet.
//
// Final-row flush: after the loop, onRowTransition(UINT64_MAX) is called with
// a sentinel row to ensure the last real row's recycling candidates are
// forwarded before shutdown.
// ============================================================================

void LabellingBlock::run() {
    FilteredPacket fp;

    while (true) {
        if (!in_queue_.pop(fp)) {
            if (!running_.load(std::memory_order_relaxed)) break;
            // Spin-yield: identical to FilterBlock hot-path idle
#if defined(_MSC_VER)
            _mm_pause();
#elif defined(__GNUC__) || defined(__clang__)
            __builtin_ia32_pause();
#endif
            continue;
        }

        // Row transition check
        if (fp.row != current_row_) {
            onRowTransition(fp.row);
        }

        // Process pixel 1 (v1 at fp.col)
        uint16_t mo1 = 0, mn1 = 0;
        const uint16_t l1 = assignLabel(fp.b1 != 0, fp.col, mo1, mn1);

        // Process pixel 2 (v2 at fp.col + 1)
        uint16_t mo2 = 0, mn2 = 0;
        const uint16_t l2 = assignLabel(fp.b2 != 0, fp.col + 1, mo2, mn2);

        last_col_ = fp.col + 1;

        // Mid-row drain: free LabelMap slots for prev_row labels whose NW
        // reach is now behind the scan head.  Must run AFTER both pixels
        // are assigned so curr_present_ reflects both columns before we
        // decide any prev label is absent.
        drainMidRowRecycles(last_col_);

        emitPacket(fp, l1, l2, mo1, mn1, mo2, mn2);
    }

    // Flush recycling candidates from the final row.
    if (current_row_ != UINT64_MAX) {
        const size_t count = row_buf_.commitAndRecycle(
            recycle_scratch_.data(),
            recycle_scratch_.size());

        for (size_t i = 0; i < count; ++i)
            label_map_.recycle(recycle_scratch_[i]);

        for (size_t i = 0; i < count; ++i)
            pending_recycles_.push_back(recycle_scratch_[i]);
    }
}


// ============================================================================
// onRowTransition
// ----------------------------------------------------------------------------
// Called exactly once per row boundary, before the first pixel of new_row
// is processed.
//
// Steps:
//   1. If this is NOT the very first row (current_row_ != UINT64_MAX),
//      commit the completed row via commitAndRecycle(), which:
//        a. identifies labels absent from the new row → recycling candidates
//        b. swaps prev_/curr_ buffers
//   2. Return recycled labels to label_map_ so they can be reused.
//   3. Push recycled labels onto pending_recycles_ for the Tracing block.
//   4. Update current_row_.
//
// pending_recycles_ is a small queue: emitPacket() pops one entry per
// packet so that each LabelledPacket carries at most one recycle event.
// ============================================================================

void LabellingBlock::onRowTransition(uint64_t new_row) {
    if (current_row_ != UINT64_MAX) {
        const size_t count = row_buf_.commitAndRecycle(
            recycle_scratch_.data(),
            recycle_scratch_.size());

        for (size_t i = 0; i < count; ++i) {
            const uint16_t lbl = recycle_scratch_[i];
            label_map_.recycle(lbl);
            pending_recycles_.push_back(lbl);
        }
    }
    current_row_ = new_row;
}


// ============================================================================
// drainMidRowRecycles
// ----------------------------------------------------------------------------
// Separates the two recycling operations that the original design conflated:
//
//   Operation A — Free the LabelMap slot.
//     Must happen as soon as the label is mathematically dead so the slot
//     is available for new blobs.  On an infinite stream, deferring this
//     to row boundaries means a label stays allocated for one extra row
//     even though no future pixel can ever reach it.  For dense small-blob
//     inputs this is the difference between the map having headroom and
//     being perpetually at its m/2 limit.
//
//   Operation B — Notify the Tracing block.
//     Can be deferred; done lazily via pending_recycles_ which emitPacket()
//     drains one entry per packet.
//
// Called after BOTH pixels in a packet are assigned so that curr_present_
// reflects both fp.col and fp.col+1 before we evaluate prev_ labels as dead.
// Calling it before assignLabel for v2 would incorrectly drain a prev_ label
// that v2 inherits as an N-neighbour.
//
// The work per call is bounded by the number of columns the drain cursor
// advances — total across the full row is O(m), not O(m) per packet.
// ============================================================================

void LabellingBlock::drainMidRowRecycles(uint64_t completed_col) noexcept {
    const size_t count = row_buf_.drainDeadFromPrev(
        static_cast<size_t>(completed_col),
        recycle_scratch_.data(),
        recycle_scratch_.size());

    for (size_t i = 0; i < count; ++i) {
        label_map_.recycle(recycle_scratch_[i]);
        pending_recycles_.push_back(recycle_scratch_[i]);
    }
}


// ============================================================================
// assignLabel
// ----------------------------------------------------------------------------
// Assigns a label to a single pixel and optionally emits a merge event.
//
// Neighbour resolution:
//   All four causal neighbours are resolved through LabelMap::find() inside
//   the neighbourN/NW/NE/W helpers.  find() applies path-halving, so the
//   values stored in roots[] are canonical roots — no further find() is
//   needed for the minimum-selection pass.
//
// Deduplication:
//   Unique non-zero roots are collected.  The minimum is chosen as the
//   surviving label because the spec example shows the lower label surviving
//   a merge.  All other distinct roots are merged into the minimum via
//   unite().
//
// Merge event emission:
//   unite() returns the surviving label.  The input to unite() is the
//   (higher) absorbed label.  One merge event is emitted per call to
//   assignLabel() — the first unique merge encountered.  If two distinct
//   neighbours are both different from the minimum (requiring two merges
//   from one pixel), only the first merge is forwarded via out_merge_old/new.
//   The second merge is applied to the Union-Find (parent[] is updated
//   correctly) but the event is not forwarded.  See LabelledPacket comment
//   for the known-limitation description.
//
// No heap allocation, no virtual calls.
// ============================================================================

uint16_t LabellingBlock::assignLabel(bool     pixel_on,
                                     uint64_t col,
                                     uint16_t& out_merge_old,
                                     uint16_t& out_merge_new) noexcept
{
    out_merge_old = 0;
    out_merge_new = 0;

    if (!pixel_on) {
        row_buf_.set(col, uint16_t(0));
        return uint16_t(0);
    }

    // Resolve all four causal neighbours to canonical roots.
    // neighbourNE reads prev_row[col+1] which is safe for col == columns-1
    // because RowLabelBuffer::prev() returns 0 for out-of-range indices.
    const uint16_t roots[4] = {
        neighbourNW(col),
        neighbourN (col),
        neighbourNE(col),
        neighbourW (col)
    };

    // Find the minimum non-zero root — this is the label that survives
    // any merges (lower label wins, matching the spec example).
    uint16_t min_root = 0;
    for (const uint16_t r : roots) {
        if (r != 0 && (min_root == 0 || r < min_root))
            min_root = r;
    }

    uint16_t assigned = 0;

    if (min_root == 0) {
        // No live neighbours — allocate a fresh label.
        assigned = label_map_.newLabel();
        // If label_map_ is full (should not occur for correct m/2 sizing),
        // treat the pixel as background.  This is a hard invariant violation
        // and should be visible in tests via the assert below.
        // In release builds the pixel is silently dropped.
    } else {
        assigned = min_root;

        // Merge every distinct root that differs from min_root.
        for (const uint16_t r : roots) {
            if (r == 0 || r == assigned) continue;

            // Re-resolve through find() — a previous iteration in this
            // loop may have already merged r under assigned.
            const uint16_t current_root = label_map_.find(r);
            if (current_root == label_map_.find(assigned)) continue;

            // Merge current_root into assigned.
            // unite() always keeps the lower label as root.
            // Since assigned <= current_root (min_root selection above),
            // the surviving root returned is assigned.
            const uint16_t absorbed = current_root;
            label_map_.unite(absorbed, assigned);

            // Emit the first merge event encountered.
            if (out_merge_old == 0) {
                out_merge_old = absorbed;
                out_merge_new = assigned;
            }
            // Second and subsequent merges from the same pixel:
            // parent[] is updated (correct labelling) but no event emitted.
            // Documented limitation: see LabelledPacket comment.
        }
    }

    row_buf_.set(col, assigned);
    return assigned;
}


// ============================================================================
// emitPacket
// ----------------------------------------------------------------------------
// Builds a LabelledPacket from the two processed pixels and any staged
// events, then pushes it to out_queue_.
//
// Recycle forwarding:
//   pending_recycles_ is treated as a FIFO.  One entry (if any) is popped
//   per emitPacket() call so recycle notifications are spread across
//   consecutive packets rather than bunched at row boundaries.  The Tracing
//   block must handle a recycled label appearing in any packet, not only in
//   packets at explicit row boundaries.
//
// No back-pressure wait: if out_queue_.push() returns false (queue full),
// the packet is silently dropped.  This mirrors the GeneratorBlock drop
// policy and maintains the timing contract: emitPacket() must return within
// T regardless of downstream pressure.  Dropped output packets are visible
// as gaps in the Tracing block's label sequence.
// ============================================================================

void LabellingBlock::emitPacket(const FilteredPacket& fp,
                                uint16_t l1,         uint16_t l2,
                                uint16_t merge_old,  uint16_t merge_new,
                                uint16_t merge_old2, uint16_t merge_new2) noexcept
{
    LabelledPacket out;
    out.row        = fp.row;
    out.col        = fp.col;
    out.l1         = l1;
    out.l2         = l2;
    out.merge_old  = merge_old;
    out.merge_new  = merge_new;
    out.merge_old2 = merge_old2;
    out.merge_new2 = merge_new2;

    // Pop one pending recycle — spreads recycle notifications across packets
    if (!pending_recycles_.empty()) {
        out.recycled = pending_recycles_.back();
        pending_recycles_.pop_back();
    }

    out_queue_.push(out);   // non-blocking; drop if full
}


// ============================================================================
// 4-causal neighbour helpers
// ----------------------------------------------------------------------------
// Each helper reads one entry from the row buffers and resolves it through
// LabelMap::find().  find() is non-const (path compression) so these helpers
// cannot be const either.
//
// Boundary handling:
//   NW / W : guard col > 0.  When col == 0 there is no left neighbour.
//   NE     : RowLabelBuffer::prev(col + 1) returns 0 when col + 1 >= columns.
//   N      : always safe — prev(col) returns 0 for out-of-range col.
//
// The find() call inside each helper means canonical roots are returned,
// not raw stored values.  This is essential: without find(), two labels
// that have already been merged but whose stored values differ would appear
// as distinct roots in assignLabel(), causing a spurious second merge.
// ============================================================================

uint16_t LabellingBlock::neighbourN(uint64_t col) noexcept {
    return label_map_.find(row_buf_.prev(col));
}

uint16_t LabellingBlock::neighbourNW(uint64_t col) noexcept {
    if (col == 0) return uint16_t(0);
    return label_map_.find(row_buf_.prev(col - 1));
}

uint16_t LabellingBlock::neighbourNE(uint64_t col) noexcept {
    // prev(col + 1) handles the right-edge case — returns 0 when col+1 >= columns
    return label_map_.find(row_buf_.prev(col + 1));
}

uint16_t LabellingBlock::neighbourW(uint64_t col) noexcept {
    if (col == 0) return uint16_t(0);
    return label_map_.find(row_buf_.curr(col - 1));
}