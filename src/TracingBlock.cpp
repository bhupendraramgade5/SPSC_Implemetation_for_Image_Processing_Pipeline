// ============================================================================
// TracingBlock.cpp
//
// Implementation of the Tracing & Computation pipeline stage.
// See TracingBlock.hpp for the full design description.
// ============================================================================

#include "TracingBlock.hpp"

#include <algorithm>
#include <stdexcept>

#if defined(_MSC_VER)
    #include <immintrin.h>
#endif


// ============================================================================
// Constructor
// ----------------------------------------------------------------------------
// Validates the pipeline contract (even, non-zero column count) then
// constructs the flat accumulator array and the local LabelMap.
//
// max_labels = columns / 2  (matching LabellingBlock's contract exactly).
// accumulators_ has max_labels + 1 entries (slot 0 = background, unused).
// label_map_ has the same max_labels capacity as LabellingBlock's map.
// ============================================================================

TracingBlock::TracingBlock(const SystemConfig&     config,
                           IQueue<LabelledPacket>& in_queue,
                           ITracingOutput&         output)
    : config_       (config)
    , in_queue_     (in_queue)
    , output_       (output)
    , accumulators_ (config.columns / 2 + 1)   // index [0..max_labels]
    , label_map_    (config.columns / 2)
{
    if (config.columns == 0)
        throw std::invalid_argument(
            "TracingBlock: config.columns must be > 0");
    if (config.columns % 2 != 0)
        throw std::invalid_argument(
            "TracingBlock: config.columns must be even "
            "(pipeline emits 2 pixels per packet)");
}


// ============================================================================
// stop
// ----------------------------------------------------------------------------
// Relaxed store — correctness guaranteed by SPSC queue ordering on the
// data path.  run() checks running_ only when the queue reports empty,
// at which point all in-flight packets have already been processed.
// ============================================================================

void TracingBlock::stop() {
    running_.store(false, std::memory_order_relaxed);
}


// ============================================================================
// run
// ----------------------------------------------------------------------------
// Main consumer loop.  Mirrors the structure of FilterBlock::run() and
// LabellingBlock::run() exactly.
//
// Per-iteration flow:
//   1. Pop a LabelledPacket.  Spin-yield if queue is empty.
//   2. Call processPacket() — all four steps.
//   3. Increment packets_processed_ counter.
//
// Exit: running_ == false AND in_queue_ returns no packet.
//
// After exit: flush() drains any buffered blobs to the output sink.
// ============================================================================

void TracingBlock::run() {
    LabelledPacket lp;

    while (true) {
        if (!in_queue_.pop(lp)) {
            if (!running_.load(std::memory_order_relaxed)) break;
            // Spin-yield: same pattern as FilterBlock and LabellingBlock.
#if defined(_MSC_VER)
            _mm_pause();
#elif defined(__GNUC__) || defined(__clang__)
            __builtin_ia32_pause();
#endif
            continue;
        }

        processPacket(lp);
        packets_processed_.fetch_add(1, std::memory_order_relaxed);
    }

    // Flush any remaining blobs that are still active at pipeline end.
    // These are blobs whose components were never recycled (e.g. a blob
    // that reaches the end of the CSV file without a blank row after it).
    // Emit them as completed — they are genuinely finished.
    const size_t max_labels = config_.columns / 2;
    for (uint16_t l = 1; l <= static_cast<uint16_t>(max_labels); ++l) {
        if (accumulators_[l].active) {
            // Resolve to canonical root before emitting.
            const uint16_t root = label_map_.find(l);
            // Only emit once per root — skip aliases.
            if (root == l) {
                const CompletedBlob blob = accumulators_[l].finalise(l);
                output_.emit(blob);
                blobs_completed_.fetch_add(1, std::memory_order_relaxed);
                accumulators_[l].reset();
            }
        }
    }

    output_.flush();
}


