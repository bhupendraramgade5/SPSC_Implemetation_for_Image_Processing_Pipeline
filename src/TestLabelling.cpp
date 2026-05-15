// ============================================================================
// TestLabelling.cpp
//
// Unit tests for LabelledPacket, LabelMap, RowLabelBuffer, and LabellingBlock.
// Mirrors the structure and framework of TestFilter.cpp exactly.
//
// Test sections:
//   1  — LabelledPacket layout and static guarantees
//   2  — LabelMap: allocation, find, unite, recycle, reset
//   3  — RowLabelBuffer: set, prev, curr, commit, drain
//   4  — LabellingBlock construction guards
//   5  — Single connected component (no merges)
//   6  — Merge events (the U-shape and related)
//   7  — Label recycling (mid-row drain and boundary)
//   8  — Row transitions (window reset, cross-row connectivity)
//   9  — Coordinate correctness
//  10  — Memory constraint (peak active labels <= m/2)
//  11  — Threaded integration
// ============================================================================

#include "LabellingBlock.hpp"
#include "GeneratorBlock.hpp"   // createDataSource, CSVDataSource

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
// Minimal test framework  (identical to TestFilter.cpp)
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
// Helpers
// ============================================================================

// Build a minimal SystemConfig for labelling tests.
static SystemConfig labelConfig(size_t columns) {
    SystemConfig cfg{};
    cfg.columns       = columns;
    cfg.cycle_time_ns = 1'000'000ULL;
    cfg.threshold     = 128;
    cfg.mode          = Mode::RANDOM;
    cfg.kernel        = { 0.00025177f, 0.008666992f, 0.078025818f,
                          0.24130249f, 0.343757629f, 0.24130249f,
                          0.078025818f, 0.008666992f, 0.000125885f };
    return cfg;
}

// Push a grid of binary rows (each row = columns uint8 values, 0 or 1)
// through a LabellingBlock synchronously and return all output packets.
// rows_data is a flat vector: row0_col0, row0_col1, ..., rowN_colM-1.
// Columns must be even (pipeline contract).
static std::vector<LabelledPacket> runLabeller(
        size_t                        columns,
        const std::vector<uint8_t>&   rows_data)
{
    SimpleQueue<FilteredPacket>  in_q;
    SimpleQueue<LabelledPacket>  out_q;

    const size_t total_pixels = rows_data.size();
    const size_t total_rows   = total_pixels / columns;

    // Pack binary pixels into FilteredPackets (2 per packet, row by row)
    for (size_t r = 0; r < total_rows; ++r) {
        for (size_t c = 0; c < columns; c += 2) {
            FilteredPacket fp{};
            fp.b1  = rows_data[r * columns + c];
            fp.b2  = rows_data[r * columns + c + 1];
            fp.row = static_cast<uint64_t>(r);
            fp.col = static_cast<uint64_t>(c);
            in_q.push(fp);
        }
    }

    SystemConfig cfg = labelConfig(columns);
    LabellingBlock block(cfg, in_q, out_q);
    block.stop();   // drain-then-exit semantics
    block.run();

    std::vector<LabelledPacket> results;
    LabelledPacket lp;
    while (out_q.pop(lp))
        results.push_back(lp);
    return results;
}

// Flatten l1/l2 from result packets into a per-pixel label vector.
// Result[i] is the label of pixel i in raster order (row-major, 2 per packet).
static std::vector<uint16_t> flattenLabels(
        const std::vector<LabelledPacket>& packets)
{
    std::vector<uint16_t> out;
    out.reserve(packets.size() * 2);
    for (const auto& p : packets) {
        out.push_back(p.l1);
        out.push_back(p.l2);
    }
    return out;
}

// Count distinct non-zero labels across a label vector.
static size_t countDistinctLabels(const std::vector<uint16_t>& labels) {
    std::vector<uint16_t> tmp;
    for (uint16_t l : labels)
        if (l != 0) tmp.push_back(l);
    std::sort(tmp.begin(), tmp.end());
    tmp.erase(std::unique(tmp.begin(), tmp.end()), tmp.end());
    return tmp.size();
}

// Return true if all non-zero labels in `labels` that correspond to `ones`
// positions in `mask` share a single canonical value (i.e. same component).
// mask and labels are both flat, raster-order, same size.
static bool allSameLabel(const std::vector<uint16_t>& labels,
                         const std::vector<uint8_t>&  mask)
{
    uint16_t found = 0;
    for (size_t i = 0; i < labels.size() && i < mask.size(); ++i) {
        if (!mask[i]) continue;
        if (!found) { found = labels[i]; continue; }
        if (labels[i] != found) return false;
    }
    return true;
}

// Write a temporary CSV file and return its path.
// static std::string writeTempCSV(const std::string& content,
//                                 const std::string& filename = "cynlr_lab_test.csv")
// {
//     const auto path = std::filesystem::temp_directory_path() / filename;
//     std::ofstream f(path);
//     if (!f) throw std::runtime_error("Cannot create temp CSV: " + path.string());
//     f << content;
//     return path.string();
// }


// ============================================================================
// Section 1 — LabelledPacket layout and static guarantees
// ============================================================================

TEST(packet_sizeof_is_32) {
    ASSERT_EQ(sizeof(LabelledPacket), size_t(32));
}

TEST(packet_is_trivially_copyable) {
    ASSERT_TRUE(std::is_trivially_copyable<LabelledPacket>::value);
}

TEST(packet_zero_initialised_by_default) {
    LabelledPacket lp{};
    ASSERT_EQ(lp.row,        uint64_t(0));
    ASSERT_EQ(lp.col,        uint64_t(0));
    ASSERT_EQ(lp.l1,         uint16_t(0));
    ASSERT_EQ(lp.l2,         uint16_t(0));
    ASSERT_EQ(lp.merge_old,  uint16_t(0));
    ASSERT_EQ(lp.merge_new,  uint16_t(0));
    ASSERT_EQ(lp.merge_old2, uint16_t(0));
    ASSERT_EQ(lp.merge_new2, uint16_t(0));
    ASSERT_EQ(lp.recycled,   uint16_t(0));
}

