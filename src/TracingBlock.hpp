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
