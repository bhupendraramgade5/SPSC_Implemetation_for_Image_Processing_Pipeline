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

#define NOMINMAX
#include <windows.h>

#include "ConfigManager.hpp"
#include "GeneratorBlock.hpp"   // IDataSource, createDataSource
#include "FilterUtils.hpp"      // SlidingWindow, BinaryThresholder, FilteredPacket
#include "Queue.hpp"            // DataPacket
#include "OutputWriter.hpp"     // IOutputWriter, makeOutputWriter

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

class LinearFilter {
public:
    explicit LinearFilter(const SystemConfig& cfg)
        : cfg_(cfg)
        , threshold_(static_cast<float>(cfg.threshold))
        , window_(cfg.kernel.size())
        , half_width_(cfg.kernel.size() / 2)
        , policy_(cfg.boundary_policy)
    {
        if (cfg.kernel.empty())        throw std::invalid_argument("LinearFilter: empty kernel");
        if (cfg.kernel.size() % 2 == 0) throw std::invalid_argument("LinearFilter: kernel size must be odd");
    }

    // Called once per row start.  Resets the sliding window and left-pads it.
    void beginRow(uint8_t left_edge, uint64_t row) {
        current_row_ = row;
        window_.reset();
        pending_ = PendingOutput{};

        for (size_t i = 0; i < half_width_; ++i) {
            uint8_t pad = applyLeft(policy_, left_edge, half_width_ - i);
            window_.push(pad, row, 0);
        }
    }

    // Process one pixel.  Returns true + fills fp when a pair is complete.
    bool processSample(uint8_t value, uint64_t row, uint64_t col, FilteredPacket& fp) {
        window_.push(value, row, col);

        if (!window_.is_full()) return false;

        const WindowSlot& c = window_.centre();
        float filtered = dotProduct();
        uint8_t binary = (filtered >= threshold_) ? uint8_t{1} : uint8_t{0};

        if (!pending_.has_b1) {
            pending_.b1 = binary; pending_.row = c.row; pending_.col = c.col;
            pending_.has_b1 = true;
            return false;
        }

        pending_.b2     = binary;
        fp.b1  = pending_.b1;
        fp.b2  = binary;
        fp.row = pending_.row;
        fp.col = pending_.col;
        pending_ = PendingOutput{};
        return true;
    }

    // Right-pad at end of row; push any leftover partial pair.
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
    float dotProduct() const {
        const auto& k = cfg_.kernel;
        if (k.size() == 9)   // fast path: unrolled
            return static_cast<float>(window_.at(0).value) * k[0]
                 + static_cast<float>(window_.at(1).value) * k[1]
                 + static_cast<float>(window_.at(2).value) * k[2]
                 + static_cast<float>(window_.at(3).value) * k[3]
                 + static_cast<float>(window_.at(4).value) * k[4]
                 + static_cast<float>(window_.at(5).value) * k[5]
                 + static_cast<float>(window_.at(6).value) * k[6]
                 + static_cast<float>(window_.at(7).value) * k[7]
                 + static_cast<float>(window_.at(8).value) * k[8];
        float s = 0.f;
        for (size_t i = 0; i < k.size(); ++i)
            s += static_cast<float>(window_.at(i).value) * k[i];
        return s;
    }

    const SystemConfig& cfg_;
    float               threshold_;
    SlidingWindow       window_;
    size_t              half_width_;
    BoundaryPolicy      policy_;
    uint64_t            current_row_ = UINT64_MAX;

    struct PendingOutput {
        bool     has_b1 = false, has_b2 = false;
        uint8_t  b1 = 0, b2 = 0;
        uint64_t row = 0, col = 0;
    } pending_;
};

// ============================================================================
// Performance stats
// ============================================================================

struct Stats {
    uint64_t min_ns = UINT64_MAX, max_ns = 0, p50_ns = 0, p99_ns = 0;
    double   avg_ns = 0.0;
    size_t   count  = 0;
};

