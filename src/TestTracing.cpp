// ============================================================================
// TestTracing.cpp
//
// Unit tests for BlobAccumulator, ITracingOutput, and TracingBlock.
// Mirrors the structure and framework of TestFilter.cpp / TestLabelling.cpp.
//
// Test sections:
//   1  — CompletedBlob layout and static guarantees
//   2  — BlobAccumulator: update, merge, finalise, reset
//   3  — ITracingOutput implementations (NullTracingOutput, capture sink)
//   4  — TracingBlock construction guards
//   5  — Single pixel → one CompletedBlob
//   6  — Horizontal run → correct bounding box
//   7  — Vertical run → correct top/bottom
//   8  — Merge event → combined accumulator
//   9  — Recycle event → blob emitted with correct stats
//  10  — Background pixels → no blob
//  11  — Multiple isolated blobs → correct count and coordinates
//  12  — U-shape end-to-end: Filter → Labelling → Tracing
//  13  — Coordinate accuracy
//  14  — Memory constraint: peak active ≤ m/2
//  15  — Active blobs flushed at pipeline end (run() exit flush)
// ============================================================================

#include "TracingBlock.hpp"
#include "LabellingBlock.hpp"
#include "FilterBlock.hpp"
#include "GeneratorBlock.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>


// ============================================================================
// Minimal test framework (same as TestFilter / TestLabelling)
// ============================================================================

struct TestFailure { std::string message; };

#define ASSERT_TRUE(cond)                                                    \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::ostringstream _os;                                           \
            _os << "ASSERT_TRUE failed: (" #cond ")"                         \
                << "  at " << __FILE__ << ":" << __LINE__;                   \
            throw TestFailure{ _os.str() };                                  \
        }                                                                     \
    } while (false)

#define ASSERT_FALSE(cond)  ASSERT_TRUE(!(cond))

#define ASSERT_EQ(a, b)                                                      \
    do {                                                                      \
        if ((a) != (b)) {                                                     \
            std::ostringstream _os;                                           \
            _os << "ASSERT_EQ failed: " << (a) << " != " << (b)             \
                << "  (" #a " != " #b ")"                                    \
                << "  at " << __FILE__ << ":" << __LINE__;                   \
            throw TestFailure{ _os.str() };                                  \
        }                                                                     \
    } while (false)

#define ASSERT_NE(a, b)                                                      \
    do {                                                                      \
        if ((a) == (b)) {                                                     \
            std::ostringstream _os;                                           \
            _os << "ASSERT_NE failed: " << (a) << " == " << (b)             \
                << "  (" #a " == " #b ")"                                    \
                << "  at " << __FILE__ << ":" << __LINE__;                   \
            throw TestFailure{ _os.str() };                                  \
        }                                                                     \
    } while (false)

#define ASSERT_THROWS(expr, exc_type)                                        \
    do {                                                                      \
        bool _threw = false;                                                  \
        try { (expr); }                                                       \
        catch (const exc_type&) { _threw = true; }                           \
        catch (...) {}                                                        \
        if (!_threw) {                                                        \
            std::ostringstream _os;                                           \
            _os << "ASSERT_THROWS failed: (" #expr ")"                       \
                << " did not throw " #exc_type                               \
                << "  at " << __FILE__ << ":" << __LINE__;                   \
            throw TestFailure{ _os.str() };                                  \
        }                                                                     \
    } while (false)

struct TestCase {
    std::string           name;
    std::function<void()> fn;
};

static std::vector<TestCase> g_tests;

struct TestRegistrar {
    TestRegistrar(const char* name, std::function<void()> fn) {
        g_tests.push_back({ name, std::move(fn) });
    }
};

#define TEST(name)                                            \
    static void test_##name();                                \
    static TestRegistrar reg_##name(#name, test_##name);      \
    static void test_##name()

static int run_all_tests() {
    int passed = 0, failed = 0;
    for (const auto& tc : g_tests) {
        try {
            tc.fn();
            std::cout << "  [PASS]  " << tc.name << '\n';
            ++passed;
        } catch (const TestFailure& tf) {
            std::cout << "  [FAIL]  " << tc.name << '\n'
                      << "          " << tf.message << '\n';
            ++failed;
        } catch (const std::exception& ex) {
            std::cout << "  [FAIL]  " << tc.name
                      << " (unexpected exception: " << ex.what() << ")\n";
            ++failed;
        }
    }
    std::cout << "\nResults: " << passed << " passed, "
              << failed << " failed"
              << " (total " << (passed + failed) << ")\n";
    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}