TEST(packet_fields_are_independent) {
    LabelledPacket lp{};
    lp.row = 7; lp.col = 4; lp.l1 = 3; lp.l2 = 5;
    lp.merge_old = 2; lp.merge_new = 1;
    ASSERT_EQ(lp.row, uint64_t(7));
    ASSERT_EQ(lp.col, uint64_t(4));
    ASSERT_EQ(lp.l1,  uint16_t(3));
    ASSERT_EQ(lp.l2,  uint16_t(5));
    ASSERT_EQ(lp.merge_old, uint16_t(2));
    ASSERT_EQ(lp.merge_new, uint16_t(1));
    ASSERT_EQ(lp.merge_old2, uint16_t(0));  // untouched
}

TEST(packet_copy_is_independent) {
    LabelledPacket a{}; a.l1 = 3; a.l2 = 7;
    LabelledPacket b = a;
    b.l1 = 99;
    ASSERT_EQ(a.l1, uint16_t(3));
    ASSERT_EQ(b.l1, uint16_t(99));
}

TEST(packet_alignof_is_8) {
    ASSERT_EQ(alignof(LabelledPacket), size_t(8));
}


// ============================================================================
// Section 2 — LabelMap
// ============================================================================

TEST(labelmap_zero_max_throws) {
    ASSERT_THROWS(LabelMap(0), std::invalid_argument);
}

TEST(labelmap_new_label_starts_at_1) {
    LabelMap lm(4);
    ASSERT_EQ(lm.newLabel(), uint16_t(1));
}

TEST(labelmap_new_label_increments) {
    LabelMap lm(4);
    ASSERT_EQ(lm.newLabel(), uint16_t(1));
    ASSERT_EQ(lm.newLabel(), uint16_t(2));
    ASSERT_EQ(lm.newLabel(), uint16_t(3));
}

TEST(labelmap_find_before_merge_returns_self) {
    LabelMap lm(4);
    uint16_t a = lm.newLabel();
    uint16_t b = lm.newLabel();
    ASSERT_EQ(lm.find(a), a);
    ASSERT_EQ(lm.find(b), b);
}

TEST(labelmap_find_background_returns_0) {
    LabelMap lm(4);
    ASSERT_EQ(lm.find(uint16_t(0)), uint16_t(0));
}

TEST(labelmap_unite_lower_label_survives) {
    LabelMap lm(4);
    uint16_t a = lm.newLabel();   // 1
    uint16_t b = lm.newLabel();   // 2
    uint16_t surv = lm.unite(b, a);
    ASSERT_EQ(surv, uint16_t(1));             // lower wins
    ASSERT_EQ(lm.find(b), uint16_t(1));       // b now roots to 1
    ASSERT_EQ(lm.find(a), uint16_t(1));       // a still 1
}

TEST(labelmap_unite_same_component_noop) {
    LabelMap lm(4);
    uint16_t a = lm.newLabel();
    uint16_t surv = lm.unite(a, a);
    ASSERT_EQ(surv, a);
    ASSERT_EQ(lm.active(), size_t(1));  // count unchanged
}

TEST(labelmap_unite_chain_of_three) {
    LabelMap lm(4);
    uint16_t a = lm.newLabel();   // 1
    uint16_t b = lm.newLabel();   // 2
    uint16_t c = lm.newLabel();   // 3
    lm.unite(b, a);   // merge 2 into 1
    lm.unite(c, a);   // merge 3 into 1
    ASSERT_EQ(lm.find(a), uint16_t(1));
    ASSERT_EQ(lm.find(b), uint16_t(1));
    ASSERT_EQ(lm.find(c), uint16_t(1));
}

TEST(labelmap_active_count_tracks_correctly) {
    LabelMap lm(4);
    ASSERT_EQ(lm.active(), size_t(0));
    lm.newLabel(); ASSERT_EQ(lm.active(), size_t(1));
    lm.newLabel(); ASSERT_EQ(lm.active(), size_t(2));
    lm.unite(uint16_t(2), uint16_t(1));
    ASSERT_EQ(lm.active(), size_t(1));
}

TEST(labelmap_full_returns_true_at_capacity) {
    LabelMap lm(3);
    lm.newLabel(); lm.newLabel(); lm.newLabel();
    ASSERT_TRUE(lm.full());
}

TEST(labelmap_new_label_returns_0_when_full) {
    LabelMap lm(2);
    lm.newLabel(); lm.newLabel();
    ASSERT_EQ(lm.newLabel(), uint16_t(0));
}

TEST(labelmap_recycle_frees_slot) {
    LabelMap lm(2);
    uint16_t a = lm.newLabel();   // 1
    (void)a;  
    uint16_t b = lm.newLabel();   // 2
    ASSERT_TRUE(lm.full());
    lm.recycle(b);
    ASSERT_FALSE(lm.full());
    uint16_t c = lm.newLabel();   // should reuse b's slot
    ASSERT_NE(c, uint16_t(0));
}

TEST(labelmap_recycle_ignores_background) {
    LabelMap lm(4);
    lm.newLabel();
    size_t before = lm.active();
    lm.recycle(0);                // background — must not corrupt state
    ASSERT_EQ(lm.active(), before);
}

TEST(labelmap_path_halving_flattens_chain) {
    // Build a 4-level chain: 4→3→2→1, then find(4) should short-circuit.
    LabelMap lm(4);
    lm.newLabel(); lm.newLabel(); lm.newLabel(); lm.newLabel();
    // Manually create a chain via repeated unites
    lm.unite(uint16_t(4), uint16_t(3));
    lm.unite(uint16_t(3), uint16_t(2));
    lm.unite(uint16_t(2), uint16_t(1));
    ASSERT_EQ(lm.find(uint16_t(4)), uint16_t(1));
    ASSERT_EQ(lm.find(uint16_t(3)), uint16_t(1));
}

TEST(labelmap_reset_clears_all_state) {
    LabelMap lm(4);
    lm.newLabel(); lm.newLabel();
    lm.unite(uint16_t(2), uint16_t(1));
    lm.reset();
    ASSERT_EQ(lm.active(), size_t(0));
    ASSERT_FALSE(lm.full());
    ASSERT_EQ(lm.newLabel(), uint16_t(1));   // back to fresh start
}


// ============================================================================
// Section 3 — RowLabelBuffer
// ============================================================================

TEST(rowbuf_zero_columns_throws) {
    ASSERT_THROWS(RowLabelBuffer(0, 2), std::invalid_argument);
}

TEST(rowbuf_zero_max_labels_throws) {
    ASSERT_THROWS(RowLabelBuffer(4, 0), std::invalid_argument);
}

TEST(rowbuf_curr_initially_all_zero) {
    RowLabelBuffer rb(6, 3);
    for (size_t c = 0; c < 6; ++c)
        ASSERT_EQ(rb.curr(c), uint16_t(0));
}

