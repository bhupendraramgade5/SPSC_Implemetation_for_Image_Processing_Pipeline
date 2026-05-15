// main_linear.cpp
// ============================================================================
// Single-threaded linear pipeline: Generate → Filter → Output
// No threads. No queues. No OS scheduling interference.
//
// Purpose: Measure PURE ALGORITHM COST of the 9-tap convolution pipeline.
// Compare avg gap here vs threaded version to isolate threading overhead.
//
// Expected: avg gap 50–150ns (vs 280ns threaded)
// If true: threading overhead = ~130–230ns per pixel
// If false (same as threaded): algorithm is the bottleneck, not threading
// ============================================================================

#include <iostream>
#include <chrono>
#include <vector>
#include <array>
#include <cstdint>
#include <memory>
#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <csignal>
#include <atomic>
#include <fstream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "ConfigManager.hpp"
#include "GeneratorBlock.hpp"   // IDataSource, createDataSource
#include "FilterUtils.hpp"      // SlidingWindow, FilteredPacket, applyLeft/Right
#include "LabellingUtils.hpp"   // LabelledPacket, LabelMap, RowLabelBuffer
#include "TracingUtils.hpp"     // BlobAccumulator, CompletedBlob           Phase 4
#include "Queue.hpp"            // DataPacket
#include "OutputWriter.hpp"     // IOutputWriter, makeOutputWriter
#include "PerfTest.hpp"         // LinearStats, computeLinearStats, printLinearStats

// ============================================================================
// Signal handling (Ctrl+C to stop)
// ============================================================================
static std::atomic<bool> g_stop{false};