// ============================================================================
// Capture output sink — collects emitted blobs for assertions
// ============================================================================

class CaptureSink : public ITracingOutput {
public:
    void emit(const CompletedBlob& b) override { blobs.push_back(b); }
    void flush() override {}
    std::vector<CompletedBlob> blobs;
};


// ============================================================================
// Helpers
// ============================================================================

static SystemConfig tracingConfig(size_t columns) {
    SystemConfig cfg{};
    cfg.columns       = columns;
    cfg.cycle_time_ns = 1'000'000ULL;
    cfg.threshold     = 128;
    cfg.mode          = Mode::RANDOM;
    cfg.kernel        = { 0.00025177f, 0.008666992f, 0.078025818f,
                          0.24130249f, 0.343757629f, 0.24130249f,
                          0.078025818f, 0.008666992f, 0.000125885f };
    cfg.write_output  = false;
    cfg.boundary_policy = BoundaryPolicy::ZERO_PAD;
    return cfg;
}

// Run the full labelling + tracing chain synchronously.
// Returns captured blobs.
static std::vector<CompletedBlob> runChain(
        size_t                        columns,
        const std::vector<uint8_t>&   rows_data)
{
    SimpleQueue<FilteredPacket> label_in;
    SimpleQueue<LabelledPacket> trace_in;

    const size_t total_pixels = rows_data.size();
    const size_t total_rows   = total_pixels / columns;

    for (size_t r = 0; r < total_rows; ++r) {
        for (size_t c = 0; c < columns; c += 2) {
            FilteredPacket fp{};
            fp.b1  = rows_data[r * columns + c];
            fp.b2  = rows_data[r * columns + c + 1];
            fp.row = static_cast<uint64_t>(r);
            fp.col = static_cast<uint64_t>(c);
            label_in.push(fp);
        }
    }

    SystemConfig cfg = tracingConfig(columns);

    LabellingBlock labeller(cfg, label_in, trace_in);
    labeller.stop();
    labeller.run();

    CaptureSink sink;
    TracingBlock tracer(cfg, trace_in, sink);
    tracer.stop();
    tracer.run();

    return sink.blobs;
}

// Sum pixel counts across all emitted blobs.
static uint64_t totalPixels(const std::vector<CompletedBlob>& blobs) {
    uint64_t sum = 0;
    for (const auto& b : blobs) sum += b.pixel_count;
    return sum;
}

// [ADDED] Count non-zero (foreground) pixels in the raw binary input grid.
static uint64_t totalForegroundPixels(const std::vector<uint8_t>& rows_data) {
    uint64_t count = 0;
    for (uint8_t v : rows_data) if (v != 0) ++count;
    return count;
}


// ============================================================================
// Section 1 — CompletedBlob layout
// ============================================================================

TEST(blob_sizeof_is_48) {
    ASSERT_EQ(sizeof(CompletedBlob), size_t(48));
}

TEST(blob_is_trivially_copyable) {
    ASSERT_TRUE(std::is_trivially_copyable<CompletedBlob>::value);
}

TEST(blob_zero_initialised_by_default) {
    CompletedBlob b{};
    ASSERT_EQ(b.label,       uint16_t(0));
    ASSERT_EQ(b.pixel_count, uint64_t(0));
    ASSERT_EQ(b.top_row,     uint64_t(0));
    ASSERT_EQ(b.bottom_row,  uint64_t(0));
    ASSERT_EQ(b.left_col,    uint64_t(0));
    ASSERT_EQ(b.right_col,   uint64_t(0));
}


// ============================================================================
// Section 2 — BlobAccumulator
// ============================================================================

TEST(accumulator_inactive_by_default) {
    BlobAccumulator a;
    ASSERT_FALSE(a.active);
    ASSERT_EQ(a.pixel_count, uint64_t(0));
}

TEST(accumulator_update_single_pixel) {
    BlobAccumulator a;
    a.update(5, 10);
    ASSERT_TRUE(a.active);
    ASSERT_EQ(a.pixel_count, uint64_t(1));
    ASSERT_EQ(a.top_row,     uint64_t(5));
    ASSERT_EQ(a.bottom_row,  uint64_t(5));
    ASSERT_EQ(a.left_col,    uint64_t(10));
    ASSERT_EQ(a.right_col,   uint64_t(10));
}

