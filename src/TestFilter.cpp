// TestFilter.cpp
// ============================================================================
// Unit tests for FilterBlock, SlidingWindow, BinaryThresholder, and the
// end-to-end convolution + threshold pipeline.
//
// Uses the same lightweight test framework as TestGenerator.cpp —
// no external dependencies (GTest optional via CYNLR_USE_GTEST).
// ============================================================================

#include "FilterBlock.hpp"     // FilterBlock (pulls FilterUtils.hpp, Queue.hpp, ConfigManager.hpp)
#include "GeneratorBlock.hpp"  // createDataSource, CSVDataSource (for integration tests)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// Minimal test framework (same as TestGenerator.cpp)
// ============================================================================

struct TestFailure { std::string message; };

#define ASSERT_TRUE(cond)                                                   \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::ostringstream _os;                                          \
            _os << "ASSERT_TRUE failed: (" #cond ")"                        \
                << "  at " << __FILE__ << ":" << __LINE__;                  \
            throw TestFailure{ _os.str() };                                 \
        }                                                                    \
    } while (false)

#define ASSERT_FALSE(cond)  ASSERT_TRUE(!(cond))

#define ASSERT_EQ(a, b)                                                     \
    do {                                                                     \
        if ((a) != (b)) {                                                    \
            std::ostringstream _os;                                          \
            _os << "ASSERT_EQ failed: " << (a) << " != " << (b)            \
                << "  (" #a " != " #b ")"                                   \
                << "  at " << __FILE__ << ":" << __LINE__;                  \
            throw TestFailure{ _os.str() };                                 \
        }                                                                    \
    } while (false)

#define ASSERT_NE(a, b)                                                     \
    do {                                                                     \
        if ((a) == (b)) {                                                    \
            std::ostringstream _os;                                          \
            _os << "ASSERT_NE failed: " << (a) << " == " << (b)            \
                << "  (" #a " == " #b ")"                                   \
                << "  at " << __FILE__ << ":" << __LINE__;                  \
            throw TestFailure{ _os.str() };                                 \
        }                                                                    \
    } while (false)

#define ASSERT_THROWS(expr, exc_type)                                       \
    do {                                                                     \
        bool _threw = false;                                                 \
        try { (expr); }                                                      \
        catch (const exc_type&) { _threw = true; }                          \
        catch (...) {}                                                       \
        if (!_threw) {                                                       \
            std::ostringstream _os;                                          \
            _os << "ASSERT_THROWS failed: (" #expr ")"                      \
                << " did not throw " #exc_type                              \
                << "  at " << __FILE__ << ":" << __LINE__;                  \
            throw TestFailure{ _os.str() };                                 \
        }                                                                    \
    } while (false)

#define ASSERT_NEAR(a, b, tol)                                              \
    do {                                                                     \
        if (std::fabs(static_cast<double>(a) - static_cast<double>(b))      \
                > static_cast<double>(tol)) {                               \
            std::ostringstream _os;                                          \
            _os << "ASSERT_NEAR failed: " << (a) << " vs " << (b)          \
                << " (tol=" << (tol) << ")"                                 \
                << "  at " << __FILE__ << ":" << __LINE__;                  \
            throw TestFailure{ _os.str() };                                 \
        }                                                                    \
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

#define TEST(name)                                           \
    static void test_##name();                               \
    static TestRegistrar reg_##name(#name, test_##name);     \
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
              << failed  << " failed"
              << " (total " << (passed + failed) << ")\n";
    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}


// ============================================================================
// Helpers
// ============================================================================

// Default 9-tap kernel from the spec.
static std::vector<float> specKernel() {
    return {
        0.00025177f,  0.008666992f, 0.078025818f,
        0.24130249f,  0.343757629f, 0.24130249f,
        0.078025818f, 0.008666992f, 0.000125885f
    };
}

// A trivial "identity" kernel: centre = 1.0, rest = 0.
// Filtered value == raw centre value.  Makes threshold math predictable.
static std::vector<float> identityKernel() {
    return { 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f };
}

// A uniform kernel where every tap = 1.0 / 9.
// Filtered value == arithmetic mean of the 9 window values.
static std::vector<float> uniformKernel() {
    const float v = 1.0f / 9.0f;
    return { v, v, v, v, v, v, v, v, v };
}