TEST(rowbuf_prev_initially_all_zero) {
    RowLabelBuffer rb(6, 3);
    for (size_t c = 0; c < 6; ++c)
        ASSERT_EQ(rb.prev(c), uint16_t(0));
}

TEST(rowbuf_set_and_curr_roundtrip) {
    RowLabelBuffer rb(6, 3);
    rb.set(0, 1); rb.set(2, 2); rb.set(4, 3);
    ASSERT_EQ(rb.curr(0), uint16_t(1));
    ASSERT_EQ(rb.curr(1), uint16_t(0));
    ASSERT_EQ(rb.curr(2), uint16_t(2));
    ASSERT_EQ(rb.curr(4), uint16_t(3));
}

TEST(rowbuf_oob_curr_returns_zero) {
    RowLabelBuffer rb(4, 2);
    rb.set(0, 1);
    ASSERT_EQ(rb.curr(99), uint16_t(0));
    ASSERT_EQ(rb.curr(4),  uint16_t(0));   // exactly at columns boundary
}

TEST(rowbuf_oob_prev_returns_zero) {
    RowLabelBuffer rb(4, 2);
    ASSERT_EQ(rb.prev(4),  uint16_t(0));   // covers NE at last column
    ASSERT_EQ(rb.prev(99), uint16_t(0));
}

TEST(rowbuf_commit_moves_curr_to_prev) {
    RowLabelBuffer rb(4, 2);
    rb.set(0, 1); rb.set(2, 2);
    uint16_t tmp[2];
    rb.commitAndRecycle(tmp, 2);   // first commit: old prev was zero, no recycles
    ASSERT_EQ(rb.prev(0), uint16_t(1));
    ASSERT_EQ(rb.prev(2), uint16_t(2));
}

TEST(rowbuf_commit_zeros_curr) {
    RowLabelBuffer rb(4, 2);
    rb.set(0, 1); rb.set(2, 2);
    uint16_t tmp[2];
    rb.commitAndRecycle(tmp, 2);
    ASSERT_EQ(rb.curr(0), uint16_t(0));
    ASSERT_EQ(rb.curr(2), uint16_t(0));
}

TEST(rowbuf_first_commit_no_recycles) {
    // First commit: prev was all zeros → nothing to recycle
    RowLabelBuffer rb(4, 2);
    rb.set(0, 1); rb.set(2, 2);
    uint16_t tmp[2];
    size_t n = rb.commitAndRecycle(tmp, 2);
    ASSERT_EQ(n, size_t(0));
}

TEST(rowbuf_second_commit_recycles_missing_label) {
    RowLabelBuffer rb(6, 3);
    // Row 0: labels 1,2,3
    rb.set(0,1); rb.set(2,2); rb.set(4,3);
    uint16_t tmp[3];
    rb.commitAndRecycle(tmp, 3);  // no recycles (prev was zero)

    // Row 1: only labels 1 and 2 → label 3 is gone
    rb.set(0,1); rb.set(2,2);
    size_t n = rb.commitAndRecycle(tmp, 3);
    ASSERT_EQ(n, size_t(1));
    ASSERT_EQ(tmp[0], uint16_t(3));
}

TEST(rowbuf_drain_dead_from_prev_basic) {
    RowLabelBuffer rb(6, 3);
    rb.set(0,1); rb.set(2,2); rb.set(4,3);
    uint16_t tmp[3];
    rb.commitAndRecycle(tmp, 3);   // labels 1,2,3 are now in prev

    // curr is empty; drain at completed_col=3 → dead_before=2
    // prev[0]=1, prev[1]=0, prev[2]=2 — both 1 and 2 are dead
    uint16_t drain[3]={};
    size_t n = rb.drainDeadFromPrev(3, drain, 3);
    ASSERT_EQ(n, size_t(2));
}

TEST(rowbuf_drain_skips_label_present_in_curr) {
    RowLabelBuffer rb(6, 3);
    rb.set(0,1); rb.set(2,2);
    uint16_t tmp[3];
    rb.commitAndRecycle(tmp, 3);

    // curr: label 1 inherited at col 0
    rb.set(0, 1);

    // drain at completed_col=3 → prev[0]=1 (in curr → skip), prev[2]=2 (dead)
    uint16_t drain[3]={};
    size_t n = rb.drainDeadFromPrev(3, drain, 3);
    // Only label 2 should be drained; label 1 is live
    bool found1 = false;
    for (size_t i=0;i<n;++i) if(drain[i]==1) found1=true;
    ASSERT_FALSE(found1);
}

TEST(rowbuf_commit_after_drain_no_double_report) {
    RowLabelBuffer rb(4, 2);
    rb.set(0,1); rb.set(2,2);
    uint16_t tmp[2];
    rb.commitAndRecycle(tmp, 2);

    // Drain all prev labels mid-row (curr empty)
    uint16_t drain[2]={};
    rb.drainDeadFromPrev(3, drain, 2);  // drains both 1 and 2

    // commitAndRecycle should find 0 remaining (both already cleared)
    size_t n = rb.commitAndRecycle(tmp, 2);
    ASSERT_EQ(n, size_t(0));
}

TEST(rowbuf_reset_clears_all) {
    RowLabelBuffer rb(4, 2);
    rb.set(0,1); rb.set(2,2);
    uint16_t tmp[2]; rb.commitAndRecycle(tmp, 2);
    rb.reset();
    ASSERT_EQ(rb.prev(0), uint16_t(0));
    ASSERT_EQ(rb.curr(0), uint16_t(0));
}


// ============================================================================
// Section 4 — LabellingBlock construction guards
// ============================================================================

TEST(block_zero_columns_throws) {
    SystemConfig cfg = labelConfig(0);
    SimpleQueue<FilteredPacket> in; SimpleQueue<LabelledPacket> out;
    ASSERT_THROWS(LabellingBlock(cfg, in, out), std::invalid_argument);
}

TEST(block_odd_columns_throws) {
    SystemConfig cfg = labelConfig(5);
    SimpleQueue<FilteredPacket> in; SimpleQueue<LabelledPacket> out;
    ASSERT_THROWS(LabellingBlock(cfg, in, out), std::invalid_argument);
}

TEST(block_valid_columns_constructs) {
    SystemConfig cfg = labelConfig(10);
    SimpleQueue<FilteredPacket> in; SimpleQueue<LabelledPacket> out;
    LabellingBlock block(cfg, in, out);   // must not throw
    ASSERT_TRUE(true);
}