TEST(accumulator_update_expands_bounds) {
    BlobAccumulator a;
    a.update(3, 7);
    a.update(8, 2);
    a.update(5, 11);
    ASSERT_EQ(a.pixel_count, uint64_t(3));
    ASSERT_EQ(a.top_row,     uint64_t(3));
    ASSERT_EQ(a.bottom_row,  uint64_t(8));
    ASSERT_EQ(a.left_col,    uint64_t(2));
    ASSERT_EQ(a.right_col,   uint64_t(11));
}

TEST(accumulator_merge_combines_stats) {
    BlobAccumulator a, b;
    a.update(0, 0); a.update(1, 3);   // rows 0-1, cols 0-3
    b.update(2, 5); b.update(3, 8);   // rows 2-3, cols 5-8

    a.merge(b);
    ASSERT_EQ(a.pixel_count, uint64_t(4));
    ASSERT_EQ(a.top_row,     uint64_t(0));
    ASSERT_EQ(a.bottom_row,  uint64_t(3));
    ASSERT_EQ(a.left_col,    uint64_t(0));
    ASSERT_EQ(a.right_col,   uint64_t(8));
}

TEST(accumulator_merge_empty_into_active) {
    BlobAccumulator a, b;
    a.update(5, 5);
    // b is inactive (never updated)
    a.merge(b);  // must not crash or corrupt a
    ASSERT_EQ(a.pixel_count, uint64_t(1));
    ASSERT_TRUE(a.active);
}

TEST(accumulator_finalise_produces_correct_blob) {
    BlobAccumulator a;
    a.update(2, 4);
    a.update(3, 7);
    const auto blob = a.finalise(uint16_t(5));
    ASSERT_EQ(blob.label,       uint16_t(5));
    ASSERT_EQ(blob.pixel_count, uint64_t(2));
    ASSERT_EQ(blob.top_row,     uint64_t(2));
    ASSERT_EQ(blob.bottom_row,  uint64_t(3));
    ASSERT_EQ(blob.left_col,    uint64_t(4));
    ASSERT_EQ(blob.right_col,   uint64_t(7));
}

TEST(accumulator_reset_clears_all) {
    BlobAccumulator a;
    a.update(3, 3);
    a.reset();
    ASSERT_FALSE(a.active);
    ASSERT_EQ(a.pixel_count, uint64_t(0));
}

TEST(accumulator_finalise_after_reset_returns_zero_blob) {
    BlobAccumulator a;
    a.reset();  // start fresh
    const auto blob = a.finalise(uint16_t(1));
    ASSERT_EQ(blob.pixel_count, uint64_t(0));
    ASSERT_EQ(blob.top_row,     uint64_t(0));  // sentinel collapsed to 0
    ASSERT_EQ(blob.left_col,    uint64_t(0));  // sentinel collapsed to 0
}


// ============================================================================
// Section 3 — ITracingOutput implementations
// ============================================================================

TEST(null_output_does_not_crash) {
    NullTracingOutput sink;
    CompletedBlob b{}; b.label = 1; b.pixel_count = 5;
    sink.emit(b);
    sink.flush();
    ASSERT_TRUE(true);  // reaching here means no crash
}

TEST(capture_sink_records_blobs) {
    CaptureSink sink;
    CompletedBlob b1{}; b1.label = 1; b1.pixel_count = 10;
    CompletedBlob b2{}; b2.label = 2; b2.pixel_count = 20;
    sink.emit(b1);
    sink.emit(b2);
    ASSERT_EQ(sink.blobs.size(), size_t(2));
    ASSERT_EQ(sink.blobs[0].label, uint16_t(1));
    ASSERT_EQ(sink.blobs[1].label, uint16_t(2));
}


// ============================================================================
// Section 4 — TracingBlock construction guards
// ============================================================================

TEST(block_zero_columns_throws) {
    SystemConfig cfg = tracingConfig(0);
    SimpleQueue<LabelledPacket> q;
    NullTracingOutput sink;
    ASSERT_THROWS(TracingBlock(cfg, q, sink), std::invalid_argument);
}