// Build a minimal SystemConfig for filter tests.
static SystemConfig filterConfig(size_t            columns,
                                 uint8_t           threshold,
                                 std::vector<float> kernel,
                                 BoundaryPolicy    policy = BoundaryPolicy::ZERO_PAD)
{
    SystemConfig cfg{};
    cfg.columns         = columns;
    cfg.cycle_time_ns   = 1'000'000ULL;   // 1 ms — irrelevant for filter logic
    cfg.threshold       = threshold;
    cfg.kernel          = std::move(kernel);
    cfg.mode            = Mode::RANDOM;
    cfg.boundary_policy = policy;
    return cfg;
}

// Feed a vector of raw uint8 values into the filter via a SimpleQueue,
// run the filter synchronously, and collect all FilteredPacket outputs.
//
// `row_width` = number of columns (m).
// `raw` must have length that is a multiple of 2 (the generator contract).
// Values are packed into DataPackets (v1, v2) with correct row/col tagging.
static std::vector<FilteredPacket> runFilter(
        const SystemConfig&       cfg,
        const std::vector<uint8_t>& raw,
        BoundaryPolicy            policy = BoundaryPolicy::ZERO_PAD)
{
    // --- Build input queue ---------------------------------------------------
    SimpleQueue<DataPacket>     in_q;
    SimpleQueue<FilteredPacket> out_q;

    const size_t cols = cfg.columns;
    uint64_t row = 0, col = 0;

    for (size_t i = 0; i + 1 < raw.size(); i += 2) {
        DataPacket dp{};
        dp.v1  = raw[i];
        dp.v2  = raw[i + 1];
        dp.row = row;
        dp.col = col;

        in_q.push(dp);

        col += 2;
        if (col >= cols) { col = 0; ++row; }
    }

    // --- Construct and run filter --------------------------------------------
    auto thresholder = std::make_unique<BinaryThresholder>(cfg.threshold);
    FilterBlock filter(cfg, in_q, out_q, std::move(thresholder), policy);

    // Filter runs until queue is empty and stop() is called.
    // We call stop() first so it exits after draining.
    filter.stop();
    filter.run();

    // --- Collect output ------------------------------------------------------
    std::vector<FilteredPacket> results;
    FilteredPacket fp;
    while (out_q.pop(fp)) {
        results.push_back(fp);
    }
    return results;
}

// Manually compute the expected filtered value for element at position `idx`
// in a flat array `data`, treating it as rows of width `cols`, using the
// given kernel and boundary policy.
static float manualFilter(const std::vector<uint8_t>& data,
                          size_t                      cols,
                          size_t                      idx,
                          const std::vector<float>&   kernel,
                          BoundaryPolicy              policy)
{
    const int half = static_cast<int>(kernel.size() / 2);
    const size_t row_start = (idx / cols) * cols;
    const size_t row_end   = row_start + cols;  // one past last

    // Determine left edge and right edge values for REPLICATE
    const uint8_t left_edge  = data[row_start];
    const uint8_t right_edge = data[row_end - 1];

    float sum = 0.0f;
    for (int k = -half; k <= half; ++k) {
        const int64_t pos = static_cast<int64_t>(idx) + k;

        uint8_t val;
        if (pos < static_cast<int64_t>(row_start)) {
            // Left boundary
            val = (policy == BoundaryPolicy::REPLICATE) ? left_edge : 0;
        } else if (pos >= static_cast<int64_t>(row_end)) {
            // Right boundary
            val = (policy == BoundaryPolicy::REPLICATE) ? right_edge : 0;
        } else {
            val = data[static_cast<size_t>(pos)];
        }

        sum += static_cast<float>(val) * kernel[static_cast<size_t>(k + half)];
    }
    return sum;
}

// Write a temporary CSV and return its path.
static std::string writeTempCSV(const std::string& content,
                                const std::string& filename = "cynlr_filter_test.csv")
{
    const auto path = std::filesystem::temp_directory_path() / filename;
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot create temp CSV: " + path.string());
    f << content;
    return path.string();
}


// ============================================================================
// Section 1 — SlidingWindow
// ============================================================================