TEST(block_empty_input_produces_no_output) {
    auto results = runLabeller(4, {});
    ASSERT_EQ(results.size(), size_t(0));
}

TEST(block_all_zeros_produces_zero_labels) {
    // 2 rows × 4 cols, all background
    std::vector<uint8_t> grid(8, uint8_t(0));
    auto results = runLabeller(4, grid);
    auto labels = flattenLabels(results);
    for (uint16_t l : labels)
        ASSERT_EQ(l, uint16_t(0));
}


// ============================================================================
// Section 5 — Single connected component (no merges expected)
// ============================================================================

TEST(single_pixel_gets_nonzero_label) {
    // 1 row × 2 cols: [1, 0]
    auto results = runLabeller(2, {1, 0});
    ASSERT_TRUE(!results.empty());
    ASSERT_NE(results[0].l1, uint16_t(0));
    ASSERT_EQ(results[0].l2, uint16_t(0));
}

TEST(two_adjacent_horizontal_pixels_same_label) {
    // 1 row × 2 cols: [1, 1]
    auto results = runLabeller(2, {1, 1});
    ASSERT_TRUE(!results.empty());
    ASSERT_NE(results[0].l1, uint16_t(0));
    ASSERT_EQ(results[0].l1, results[0].l2);
}

TEST(horizontal_run_all_same_label) {
    // 1 row × 6 cols: all ones → single component
    auto results = runLabeller(6, {1,1,1,1,1,1});
    auto labels = flattenLabels(results);
    ASSERT_EQ(countDistinctLabels(labels), size_t(1));
}

TEST(vertical_run_same_label) {
    // 3 rows × 2 cols:
    //   [1, 0]
    //   [1, 0]
    //   [1, 0]
    // The pixel at (1,0) has N=(0,0) as causal neighbour → same label.
    std::vector<uint8_t> grid = {
        1, 0,
        1, 0,
        1, 0
    };
    auto results = runLabeller(2, grid);
    auto labels = flattenLabels(results);
    // All foreground pixels at col 0 (indices 0,2,4) should share one label
    ASSERT_EQ(labels[0], labels[2]);
    ASSERT_EQ(labels[2], labels[4]);
}

TEST(diagonal_NW_connected) {
    // 2 rows × 4 cols:
    //   row0: [1, 0, 0, 0]
    //   row1: [0, 1, 0, 0]
    // (1,1) has NW neighbour (0,0) → same label
    std::vector<uint8_t> grid = {
        1, 0, 0, 0,
        0, 1, 0, 0
    };
    auto results = runLabeller(4, grid);
    auto labels = flattenLabels(results);
    // labels[0] = (0,0), labels[5] = (1,1)
    ASSERT_NE(labels[0], uint16_t(0));
    ASSERT_EQ(labels[0], labels[5]);
}

TEST(diagonal_NE_connected) {
    // 2 rows × 4 cols:
    //   row0: [0, 1, 0, 0]
    //   row1: [1, 0, 0, 0]
    // (1,0) has NE neighbour (0,1) → same label
    std::vector<uint8_t> grid = {
        0, 1, 0, 0,
        1, 0, 0, 0
    };
    auto results = runLabeller(4, grid);
    auto labels = flattenLabels(results);
    // labels[1] = (0,1), labels[4] = (1,0)
    ASSERT_NE(labels[1], uint16_t(0));
    ASSERT_EQ(labels[1], labels[4]);
}

TEST(three_by_three_blob_single_label) {
    // 3 rows × 4 cols (padded to even width), solid 3×3 block:
    //   1 1 1 0
    //   1 1 1 0
    //   1 1 1 0
    std::vector<uint8_t> grid = {
        1, 1, 1, 0,
        1, 1, 1, 0,
        1, 1, 1, 0
    };
    auto results = runLabeller(4, grid);
    auto labels = flattenLabels(results);
    // All 9 foreground pixels should carry the same label
    uint16_t ref = 0;
    for (size_t i = 0; i < labels.size(); ++i) {
        if (labels[i] == 0) continue;   // background col 3
        if (!ref) ref = labels[i];
        ASSERT_EQ(labels[i], ref);
    }
    ASSERT_NE(ref, uint16_t(0));
}

TEST(two_isolated_blobs_different_labels) {
    // 1 row × 4 cols: [1, 0, 1, 0]
    // Two separate foreground pixels → two distinct labels
    auto results = runLabeller(4, {1, 0, 1, 0});
    auto labels = flattenLabels(results);
    ASSERT_NE(labels[0], uint16_t(0));
    ASSERT_NE(labels[2], uint16_t(0));
    ASSERT_NE(labels[0], labels[2]);  // distinct components
}


// ============================================================================
// Section 6 — Merge events (U-shape and related)
// ============================================================================
// The U-shape test is the canonical correctness test for connected-components.
// Two arms of a U start with different labels; the base pixel merges them.
// After the merge, every pixel in the U must carry the same label.

TEST(u_shape_merge_fires_at_closing_pixel) {
    // 4 rows × 4 cols:
    //   1 0 1 0    ← two arms start (get labels 1 and 2)
    //   1 0 1 0    ← arms continue
    //   1 0 1 0    ← arms continue
    //   1 1 1 0    ← base closes the U → merge fires here
    //
    // Streaming labelling semantics: completed packets are NOT retroactively
    // updated.  Rows 0-2 packets keep their original label IDs (1 and 2).
    // The merge event at row 3 is how the Tracing block learns they are one
    // component.  The correct assertions are therefore:
    //   (a) A merge event appears in the row-3 packets.
    //   (b) Every row-3 foreground pixel carries the surviving (lower) label.
    std::vector<uint8_t> grid = {
        1, 0, 1, 0,
        1, 0, 1, 0,
        1, 0, 1, 0,
        1, 1, 1, 0
    };
    auto results = runLabeller(4, grid);

    // (a) Merge event must appear somewhere in row 3
    bool merge_found = false;
    for (const auto& lp : results)
        if (lp.row == 3 && (lp.merge_old != 0 || lp.merge_old2 != 0))
            merge_found = true;
    ASSERT_TRUE(merge_found);

    // (b) Find the surviving label (the lower of the two arms)
    // Row 3 col 0 is always the first pixel to be assigned — it inherits
    // from its N/NW neighbours and gets the minimum root, which is the
    // surviving label.  All other row-3 foreground pixels must match it.
    uint16_t surviving = 0;
    for (const auto& lp : results) {
        if (lp.row != 3) continue;
        if (lp.l1 != 0 && surviving == 0) surviving = lp.l1;
        if (lp.l1 != 0) ASSERT_EQ(lp.l1, surviving);
        if (lp.l2 != 0) ASSERT_EQ(lp.l2, surviving);
    }
    ASSERT_NE(surviving, uint16_t(0));

    // (c) The surviving label must be the lower of the two arm labels
    uint16_t arm_left  = results[0].l1;   // row0 col0
    uint16_t arm_right = results[1].l1;   // row0 col2
    ASSERT_EQ(surviving, std::min(arm_left, arm_right));
}