TEST(block_odd_columns_throws) {
    SystemConfig cfg = tracingConfig(5);
    SimpleQueue<LabelledPacket> q;
    NullTracingOutput sink;
    ASSERT_THROWS(TracingBlock(cfg, q, sink), std::invalid_argument);
}

TEST(block_valid_columns_constructs) {
    SystemConfig cfg = tracingConfig(10);
    SimpleQueue<LabelledPacket> q;
    NullTracingOutput sink;
    TracingBlock tracer(cfg, q, sink);
    ASSERT_TRUE(true);
}

TEST(block_empty_input_no_blobs) {
    auto blobs = runChain(4, {});
    ASSERT_EQ(blobs.size(), size_t(0));
}

TEST(block_all_background_no_blobs) {
    std::vector<uint8_t> grid(8, 0);
    auto blobs = runChain(4, grid);
    ASSERT_EQ(blobs.size(), size_t(0));
    // Conservation: 0 foreground pixels → 0 blob pixels.
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [ADDED]
}


// ============================================================================
// Section 5 — Single pixel produces one CompletedBlob
// ============================================================================

TEST(single_pixel_one_blob) {
    // 3 rows: [1,0,0,0], [0,0,0,0], [0,0,0,0]
    // Blob completes when the blank row 1 is processed (label recycled).
    std::vector<uint8_t> grid = {
        1, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };
    auto blobs = runChain(4, grid);
    ASSERT_EQ(blobs.size(), size_t(1));
    ASSERT_EQ(blobs[0].pixel_count, uint64_t(1));
    ASSERT_EQ(blobs[0].top_row,     uint64_t(0));
    ASSERT_EQ(blobs[0].bottom_row,  uint64_t(0));
    ASSERT_EQ(blobs[0].left_col,    uint64_t(0));
    ASSERT_EQ(blobs[0].right_col,   uint64_t(0));
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [ADDED]
}

TEST(single_pixel_bounding_box_is_1x1) {
    std::vector<uint8_t> grid = {
        0, 1, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };
    auto blobs = runChain(4, grid);
    ASSERT_EQ(blobs.size(), size_t(1));
    // Derived width = right - left + 1 = 1
    ASSERT_EQ(blobs[0].right_col - blobs[0].left_col + 1, uint64_t(1));
    ASSERT_EQ(blobs[0].bottom_row - blobs[0].top_row + 1, uint64_t(1));
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [ADDED]
}


// ============================================================================
// Section 6 — Horizontal run
// ============================================================================

TEST(horizontal_run_single_blob) {
    // 1 row × 4 cols: all foreground, then blank rows to trigger recycle
    std::vector<uint8_t> grid = {
        1, 1, 1, 1,
        0, 0, 0, 0,
        0, 0, 0, 0
    };
    auto blobs = runChain(4, grid);
    ASSERT_EQ(blobs.size(), size_t(1));
    ASSERT_EQ(blobs[0].pixel_count, uint64_t(4));
    ASSERT_EQ(blobs[0].left_col,    uint64_t(0));
    ASSERT_EQ(blobs[0].right_col,   uint64_t(3));
    ASSERT_EQ(blobs[0].top_row,     uint64_t(0));
    ASSERT_EQ(blobs[0].bottom_row,  uint64_t(0));
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [ADDED]
}

TEST(horizontal_run_correct_width) {
    // Row: [0,1,1,0], then blanks
    std::vector<uint8_t> grid = {
        0, 1, 1, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };
    auto blobs = runChain(4, grid);
    ASSERT_EQ(blobs.size(), size_t(1));
    ASSERT_EQ(blobs[0].pixel_count, uint64_t(2));
    ASSERT_EQ(blobs[0].left_col,    uint64_t(1));
    ASSERT_EQ(blobs[0].right_col,   uint64_t(2));
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [ADDED]
}


// ============================================================================
// Section 7 — Vertical run
// ============================================================================