TEST(window_zero_capacity_throws) {
    ASSERT_THROWS(SlidingWindow(0), std::invalid_argument);
}

TEST(window_not_full_until_capacity_reached) {
    SlidingWindow w(5);
    for (size_t i = 0; i < 4; ++i) {
        w.push(static_cast<uint8_t>(i), 0, i);
        ASSERT_FALSE(w.is_full());
    }
    w.push(99, 0, 4);
    ASSERT_TRUE(w.is_full());
}

TEST(window_centre_is_correct_index) {
    // capacity=9, centre = index 4 (the 5th element)
    SlidingWindow w(9);
    for (size_t i = 0; i < 9; ++i) {
        w.push(static_cast<uint8_t>(i * 10), 0, i);
    }
    // Values pushed: 0, 10, 20, 30, 40, 50, 60, 70, 80
    // Centre (logical index 4) should be 40.
    ASSERT_EQ(w.centre().value, uint8_t(40));
    ASSERT_EQ(w.centre().col,   uint64_t(4));
}

TEST(window_ring_buffer_wraps_correctly) {
    SlidingWindow w(5);
    // Fill initially
    for (uint8_t i = 0; i < 5; ++i) w.push(i, 0, i);
    ASSERT_EQ(w.centre().value, uint8_t(2));  // centre of {0,1,2,3,4}

    // Push one more — window becomes {1,2,3,4,5}
    w.push(5, 0, 5);
    ASSERT_EQ(w.centre().value, uint8_t(3));  // centre of {1,2,3,4,5}

    // Push another — {2,3,4,5,6}
    w.push(6, 0, 6);
    ASSERT_EQ(w.centre().value, uint8_t(4));
}

TEST(window_reset_clears_state) {
    SlidingWindow w(5);
    for (uint8_t i = 0; i < 5; ++i) w.push(i, 0, i);
    ASSERT_TRUE(w.is_full());

    w.reset();
    ASSERT_FALSE(w.is_full());
    ASSERT_EQ(w.filled(), size_t(0));
}

TEST(window_at_returns_logical_order) {
    // After filling, at(0) should be the oldest, at(N-1) the newest.
    SlidingWindow w(5);
    for (uint8_t i = 10; i < 15; ++i) w.push(i, 0, i - 10);

    // Logical order: 10, 11, 12, 13, 14
    for (size_t i = 0; i < 5; ++i) {
        ASSERT_EQ(w.at(i).value, static_cast<uint8_t>(10 + i));
    }
}

TEST(window_at_after_multiple_wraps) {
    // Push 20 values into a capacity-5 window.  Final state: {15,16,17,18,19}
    SlidingWindow w(5);
    for (uint8_t i = 0; i < 20; ++i) w.push(i, 0, i);

    ASSERT_EQ(w.at(0).value, uint8_t(15));
    ASSERT_EQ(w.at(4).value, uint8_t(19));
    ASSERT_EQ(w.centre().value, uint8_t(17));
}


// ============================================================================
// Section 2 — BinaryThresholder
// ============================================================================

TEST(thresholder_at_boundary) {
    BinaryThresholder t(128);
    ASSERT_EQ(t.apply(128.0f), uint8_t(1));   // >= TV → 1
    ASSERT_EQ(t.apply(127.9f), uint8_t(0));   // <  TV → 0
}

TEST(thresholder_zero_threshold_all_ones) {
    BinaryThresholder t(0);
    ASSERT_EQ(t.apply(0.0f),   uint8_t(1));
    ASSERT_EQ(t.apply(255.0f), uint8_t(1));
}

TEST(thresholder_max_threshold) {
    BinaryThresholder t(255);
    ASSERT_EQ(t.apply(254.9f), uint8_t(0));
    ASSERT_EQ(t.apply(255.0f), uint8_t(1));
    ASSERT_EQ(t.apply(256.0f), uint8_t(1));
}

TEST(thresholder_set_threshold_updates_behaviour) {
    BinaryThresholder t(100);
    ASSERT_EQ(t.apply(99.0f), uint8_t(0));
    ASSERT_EQ(t.apply(100.0f), uint8_t(1));

    t.set_threshold(50);
    ASSERT_EQ(t.apply(50.0f), uint8_t(1));
    ASSERT_EQ(t.apply(49.0f), uint8_t(0));
}