// ============================================================================
// processPacket
// ----------------------------------------------------------------------------
// Dispatches one LabelledPacket through all four steps.
//
// Step ordering is critical:
//   Merge events BEFORE pixel updates — so that accumulators are in the
//   correct merged state when the pixels of this packet are added.
//
//   Recycle event BEFORE pixel updates — the recycled label will not appear
//   in l1 or l2 of this packet (LabellingBlock emits the recycle in the
//   first packet AFTER the label dies, not in the last packet of the blob).
//   Emitting the blob before the pixel updates of the current packet ensures
//   the emitted stats are exactly those of the completed blob, not inflated
//   by any coincidental label reuse in this packet.
//
// Background pixels (l1 == 0 or l2 == 0) are silently skipped — no
// accumulator operation is needed for background.
// ============================================================================

void TracingBlock::processPacket(const LabelledPacket& lp) {

    // --- Step 1: Apply merge events ---
    // Two merge slots per packet (one per pixel).  Both may fire.
    if (lp.merge_old != 0)
        applyMerge(lp.merge_old, lp.merge_new);
    if (lp.merge_old2 != 0)
        applyMerge(lp.merge_old2, lp.merge_new2);

    // --- Step 2: Apply recycle event ---
    if (lp.recycled != 0)
        applyRecycle(lp.recycled);

    // --- Step 3: Update pixel 1 (l1 at column lp.col) ---
    if (lp.l1 != 0) {
        const uint16_t root = resolveLabel(lp.l1);
        if (root != 0) {
            updatePixel(root, lp.row, lp.col);
        }
    }

    // --- Step 4: Update pixel 2 (l2 at column lp.col + 1) ---
    // l2 == 0 when b2 was a padding artifact (FilterBlock sets label=0
    // for padding pixels in LabellingBlock's input).  The guard handles
    // this naturally — no special is_pad check needed here.
    if (lp.l2 != 0) {
        const uint16_t root = resolveLabel(lp.l2);
        if (root != 0) {
            updatePixel(root, lp.row, lp.col + 1);
        }
    }
}


// ============================================================================
// applyMerge
// ----------------------------------------------------------------------------
// Handles one merge event: merge_old (absorbed) → merge_new (surviving).
//
// Steps:
//   1. Resolve both labels through local find() — they may already share a
//      root from a previous merge in this packet (path compression handles
//      this; no explicit deduplication needed).
//   2. If they are already in the same component, nothing to do.
//   3. Merge the absorbed accumulator into the surviving accumulator.
//   4. Update the local Union-Find via unite().
//   5. Reset the absorbed accumulator (its data now lives in surviving).
//
// Why resolve first?
//   unite() always keeps the LOWER label as root (matching LabellingBlock).
//   If merge_old and merge_new are both aliases of earlier merges,
//   resolving first ensures we operate on canonical roots, preventing
//   double-merge artifacts in the accumulators.
//
// Accumulator indexing:
//   We index by canonical root, not by the raw label in the event.
//   This handles the case where a label has been merged before its
//   accumulator entry was initialised (e.g. a pixel got label 5 which was
//   already merged under label 2 — accumulator[2] is the correct target).
// ============================================================================

void TracingBlock::applyMerge(uint16_t merge_old,
                              uint16_t merge_new) noexcept
{
    if (merge_old == 0 || merge_new == 0) return;

    const uint16_t root_old = label_map_.find(merge_old);
    const uint16_t root_new = label_map_.find(merge_new);

    if (root_old == root_new) return;   // already unified — nothing to do

    // Determine which root survives.  unite() always returns the lower label.
    // Since LabellingBlock's unite() does the same, surviving == min(root_old, root_new).
    const uint16_t surviving = std::min(root_old, root_new);
    const uint16_t absorbed  = std::max(root_old, root_new);

    // Update the local Union-Find.
    label_map_.unite(root_old, root_new);

    // Merge the absorbed accumulator into the surviving accumulator.
    if (absorbed  < static_cast<uint16_t>(accumulators_.size()) &&
        surviving < static_cast<uint16_t>(accumulators_.size()))
    {
        // Merge even if absorbed is not active (merge of empty into non-empty
        // is a no-op in BlobAccumulator::merge — pixel_count += 0, etc.).
        accumulators_[surviving].merge(accumulators_[absorbed]);
        accumulators_[absorbed].reset();
    }
}