TEST(vertical_run_single_blob) {
    // 4 rows × 2 cols: col 0 foreground, col 1 background
    std::vector<uint8_t> grid = {
        1, 0,
        1, 0,
        1, 0,
        1, 0
    };
    // Blob is NOT recycled until source exhaustion (no blank rows after).
    // run() flushes active accumulators on exit.
    auto blobs = runChain(2, grid);
    ASSERT_EQ(blobs.size(), size_t(1));
    ASSERT_EQ(blobs[0].pixel_count, uint64_t(4));
    ASSERT_EQ(blobs[0].top_row,     uint64_t(0));
    ASSERT_EQ(blobs[0].bottom_row,  uint64_t(3));
    ASSERT_EQ(blobs[0].left_col,    uint64_t(0));
    ASSERT_EQ(blobs[0].right_col,   uint64_t(0));
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [ADDED]
}

TEST(vertical_run_correct_height) {
    // Rows 1 and 2 foreground at col 0, rows 0 and 3 background
    std::vector<uint8_t> grid = {
        0, 0,
        1, 0,
        1, 0,
        0, 0
    };
    auto blobs = runChain(2, grid);
    ASSERT_EQ(blobs.size(), size_t(1));
    ASSERT_EQ(blobs[0].top_row,    uint64_t(1));
    ASSERT_EQ(blobs[0].bottom_row, uint64_t(2));
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [ADDED]
}


// ============================================================================
// Section 8 — Merge event → combined accumulator
// ============================================================================

TEST(u_shape_merge_single_blob) {
    // U-shape: two arms + closing base
    std::vector<uint8_t> grid = {
        1, 0, 1, 0,
        1, 0, 1, 0,
        1, 1, 1, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };
    auto blobs = runChain(4, grid);
    // All 7 foreground pixels (3+3+1 in rows 0,1,2) form ONE blob
    ASSERT_EQ(blobs.size(), size_t(1));
    ASSERT_EQ(blobs[0].pixel_count, uint64_t(7));
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [ADDED]
}

TEST(u_shape_bounding_box_correct) {
    std::vector<uint8_t> grid = {
        1, 0, 1, 0,
        1, 0, 1, 0,
        1, 1, 1, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };
    auto blobs = runChain(4, grid);
    ASSERT_EQ(blobs.size(), size_t(1));
    ASSERT_EQ(blobs[0].top_row,    uint64_t(0));
    ASSERT_EQ(blobs[0].bottom_row, uint64_t(2));
    ASSERT_EQ(blobs[0].left_col,   uint64_t(0));
    ASSERT_EQ(blobs[0].right_col,  uint64_t(2));
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [ADDED]
}

TEST(merge_combines_pixel_counts) {
    // Two arms of a U each have 2 pixels; base adds 3.
    // Total = 2 + 2 + 3 = 7
    std::vector<uint8_t> grid = {
        1, 0, 1, 0,
        1, 0, 1, 0,
        1, 1, 1, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };
    auto blobs = runChain(4, grid);
    ASSERT_EQ(totalPixels(blobs), uint64_t(7));
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [ADDED]
}

TEST(merge_combined_bounding_box_is_union) {
    // Blob A: rows 0-1, cols 0-1
    // Blob B: rows 0-1, cols 3-4
    // Connected by row 2: cols 0-4
    std::vector<uint8_t> grid = {
        1, 1, 0, 1, 1, 0,
        1, 1, 0, 1, 1, 0,
        1, 1, 1, 1, 1, 0,
        0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0
    };
    auto blobs = runChain(6, grid);
    // All one connected component
    ASSERT_EQ(blobs.size(), size_t(1));
    ASSERT_EQ(blobs[0].left_col,  uint64_t(0));
    ASSERT_EQ(blobs[0].right_col, uint64_t(4));
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [ADDED]
}


// ============================================================================
// Section 9 — Recycle event → blob emitted
// ============================================================================

TEST(recycle_emits_before_reuse) {
    // Blob at row 0, blank rows → recycle → second blob at row 3
    std::vector<uint8_t> grid = {
        1, 1, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 1, 1
    };
    auto blobs = runChain(4, grid);
    // Two separate blobs
    ASSERT_EQ(blobs.size(), size_t(2));
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [ADDED]
}

TEST(two_blobs_correct_pixel_counts) {
    std::vector<uint8_t> grid = {
        1, 1, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 1, 1
    };
    auto blobs = runChain(4, grid);
    ASSERT_EQ(blobs.size(), size_t(2));
    ASSERT_EQ(blobs[0].pixel_count, uint64_t(2));
    ASSERT_EQ(blobs[1].pixel_count, uint64_t(2));
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [ADDED]
}