TEST(thresholder_negative_filtered_value) {
    // Possible if kernel has negative taps and input is zero-padded.
    BinaryThresholder t(0);
    ASSERT_EQ(t.apply(-1.0f), uint8_t(0));
}


// ============================================================================
// Section 3 — FilterBlock Construction
// ============================================================================

TEST(filter_empty_kernel_throws) {
    SystemConfig cfg = filterConfig(4, 128, {});
    SimpleQueue<DataPacket>     in_q;
    SimpleQueue<FilteredPacket> out_q;

    ASSERT_THROWS(
        FilterBlock(cfg, in_q, out_q,
                    std::make_unique<BinaryThresholder>(static_cast<uint8_t>(128))),
        std::invalid_argument
    );
}

TEST(filter_even_kernel_size_throws) {
    SystemConfig cfg = filterConfig(4, 128, {1.f, 1.f, 1.f, 1.f});
    SimpleQueue<DataPacket>     in_q;
    SimpleQueue<FilteredPacket> out_q;

    ASSERT_THROWS(
        FilterBlock(cfg, in_q, out_q,
                    std::make_unique<BinaryThresholder>(static_cast<uint8_t>(128))),
        std::invalid_argument
    );
}

TEST(filter_null_thresholder_throws) {
    SystemConfig cfg = filterConfig(4, 128, identityKernel());
    SimpleQueue<DataPacket>     in_q;
    SimpleQueue<FilteredPacket> out_q;

    ASSERT_THROWS(
        FilterBlock(cfg, in_q, out_q, nullptr),
        std::invalid_argument
    );
}


// ============================================================================
// Section 4 — Identity Kernel Tests (filtered value == raw value)
//             These isolate the threshold logic from convolution math.
// ============================================================================

TEST(filter_identity_kernel_all_above_threshold) {
    // All values = 200, threshold = 100.  Every output pixel should be 1.
    auto cfg = filterConfig(4, 100, identityKernel());
    std::vector<uint8_t> raw(8, 200);  // 2 rows × 4 cols

    auto results = runFilter(cfg, raw);

    // 8 pixels → 4 output packets (each has b1, b2)
    ASSERT_EQ(results.size(), size_t(4));
    for (const auto& fp : results) {
        ASSERT_EQ(fp.b1, uint8_t(1));
        ASSERT_EQ(fp.b2, uint8_t(1));
    }
}

TEST(filter_identity_kernel_all_below_threshold) {
    // All values = 50, threshold = 100.  Every output pixel should be 0.
    auto cfg = filterConfig(4, 100, identityKernel());
    std::vector<uint8_t> raw(4, 50);  // 1 row × 4 cols

    auto results = runFilter(cfg, raw);

    ASSERT_EQ(results.size(), size_t(2));
    for (const auto& fp : results) {
        ASSERT_EQ(fp.b1, uint8_t(0));
        ASSERT_EQ(fp.b2, uint8_t(0));
    }
}

TEST(filter_identity_kernel_mixed_values) {
    // Row: [50, 200, 50, 200], threshold = 100.
    // With identity kernel, filtered values == raw values.
    // Expected output: 0, 1, 0, 1
    auto cfg = filterConfig(4, 100, identityKernel());
    std::vector<uint8_t> raw = { 50, 200, 50, 200 };

    auto results = runFilter(cfg, raw);

    ASSERT_EQ(results.size(), size_t(2));
    ASSERT_EQ(results[0].b1, uint8_t(0));   // 50  < 100
    ASSERT_EQ(results[0].b2, uint8_t(1));   // 200 >= 100
    ASSERT_EQ(results[1].b1, uint8_t(0));   // 50  < 100
    ASSERT_EQ(results[1].b2, uint8_t(1));   // 200 >= 100
}


// ============================================================================
// Section 5 — Convolution Correctness (known input, manual calculation)
// ============================================================================