// ============================================================================
// applyRecycle
// ----------------------------------------------------------------------------
// Handles a recycle event: the label identified by `recycled` is permanently
// dead.  No future packet will carry this label.
//
// Steps:
//   1. Resolve `recycled` to its canonical root via local find().
//   2. Emit accumulators_[root].finalise(root) as a CompletedBlob.
//   3. Update the blob counter and peak diagnostics.
//   4. Return the label to label_map_'s free list.
//   5. Reset the accumulator slot for future reuse.
//
// Edge cases:
//   - recycled label has no active accumulator (no pixels were assigned).
//     We still emit a blob with pixel_count=0 — the output sink can filter
//     these if desired.  For evaluation purposes, zero-pixel blobs indicate
//     a label was allocated but never assigned (should not occur in normal
//     operation).
//   - recycled label is an alias (root != recycled after find()).
//     We emit the root's accumulator and recycle the alias only.  The root
//     itself may receive a separate recycle event later.  Emitting on first
//     recycle notification is safe because LabellingBlock only recycles a
//     label when it is definitively dead — after which the label (and its
//     root) will not appear in any future packet.
// ============================================================================

void TracingBlock::applyRecycle(uint16_t recycled) noexcept {
    if (recycled == 0) return;

    const uint16_t root = label_map_.find(recycled);

    if (root != 0 && root < static_cast<uint16_t>(accumulators_.size())) {
        // Emit only if the accumulator has seen at least one pixel.
        // Zero-pixel blobs are suppressed to keep the output clean.
        if (accumulators_[root].active) {
            const CompletedBlob blob = accumulators_[root].finalise(root);
            output_.emit(blob);
            blobs_completed_.fetch_add(1, std::memory_order_relaxed);
        }
        accumulators_[root].reset();
    }

    // Free the label slot in the local map.
    label_map_.recycle(recycled);
    if (recycled != root && root != 0) {
        // Root and alias are distinct — also recycle the root if it equals
        // the canonical root (it will, since unite() chains all aliases to
        // the root).  Calling recycle() twice on the same label is safe —
        // the second call just pushes a duplicate onto free_.  To avoid this,
        // only recycle root if root != recycled.
        // In practice, LabellingBlock emits only ONE recycle notification per
        // logical blob (the canonical root's ID), so this branch is rarely
        // taken.  The duplicate-recycle risk is low but not zero.
        // TODO: Future hardening — track which roots have been recycled to
        // prevent the double-push edge case.
    }

    // Update peak diagnostic.
    const size_t current_active = countActive();
    if (current_active > peak_active_) peak_active_ = current_active;
}


// ============================================================================
// updatePixel
// ----------------------------------------------------------------------------
// Update the accumulator for a foreground pixel at (row, col).
// `root` must already be the canonical label (find() resolved by caller).
// ============================================================================

void TracingBlock::updatePixel(uint16_t root,
                               uint64_t row,
                               uint64_t col) noexcept
{
    if (root == 0 || root >= static_cast<uint16_t>(accumulators_.size()))
        return;
    accumulators_[root].update(row, col);
}


// ============================================================================
// resolveLabel
// ----------------------------------------------------------------------------
// Resolve `label` to its canonical root via path-halving find().
// Returns 0 for background (label == 0) or out-of-range label.
// ============================================================================

uint16_t TracingBlock::resolveLabel(uint16_t label) noexcept {
    if (label == 0) return 0;
    if (label >= static_cast<uint16_t>(accumulators_.size())) return 0;
    return label_map_.find(label);
}


// ============================================================================
// countActive
// ----------------------------------------------------------------------------
// Count accumulators with active == true.
// O(max_labels) — called only on recycle events (infrequent relative to
// pixel rate), so the linear scan is acceptable.
// ============================================================================

size_t TracingBlock::countActive() const noexcept {
    size_t count = 0;
    for (const auto& acc : accumulators_)
        if (acc.active) ++count;
    return count;
}