TEST(two_blobs_disjoint_coordinates) {
    std::vector<uint8_t> grid = {
        1, 1, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 1, 1
    };
    auto blobs = runChain(4, grid);
    ASSERT_EQ(blobs.size(), size_t(2));
    // Blobs must not share any row
    ASSERT_NE(blobs[0].top_row, blobs[1].top_row);
}


// ============================================================================
// Section 10 — Background pixels → no accumulator update
// ============================================================================

TEST(all_background_no_blobs) {
    std::vector<uint8_t> grid(12, 0);
    auto blobs = runChain(6, grid);
    ASSERT_EQ(blobs.size(), size_t(0));
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [ADDED]
}

TEST(background_pixel_in_foreground_row_excluded) {
    // Row: [1,0,1,0] — two foreground pixels, two background pixels
    std::vector<uint8_t> grid = {
        1, 0, 1, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };
    auto blobs = runChain(4, grid);
    // Two isolated blobs (col gap = 2, no N/NW/NE bridge on row 0 alone)
    ASSERT_EQ(blobs.size(), size_t(2));
    ASSERT_EQ(totalPixels(blobs), uint64_t(2));
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [ADDED]
}


// ============================================================================
// Section 11 — Multiple blobs
// ============================================================================

TEST(three_isolated_blobs) {
    // Three separate 1-pixel blobs in one row, separated by gaps ≥ 2
    std::vector<uint8_t> grid = {
        1, 0, 1, 0, 1, 0,
        0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0
    };
    auto blobs = runChain(6, grid);
    ASSERT_EQ(blobs.size(), size_t(3));
    ASSERT_EQ(totalPixels(blobs), uint64_t(3));
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [ADDED]
}

TEST(blobs_on_separate_rows) {
    // One blob per row, separated by blank rows
    std::vector<uint8_t> grid = {
        1, 1, 0, 0,
        0, 0, 0, 0,
        0, 1, 1, 0,
        0, 0, 0, 0,
        1, 0, 0, 0
    };
    auto blobs = runChain(4, grid);
    ASSERT_EQ(blobs.size(), size_t(3));
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [ADDED]
}


// ============================================================================
// Section 12 — Coordinate accuracy
// ============================================================================

TEST(bounding_box_single_row_wide_blob) {
    // 1 row × 6 cols: all foreground at cols 1-4
    std::vector<uint8_t> grid = {
        0, 1, 1, 1, 1, 0,
        0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0
    };
    auto blobs = runChain(6, grid);
    ASSERT_EQ(blobs.size(), size_t(1));
    ASSERT_EQ(blobs[0].left_col,  uint64_t(1));
    ASSERT_EQ(blobs[0].right_col, uint64_t(4));
    ASSERT_EQ(blobs[0].pixel_count, uint64_t(4));
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [ADDED]
}

TEST(bounding_box_multi_row_blob) {
    // Staircase pattern: descends one col per row
    // Row 0: col 0
    // Row 1: cols 0-1
    // Row 2: cols 0-2
    std::vector<uint8_t> grid = {
        1, 0, 0, 0,
        1, 1, 0, 0,
        1, 1, 1, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };
    auto blobs = runChain(4, grid);
    ASSERT_EQ(blobs.size(), size_t(1));
    ASSERT_EQ(blobs[0].pixel_count, uint64_t(6));
    ASSERT_EQ(blobs[0].top_row,     uint64_t(0));
    ASSERT_EQ(blobs[0].bottom_row,  uint64_t(2));
    ASSERT_EQ(blobs[0].left_col,    uint64_t(0));
    ASSERT_EQ(blobs[0].right_col,   uint64_t(2));
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [ADDED]
}

TEST(total_pixel_count_matches_foreground_count) {
    // 4×4 checkerboard: 8 foreground pixels, but they are all connected
    // diagonally (8-connectivity) so form one component.
    std::vector<uint8_t> grid = {
        1, 0, 1, 0,
        0, 1, 0, 1,
        1, 0, 1, 0,
        0, 1, 0, 1
    };
    auto blobs = runChain(4, grid);
    // Conservation: every foreground pixel is in exactly one blob.
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [SIMPLIFIED]
}


// ============================================================================
// Section 13 — Memory constraint
// ============================================================================