TEST(filter_uniform_kernel_single_row_zero_pad) {
    // Uniform kernel (all 1/9).  Row = [0, 0, 0, 0, 90, 0, 0, 0, 0, 0]
    // With zero-pad at boundaries:
    //   Centre pixel at index 4 (value 90):
    //     window = [0, 0, 0, 0, 90, 0, 0, 0, 0]
    //     filtered = 90/9 = 10.0
    //   Threshold = 5 → output = 1
    auto cfg = filterConfig(10, 5, uniformKernel(), BoundaryPolicy::ZERO_PAD);
    std::vector<uint8_t> raw = { 0, 0, 0, 0, 90, 0, 0, 0, 0, 0 };

    auto results = runFilter(cfg, raw, BoundaryPolicy::ZERO_PAD);

    // 10 pixels → 5 packets
    ASSERT_EQ(results.size(), size_t(5));

    // Find the packet covering col=4 (the pixel with value 90).
    // It should be in packet index 2 (col=4, col=5) → b1 is col 4.
    ASSERT_EQ(results[2].b1, uint8_t(1));   // 10.0 >= 5 → 1
}

TEST(filter_spec_kernel_constant_input) {
    // All values = 100, spec kernel.
    // Kernel taps sum to ~1.0, so filtered value ≈ 100 everywhere.
    // Threshold = 99 → all outputs should be 1.
    auto cfg = filterConfig(10, 99, specKernel(), BoundaryPolicy::REPLICATE);
    std::vector<uint8_t> raw(10, 100);  // single row

    auto results = runFilter(cfg, raw, BoundaryPolicy::REPLICATE);

    ASSERT_EQ(results.size(), size_t(5));
    for (const auto& fp : results) {
        ASSERT_EQ(fp.b1, uint8_t(1));
        ASSERT_EQ(fp.b2, uint8_t(1));
    }
}

TEST(filter_manual_computation_against_reference) {
    // Verify every pixel in a short row against the manual filter function.
    const size_t COLS = 10;
    std::vector<uint8_t> raw = { 10, 30, 50, 70, 90, 110, 130, 150, 170, 190 };
    auto kernel = specKernel();
    const uint8_t TV = 80;
    auto cfg = filterConfig(COLS, TV, kernel, BoundaryPolicy::ZERO_PAD);

    auto results = runFilter(cfg, raw, BoundaryPolicy::ZERO_PAD);

    // Flatten b1/b2 from results into a single vector of binary outputs.
    std::vector<uint8_t> actual_bits;
    for (const auto& fp : results) {
        actual_bits.push_back(fp.b1);
        actual_bits.push_back(fp.b2);
    }

    // Compute expected values manually.
    std::vector<uint8_t> expected_bits;
    for (size_t i = 0; i < COLS; ++i) {
        float fv = manualFilter(raw, COLS, i, kernel, BoundaryPolicy::ZERO_PAD);
        expected_bits.push_back(fv >= static_cast<float>(TV) ? 1 : 0);
    }

    // The filter may produce one extra trailing pixel (padding artifact at
    // row end when COLS is odd relative to packet pairing).  Compare up to
    // min(actual, expected).
    const size_t compare_len = std::min(actual_bits.size(), expected_bits.size());
    ASSERT_TRUE(compare_len >= COLS);  // must have at least COLS outputs

    for (size_t i = 0; i < COLS; ++i) {
        if (actual_bits[i] != expected_bits[i]) {
            std::ostringstream os;
            os << "Pixel " << i << ": expected " << int(expected_bits[i])
               << " got " << int(actual_bits[i]);
            throw TestFailure{ os.str() };
        }
    }
}


// ============================================================================
// Section 6 — Boundary Policy: REPLICATE vs ZERO_PAD
// ============================================================================