static Stats computeStats(std::vector<uint64_t>& gaps) {
    Stats s;
    if (gaps.empty()) return s;

    uint64_t sum = 0;
    for (uint64_t g : gaps) {
        if (g < s.min_ns) s.min_ns = g;
        if (g > s.max_ns) s.max_ns = g;
        sum += g;
    }
    s.count  = gaps.size();
    s.avg_ns = static_cast<double>(sum) / s.count;
    std::sort(gaps.begin(), gaps.end());
    s.p50_ns = gaps[s.count / 2];
    s.p99_ns = gaps[static_cast<size_t>(0.99 * s.count)];
    return s;
}

// ============================================================================
// Main
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
    try { config = ConfigManager::load(argc, argv); }
    catch (const std::exception& ex) {
        std::cerr << "[ERROR] Config: " << ex.what() << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "========================================\n"
              << " CynLr Linear Pipeline  (single thread)\n"
              << "========================================\n"
              << " Mode             : " << config.mode         << "\n"
              << " Columns (m)      : " << config.columns      << "\n"
              << " Cycle (T)        : " << config.cycle_time_ns << " ns\n"
              << " Threshold        : " << static_cast<int>(config.threshold) << "\n"
              << " Kernel size      : " << config.kernel.size() << "\n"
              << " Boundary policy  : " << config.boundary_policy << "\n"
              << " Write output     : " << (config.write_output ? "yes" : "no") << "\n";
    if (config.write_output)
        std::cout << " Output file      : " << config.output_file << "\n";
    std::cout << " Duration         : ";
    if (config.run_duration_ms == 0) std::cout << "unlimited\n";
    else                             std::cout << config.run_duration_ms << " ms\n";
    std::cout << " Max rows         : ";
    if (config.max_rows == 0) std::cout << "unlimited\n";
    else                      std::cout << config.max_rows << "\n";
    std::cout << "========================================\n\n";

    // -------------------------------------------------------------------------
    // 2. Build data source and filter
    // -------------------------------------------------------------------------
    auto source = createDataSource(config);
    LinearFilter filter(config);
    auto   writer = makeOutputWriter(config);   // <-- OutputWriter adapter

    // 3. Timing & stats storage
    std::vector<uint64_t> pixel_timestamps;
    pixel_timestamps.reserve(2'000'000);

    size_t   total_pixels = 0, ones = 0, zeros = 0;
    uint64_t rows_done    = 0;

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
                std::vector<FilteredPacket> flush_out;
                filter.flush(last_val, prev_row, last_col, flush_out);
                for (const auto& fp : flush_out) {
                    writer->write(fp);   // <-- OutputWriter
                    uint64_t ts = static_cast<uint64_t>(
                        std::chrono::steady_clock::now().time_since_epoch().count());
                    pixel_timestamps.push_back(ts);
                    pixel_timestamps.push_back(ts);
                    if (fp.b1) ++ones; else ++zeros;
                    if (fp.b2) ++ones; else ++zeros;
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
            writer->write(fp);   // <-- OutputWriter
            uint64_t ts = static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            pixel_timestamps.push_back(ts);
            pixel_timestamps.push_back(ts);
            if (fp.b1) ++ones; else ++zeros;
            if (fp.b2) ++ones; else ++zeros;
            total_pixels += 2;
        }

        // --- Filter pixel 2 --------------------------------------------------
        if (filter.processSample(packet.v2, packet.row, packet.col + 1, fp)) {
            writer->write(fp);   // <-- OutputWriter
            uint64_t ts = static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            pixel_timestamps.push_back(ts);
            pixel_timestamps.push_back(ts);
            if (fp.b1) ++ones; else ++zeros;
            if (fp.b2) ++ones; else ++zeros;
            total_pixels += 2;
        }

        last_val = packet.v2;
        last_col = packet.col + 1;
    }

    // Flush final row
    if (prev_row != UINT64_MAX) {
        std::vector<FilteredPacket> flush_out;
        filter.flush(last_val, prev_row, last_col, flush_out);
        for (const auto& fp : flush_out) {
            writer->write(fp);   // <-- OutputWriter
            uint64_t ts = static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            pixel_timestamps.push_back(ts);
            pixel_timestamps.push_back(ts);
            if (fp.b1) ++ones; else ++zeros;
            if (fp.b2) ++ones; else ++zeros;
            total_pixels += 2;
        }
        ++rows_done;
    }

    writer->finalize();   // flush & close CSV

    // 5. Inter-pixel gap stats
    std::vector<uint64_t> gaps;
    gaps.reserve(pixel_timestamps.size());
    for (size_t i = 1; i < pixel_timestamps.size(); ++i)
        if (pixel_timestamps[i] >= pixel_timestamps[i - 1])
            gaps.push_back(pixel_timestamps[i] - pixel_timestamps[i - 1]);

    Stats stats = computeStats(gaps);

    // 6. Performance report
    std::cout << "========================================\n"
              << " Performance Report  (single thread)\n"
              << "========================================\n"
              << " Samples      : " << stats.count   << "\n"
              << " Min gap (ns) : " << stats.min_ns  << "\n"
              << " Max gap (ns) : " << stats.max_ns  << "\n"
              << " Avg gap (ns) : " << stats.avg_ns  << "\n"
              << " P50 gap (ns) : " << stats.p50_ns  << "\n"
              << " P99 gap (ns) : " << stats.p99_ns  << "\n"
              << " Requirement  : gap <= T (" << config.cycle_time_ns << " ns)\n";

    if (stats.max_ns <= config.cycle_time_ns)
        std::cout << " RESULT       : PASS\n";
    else if (stats.avg_ns <= static_cast<double>(config.cycle_time_ns))
        std::cout << " RESULT       : AVG PASS / MAX FAIL  (OS jitter)\n";
    else
        std::cout << " RESULT       : FAIL\n";

    std::cout << "========================================\n\n"
              << "========================================\n"
              << " Pipeline Summary  (single thread)\n"
              << "========================================\n"
              << " Rows processed : " << rows_done    << "\n"
              << " Output pixels  : " << total_pixels << "\n"
              << " Ones  (1)      : " << ones         << "\n"
              << " Zeros (0)      : " << zeros        << "\n"
              << " Shutdown cause : ";

    if (g_stop.load())
        std::cout << "signal (Ctrl+C)\n";
    else if (config.max_rows > 0 && rows_done >= config.max_rows)
        std::cout << "max_rows reached\n";
    else if (std::chrono::steady_clock::now() >= deadline)
        std::cout << "duration limit reached\n";
    else
        std::cout << "source exhausted (CSV)\n";

    std::cout << "========================================\n\n";

    // -------------------------------------------------------------------------
    // 7. Comparison hint
    double threaded_ref = 300.0;
    std::cout << "========================================\n"
              << " Interpretation\n"
              << "========================================\n"
              << " Threaded avg gap : ~280-530 ns  (prior runs)\n"
              << " Linear  avg gap  : " << stats.avg_ns << " ns\n\n";

    if (stats.avg_ns < threaded_ref * 0.5)
        std::cout << " Threading overhead dominates (~"
                  << static_cast<int>(threaded_ref - stats.avg_ns)
                  << " ns/pixel from queue + context switch).\n"
                  << " Algorithm itself is fast. SIMD would help further.\n";
    else if (stats.avg_ns < threaded_ref * 0.8)
        std::cout << " Moderate threading overhead (~"
                  << static_cast<int>(threaded_ref - stats.avg_ns)
                  << " ns). Both algorithm and threading contribute.\n";
    else
        std::cout << " Algorithm cost dominates. Threading overhead is small.\n"
                  << " To go faster: need SIMD (AVX2) in dotProduct().\n";

    std::cout << "========================================\n";

    return EXIT_SUCCESS;
}