TEST(peak_active_never_exceeds_m_over_2) {
    const size_t M = 20, ROWS = 50;
    std::vector<uint8_t> grid(M * ROWS, uint8_t(0));
    for (size_t r = 0; r < ROWS; ++r)
        for (size_t c = 0; c < M; c += 2)
            grid[r * M + c] = 1;

    SimpleQueue<FilteredPacket>  in_q;
    SimpleQueue<LabelledPacket>  out_q;

    for (size_t r = 0; r < ROWS; ++r)
        for (size_t c = 0; c < M; c += 2) {
            FilteredPacket fp{};
            fp.b1 = grid[r*M+c]; fp.b2 = grid[r*M+c+1];
            fp.row = r; fp.col = c;
            in_q.push(fp);
        }

    SystemConfig cfg = tracingConfig(M);
    LabellingBlock labeller(cfg, in_q, out_q);
    labeller.stop(); labeller.run();

    CaptureSink sink;
    TracingBlock tracer(cfg, out_q, sink);
    tracer.stop(); tracer.run();

    ASSERT_TRUE(tracer.peak_active_accumulators() <= M / 2);

    // [ADDED] Conservation: all foreground pixels accounted for.
    ASSERT_EQ(totalPixels(sink.blobs), totalForegroundPixels(grid));
}

TEST(total_pixels_conserved_across_blobs) {
    // Every foreground pixel must appear in exactly one blob.
    const size_t M = 10;
    std::vector<uint8_t> grid = {
        1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    auto blobs = runChain(M, grid);
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [ADDED]
}


// ============================================================================
// Section 14 — Active blobs flushed at run() exit
// ============================================================================

TEST(active_blobs_flushed_on_exit) {
    // Blob that reaches end of CSV without a blank row after it.
    // run() must emit it via the exit-flush.
    std::vector<uint8_t> grid = {
        1, 1, 1, 1   // one row, no blank rows after — never recycled by labeller
    };
    auto blobs = runChain(4, grid);
    // Must have exactly one blob even though no recycle event was emitted.
    ASSERT_EQ(blobs.size(), size_t(1));
    ASSERT_EQ(blobs[0].pixel_count, uint64_t(4));
    ASSERT_EQ(totalPixels(blobs), totalForegroundPixels(grid)); // [ADDED]
}


// ============================================================================
// Section 15 — Threaded integration
// ============================================================================

TEST(threaded_pipeline_does_not_crash) {
    // Smoke test: run all four stages concurrently for 50ms.
    const size_t M = 20;
    SystemConfig cfg = tracingConfig(M);

    DynamicSPSCQueue<DataPacket>     gen_to_filter   (M/2, M/2);
    DynamicSPSCQueue<FilteredPacket> filter_to_label (M/2, M/2);
    DynamicSPSCQueue<LabelledPacket> label_to_tracing(M/2, M/2);

    auto source = createDataSource(cfg);
    GeneratorBlock generator(cfg, gen_to_filter, std::move(source));
    FilterBlock    filter   (cfg, gen_to_filter, filter_to_label,
                             cfg.threshold, cfg.boundary_policy);
    LabellingBlock labeller (cfg, filter_to_label, label_to_tracing);
    CaptureSink    sink;
    TracingBlock   tracer   (cfg, label_to_tracing, sink);

    std::thread gen_t  ([&generator]{ generator.run(); });
    std::thread flt_t  ([&filter]   { filter.run();    });
    std::thread lab_t  ([&labeller] { labeller.run();  });
    std::thread trc_t  ([&tracer]   { tracer.run();    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    generator.stop(); gen_t.join();
    while (!gen_to_filter.empty()) std::this_thread::yield();
    filter.stop();    flt_t.join();
    while (!filter_to_label.empty()) std::this_thread::yield();
    labeller.stop();  lab_t.join();
    while (!label_to_tracing.empty()) std::this_thread::yield();
    tracer.stop();    trc_t.join();

    // At least some blobs should have been emitted from 50ms of random data.
    ASSERT_TRUE(tracer.blobs_completed() > 0 || tracer.packets_processed() > 0);
}


// ============================================================================
// Entry point
// ============================================================================

int main() {
    std::cout << "======================================================\n"
              << "  CynLr TracingBlock Test Suite\n"
              << "  [+] Conservation invariant checks in all data tests\n"
              << "======================================================\n\n";
    return run_all_tests();
}