TEST(filter_zero_pad_reduces_edge_values) {
    // Row of all 200s.  With ZERO_PAD, edge pixels see zeros in the window,
    // so their filtered value is lower than interior pixels.
    // With a high threshold, edge pixels may become 0 while interior stays 1.
    const size_t COLS = 10;
    std::vector<uint8_t> raw(COLS, 200);
    auto kernel = specKernel();

    // Threshold = 150:  Interior filtered ≈ 200 → 1.
    // Edge filtered < 200 due to zero padding → may be < 150 → 0.
    auto cfg_zero = filterConfig(COLS, 150, kernel, BoundaryPolicy::ZERO_PAD);
    auto results_zero = runFilter(cfg_zero, raw, BoundaryPolicy::ZERO_PAD);

    auto cfg_rep = filterConfig(COLS, 150, kernel, BoundaryPolicy::REPLICATE);
    auto results_rep = runFilter(cfg_rep, raw, BoundaryPolicy::REPLICATE);

    // With REPLICATE, all values should be ≈200, so all 1.
    std::vector<uint8_t> rep_bits;
    for (const auto& fp : results_rep) {
        rep_bits.push_back(fp.b1);
        rep_bits.push_back(fp.b2);
    }
    for (size_t i = 0; i < COLS; ++i) {
        ASSERT_EQ(rep_bits[i], uint8_t(1));
    }

    // With ZERO_PAD, at least the first pixel should have a lower filtered
    // value.  Check that the first pixel's filtered value is below threshold.
    std::vector<uint8_t> zero_bits;
    for (const auto& fp : results_zero) {
        zero_bits.push_back(fp.b1);
        zero_bits.push_back(fp.b2);
    }
    // First pixel (index 0): 4 left neighbours are zero-padded.
    // filtered = 200 * (k4 + k5 + k6 + k7 + k8) + 0 * (k0+k1+k2+k3)
    // ≈ 200 * 0.671 ≈ 134 < 150 → should be 0
    ASSERT_EQ(zero_bits[0], uint8_t(0));
}

TEST(filter_replicate_preserves_constant_row) {
    // Constant row → REPLICATE produces identical edge and interior values.
    const size_t COLS = 10;
    std::vector<uint8_t> raw(COLS, 128);
    auto kernel = specKernel();

    auto cfg = filterConfig(COLS, 127, kernel, BoundaryPolicy::REPLICATE);
    auto results = runFilter(cfg, raw, BoundaryPolicy::REPLICATE);

    // Every pixel should be 1 (filtered ≈ 128 >= 127).
    for (const auto& fp : results) {
        ASSERT_EQ(fp.b1, uint8_t(1));
        ASSERT_EQ(fp.b2, uint8_t(1));
    }
}


// ============================================================================
// Section 7 — Row Transitions
// ============================================================================

TEST(filter_multi_row_resets_window) {
    // Two rows with very different values.
    // Row 0: all 0s → all outputs 0 (threshold > 0)
    // Row 1: all 255s → all outputs 1 (threshold < 255)
    // If the window doesn't reset between rows, row 1's edge values
    // would be contaminated by row 0's data.
    const size_t COLS = 10;
    std::vector<uint8_t> raw;
    raw.insert(raw.end(), COLS, 0);     // row 0: all zeros
    raw.insert(raw.end(), COLS, 255);   // row 1: all 255

    auto cfg = filterConfig(COLS, 1, specKernel(), BoundaryPolicy::REPLICATE);
    auto results = runFilter(cfg, raw, BoundaryPolicy::REPLICATE);

    // 20 pixels → 10 packets
    ASSERT_EQ(results.size(), size_t(10));

    // Row 0 packets (indices 0–4): all 0
    for (size_t i = 0; i < 5; ++i) {
        ASSERT_EQ(results[i].b1, uint8_t(0));
        ASSERT_EQ(results[i].b2, uint8_t(0));
        ASSERT_EQ(results[i].row, uint64_t(0));
    }

    // Row 1 packets (indices 5–9): all 1
    for (size_t i = 5; i < 10; ++i) {
        ASSERT_EQ(results[i].b1, uint8_t(1));
        ASSERT_EQ(results[i].b2, uint8_t(1));
        ASSERT_EQ(results[i].row, uint64_t(1));
    }
}

TEST(filter_row_col_coordinates_correct) {
    // Verify that output packets carry the right row/col from the centre pixel.
    const size_t COLS = 6;
    std::vector<uint8_t> raw(COLS * 2, 100);  // 2 rows × 6 cols

    auto cfg = filterConfig(COLS, 50, identityKernel(), BoundaryPolicy::ZERO_PAD);
    auto results = runFilter(cfg, raw, BoundaryPolicy::ZERO_PAD);

    // 12 pixels → 6 packets  (3 per row)
    ASSERT_EQ(results.size(), size_t(6));

    // Row 0: packets at col 0, 2, 4
    ASSERT_EQ(results[0].row, 0ULL);  ASSERT_EQ(results[0].col, 0ULL);
    ASSERT_EQ(results[1].row, 0ULL);  ASSERT_EQ(results[1].col, 2ULL);
    ASSERT_EQ(results[2].row, 0ULL);  ASSERT_EQ(results[2].col, 4ULL);

    // Row 1: packets at col 0, 2, 4
    ASSERT_EQ(results[3].row, 1ULL);  ASSERT_EQ(results[3].col, 0ULL);
    ASSERT_EQ(results[4].row, 1ULL);  ASSERT_EQ(results[4].col, 2ULL);
    ASSERT_EQ(results[5].row, 1ULL);  ASSERT_EQ(results[5].col, 4ULL);
}