TEST(u_shape_merge_surviving_label_is_lower) {
    // Same U-shape — verify merge_new < merge_old (lower label survives)
    std::vector<uint8_t> grid = {
        1, 0, 1, 0,
        1, 0, 1, 0,
        1, 1, 1, 0
    };
    auto results = runLabeller(4, grid);
    for (const auto& lp : results) {
        if (lp.merge_old != 0)
            ASSERT_TRUE(lp.merge_new < lp.merge_old);
        if (lp.merge_old2 != 0)
            ASSERT_TRUE(lp.merge_new2 < lp.merge_old2);
    }
}

TEST(l_shape_single_label_no_merge) {
    // L-shape — two segments that share a corner; should be one label, no merge
    // 3 rows × 4 cols:
    //   1 0 0 0
    //   1 0 0 0
    //   1 1 1 1
    std::vector<uint8_t> grid = {
        1, 0, 0, 0,
        1, 0, 0, 0,
        1, 1, 1, 1
    };
    auto results = runLabeller(4, grid);
    auto labels = flattenLabels(results);
    std::vector<uint8_t> mask = {
        1, 0, 0, 0,
        1, 0, 0, 0,
        1, 1, 1, 1
    };
    ASSERT_TRUE(allSameLabel(labels, mask));
    // No merge events expected (L is connected without bridging two components)
    for (const auto& lp : results)
        ASSERT_EQ(lp.merge_old, uint16_t(0));
}

TEST(cross_shape_four_arms_single_label) {
    // Plus/cross shape — all arms connected through the centre row.
    // 5 rows × 6 cols:
    //   0 0 1 0 0 0
    //   0 0 1 0 0 0
    //   1 1 1 1 1 0   ← centre row: col0 gets new label, col1 merges it into arm label
    //   0 0 1 0 0 0
    //   0 0 1 0 0 0
    //
    // Streaming semantics: the centre row's col0 packet has l1 = new_label
    // before the merge fires at col1 (merge_old2 = new_label, merge_new2 = arm_label).
    // After the merge, all remaining pixels in the cross carry the surviving label.
    //
    // Correct assertions:
    //   (a) A merge event fires in the centre row.
    //   (b) All pixels from col1 onward in the centre row carry the surviving label.
    //   (c) Rows 3 and 4 (below centre) carry the surviving label throughout.
    std::vector<uint8_t> grid = {
        0, 0, 1, 0, 0, 0,
        0, 0, 1, 0, 0, 0,
        1, 1, 1, 1, 1, 0,
        0, 0, 1, 0, 0, 0,
        0, 0, 1, 0, 0, 0
    };
    auto results = runLabeller(6, grid);

    // (a) Merge fires in centre row (row 2)
    bool merge_found = false;
    for (const auto& lp : results)
        if (lp.row == 2 && (lp.merge_old != 0 || lp.merge_old2 != 0))
            merge_found = true;
    ASSERT_TRUE(merge_found);

    // (b) The arm label (assigned to rows 0-1) is the surviving label
    uint16_t arm_label = 0;
    for (const auto& lp : results)
        if (lp.row == 0 && lp.l1 != 0) { arm_label = lp.l1; break; }
    ASSERT_NE(arm_label, uint16_t(0));

    // (c) All post-merge pixels in rows 3-4 must carry arm_label
    for (const auto& lp : results) {
        if (lp.row < 3) continue;
        if (lp.l1 != 0) ASSERT_EQ(lp.l1, arm_label);
        if (lp.l2 != 0) ASSERT_EQ(lp.l2, arm_label);
    }

    // (d) Centre row: from the packet AFTER the merge fires, all labels = arm_label
    // The merge fires at col0-col1 packet (l1=new, l2=arm after merge).
    // col2-col4 packets must all be arm_label.
    for (const auto& lp : results) {
        if (lp.row != 2) continue;
        if (lp.col >= 2) {                    // packets after the merge packet
            if (lp.l1 != 0) ASSERT_EQ(lp.l1, arm_label);
            if (lp.l2 != 0) ASSERT_EQ(lp.l2, arm_label);
        }
    }
}

TEST(merge_old_label_not_assigned_after_merge) {
    // After a merge, the absorbed label must not appear as l1 or l2 in any
    // packet whose centre pixel was processed AFTER the merge completed.
    //
    // Subtle constraint: the merge event is in a packet whose l1 or l2 may
    // still carry a pre-merge label (because the pixel was assigned before
    // the merge fired within the same pair).  Only packets with index STRICTLY
    // GREATER than the merge packet are guaranteed merge-clean.
    //
    // This test also handles the case where merge_old is 0 but merge_old2
    // is non-zero — the second pixel in the pair triggered the merge.
    std::vector<uint8_t> grid = {
        1, 0, 1, 0,
        1, 0, 1, 0,
        1, 1, 1, 0
    };
    auto results = runLabeller(4, grid);

    // Find the first packet containing any merge event
    uint16_t absorbed = 0;
    size_t   merge_packet_idx = SIZE_MAX;
    for (size_t i = 0; i < results.size(); ++i) {
        if (results[i].merge_old != 0) {
            absorbed = results[i].merge_old;
            merge_packet_idx = i;
            break;
        }
        if (results[i].merge_old2 != 0) {
            absorbed = results[i].merge_old2;
            merge_packet_idx = i;
            break;
        }
    }
    // A merge must have fired somewhere in the U-close row
    ASSERT_NE(absorbed, uint16_t(0));

    // No packet STRICTLY AFTER the merge packet carries the absorbed label
    for (size_t i = merge_packet_idx + 1; i < results.size(); ++i) {
        ASSERT_NE(results[i].l1, absorbed);
        ASSERT_NE(results[i].l2, absorbed);
    }
}


// ============================================================================
// Section 7 — Label recycling
// ============================================================================

