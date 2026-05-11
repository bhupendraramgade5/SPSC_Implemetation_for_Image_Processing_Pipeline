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