// ============================================================================
// Section 8 — Edge Cases
// ============================================================================

TEST(filter_minimum_columns_m_equals_2) {
    // m=2 is the smallest valid column count.
    // Each row produces exactly 1 packet.
    auto cfg = filterConfig(2, 50, identityKernel(), BoundaryPolicy::ZERO_PAD);
    std::vector<uint8_t> raw = { 100, 200 };  // 1 row

    auto results = runFilter(cfg, raw, BoundaryPolicy::ZERO_PAD);

    ASSERT_TRUE(results.size() >= 1);
    ASSERT_EQ(results[0].b1, uint8_t(1));   // 100 >= 50
    ASSERT_EQ(results[0].b2, uint8_t(1));   // 200 >= 50
}

TEST(filter_single_row_exact_output_count) {
    // m=8, 1 row → 4 packets.
    const size_t COLS = 8;
    std::vector<uint8_t> raw(COLS, 100);
    auto cfg = filterConfig(COLS, 50, identityKernel());

    auto results = runFilter(cfg, raw);
    ASSERT_EQ(results.size(), size_t(4));
}

TEST(filter_empty_input_produces_no_output) {
    auto cfg = filterConfig(4, 50, identityKernel());
    std::vector<uint8_t> raw;  // no data

    auto results = runFilter(cfg, raw);
    ASSERT_EQ(results.size(), size_t(0));
}

TEST(filter_large_row_1000_columns) {
    // Stress test: m=1000, single row.  All 128, threshold = 127 → all 1s.
    const size_t COLS = 1000;
    std::vector<uint8_t> raw(COLS, 128);
    auto cfg = filterConfig(COLS, 127, specKernel(), BoundaryPolicy::REPLICATE);

    auto results = runFilter(cfg, raw, BoundaryPolicy::REPLICATE);

    // 1000 pixels → 500 packets
    ASSERT_EQ(results.size(), size_t(500));
    for (const auto& fp : results) {
        ASSERT_EQ(fp.b1, uint8_t(1));
        ASSERT_EQ(fp.b2, uint8_t(1));
    }
}


// ============================================================================
// Section 9 — Threaded Integration (Generator → Filter → Output)
// ============================================================================

TEST(filter_threaded_csv_end_to_end) {
    // Feed known CSV data through GeneratorBlock → FilterBlock → output queue.
    // Verify the output matches manual calculation.
    const size_t COLS = 10;
    const uint8_t TV  = 80;
    auto kernel = specKernel();

    // Single row: 10, 30, 50, 70, 90, 110, 130, 150, 170, 190
    const auto csv_path = writeTempCSV(
        "10,30,50,70,90,110,130,150,170,190\n", "cynlr_filter_e2e.csv");

    SystemConfig cfg{};
    cfg.columns         = COLS;
    cfg.cycle_time_ns   = 100'000ULL;  // 100 µs
    cfg.threshold       = TV;
    cfg.kernel          = kernel;
    cfg.mode            = Mode::CSV;
    cfg.input_file      = csv_path;
    cfg.boundary_policy = BoundaryPolicy::ZERO_PAD;

    // Queues
    SimpleQueue<DataPacket>     gen_to_filter;
    SimpleQueue<FilteredPacket> filter_out;

    // Blocks
    auto source = createDataSource(cfg);
    GeneratorBlock generator(cfg, gen_to_filter, std::move(source));

    auto thresholder = std::make_unique<BinaryThresholder>(static_cast<uint8_t>(TV));
    FilterBlock filter(cfg, gen_to_filter, filter_out,
                       std::move(thresholder), BoundaryPolicy::ZERO_PAD);

    // Run generator — CSV will exhaust and return.
    std::thread gen_t([&]{ generator.run(); });
    gen_t.join();

    // Run filter — stop first, then run to drain.
    filter.stop();
    filter.run();

    // Collect results.
    std::vector<uint8_t> actual_bits;
    FilteredPacket fp;
    while (filter_out.pop(fp)) {
        actual_bits.push_back(fp.b1);
        actual_bits.push_back(fp.b2);
    }

    // Compute expected.
    std::vector<uint8_t> raw = { 10, 30, 50, 70, 90, 110, 130, 150, 170, 190 };
    for (size_t i = 0; i < COLS; ++i) {
        float fv = manualFilter(raw, COLS, i, kernel, BoundaryPolicy::ZERO_PAD);
        uint8_t expected = (fv >= static_cast<float>(TV)) ? 1 : 0;

        if (i < actual_bits.size()) {
            if (actual_bits[i] != expected) {
                std::ostringstream os;
                os << "E2E pixel " << i << ": expected " << int(expected)
                   << " got " << int(actual_bits[i])
                   << " (filtered=" << fv << ", TV=" << int(TV) << ")";
                throw TestFailure{ os.str() };
            }
        }
    }
    ASSERT_TRUE(actual_bits.size() >= COLS);
}