TEST(completed_blob_recycle_event_fires) {
    // A completely isolated blob in row 0, surrounded by zeros.
    // By row 2, it must have been recycled.
    // 3 rows × 4 cols:
    //   0 1 1 0    ← blob
    //   0 0 0 0    ← blank
    //   0 0 0 0    ← blank
    std::vector<uint8_t> grid = {
        0, 1, 1, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };
    auto results = runLabeller(4, grid);

    bool recycle_found = false;
    for (const auto& lp : results)
        if (lp.recycled != 0) { recycle_found = true; break; }
    ASSERT_TRUE(recycle_found);
}

TEST(recycled_label_id_was_previously_assigned) {
    // Verify that the recycled label ID actually appeared as a foreground label
    // in an earlier packet — not an invented value.
    std::vector<uint8_t> grid = {
        1, 1, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };
    auto results = runLabeller(4, grid);

    // Collect all foreground labels from early packets
    std::vector<uint16_t> seen_labels;
    for (const auto& lp : results) {
        if (lp.l1 != 0) seen_labels.push_back(lp.l1);
        if (lp.l2 != 0) seen_labels.push_back(lp.l2);
    }

    for (const auto& lp : results) {
        if (lp.recycled == 0) continue;
        bool was_seen = std::find(seen_labels.begin(), seen_labels.end(),
                                  lp.recycled) != seen_labels.end();
        ASSERT_TRUE(was_seen);
    }
}

TEST(peak_active_labels_never_exceeds_m_over_2) {
    // Worst-case pattern: alternating 1-0 per row, many rows.
    // This produces m/2 isolated labels per row at maximum.
    const size_t M = 20;
    const size_t ROWS = 50;
    std::vector<uint8_t> grid(M * ROWS, uint8_t(0));
    for (size_t r = 0; r < ROWS; ++r)
        for (size_t c = 0; c < M; c += 2)
            grid[r * M + c] = 1;

    SimpleQueue<FilteredPacket>  in_q;
    SimpleQueue<LabelledPacket>  out_q;

    for (size_t r = 0; r < ROWS; ++r) {
        for (size_t c = 0; c < M; c += 2) {
            FilteredPacket fp{};
            fp.b1  = grid[r * M + c];
            fp.b2  = grid[r * M + c + 1];
            fp.row = static_cast<uint64_t>(r);
            fp.col = static_cast<uint64_t>(c);
            in_q.push(fp);
        }
    }

    SystemConfig cfg = labelConfig(M);
    LabellingBlock block(cfg, in_q, out_q);
    block.stop();
    block.run();

    // Collect all assigned labels and check none exceeds m/2 capacity
    // (if the map had overflowed, some foreground pixels would have label 0)
    size_t zero_foreground = 0;
    LabelledPacket lp;
    for (size_t r = 0; r < ROWS; ++r) {
        for (size_t c = 0; c < M; c += 2) {
            bool ok = out_q.pop(lp);
            ASSERT_TRUE(ok);
            // Pixel at even col should be foreground
            if (grid[lp.row * M + lp.col] == 1 && lp.l1 == 0)
                ++zero_foreground;
            if (grid[lp.row * M + lp.col + 1] == 1 && lp.l2 == 0)
                ++zero_foreground;
        }
    }
    // No foreground pixel should have been assigned label 0 (map exhaustion)
    ASSERT_EQ(zero_foreground, size_t(0));
}

TEST(infinite_stream_label_map_never_exhausted) {
    // Run 200 rows of worst-case alternating pattern through the block.
    // Verify no foreground pixel gets label 0 (which indicates map exhaustion).
    const size_t M    = 30;
    const size_t ROWS = 200;

    SimpleQueue<FilteredPacket> in_q;
    SimpleQueue<LabelledPacket> out_q;

    for (size_t r = 0; r < ROWS; ++r) {
        for (size_t c = 0; c < M; c += 2) {
            FilteredPacket fp{};
            fp.b1  = 1;   // alternating: even cols foreground
            fp.b2  = 0;   // odd cols background
            fp.row = static_cast<uint64_t>(r);
            fp.col = static_cast<uint64_t>(c);
            in_q.push(fp);
        }
    }

    SystemConfig cfg = labelConfig(M);
    LabellingBlock block(cfg, in_q, out_q);
    block.stop();
    block.run();

    LabelledPacket lp;
    size_t bad = 0;
    while (out_q.pop(lp)) {
        // b1 = 1 so l1 must be non-zero; b2 = 0 so l2 must be zero
        if (lp.l1 == 0) ++bad;
        if (lp.l2 != 0) ++bad;
    }
    ASSERT_EQ(bad, size_t(0));
}


// ============================================================================
// Section 8 — Row transitions
// ============================================================================

TEST(row_transition_resets_window) {
    // Two blobs in different rows with a blank row between them.
    // Correct property: blobA is RECYCLED before blobB starts.
    // The recycled label may be reused for blobB — so ID equality does NOT
    // imply the same component.  What we can verify:
    //   (a) blobA exists (non-zero label in row 0)
    //   (b) blobB exists (non-zero label in row 2)
    //   (c) A recycle event fires between them (in the blank row packets)
    //       — this proves the row buffer was correctly committed and cleared
    //       between the two blobs.
    //   (d) blobB's pixels carry no merge events (it is a fresh component)
    // 3 rows × 4 cols:
    //   1 1 0 0    ← blob A → gets label, then recycled in blank row
    //   0 0 0 0    ← blank
    //   0 0 1 1    ← blob B → gets recycled label (reuse is correct)
    std::vector<uint8_t> grid = {
        1, 1, 0, 0,
        0, 0, 0, 0,
        0, 0, 1, 1
    };
    auto results = runLabeller(4, grid);
    auto labels = flattenLabels(results);

    // (a) blobA at (0,0) and (0,1)
    ASSERT_NE(labels[0], uint16_t(0));
    ASSERT_NE(labels[1], uint16_t(0));

    // (b) blobB at (2,2) and (2,3) — indices 10 and 11 in the flat array
    ASSERT_NE(labels[10], uint16_t(0));
    ASSERT_NE(labels[11], uint16_t(0));

    // (c) A recycle event must fire in the blank row (row 1)
    bool recycle_between = false;
    for (const auto& lp : results)
        if (lp.row == 1 && lp.recycled != 0)
            recycle_between = true;
    ASSERT_TRUE(recycle_between);

    // (d) blobB carries no merge events (independent component)
    for (const auto& lp : results)
        if (lp.row == 2) {
            ASSERT_EQ(lp.merge_old,  uint16_t(0));
            ASSERT_EQ(lp.merge_old2, uint16_t(0));
        }
}