extern "C" void signalHandler(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

// ============================================================================
// Inline Filter: No queue, no virtual dispatch, hardcoded 9-tap + fallback
// ============================================================================

class InlineLinearFilter {
public:
    explicit InlineLinearFilter(const SystemConfig& cfg)
        : cfg_(cfg)
        , threshold_(static_cast<float>(cfg.threshold))
        , window_(cfg.kernel.size())
        , half_width_(cfg.kernel.size() / 2)
        , policy_(cfg.boundary_policy)
    {
        if (cfg.kernel.empty())
            throw std::invalid_argument("InlineLinearFilter: empty kernel");
        if (cfg.kernel.size() % 2 == 0)
            throw std::invalid_argument("InlineLinearFilter: kernel size must be odd");
    }

    // Called once at the start of each new scan row.
    // Resets the sliding window and applies left-padding.
    void beginRow(uint8_t left_edge, uint64_t row) {
        current_row_ = row;
        window_.reset();
        pending_ = PendingOutput{};

        for (size_t i = 0; i < half_width_; ++i) {
            uint8_t pad = applyLeft(policy_, left_edge, half_width_ - i);
            window_.push(pad, row, 0);
        }
    }

    // Process one pixel.
    // Returns true and fills fp when a complete pair (b1, b2) is ready.
    // Returns false while the window is filling or only b1 is staged.
    bool processSample(uint8_t value, uint64_t row, uint64_t col,
                       FilteredPacket& fp)
    {
        window_.push(value, row, col);

        if (!window_.is_full()) return false;

        const WindowSlot& c    = window_.centre();
        const float       fval = dotProduct();
        const uint8_t     bin  = (fval >= threshold_) ? uint8_t{1} : uint8_t{0};

        if (!pending_.has_b1) {
            pending_.b1 = bin; pending_.row = c.row;
            pending_.col = c.col; pending_.has_b1 = true;
            return false;
        }
        fp.b1 = pending_.b1; fp.b2 = bin;
        fp.row = pending_.row; fp.col = pending_.col;
        pending_ = PendingOutput{};
        return true;
    }

    // Right-pad at end of row and drain any unpaired b1.
    void flush(uint8_t edge, uint64_t row, uint64_t last_col,
               std::vector<FilteredPacket>& out)
    {
        for (size_t i = 0; i < half_width_; ++i) {
            uint8_t pad = applyRight(policy_, edge, i + 1);
            FilteredPacket fp;
            if (processSample(pad, row, last_col + 1 + i, fp))
                out.push_back(fp);
        }

        if (pending_.has_b1 && !pending_.has_b2) {
            FilteredPacket fp;
            fp.b1 = pending_.b1; fp.b2 = 0;
            fp.flags = FP_FLAG_B2_IS_PAD;   // Phase 4: mark padding b2
            fp.row = pending_.row; fp.col = pending_.col;
            out.push_back(fp);
            pending_ = PendingOutput{};
        }
    }

private:
    // 9-tap unrolled fast path — no loop, no branch, compiler schedules
    // these as independent multiply-accumulate chains.
    // Generic fallback for non-standard kernel sizes.
    float dotProduct() const {
        const auto& k = cfg_.kernel;
        if (k.size() == 9)
            return static_cast<float>(window_.at(0).value)*k[0]
                 + static_cast<float>(window_.at(1).value)*k[1]
                 + static_cast<float>(window_.at(2).value)*k[2]
                 + static_cast<float>(window_.at(3).value)*k[3]
                 + static_cast<float>(window_.at(4).value)*k[4]
                 + static_cast<float>(window_.at(5).value)*k[5]
                 + static_cast<float>(window_.at(6).value)*k[6]
                 + static_cast<float>(window_.at(7).value)*k[7]
                 + static_cast<float>(window_.at(8).value)*k[8];
        float s = 0.f;
        for (size_t i = 0; i < k.size(); ++i)
            s += static_cast<float>(window_.at(i).value) * k[i];
        return s;
    }

    const SystemConfig& cfg_;
    float               threshold_;
    SlidingWindow       window_;
    const size_t        half_width_;
    BoundaryPolicy      policy_;
    uint64_t            current_row_ = UINT64_MAX;

    struct PendingOutput {
        bool has_b1=false, has_b2=false;
        uint8_t b1=0, b2=0;
        uint64_t row=0, col=0;
    } pending_;
};


// ============================================================================
// LinearLabeller
// ----------------------------------------------------------------------------
// Single-threaded labelling stage.  Wraps LabelMap and RowLabelBuffer in a
// simple call-per-packet interface that matches the linear pipeline model:
// no queues, no threads, returns LabelledPacket by value.
//
// assignPacket() is the equivalent of LabellingBlock::run()'s inner loop
// body — one FilteredPacket in, one LabelledPacket out.
//
// The mid-row drain (drainMidRowRecycles) is called inline after both pixels
// are processed, matching the threaded block's behaviour exactly.
// ============================================================================

class LinearLabeller {
public:
    explicit LinearLabeller(const SystemConfig& cfg)
        : cfg_(cfg)
        , label_map_(cfg.columns / 2)
        , row_buf_  (cfg.columns, cfg.columns / 2)
    {
        const size_t max_labels = cfg.columns / 2;
        recycle_scratch_.resize(max_labels, uint16_t(0));
        pending_recycles_.reserve(max_labels);
    }

    // Process one FilteredPacket and produce one LabelledPacket.
    // Handles row transitions internally.
    LabelledPacket assignPacket(const FilteredPacket& fp) {
        if (fp.row != current_row_)
            onRowTransition(fp.row);

        uint16_t mo1=0, mn1=0;
        const uint16_t l1 = assignLabel(fp.b1 != 0, fp.col,     mo1, mn1);

        // If b2 is a padding artifact, treat it as background for labelling.
        const bool b2_is_pad = (fp.flags & FP_FLAG_B2_IS_PAD) != 0;
        uint16_t mo2=0, mn2=0;
        const uint16_t l2 = b2_is_pad
                          ? uint16_t(0)
                          : assignLabel(fp.b2 != 0, fp.col + 1, mo2, mn2);

        last_col_ = fp.col + (b2_is_pad ? 0 : 1);
        drainMidRow(last_col_);

        LabelledPacket out;
        out.row        = fp.row;
        out.col        = fp.col;
        out.l1         = l1;
        out.l2         = l2;
        out.merge_old  = mo1;
        out.merge_new  = mn1;
        out.merge_old2 = mo2;
        out.merge_new2 = mn2;
        out.recycled   = 0;
        if (!pending_recycles_.empty()) {
            out.recycled = pending_recycles_.back();
            pending_recycles_.pop_back();
        }
        return out;
    }

    // Call once after the last packet of the entire stream to release
    // any labels that were alive in the final row.
    void flushFinalRow() {
        if (current_row_ == UINT64_MAX) return;
        const size_t count = row_buf_.commitAndRecycle(
            recycle_scratch_.data(), recycle_scratch_.size());
        for (size_t i = 0; i < count; ++i)
            label_map_.recycle(recycle_scratch_[i]);
    }

    // Per-row stats for the pipeline summary
    size_t peak_active_labels() const { return peak_active_; }

private:
    void onRowTransition(uint64_t new_row) {
        if (current_row_ != UINT64_MAX) {
            const size_t count = row_buf_.commitAndRecycle(
                recycle_scratch_.data(), recycle_scratch_.size());
            for (size_t i = 0; i < count; ++i) {
                label_map_.recycle(recycle_scratch_[i]);
                pending_recycles_.push_back(recycle_scratch_[i]);
            }
        }
        current_row_ = new_row;
    }

    void drainMidRow(uint64_t completed_col) {
        const size_t count = row_buf_.drainDeadFromPrev(
            static_cast<size_t>(completed_col),
            recycle_scratch_.data(), recycle_scratch_.size());
        for (size_t i = 0; i < count; ++i) {
            label_map_.recycle(recycle_scratch_[i]);
            pending_recycles_.push_back(recycle_scratch_[i]);
        }
        if (label_map_.active() > peak_active_)
            peak_active_ = label_map_.active();
    }

    uint16_t assignLabel(bool pixel_on, uint64_t col,
                         uint16_t& out_old, uint16_t& out_new) noexcept {
        out_old = 0; out_new = 0;
        if (!pixel_on) { row_buf_.set(col, 0); return 0; }

        // Resolve 4 causal neighbours
        auto find = [this](uint16_t l) noexcept { return label_map_.find(l); };
        const uint16_t nw = (col>0) ? find(row_buf_.prev(col-1)) : 0;
        const uint16_t n  =           find(row_buf_.prev(col));
        const uint16_t ne =           find(row_buf_.prev(col+1));
        const uint16_t w  = (col>0) ? find(row_buf_.curr(col-1)) : 0;

        const uint16_t roots[4] = {nw, n, ne, w};
        uint16_t min_root = 0;
        for (uint16_t r : roots)
            if (r && (!min_root || r < min_root)) min_root = r;

        uint16_t assigned = min_root ? min_root : label_map_.newLabel();

        if (min_root) {
            for (uint16_t r : roots) {
                if (!r || r == assigned) continue;
                const uint16_t cr = label_map_.find(r);
                if (cr == label_map_.find(assigned)) continue;
                const uint16_t absorbed = cr;
                label_map_.unite(absorbed, assigned);
                if (!out_old) { out_old = absorbed; out_new = assigned; }
            }
        }

        row_buf_.set(col, assigned);
        return assigned;
    }

    const SystemConfig& cfg_;
    LabelMap            label_map_;
    RowLabelBuffer      row_buf_;

    uint64_t current_row_ = UINT64_MAX;
    uint64_t last_col_    = 0;
    size_t   peak_active_ = 0;

    std::vector<uint16_t> recycle_scratch_;
    std::vector<uint16_t> pending_recycles_;
};


// ============================================================================
// InlineLinearTracer                                                Phase 4
// ----------------------------------------------------------------------------
// Single-threaded equivalent of TracingBlock.  Owns its own LabelMap
// (mirroring LinearLabeller's map via merge events) and a flat array of
// BlobAccumulator indexed by label ID.
//
// Interface:
//   assignPacket(lp, out)  — process one LabelledPacket.
//                            Appends completed CompletedBlobs to `out` when
//                            a recycle event fires.
//   flush(out)             — emit all still-active accumulators as blobs.
//                            Called once after the last packet of the stream.
//
// Design mirrors TracingBlock::processPacket() exactly, with the difference
// that output goes to a local std::vector<CompletedBlob> rather than a queue.
// This keeps I/O out of the timed loop — the caller writes blobs to CSV after
// the timestamp is recorded.
// ============================================================================

class InlineLinearTracer {
public:
    explicit InlineLinearTracer(const SystemConfig& cfg)
        : max_labels_(cfg.columns / 2)
        , accumulators_(cfg.columns / 2 + 1)  // index [0..max_labels]
        , label_map_(cfg.columns / 2)
    {}

    // Process one LabelledPacket.  Appends any CompletedBlobs triggered by
    // recycle events to `out`.  `out` is cleared by the caller between calls
    // if it wants per-packet results; left uncleaned for batched processing.
    void assignPacket(const LabelledPacket& lp,
                      std::vector<CompletedBlob>& out) noexcept
    {
        // Step 1: merge events
        if (lp.merge_old  != 0) applyMerge(lp.merge_old,  lp.merge_new);
        if (lp.merge_old2 != 0) applyMerge(lp.merge_old2, lp.merge_new2);

        // Step 2: recycle event
        if (lp.recycled != 0) applyRecycle(lp.recycled, out);

        // Step 3: update pixel 1
        if (lp.l1 != 0) {
            const uint16_t root = label_map_.find(lp.l1);
            if (root && root <= max_labels_)
                accumulators_[root].update(lp.row, lp.col);
        }

        // Step 4: update pixel 2 (l2==0 for padding — guard handles it)
        if (lp.l2 != 0) {
            const uint16_t root = label_map_.find(lp.l2);
            if (root && root <= max_labels_)
                accumulators_[root].update(lp.row, lp.col + 1);
        }
    }

    // Emit all still-active accumulators as blobs.
    // Call once after the main loop to handle blobs that reached end-of-stream
    // without a recycle event (e.g. a blob that touches the last row of a CSV).
    void flush(std::vector<CompletedBlob>& out) noexcept {
        for (uint16_t l = 1; l <= static_cast<uint16_t>(max_labels_); ++l) {
            if (!accumulators_[l].active) continue;
            // Only emit canonical roots to avoid double-emitting aliases.
            if (label_map_.find(l) == l) {
                out.push_back(accumulators_[l].finalise(l));
                accumulators_[l].reset();
            }
        }
    }

    // Number of blobs emitted so far (via recycle events, not including flush).
    uint64_t blobs_completed() const noexcept { return blobs_completed_; }

    size_t peak_active() const noexcept { return peak_active_; }

private:
    void applyMerge(uint16_t mo, uint16_t mn) noexcept {
        const uint16_t ro = label_map_.find(mo);
        const uint16_t rn = label_map_.find(mn);
        if (ro == rn) return;

        const uint16_t surv = std::min(ro, rn);
        const uint16_t abs  = std::max(ro, rn);

        label_map_.unite(ro, rn);

        if (abs <= max_labels_ && surv <= max_labels_) {
            accumulators_[surv].merge(accumulators_[abs]);
            accumulators_[abs].reset();
        }
    }

    void applyRecycle(uint16_t recycled,
                      std::vector<CompletedBlob>& out) noexcept {
        const uint16_t root = label_map_.find(recycled);
        if (root && root <= max_labels_ && accumulators_[root].active) {
            out.push_back(accumulators_[root].finalise(root));
            accumulators_[root].reset();
            ++blobs_completed_;
        }
        label_map_.recycle(recycled);

        // Track peak active
        size_t active = 0;
        for (const auto& a : accumulators_) if (a.active) ++active;
        if (active > peak_active_) peak_active_ = active;
    }

    size_t                       max_labels_;
    std::vector<BlobAccumulator> accumulators_;
    LabelMap                     label_map_;
    uint64_t                     blobs_completed_ = 0;
    size_t                       peak_active_     = 0;
};


// ============================================================================
// main
// ============================================================================

int main(int argc, char** argv) {

    // Pin to one core, high priority — same as threaded version for fair compare
    SetProcessAffinityMask(GetCurrentProcess(), 1);   // Core 0 only
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    // -------------------------------------------------------------------------
    // 1. Config
    // -------------------------------------------------------------------------
    SystemConfig config;
    try {
        config = ConfigManager::load(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << "[ERROR] Config: " << ex.what() << "\n";
        return EXIT_FAILURE;
    }

    // Sync columns from CSV file if in CSV mode
    auto source = createDataSource(config);
    if (config.mode == Mode::CSV) {
        config.columns = source->detectedColumns();
        std::cout << "[Main] CSV columns synced: " << config.columns << "\n";
    }

    std::cout << "========================================\n"
              << " CynLr Linear Pipeline  (single thread, 4 stages)\n"
              << "========================================\n"
              << " Mode             : " << config.mode         << "\n"
              << " Columns (m)      : " << config.columns      << "\n"
              << " Cycle (T)        : " << config.cycle_time_ns << " ns\n"
              << " Threshold        : " << static_cast<int>(config.threshold) << "\n"
              << " Kernel size      : " << config.kernel.size() << "\n"
              << " Boundary policy  : " << config.boundary_policy << "\n"
              << " Write output     : " << (config.write_output ? "yes" : "no") << "\n"
              << " Duration         : ";
    if (config.run_duration_ms == 0) std::cout << "unlimited\n";
    else                             std::cout << config.run_duration_ms << " ms\n";
    std::cout << " Max rows         : ";
    if (config.max_rows == 0) std::cout << "unlimited\n";
    else                      std::cout << config.max_rows << "\n";
    std::cout << "========================================\n\n";

    // -------------------------------------------------------------------------
    // 2. Build stages
    // -------------------------------------------------------------------------
    InlineLinearFilter  filter  (config);
    LinearLabeller      labeller(config);
    InlineLinearTracer  tracer  (config);   // Phase 4

    // -------------------------------------------------------------------------
    // 3. Optional output files
    //    - Labelled-packet CSV  (same as Phase 3)
    //    - Blob CSV             (Phase 4: one row per completed blob)
    // -------------------------------------------------------------------------
    std::ofstream out_label_csv;
    std::ofstream out_blob_csv;

    if (config.write_output) {
        out_label_csv.open(config.output_file, std::ios::out | std::ios::trunc);
        if (out_label_csv.is_open())
            out_label_csv << "row,col,l1,l2,merge_old,merge_new,"
                             "merge_old2,merge_new2,recycled\n";

        const std::string blob_path = config.output_file + "_blobs.csv";
        out_blob_csv.open(blob_path, std::ios::out | std::ios::trunc);
        if (out_blob_csv.is_open())
            out_blob_csv << "label,pixel_count,top_row,bottom_row,"
                            "left_col,right_col,width,height\n";
    }

    // 3. Timing & stats storage
    std::vector<uint64_t> pixel_timestamps;
    pixel_timestamps.reserve(2'000'000);

    size_t total_pixels  = 0;
    size_t foreground    = 0;
    size_t background    = 0;
    size_t merge_events  = 0;
    size_t recycle_events= 0;
    uint64_t rows_done   = 0;
    uint64_t blobs_total    = 0;    // Phase 4

    const auto deadline =
        (config.run_duration_ms > 0)
            ? std::chrono::steady_clock::now()
                + std::chrono::milliseconds(config.run_duration_ms)
            : std::chrono::steady_clock::time_point::max();

    // Helper: write one LabelledPacket row to the label CSV
    auto writeLabelCSV = [&](const LabelledPacket& lp) {
        if (!out_label_csv.is_open()) return;
        out_label_csv << lp.row        << ','
                      << lp.col        << ','
                      << lp.l1         << ','
                      << lp.l2         << ','
                      << lp.merge_old  << ','
                      << lp.merge_new  << ','
                      << lp.merge_old2 << ','
                      << lp.merge_new2 << ','
                      << lp.recycled   << '\n';
    };

    // Helper: write one CompletedBlob row to the blob CSV           Phase 4
    auto writeBlobCSV = [&](const CompletedBlob& b) {
        if (!out_blob_csv.is_open()) return;
        const uint64_t w = (b.right_col  >= b.left_col)
                         ? (b.right_col  - b.left_col  + 1) : 0;
        const uint64_t h = (b.bottom_row >= b.top_row)
                         ? (b.bottom_row - b.top_row   + 1) : 0;
        out_blob_csv << b.label        << ','
                     << b.pixel_count  << ','
                     << b.top_row      << ','
                     << b.bottom_row   << ','
                     << b.left_col     << ','
                     << b.right_col    << ','
                     << w              << ','
                     << h              << '\n';
    };

    // Helper: process one LabelledPacket through all downstream stages
    // (tracer, CSV writers, stats, timestamp).
    // Extracted to avoid duplicating the identical block at three call sites
    // (v1, v2, and flush results).
    std::vector<CompletedBlob> completed_blobs;   // reused per packet

    auto processLabelledPacket = [&](const LabelledPacket& lp) {
        // ---- Tracer (Phase 4) ----
        completed_blobs.clear();
        tracer.assignPacket(lp, completed_blobs);
        for (const auto& b : completed_blobs) {
            writeBlobCSV(b);
            ++blobs_total;
        }

        // ---- Label CSV ----
        writeLabelCSV(lp);

        // ---- Timestamp (one per output FilteredPacket = two pixels) ----
        const uint64_t ts = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        pixel_timestamps.push_back(ts);
        pixel_timestamps.push_back(ts);

        // ---- Pixel-level stats ----
        if (lp.l1) ++foreground; else ++background;
        if (lp.l2) ++foreground; else ++background;
        if (lp.merge_old  != 0) ++merge_events;
        if (lp.merge_old2 != 0) ++merge_events;
        if (lp.recycled   != 0) ++recycle_events;
        total_pixels += 2;
    };

    // -------------------------------------------------------------------------
    // 4. Single-thread pipeline loop
    //    generate → filter → timestamp → store
    // -------------------------------------------------------------------------
    uint64_t prev_row = UINT64_MAX;
    uint8_t  last_val = 0;
    uint64_t last_col = 0;

    DataPacket packet{};

    while (!g_stop.load(std::memory_order_relaxed)) {
        if (std::chrono::steady_clock::now() >= deadline) break;
        if (config.max_rows > 0 && rows_done >= config.max_rows) break;

        // --- Generate --------------------------------------------------------
        if (!source->next(packet)) break;   // CSV exhausted

        // --- Row transition --------------------------------------------------
        if (packet.row != prev_row) {
            if (prev_row != UINT64_MAX) {
                // Flush filter right-padding
                std::vector<FilteredPacket> flush_fps;
                filter.flush(last_val, prev_row, last_col, flush_fps);

                for (const auto& ffp : flush_fps) {
                    const LabelledPacket lp = labeller.assignPacket(ffp);
                    processLabelledPacket(lp);
                }
                ++rows_done;
            }
            filter.beginRow(packet.v1, packet.row);
            prev_row = packet.row;
        }

        // --- Filter pixel 1 --------------------------------------------------
        FilteredPacket fp;

        if (filter.processSample(packet.v1, packet.row, packet.col, fp)) {
            const LabelledPacket lp = labeller.assignPacket(fp);
            processLabelledPacket(lp);
        }

        // --- Filter pixel 2 --------------------------------------------------
        if (filter.processSample(packet.v2, packet.row, packet.col + 1, fp)) {
            const LabelledPacket lp = labeller.assignPacket(fp);
            processLabelledPacket(lp);
        }

        last_val = packet.v2;
        last_col = packet.col + 1;
    }

    // Flush final row
    if (prev_row != UINT64_MAX) {
        std::vector<FilteredPacket> flush_fps;
        filter.flush(last_val, prev_row, last_col, flush_fps);

        for (const auto& ffp : flush_fps) {
            const LabelledPacket lp = labeller.assignPacket(ffp);
            processLabelledPacket(lp);
        }
        ++rows_done;
        labeller.flushFinalRow();
    }

    // Emit all still-active blobs (those that never received a recycle event
    // because they touched the last row of the input).
    {
        std::vector<CompletedBlob> final_blobs;
        tracer.flush(final_blobs);
        for (const auto& b : final_blobs) {
            writeBlobCSV(b);
            ++blobs_total;
        }
    }

    // -------------------------------------------------------------------------
    // 5. Compute and print stats
    // -------------------------------------------------------------------------
    if (out_label_csv.is_open()) { out_label_csv.flush(); out_label_csv.close(); }
    if (out_blob_csv.is_open())  { out_blob_csv.flush();  out_blob_csv.close();  }

    // -------------------------------------------------------------------------
    // 8. Performance report
    // -------------------------------------------------------------------------
    LinearStats stats = computeLinearStats(pixel_timestamps);
    printLinearStats(stats, config.cycle_time_ns);

    // 6. Performance report
    std::cout << "========================================\n"
              << " Pipeline Summary  (linear, 4 stages)\n"
              << "========================================\n"
              << " Rows processed       : " << rows_done              << "\n"
              << " Output pixels        : " << total_pixels           << "\n"
              << " Foreground (label>0) : " << foreground             << "\n"
              << " Background (label=0) : " << background             << "\n"
              << " Merge events         : " << merge_events           << "\n"
              << " Recycle events       : " << recycle_events         << "\n"
              << "\n"
              << " Blobs completed      : " << blobs_total            << "\n"
              << " Peak active labels   : " << labeller.peak_active_labels()
              <<   " / " << (config.columns / 2) << " (m/2)\n"
              << " Peak active accum.   : " << tracer.peak_active()
              <<   " / " << (config.columns / 2) << " (m/2)\n"
              << " Memory OK            : "
              << ((labeller.peak_active_labels() <= config.columns / 2 &&
                   tracer.peak_active()          <= config.columns / 2)
                      ? "YES" : "NO")
              << "\n"
              << "\n"
              << " Shutdown cause       : ";

    if (g_stop.load())
        std::cout << "signal (Ctrl+C)\n";
    else if (config.max_rows > 0 && rows_done >= config.max_rows)
        std::cout << "max_rows reached\n";
    else if (std::chrono::steady_clock::now() >= deadline)
        std::cout << "duration limit reached\n";
    else
        std::cout << "source exhausted (CSV)\n";

    std::cout << "\n"
              << " Phase 1 linear avg (filter only) :  ~144 ns\n"
              << " Phase 4 linear avg (all 4 stages):  " << stats.avg_ns << " ns\n"
              << " Phase 4 overhead vs Phase 1       :  "
              << (stats.avg_ns > 144.0 ? stats.avg_ns - 144.0 : 0.0)
              << " ns/pixel  (label + trace)\n"
              << "========================================\n\n";

    return EXIT_SUCCESS;
}