TEST(filter_threaded_random_does_not_crash) {
    // Smoke test: run the full pipeline in random mode for a short duration.
    // No crash, no hang.
    const size_t COLS = 20;
    SystemConfig cfg{};
    cfg.columns       = COLS;
    cfg.cycle_time_ns = 50'000ULL;  // 50 µs
    cfg.threshold     = 128;
    cfg.kernel        = specKernel();
    cfg.mode          = Mode::RANDOM;

    SimpleQueue<DataPacket>     gen_to_filter;
    SimpleQueue<FilteredPacket> filter_out;

    auto source = createDataSource(cfg);
    GeneratorBlock generator(cfg, gen_to_filter, std::move(source));

    auto thresholder = std::make_unique<BinaryThresholder>(static_cast<uint8_t>(128));
    FilterBlock filter(cfg, gen_to_filter, filter_out,
                       std::move(thresholder), BoundaryPolicy::REPLICATE);

    std::thread gen_t([&]{ generator.run(); });
    std::thread flt_t([&]{ filter.run(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    generator.stop();
    gen_t.join();

    // Drain remaining packets, then stop filter.
    while (!gen_to_filter.empty()) {
        std::this_thread::yield();
    }
    filter.stop();
    flt_t.join();

    // Just verify we got some output.
    size_t count = 0;
    FilteredPacket fp;
    while (filter_out.pop(fp)) ++count;
    ASSERT_TRUE(count > 0);
}


// ============================================================================
// Section 10 — Kernel Symmetry Sanity Checks
// ============================================================================

TEST(filter_symmetric_input_symmetric_output) {
    // Symmetric row + symmetric kernel → mirror-symmetric binary output.
    // Row: [10, 50, 100, 200, 255, 200, 100, 50, 10, 10]
    // Note: not perfectly symmetric (last element), but the first 9 are.
    const size_t COLS = 10;
    std::vector<uint8_t> raw = { 10, 50, 100, 200, 255, 255, 200, 100, 50, 10 };

    auto cfg = filterConfig(COLS, 100, specKernel(), BoundaryPolicy::REPLICATE);
    auto results = runFilter(cfg, raw, BoundaryPolicy::REPLICATE);

    std::vector<uint8_t> bits;
    for (const auto& fp : results) {
        bits.push_back(fp.b1);
        bits.push_back(fp.b2);
    }

    // With REPLICATE and a symmetric input, output should be symmetric.
    // bits[i] should equal bits[COLS-1-i] for the first COLS entries.
    for (size_t i = 0; i < COLS / 2; ++i) {
        ASSERT_EQ(bits[i], bits[COLS - 1 - i]);
    }
}


// ============================================================================
// Entry point
// ============================================================================

int main() {
    std::cout << "======================================================\n"
              << "  CynLr FilterBlock Test Suite\n"
              << "======================================================\n\n";

    return run_all_tests();
}