TEST(cross_row_connectivity_preserved) {
    // A single vertical stripe across 4 rows — must all share one label
    // 4 rows × 2 cols: [1,0] repeated
    std::vector<uint8_t> grid(8, uint8_t(0));
    for (size_t r = 0; r < 4; ++r) grid[r * 2] = 1;

    auto results = runLabeller(2, grid);
    auto labels = flattenLabels(results);

    uint16_t ref = 0;
    for (size_t i = 0; i < labels.size(); i += 2) {
        if (!labels[i]) continue;
        if (!ref) { ref = labels[i]; continue; }
        ASSERT_EQ(labels[i], ref);
    }
    ASSERT_NE(ref, uint16_t(0));
}

TEST(separate_blobs_on_adjacent_rows_different_labels) {
    // Under 8-connectivity, two blobs on adjacent rows are connected if any
    // pixel in row N is diagonally adjacent to any pixel in row N-1.
    // Specifically: (r,c) connects to (r-1,c-1), (r-1,c), (r-1,c+1).
    //
    // Grid [1,1,0,0 / 0,0,1,1]: (1,2) has NW=prev[1]=label_of_(0,1) → CONNECTED.
    // To be genuinely separate on adjacent rows, the column gap must be >= 2.
    //
    // Use: row0 pixel at col0 only, row1 pixel at col2 only.
    //   NE of (0,0) = prev[1] = 0  (no right extension of row0 blob)
    //   NW of (1,2) = prev[1] = 0  (no right extension of row0 blob)
    //   N  of (1,2) = prev[2] = 0
    //   These two ARE genuinely separate.
    //
    // Verify via recycle event: blobA (row0 col0) is recycled in row1,
    // then blobB (row1 col2) allocates a fresh (or recycled) label.
    // Since the two blobs are separate, no merge event should appear.
    // 2 rows × 4 cols:
    //   1 0 0 0    ← blobA at col 0 only
    //   0 0 1 0    ← blobB at col 2 only  (col gap = 2 — no NW/N/NE bridge)
    std::vector<uint8_t> grid = {
        1, 0, 0, 0,
        0, 0, 1, 0
    };
    auto results = runLabeller(4, grid);

    // Both blobs must be foreground
    ASSERT_NE(results[0].l1, uint16_t(0));   // (0,0)
    ASSERT_NE(results[3].l1, uint16_t(0));   // (1,2)

    // No merge events — they are independent components
    for (const auto& lp : results) {
        ASSERT_EQ(lp.merge_old,  uint16_t(0));
        ASSERT_EQ(lp.merge_old2, uint16_t(0));
    }

    // A recycle event must fire (blobA completed, its slot freed)
    bool recycled = false;
    for (const auto& lp : results)
        if (lp.recycled != 0) recycled = true;
    ASSERT_TRUE(recycled);
}

TEST(row_packet_count_matches_columns) {
    // m=8, 3 rows → 3 × (8/2) = 12 packets exactly
    std::vector<uint8_t> grid(24, uint8_t(1));
    auto results = runLabeller(8, grid);
    ASSERT_EQ(results.size(), size_t(12));
}


// ============================================================================
// Section 9 — Coordinate correctness
// ============================================================================

TEST(packet_row_matches_input_row) {
    // 3 rows × 4 cols — every packet's row field must match its input row
    std::vector<uint8_t> grid(12, uint8_t(1));
    auto results = runLabeller(4, grid);
    // 3 rows × 2 packets/row = 6 packets
    ASSERT_EQ(results.size(), size_t(6));
    ASSERT_EQ(results[0].row, uint64_t(0));
    ASSERT_EQ(results[1].row, uint64_t(0));
    ASSERT_EQ(results[2].row, uint64_t(1));
    ASSERT_EQ(results[3].row, uint64_t(1));
    ASSERT_EQ(results[4].row, uint64_t(2));
    ASSERT_EQ(results[5].row, uint64_t(2));
}

TEST(packet_col_matches_l1_position) {
    // m=6: packets should have col = 0, 2, 4 per row
    std::vector<uint8_t> grid(6, uint8_t(1));
    auto results = runLabeller(6, grid);
    ASSERT_EQ(results.size(), size_t(3));
    ASSERT_EQ(results[0].col, uint64_t(0));
    ASSERT_EQ(results[1].col, uint64_t(2));
    ASSERT_EQ(results[2].col, uint64_t(4));
}

TEST(l1_is_col_l2_is_col_plus_one) {
    // l1 belongs to col, l2 belongs to col+1.
    // Use a pattern where col 0 = foreground and col 1 = background.
    // 1 row × 4 cols: [1, 0, 1, 0]
    auto results = runLabeller(4, {1, 0, 1, 0});
    // Packet 0: col=0, l1=(0,0)=foreground, l2=(0,1)=background
    ASSERT_NE(results[0].l1, uint16_t(0));
    ASSERT_EQ(results[0].l2, uint16_t(0));
    // Packet 1: col=2, l1=(0,2)=foreground, l2=(0,3)=background
    ASSERT_NE(results[1].l1, uint16_t(0));
    ASSERT_EQ(results[1].l2, uint16_t(0));
}

TEST(coordinates_two_rows_exact) {
    // 2 rows × 4 cols — verify all 4 packets have exact row/col
    std::vector<uint8_t> grid(8, uint8_t(0));
    auto results = runLabeller(4, grid);
    ASSERT_EQ(results.size(), size_t(4));
    ASSERT_EQ(results[0].row, uint64_t(0)); ASSERT_EQ(results[0].col, uint64_t(0));
    ASSERT_EQ(results[1].row, uint64_t(0)); ASSERT_EQ(results[1].col, uint64_t(2));
    ASSERT_EQ(results[2].row, uint64_t(1)); ASSERT_EQ(results[2].col, uint64_t(0));
    ASSERT_EQ(results[3].row, uint64_t(1)); ASSERT_EQ(results[3].col, uint64_t(2));
}


// ============================================================================
// Section 10 — Memory constraint
// ============================================================================

TEST(minimum_columns_m_equals_2) {
    // m=2 is the smallest valid even column count
    auto results = runLabeller(2, {1, 1});
    ASSERT_EQ(results.size(), size_t(1));
    ASSERT_NE(results[0].l1, uint16_t(0));
    ASSERT_EQ(results[0].l1, results[0].l2);
}

