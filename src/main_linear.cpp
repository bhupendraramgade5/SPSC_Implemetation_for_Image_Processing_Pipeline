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

        uint16_t mo2=0, mn2=0;
        const uint16_t l2 = assignLabel(fp.b2 != 0, fp.col + 1, mo2, mn2);

        last_col_ = fp.col + 1;
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
              << " CynLr Linear Pipeline  (single thread, 3 stages)\n"
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

    // ---- 2. Build stages -------------------------------------------------
    InlineLinearFilter filter (config);
    LinearLabeller     labeller(config);

    // Optional CSV output for labelled packets
    std::ofstream out_csv;
    if (config.write_output) {
        out_csv.open(config.output_file, std::ios::out | std::ios::trunc);
        if (out_csv.is_open())
            out_csv << "row,col,l1,l2,merge_old,merge_new,"
                       "merge_old2,merge_new2,recycled\n";
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

    const auto deadline =
        (config.run_duration_ms > 0)
            ? std::chrono::steady_clock::now()
                + std::chrono::milliseconds(config.run_duration_ms)
            : std::chrono::steady_clock::time_point::max();

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
                    // Pass each flushed FilteredPacket through labeller
                    LabelledPacket lp = labeller.assignPacket(ffp);

                    if (out_csv.is_open())
                        out_csv << lp.row << ',' << lp.col << ','
                                << lp.l1  << ',' << lp.l2  << ','
                                << lp.merge_old  << ',' << lp.merge_new  << ','
                                << lp.merge_old2 << ',' << lp.merge_new2 << ','
                                << lp.recycled   << '\n';

                    const uint64_t ts = static_cast<uint64_t>(
                        std::chrono::steady_clock::now().time_since_epoch().count());
                    pixel_timestamps.push_back(ts);
                    pixel_timestamps.push_back(ts);

                    if (lp.l1) ++foreground; else ++background;
                    if (lp.l2) ++foreground; else ++background;
                    if (lp.merge_old  != 0) ++merge_events;
                    if (lp.merge_old2 != 0) ++merge_events;
                    if (lp.recycled   != 0) ++recycle_events;
                    total_pixels += 2;
                }
                ++rows_done;
            }
            filter.beginRow(packet.v1, packet.row);
            prev_row = packet.row;
        }

        // --- Filter pixel 1 --------------------------------------------------
        FilteredPacket fp;

        if (filter.processSample(packet.v1, packet.row, packet.col, fp)) {
            LabelledPacket lp = labeller.assignPacket(fp);

            if (out_csv.is_open())
                out_csv << lp.row << ',' << lp.col << ','
                        << lp.l1  << ',' << lp.l2  << ','
                        << lp.merge_old  << ',' << lp.merge_new  << ','
                        << lp.merge_old2 << ',' << lp.merge_new2 << ','
                        << lp.recycled   << '\n';

            const uint64_t ts = static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            pixel_timestamps.push_back(ts);
            pixel_timestamps.push_back(ts);

            if (lp.l1) ++foreground; else ++background;
            if (lp.l2) ++foreground; else ++background;
            if (lp.merge_old  != 0) ++merge_events;
            if (lp.merge_old2 != 0) ++merge_events;
            if (lp.recycled   != 0) ++recycle_events;
            total_pixels += 2;
        }

        // --- Filter pixel 2 --------------------------------------------------
        if (filter.processSample(packet.v2, packet.row, packet.col + 1, fp)) {
            LabelledPacket lp = labeller.assignPacket(fp);

            if (out_csv.is_open())
                out_csv << lp.row << ',' << lp.col << ','
                        << lp.l1  << ',' << lp.l2  << ','
                        << lp.merge_old  << ',' << lp.merge_new  << ','
                        << lp.merge_old2 << ',' << lp.merge_new2 << ','
                        << lp.recycled   << '\n';

            const uint64_t ts = static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            pixel_timestamps.push_back(ts);
            pixel_timestamps.push_back(ts);

            if (lp.l1) ++foreground; else ++background;
            if (lp.l2) ++foreground; else ++background;
            if (lp.merge_old  != 0) ++merge_events;
            if (lp.merge_old2 != 0) ++merge_events;
            if (lp.recycled   != 0) ++recycle_events;
            total_pixels += 2;
        }

        last_val = packet.v2;
        last_col = packet.col + 1;
    }

    // Flush final row
    if (prev_row != UINT64_MAX) {
        std::vector<FilteredPacket> flush_fps;
        filter.flush(last_val, prev_row, last_col, flush_fps);

        for (const auto& ffp : flush_fps) {
            LabelledPacket lp = labeller.assignPacket(ffp);

            if (out_csv.is_open())
                out_csv << lp.row << ',' << lp.col << ','
                        << lp.l1  << ',' << lp.l2  << ','
                        << lp.merge_old  << ',' << lp.merge_new  << ','
                        << lp.merge_old2 << ',' << lp.merge_new2 << ','
                        << lp.recycled   << '\n';

            const uint64_t ts = static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            pixel_timestamps.push_back(ts);
            pixel_timestamps.push_back(ts);

            if (lp.l1) ++foreground; else ++background;
            if (lp.l2) ++foreground; else ++background;
            if (lp.merge_old  != 0) ++merge_events;
            if (lp.merge_old2 != 0) ++merge_events;
            if (lp.recycled   != 0) ++recycle_events;
            total_pixels += 2;
        }
        ++rows_done;
        labeller.flushFinalRow();
    }

    if (out_csv.is_open()) {
        out_csv.flush();
        out_csv.close();
    }

    // -------------------------------------------------------------------------
    // 5. Compute and print stats
    // -------------------------------------------------------------------------
    LinearStats stats = computeLinearStats(pixel_timestamps);
    printLinearStats(stats, config.cycle_time_ns);

    // 6. Performance report
    std::cout << "========================================\n"
              << " Pipeline Summary  (linear, 3 stages)\n"
              << "========================================\n"
              << " Rows processed       : " << rows_done              << "\n"
              << " Output pixels        : " << total_pixels           << "\n"
              << " Foreground (label>0) : " << foreground             << "\n"
              << " Background (label=0) : " << background             << "\n"
              << " Merge events         : " << merge_events           << "\n"
              << " Recycle events       : " << recycle_events         << "\n"
              << " Peak active labels   : " << labeller.peak_active_labels()
              << " / " << (config.columns / 2) << " (m/2)\n"
              << " Memory OK            : "
              << (labeller.peak_active_labels() <= config.columns / 2 ? "YES" : "NO")
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
              << " Threaded avg gap (Phase 1): ~280-530 ns\n"
              << " Linear   avg gap (Phase 1):  ~144 ns\n"
              << " Linear   avg gap (Phase 3):  " << stats.avg_ns << " ns\n"
              << " Labelling overhead est.   :  "
              << (stats.avg_ns > 144.0 ? stats.avg_ns - 144.0 : 0.0)
              << " ns/pixel\n"
              << "========================================\n\n";

    return EXIT_SUCCESS;
}