// test_generator.cpp

#include "GeneratorBlock.hpp"
#include "Queue.hpp"
#include "ConfigManager.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>       // std::filesystem::temp_directory_path  (C++17)
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>


// Minimal test framework

struct TestFailure {
    std::string message;
};

// Assertion helpers — throw on failure so the rest of the suite still runs.
#define ASSERT_TRUE(cond)                                                   \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::ostringstream _os;                                          \
            _os << "ASSERT_TRUE failed: (" #cond ")"                        \
                << "  at " << __FILE__ << ":" << __LINE__;                  \
            throw TestFailure{ _os.str() };                                 \
        }                                                                    \
    } while (false)

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
            _os << "ASSERT_THROWS failed: " #expr                           \
                << " did not throw " #exc_type                              \
                << "  at " << __FILE__ << ":" << __LINE__;                  \
            throw TestFailure{ _os.str() };                                 \
        }                                                                    \
    } while (false)


// Registry of all tests.
struct TestCase {
    std::string              name;
    std::function<void()>    fn;
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


// Run all registered tests and print a summary.
static int run_all_tests() {
    int passed = 0, failed = 0;
    for (const auto& tc : g_tests) {
        try {
            tc.fn();
            std::cout << "  [PASS]  " << tc.name << '\n';
            ++passed;
        } catch (const TestFailure& tf) {
            std::cout << "  [FAIL]  " << tc.name << "\n"
                      << "          " << tf.message << '\n';
            ++failed;
        } catch (const std::exception& ex) {
            std::cout << "  [FAIL]  " << tc.name
                      << " (unexpected exception: " << ex.what() << ")\n";
            ++failed;
        }
    }
    std::cout << '\n'
              << "Results: " << passed << " passed, "
              << failed << " failed"
              << " (total " << (passed + failed) << ")\n";
    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}


// Helpers
// Write a temporary CSV file and return its path.
static std::string writeTempCSV(const std::string& content) {
    const auto tmp = std::filesystem::temp_directory_path()
                   / "cynlr_test_input.csv";
    std::ofstream f(tmp);
    if (!f) throw std::runtime_error("Cannot create temp CSV");
    f << content;
    return tmp.string();
}

// Build a minimal SystemConfig — tests override fields as needed.
static SystemConfig makeConfig(size_t columns      = 4,
                               uint64_t cycle_ns   = 1'000'000ULL,  // 1 ms
                               uint8_t  threshold  = 128,
                               Mode     mode       = Mode::RANDOM,
                               const std::string& file = "") {
    SystemConfig cfg{};
    cfg.columns       = columns;
    cfg.cycle_time_ns = cycle_ns;
    cfg.threshold     = threshold;
    cfg.mode          = mode;
    cfg.input_file    = file;
    cfg.kernel        = { 0.00025177f, 0.008666992f, 0.078025818f,
                          0.24130249f, 0.343757629f, 0.24130249f,
                          0.078025818f, 0.008666992f, 0.000125885f };
    return cfg;
}


// Section 1 — RandomDataSource

TEST(random_values_in_uint8_range) {
    // Every generated value must be in [0, 255].
    RandomDataSource src(4);
    for (int i = 0; i < 1000; ++i) {
        DataPacket pkt{};
        ASSERT_TRUE(src.next(pkt));
        // uint8_t is always [0,255] by type, but check the cast didn't overflow.
        ASSERT_TRUE(static_cast<int>(pkt.v1) >= 0 && static_cast<int>(pkt.v1) <= 255);
        ASSERT_TRUE(static_cast<int>(pkt.v2) >= 0 && static_cast<int>(pkt.v2) <= 255);
    }
}

TEST(random_never_returns_false) {
    // next() must always return true — random data is infinite.
    RandomDataSource src(4);
    for (int i = 0; i < 10'000; ++i) {
        DataPacket pkt{};
        ASSERT_TRUE(src.next(pkt));
    }
}

TEST(random_col_advances_by_two) {
    // Consecutive packets in the same row must have col differing by 2.
    RandomDataSource src(6);   // 6 columns → 3 packets per row
    DataPacket a{}, b{};
    src.next(a);   // col = 0
    src.next(b);   // col = 2
    ASSERT_EQ(a.row, b.row);
    ASSERT_EQ(b.col - a.col, static_cast<uint64_t>(2));
}

TEST(random_row_wraps_correctly) {
    
    RandomDataSource src(4);
    DataPacket pkt{};
    src.next(pkt);  // row 0, col 0
    ASSERT_EQ(pkt.row, static_cast<uint64_t>(0));
    ASSERT_EQ(pkt.col, static_cast<uint64_t>(0));

    src.next(pkt);  // row 0, col 2
    ASSERT_EQ(pkt.row, static_cast<uint64_t>(0));
    ASSERT_EQ(pkt.col, static_cast<uint64_t>(2));

    src.next(pkt);  // row 1, col 0
    ASSERT_EQ(pkt.row, static_cast<uint64_t>(1));
    ASSERT_EQ(pkt.col, static_cast<uint64_t>(0));
}

TEST(random_many_rows_coordinate_sequence) {
    // Walk through 10 full rows and verify every (row, col) pair is correct.
    const size_t COLS = 8;   // must be even
    RandomDataSource src(COLS);
    for (uint64_t expected_row = 0; expected_row < 10; ++expected_row) {
        for (uint64_t expected_col = 0; expected_col < COLS; expected_col += 2) {
            DataPacket pkt{};
            src.next(pkt);
            ASSERT_EQ(pkt.row, expected_row);
            ASSERT_EQ(pkt.col, expected_col);
        }
    }
}

TEST(random_zero_columns_throws) {
    ASSERT_THROWS(RandomDataSource(0), std::invalid_argument);
}

TEST(random_odd_column_count_works) {
    
    RandomDataSource src(3);     
    DataPacket pkt{};
    ASSERT_TRUE(src.next(pkt));  
    ASSERT_TRUE(src.next(pkt));  
    ASSERT_TRUE(src.next(pkt));  
    ASSERT_EQ(pkt.row, static_cast<uint64_t>(1));
}


// Section 2 — CSVDataSource

TEST(csv_basic_read_two_rows) {
    // 2 rows × 4 columns → 4 packets total.
    const auto path = writeTempCSV("10,20,30,40\n50,60,70,80\n");
    CSVDataSource src(path, 4);

    DataPacket pkt{};

    // Packet 1: row 0, col 0, v1=10 v2=20
    ASSERT_TRUE(src.next(pkt));
    ASSERT_EQ(pkt.row, static_cast<uint64_t>(0));
    ASSERT_EQ(pkt.col, static_cast<uint64_t>(0));
    ASSERT_EQ(pkt.v1,  static_cast<uint8_t>(10));
    ASSERT_EQ(pkt.v2,  static_cast<uint8_t>(20));

    // Packet 2: row 0, col 2, v1=30 v2=40
    ASSERT_TRUE(src.next(pkt));
    ASSERT_EQ(pkt.row, static_cast<uint64_t>(0));
    ASSERT_EQ(pkt.col, static_cast<uint64_t>(2));
    ASSERT_EQ(pkt.v1,  static_cast<uint8_t>(30));
    ASSERT_EQ(pkt.v2,  static_cast<uint8_t>(40));

    // Packet 3: row 1, col 0, v1=50 v2=60
    ASSERT_TRUE(src.next(pkt));
    ASSERT_EQ(pkt.row, static_cast<uint64_t>(1));
    ASSERT_EQ(pkt.col, static_cast<uint64_t>(0));
    ASSERT_EQ(pkt.v1,  static_cast<uint8_t>(50));
    ASSERT_EQ(pkt.v2,  static_cast<uint8_t>(60));

    // Packet 4: row 1, col 2, v1=70 v2=80
    ASSERT_TRUE(src.next(pkt));
    ASSERT_EQ(pkt.row, static_cast<uint64_t>(1));
    ASSERT_EQ(pkt.col, static_cast<uint64_t>(2));
    ASSERT_EQ(pkt.v1,  static_cast<uint8_t>(70));
    ASSERT_EQ(pkt.v2,  static_cast<uint8_t>(80));
}

TEST(csv_returns_false_at_eof) {
    const auto path = writeTempCSV("1,2,3,4\n");
    CSVDataSource src(path, 4);

    DataPacket pkt{};
    ASSERT_TRUE(src.next(pkt));   
    ASSERT_TRUE(src.next(pkt));   
    ASSERT_TRUE(!src.next(pkt));  
    ASSERT_TRUE(!src.next(pkt));  
}

TEST(csv_whitespace_tolerance) {
    
    const auto path = writeTempCSV("  5 , 10 , 15 , 20 \n");
    CSVDataSource src(path, 4);

    DataPacket pkt{};
    ASSERT_TRUE(src.next(pkt));
    ASSERT_EQ(pkt.v1, static_cast<uint8_t>(5));
    ASSERT_EQ(pkt.v2, static_cast<uint8_t>(10));
    ASSERT_TRUE(src.next(pkt));
    ASSERT_EQ(pkt.v1, static_cast<uint8_t>(15));
    ASSERT_EQ(pkt.v2, static_cast<uint8_t>(20));
}

TEST(csv_boundary_values_0_and_255) {
    
    const auto path = writeTempCSV("0,255,0,255\n");
    CSVDataSource src(path, 4);

    DataPacket pkt{};
    ASSERT_TRUE(src.next(pkt));
    ASSERT_EQ(pkt.v1, static_cast<uint8_t>(0));
    ASSERT_EQ(pkt.v2, static_cast<uint8_t>(255));
}

TEST(csv_row_index_correct_across_multiple_rows) {
    

    const auto path = writeTempCSV("1,2,3,4\n5,6,7,8\n9,10,11,12\n");
    CSVDataSource src(path, 4);

    DataPacket pkt{};
    for (uint64_t r = 0; r < 3; ++r) {
        for (uint64_t c = 0; c < 4; c += 2) {
            ASSERT_TRUE(src.next(pkt));
            ASSERT_EQ(pkt.row, r);
            ASSERT_EQ(pkt.col, c);
        }
    }
    ASSERT_TRUE(!src.next(pkt));
}

TEST(csv_bad_file_path_throws) {
    ASSERT_THROWS(
        CSVDataSource("/nonexistent/path/that/does_not_exist.csv", 4),
        std::runtime_error
    );
}

TEST(csv_zero_columns_throws) {
    const auto path = writeTempCSV("1,2,3,4\n");
    ASSERT_THROWS(CSVDataSource(path, 0), std::invalid_argument);
}

TEST(csv_single_row_two_columns) {
    

    const auto path = writeTempCSV("100,200\n");
    CSVDataSource src(path, 2);

    DataPacket pkt{};
    ASSERT_TRUE(src.next(pkt));
    ASSERT_EQ(pkt.v1,  static_cast<uint8_t>(100));
    ASSERT_EQ(pkt.v2,  static_cast<uint8_t>(200));
    ASSERT_EQ(pkt.row, static_cast<uint64_t>(0));
    ASSERT_EQ(pkt.col, static_cast<uint64_t>(0));

    ASSERT_TRUE(!src.next(pkt));
}

TEST(csv_large_grid_coordinate_exhaustive) {

    std::ostringstream ss;
    const size_t ROWS = 5, COLS = 6;
    for (size_t r = 0; r < ROWS; ++r) {
        for (size_t c = 0; c < COLS; ++c) {
            if (c) ss << ',';
            ss << ((r * 10 + c) % 256);
        }
        ss << '\n';
    }
    const auto path = writeTempCSV(ss.str());
    CSVDataSource src(path, COLS);

    DataPacket pkt{};
    for (size_t r = 0; r < ROWS; ++r) {
        for (size_t c = 0; c < COLS; c += 2) {
            ASSERT_TRUE(src.next(pkt));
            ASSERT_EQ(pkt.row, static_cast<uint64_t>(r));
            ASSERT_EQ(pkt.col, static_cast<uint64_t>(c));
            ASSERT_EQ(pkt.v1,  static_cast<uint8_t>((r * 10 + c)     % 256));
            ASSERT_EQ(pkt.v2,  static_cast<uint8_t>((r * 10 + c + 1) % 256));
        }
    }
    ASSERT_TRUE(!src.next(pkt));
}



TEST(spsc_empty_on_construction) {
    SPSCQueue<DataPacket, 8> q;
    ASSERT_TRUE(q.empty());
    ASSERT_EQ(q.size(), static_cast<size_t>(0));
}

TEST(spsc_push_then_pop_single_item) {
    SPSCQueue<DataPacket, 8> q;
    DataPacket in{ 42, 84, 7, 3 };
    ASSERT_TRUE(q.push(in));

    DataPacket out{};
    ASSERT_TRUE(q.pop(out));
    ASSERT_EQ(out.v1,  static_cast<uint8_t>(42));
    ASSERT_EQ(out.v2,  static_cast<uint8_t>(84));
    ASSERT_EQ(out.row, static_cast<uint64_t>(7));
    ASSERT_EQ(out.col, static_cast<uint64_t>(3));
    ASSERT_TRUE(q.empty());
}

TEST(spsc_fifo_ordering) {
    // Items must come out in the same order they went in.
    SPSCQueue<DataPacket, 16> q;
    for (uint8_t i = 0; i < 10; ++i) {
        DataPacket pkt{ i, static_cast<uint8_t>(i * 2), 0, 0 };
        ASSERT_TRUE(q.push(pkt));
    }
    for (uint8_t i = 0; i < 10; ++i) {
        DataPacket pkt{};
        ASSERT_TRUE(q.pop(pkt));
        ASSERT_EQ(pkt.v1, i);
        ASSERT_EQ(pkt.v2, static_cast<uint8_t>(i * 2));
    }
    ASSERT_TRUE(q.empty());
}

TEST(spsc_pop_returns_false_when_empty) {
    SPSCQueue<DataPacket, 8> q;
    DataPacket pkt{};
    ASSERT_TRUE(!q.pop(pkt));
}

TEST(spsc_push_returns_false_when_full) {
    // Capacity 4 — fill it, then verify push fails.
    SPSCQueue<DataPacket, 4> q;
    DataPacket pkt{1, 2, 0, 0};
    ASSERT_TRUE(q.push(pkt));
    ASSERT_TRUE(q.push(pkt));
    ASSERT_TRUE(q.push(pkt));
    // 4th push — queue is now full (head - tail == CAPACITY == 4)
    ASSERT_TRUE(!q.push(pkt));
}

TEST(spsc_wrap_around_correctness) {

    SPSCQueue<DataPacket, 4> q;
    for (int round = 0; round < 12; ++round) {
        DataPacket in{ static_cast<uint8_t>(round), 0, 0, 0 };
        // May need to wait if full — just retry until space is available.
        while (!q.push(in)) { /* spin */ }

        DataPacket out{};
        ASSERT_TRUE(q.pop(out));
        ASSERT_EQ(out.v1, static_cast<uint8_t>(round));
    }
}

TEST(spsc_two_thread_producer_consumer) {

    static constexpr int N = 1000;
    SPSCQueue<DataPacket, 64> q;

    std::atomic<int> consumed{ 0 };
    bool order_ok = true;

    std::thread producer([&]() {
        for (int i = 0; i < N; ++i) {
            DataPacket pkt{
                static_cast<uint8_t>(i & 0xFF),
                static_cast<uint8_t>((i >> 8) & 0xFF),
                static_cast<uint64_t>(i),
                0
            };
            while (!q.push(pkt)) { /* back-pressure spin */ }
        }
    });

    std::thread consumer([&]() {
        int expected = 0;
        while (consumed.load(std::memory_order_relaxed) < N) {
            DataPacket pkt{};
            if (q.pop(pkt)) {
                if (pkt.row != static_cast<uint64_t>(expected)) {
                    order_ok = false;
                }
                ++expected;
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(consumed.load(), N);
    ASSERT_TRUE(order_ok);
}


TEST(generator_random_mode_produces_packets) {
    // Run in random mode for a short time; verify packets arrive in the queue.
    auto cfg = makeConfig(4, 100'000ULL);   // T = 100 µs
    PipelineQueue q;

    GeneratorBlock gen(cfg, q, createDataSource(cfg));
    std::thread t([&gen]{ gen.run(); });

    // Give the generator ~5 ms to produce packets.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    gen.stop();
    t.join();

    // At T=100µs over 5ms we expect ~50 packets (generous lower bound of 10).
    ASSERT_TRUE(q.size() >= 10);
}

TEST(generator_csv_mode_exact_packet_values) {
    // Feed a known 2-row × 4-column CSV. Verify every packet value and coord.
    const auto path = writeTempCSV("10,20,30,40\n50,60,70,80\n");
    auto cfg = makeConfig(4, 100'000ULL, 128, Mode::CSV, path);

    SimpleQueue<DataPacket> q;   // use SimpleQueue for easy single-thread drain
    GeneratorBlock gen(cfg, q, createDataSource(cfg));

    // Run synchronously (no thread) — CSV source will exhaust and run() returns.
    gen.run();

    // Should have produced exactly 4 packets.
    struct Expected { uint8_t v1, v2; uint64_t row, col; };
    const Expected expected[4] = {
        { 10, 20, 0, 0 },
        { 30, 40, 0, 2 },
        { 50, 60, 1, 0 },
        { 70, 80, 1, 2 },
    };

    for (int i = 0; i < 4; ++i) {
        DataPacket pkt{};
        ASSERT_TRUE(q.pop(pkt));
        ASSERT_EQ(pkt.v1,  expected[i].v1);
        ASSERT_EQ(pkt.v2,  expected[i].v2);
        ASSERT_EQ(pkt.row, expected[i].row);
        ASSERT_EQ(pkt.col, expected[i].col);
    }

    DataPacket extra{};
    ASSERT_TRUE(!q.pop(extra));   // queue should now be empty
}

TEST(generator_stop_halts_random_mode) {

    auto cfg = makeConfig(4, 50'000ULL);   // T = 50 µs
    PipelineQueue q;

    GeneratorBlock gen(cfg, q, createDataSource(cfg));

    const auto start = std::chrono::steady_clock::now();
    std::thread t([&gen]{ gen.run(); });

    // Signal stop almost immediately.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    gen.stop();
    t.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // join() should return within 100 ms of stop() being called.
    ASSERT_TRUE(elapsed < std::chrono::milliseconds(100));
}

TEST(generator_csv_mode_no_extra_packets) {
    const auto path = writeTempCSV("1,2\n");   
    auto cfg = makeConfig(2, 100'000ULL, 128, Mode::CSV, path);

    SimpleQueue<DataPacket> q;
    GeneratorBlock gen(cfg, q, createDataSource(cfg));
    gen.run();   // returns when CSV is exhausted

    DataPacket pkt{};
    int count = 0;
    while (q.pop(pkt)) ++count;
    ASSERT_EQ(count, 1);
}

TEST(generator_timing_within_tolerance) {

    const uint64_t T_ns = 500'000ULL;   // 500 µs
    auto cfg = makeConfig(4, T_ns);

    PipelineQueue q;
    GeneratorBlock gen(cfg, q, createDataSource(cfg));
    std::thread t([&gen]{ gen.run(); });

    std::vector<std::chrono::steady_clock::time_point> timestamps;
    timestamps.reserve(22);

    while (timestamps.size() < 21) {
        DataPacket pkt{};
        if (q.pop(pkt)) {
            timestamps.push_back(std::chrono::steady_clock::now());
        }
    }

    gen.stop();
    t.join();

    for (size_t i = 1; i < timestamps.size(); ++i) {
        const auto interval_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            timestamps[i] - timestamps[i - 1]).count();

        // Lower bound: must have waited at least half T.
        ASSERT_TRUE(interval_ns >= static_cast<int64_t>(T_ns / 2));
        // Upper bound: must not have overslept by more than 3×T.
        ASSERT_TRUE(interval_ns <= static_cast<int64_t>(T_ns * 3));
    }
}


int main() {
    std::cout << "======================================\n"
              << " CynLr GeneratorBlock Test Suite\n"
              << "======================================\n\n";
    return run_all_tests();
}