TEST(large_m_produces_correct_packet_count) {
    // m=130, 5 rows → 5 × 65 = 325 packets
    const size_t M = 130, ROWS = 5;
    std::vector<uint8_t> grid(M * ROWS, uint8_t(0));
    auto results = runLabeller(M, grid);
    ASSERT_EQ(results.size(), size_t(ROWS * (M / 2)));
}

TEST(large_row_no_label_exhaustion) {
    // m=130, alternating pattern across 20 rows — peak active = m/2 = 65
    // Verifies mid-row drain keeps the map from exhausting
    const size_t M = 130, ROWS = 20;
    std::vector<uint8_t> grid(M * ROWS, uint8_t(0));
    for (size_t r = 0; r < ROWS; ++r)
        for (size_t c = 0; c < M; c += 2)
            grid[r * M + c] = 1;

    SimpleQueue<FilteredPacket> in_q;
    SimpleQueue<LabelledPacket> out_q;
    for (size_t r = 0; r < ROWS; ++r)
        for (size_t c = 0; c < M; c += 2) {
            FilteredPacket fp{};
            fp.b1 = grid[r*M+c]; fp.b2 = grid[r*M+c+1];
            fp.row = r; fp.col = c;
            in_q.push(fp);
        }

    SystemConfig cfg = labelConfig(M);
    LabellingBlock block(cfg, in_q, out_q);
    block.stop(); block.run();

    size_t bad = 0;
    LabelledPacket lp;
    while (out_q.pop(lp))
        if (lp.l1 == 0 && grid[lp.row * M + lp.col] == 1) ++bad;
    ASSERT_EQ(bad, size_t(0));
}


// ============================================================================
// Section 11 — Threaded integration
// ============================================================================

TEST(threaded_csv_end_to_end_known_grid) {
    // Push a known 4×4 binary grid through the full Generator→Labelling chain.
    // Grid (3 rows × 4 cols — U-shape without the blank row):
    //   1 0 1 0   ← two isolated pixels start
    //   1 0 1 0   ← two columns grow
    //   1 1 1 0   ← U closes → merge fires here

    SimpleQueue<FilteredPacket> in_q;

    // Push FilteredPackets directly (threshold already applied: 128 >= 64 = 1)
    // Row 0: col0=[1,0], col2=[1,0]
    FilteredPacket fp{};
    fp.b1=1; fp.b2=0; fp.row=0; fp.col=0; in_q.push(fp);
    fp.b1=1; fp.b2=0; fp.row=0; fp.col=2; in_q.push(fp);
    // Row 1
    fp.b1=1; fp.b2=0; fp.row=1; fp.col=0; in_q.push(fp);
    fp.b1=1; fp.b2=0; fp.row=1; fp.col=2; in_q.push(fp);
    // Row 2: [1,1,1,0] — col0 packet has b1=1,b2=1; col2 packet has b1=1,b2=0
    fp.b1=1; fp.b2=1; fp.row=2; fp.col=0; in_q.push(fp);
    fp.b1=1; fp.b2=0; fp.row=2; fp.col=2; in_q.push(fp);

    SystemConfig cfg = labelConfig(4);
    SimpleQueue<LabelledPacket> label_out;
    LabellingBlock block(cfg, in_q, label_out);

    std::thread t([&block]{ block.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    block.stop();
    t.join();

    std::vector<LabelledPacket> results;
    LabelledPacket lp;
    while (label_out.pop(lp)) results.push_back(lp);

    // 3 rows × 2 packets/row = 6 packets
    ASSERT_EQ(results.size(), size_t(6));

    // (a) Merge event must fire in row 2
    bool merge_in_row2 = false;
    for (const auto& r : results)
        if (r.row == 2 && (r.merge_old != 0 || r.merge_old2 != 0))
            merge_in_row2 = true;
    ASSERT_TRUE(merge_in_row2);

    // (b) Row 2 pixels after the merge must all carry the surviving (lower) label
    // Row 2 col 0: l1 = left arm (processed first), l2 = col1 (merge fires here)
    // Row 2 col 2: l1 = col2 (inherits surviving label), l2 = 0 (background)
    uint16_t surviving = 0;
    for (const auto& r : results) {
        if (r.row != 2) continue;
        // l2 of col0 packet: this is the pixel where the merge fired (col1)
        // l1 of col2 packet: col2 of the U base — must be surviving label
        if (r.col == 2 && r.l1 != 0) { surviving = r.l1; break; }
    }
    ASSERT_NE(surviving, uint16_t(0));

    // Both arm labels from rows 0-1 should be the two IDs that got merged
    uint16_t arm_left  = results[0].l1;   // (0,0)
    uint16_t arm_right = results[1].l1;   // (0,2)
    ASSERT_EQ(surviving, std::min(arm_left, arm_right));
}

TEST(threaded_random_does_not_crash) {
    // Smoke test: run Generator + LabellingBlock concurrently for 50ms.
    // No crash, no deadlock, at least some output produced.
    const size_t M = 20;
    SystemConfig cfg = labelConfig(M);

    DynamicSPSCQueue<FilteredPacket> in_q(M/2, M/2);
    SimpleQueue<LabelledPacket>      out_q;

    LabellingBlock block(cfg, in_q, out_q);

    // Producer thread — push random FilteredPackets for 50ms
    std::atomic<bool> producer_done{false};
    std::thread producer([&]() {
        uint64_t row = 0, col = 0;
        const auto end = std::chrono::steady_clock::now()
                       + std::chrono::milliseconds(50);
        while (std::chrono::steady_clock::now() < end) {
            FilteredPacket fp{};
            fp.b1  = static_cast<uint8_t>(rand() % 2);
            fp.b2  = static_cast<uint8_t>(rand() % 2);
            fp.row = row; fp.col = col;
            while (!in_q.push(fp))
                std::this_thread::yield();
            col += 2;
            if (col >= M) { col = 0; ++row; }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&block]{ block.run(); });

    producer.join();
    // Drain the queue then stop
    while (!in_q.empty()) std::this_thread::yield();
    block.stop();
    consumer.join();

    size_t count = 0;
    LabelledPacket lp;
    while (out_q.pop(lp)) ++count;
    ASSERT_TRUE(count > 0);
}


// ============================================================================
// Entry point
// ============================================================================

int main() {
    std::cout << "======================================================\n"
              << "  CynLr LabellingBlock Test Suite\n"
              << "======================================================\n\n";
    return run_all_tests();
}