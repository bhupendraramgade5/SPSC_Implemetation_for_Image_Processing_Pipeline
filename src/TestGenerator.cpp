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
#include <type_traits>
#include <vector>


// Minimal test framework

struct TestFailure { std::string message; };

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
            _os << "ASSERT_THROWS failed: (" #expr ")"                         \
                << " did not throw " #exc_type                              \
                << "  at " << __FILE__ << ":" << __LINE__;                  \
            throw TestFailure{ _os.str() };                                 \
        }                                                                    \
    } while (false)

#define ASSERT_NO_THROW(expr)                                                  \
    do {                                                                        \
        try { (expr); }                                                         \
        catch (const std::exception& _ex) {                                    \
            std::ostringstream _os;                                             \
            _os << "ASSERT_NO_THROW failed: (" #expr ")"                       \
                << " threw: " << _ex.what()                                    \
                << "  at " << __FILE__ << ":" << __LINE__;                     \
            throw TestFailure{ _os.str() };                                    \
        }                                                                       \
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


// Helpers
// Write a temporary CSV file and return its path.
static std::string writeTempCSV(const std::string& content,
                                 const std::string& filename = "cynlr_test.csv")
{
    const auto path = std::filesystem::temp_directory_path() / filename;
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot create temp CSV: " + path.string());
    f << content;
    return path.string();
}

// Write a minimal valid config file and return its path.
static std::string writeTempConfig(const std::string& content) {
    const auto path = std::filesystem::temp_directory_path() / "cynlr_test.cfg";
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot create temp config");
    f << content;
    return path.string();
}

// Build a minimal SystemConfig — tests override fields as needed.
static SystemConfig makeConfig(size_t columns      = 4,
                               uint64_t cycle_ns   = 1'000'000ULL,  // 1 ms
                               uint8_t  threshold  = 128,
                               Mode     mode       = Mode::RANDOM,
                               const std::string& file = "") 
{
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


// Section 1 — DataPacket
TEST(datapacket_zero_initialised_by_default) {
    // Aggregate initialisation with {} must zero all fields.
    DataPacket pkt{};
    ASSERT_EQ(pkt.v1,  static_cast<uint8_t>(0));
    ASSERT_EQ(pkt.v2,  static_cast<uint8_t>(0));
    ASSERT_EQ(pkt.row, static_cast<uint64_t>(0));
    ASSERT_EQ(pkt.col, static_cast<uint64_t>(0));
}
TEST(datapacket_fields_are_independent) {
    // Writing one field must not affect the others.
    DataPacket pkt{};
    pkt.v1  = 255;
    pkt.v2  = 1;
    pkt.row = 999;
    pkt.col = 42;

    ASSERT_EQ(pkt.v1,  static_cast<uint8_t>(255));
    ASSERT_EQ(pkt.v2,  static_cast<uint8_t>(1));
    ASSERT_EQ(pkt.row, static_cast<uint64_t>(999));
    ASSERT_EQ(pkt.col, static_cast<uint64_t>(42));
}
TEST(datapacket_is_trivially_copyable) {
    // Required by SPSCQueue's static_assert — verify it holds at runtime too.
    ASSERT_TRUE(std::is_trivially_copyable<DataPacket>::value);
}
TEST(datapacket_copy_is_independent) {
    // Copying a packet must produce an independent value, not an alias.
    DataPacket a{ 10, 20, 1, 2 };
    DataPacket b = a;
    b.v1 = 99;
    ASSERT_EQ(a.v1, static_cast<uint8_t>(10));  // a must be unchanged
    ASSERT_EQ(b.v1, static_cast<uint8_t>(99));
}
// Section 2 — ConfigManager
TEST(config_default_kernel_applied_when_missing) {
    const auto path = writeTempConfig("m=4\nT=1000000\nthreshold=128\nmode=random\n");
    const auto tmp_dir = std::filesystem::temp_directory_path();
    const auto cfg_path = tmp_dir / "config.cfg";
    {
        std::ofstream f(cfg_path);
        f << "m=4\nT=1000000\nthreshold=128\nmode=random\n";
    }

    const auto orig_dir = std::filesystem::current_path();
    std::filesystem::current_path(tmp_dir);

    SystemConfig cfg{};
    ASSERT_NO_THROW(cfg = ConfigManager::load(0, nullptr));

    std::filesystem::current_path(orig_dir);

    // Default kernel has exactly 9 elements.
    ASSERT_EQ(cfg.kernel.size(), static_cast<size_t>(9));
    ASSERT_EQ(cfg.columns, static_cast<size_t>(4));
    ASSERT_EQ(cfg.cycle_time_ns, static_cast<uint64_t>(1'000'000));
    ASSERT_EQ(cfg.threshold, static_cast<uint8_t>(128));
}
TEST(config_csv_mode_parsed_correctly) {
    const auto tmp_dir = std::filesystem::temp_directory_path();
    {
        std::ofstream f(tmp_dir / "config.cfg");
        f << "m=8\nT=500000\nthreshold=64\nmode=csv\ninput_file=test.csv\n";
    }
    const auto orig = std::filesystem::current_path();
    std::filesystem::current_path(tmp_dir);

    SystemConfig cfg{};
    ASSERT_NO_THROW(cfg = ConfigManager::load(0, nullptr));
    std::filesystem::current_path(orig);

    ASSERT_EQ(cfg.columns, static_cast<size_t>(8));
    ASSERT_EQ(cfg.mode, Mode::CSV);
    ASSERT_EQ(cfg.input_file, std::string("test.csv"));
}
TEST(config_zero_columns_throws) {
    const auto tmp_dir = std::filesystem::temp_directory_path();
    {
        std::ofstream f(tmp_dir / "config.cfg");
        f << "m=0\nT=1000000\nthreshold=128\nmode=random\n";
    }
    const auto orig = std::filesystem::current_path();
    std::filesystem::current_path(tmp_dir);

    ASSERT_THROWS(ConfigManager::load(0, nullptr), std::runtime_error);
    std::filesystem::current_path(orig);
}
TEST(config_zero_cycle_time_throws) {
    const auto tmp_dir = std::filesystem::temp_directory_path();
    {
        std::ofstream f(tmp_dir / "config.cfg");
        f << "m=4\nT=0\nthreshold=128\nmode=random\n";
    }
    const auto orig = std::filesystem::current_path();
    std::filesystem::current_path(tmp_dir);

    ASSERT_THROWS(ConfigManager::load(0, nullptr), std::runtime_error);
    std::filesystem::current_path(orig);
}
TEST(config_custom_kernel_parsed) {
    const auto tmp_dir = std::filesystem::temp_directory_path();
    {
        std::ofstream f(tmp_dir / "config.cfg");
        f << "m=4\nT=1000000\nthreshold=128\nmode=random\n"
          << "kernel=0.1,0.2,0.3,0.4,0.5,0.4,0.3,0.2,0.1\n";
    }
    const auto orig = std::filesystem::current_path();
    std::filesystem::current_path(tmp_dir);

    SystemConfig cfg{};
    ASSERT_NO_THROW(cfg = ConfigManager::load(0, nullptr));
    std::filesystem::current_path(orig);

    ASSERT_EQ(cfg.kernel.size(), static_cast<size_t>(9));
    // Spot-check the centre value (index 4).
    ASSERT_TRUE(cfg.kernel[4] > 0.49f && cfg.kernel[4] < 0.51f);
}
// TEST(config_missing_file_uses_defaults_or_warns) {
//     const auto tmp_dir = std::filesystem::temp_directory_path() / "cynlr_empty_dir";
//     std::filesystem::create_directories(tmp_dir);
//     // Make sure there's no config.cfg there.
//     std::filesystem::remove(tmp_dir / "config.cfg");

//     const auto orig = std::filesystem::current_path();
//     std::filesystem::current_path(tmp_dir);

//     ASSERT_THROWS(ConfigManager::load(0, nullptr), std::runtime_error);
//     std::filesystem::current_path(orig);
// }

TEST(config_missing_file_uses_defaults_or_warns) {
    // Use a unique temp directory that definitely has no config.cfg
    const auto tmp_dir = std::filesystem::temp_directory_path() / "cynlr_no_config_dir";
    std::filesystem::create_directories(tmp_dir);
    std::filesystem::remove(tmp_dir / "config.cfg");  // ensure it's gone

    const auto orig = std::filesystem::current_path();
    std::filesystem::current_path(tmp_dir);

    // Missing file → columns defaults to 0 → validate() throws runtime_error
    ASSERT_THROWS(ConfigManager::load(0, nullptr), std::runtime_error);

    std::filesystem::current_path(orig);
}
// Section 3 — RandomDataSource

TEST(random_values_in_uint8_range) {
    // Every generated value must be in [0, 255].
    RandomDataSource src(4);
//    for (int i = 0; i < 1000; ++i) {
    for (int i = 0; i < 2000; ++i) {
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

TEST(random_col_advances_by_two_within_row) {
    RandomDataSource src(6);  // 3 packets per row
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

TEST(random_row_wraps_at_column_boundary) {
    // columns=4 → packets per row = 4/2 = 2
    RandomDataSource src(4);
    DataPacket pkt{};

    src.next(pkt); ASSERT_EQ(pkt.row, 0ULL); ASSERT_EQ(pkt.col, 0ULL);
    src.next(pkt); ASSERT_EQ(pkt.row, 0ULL); ASSERT_EQ(pkt.col, 2ULL);
    src.next(pkt); ASSERT_EQ(pkt.row, 1ULL); ASSERT_EQ(pkt.col, 0ULL);
    src.next(pkt); ASSERT_EQ(pkt.row, 1ULL); ASSERT_EQ(pkt.col, 2ULL);
    src.next(pkt); ASSERT_EQ(pkt.row, 2ULL); ASSERT_EQ(pkt.col, 0ULL);
}

TEST(random_coordinate_sequence_exhaustive) {
    const size_t COLS = 8;
    RandomDataSource src(COLS);
    for (uint64_t r = 0; r < 10; ++r) {
        for (uint64_t c = 0; c < COLS; c += 2) {
            DataPacket pkt{};
            src.next(pkt);
            ASSERT_EQ(pkt.row, r);
            ASSERT_EQ(pkt.col, c);
        }
    }
}

TEST(random_zero_columns_throws) {
    ASSERT_THROWS(RandomDataSource(0), std::invalid_argument);
}

TEST(random_v1_and_v2_are_independent) {
    // Over 500 samples, v1 and v2 should not always be equal.
    // P(v1==v2 for all 500) ≈ (1/256)^499 — effectively impossible.
    RandomDataSource src(4);
    bool found_different = false;
    for (int i = 0; i < 500 && !found_different; ++i) {
        DataPacket pkt{};
        src.next(pkt);
        if (pkt.v1 != pkt.v2) found_different = true;
    }
    ASSERT_TRUE(found_different);
}


TEST(random_produces_varied_values) {
    // Over 500 samples, not all v1 values should be identical.
    RandomDataSource src(4);
    uint8_t first_v1 = 0;
    {   DataPacket p{}; src.next(p); first_v1 = p.v1; }

    bool found_different = false;
    for (int i = 0; i < 500 && !found_different; ++i) {
        DataPacket pkt{};
        src.next(pkt);
        if (pkt.v1 != first_v1) found_different = true;
    }
    ASSERT_TRUE(found_different);
}


// Section 2 — CSVDataSource

TEST(csv_basic_two_rows_four_columns) {
    const auto path = writeTempCSV("10,20,30,40\n50,60,70,80\n");
    CSVDataSource src(path);
    DataPacket pkt{};

    ASSERT_TRUE(src.next(pkt));
    ASSERT_EQ(pkt.row, 0ULL); ASSERT_EQ(pkt.col, 0ULL);
    ASSERT_EQ(pkt.v1, uint8_t(10)); ASSERT_EQ(pkt.v2, uint8_t(20));

    ASSERT_TRUE(src.next(pkt));
    ASSERT_EQ(pkt.row, 0ULL); ASSERT_EQ(pkt.col, 2ULL);
    ASSERT_EQ(pkt.v1, uint8_t(30)); ASSERT_EQ(pkt.v2, uint8_t(40));

    // Packet 3: row 1, col 0, v1=50 v2=60
    ASSERT_TRUE(src.next(pkt));
    ASSERT_EQ(pkt.row, 1ULL); ASSERT_EQ(pkt.col, 0ULL);
    ASSERT_EQ(pkt.v1, uint8_t(50)); ASSERT_EQ(pkt.v2, uint8_t(60));

    // Packet 4: row 1, col 2, v1=70 v2=80
    ASSERT_TRUE(src.next(pkt));
    ASSERT_EQ(pkt.row, 1ULL); ASSERT_EQ(pkt.col, 2ULL);
    ASSERT_EQ(pkt.v1, uint8_t(70)); ASSERT_EQ(pkt.v2, uint8_t(80));
}

TEST(csv_returns_false_at_eof_and_stays_false) {
    const auto path = writeTempCSV("1,2,3,4\n");
    CSVDataSource src(path);;
    DataPacket pkt{};

    ASSERT_TRUE(src.next(pkt));
    ASSERT_TRUE(src.next(pkt));
    ASSERT_FALSE(src.next(pkt));   // EOF
    ASSERT_FALSE(src.next(pkt));   // stays false
    ASSERT_FALSE(src.next(pkt));   // stays false again
}

TEST(csv_whitespace_around_commas) {
    const auto path = writeTempCSV("  5 , 10 , 15 , 20 \n");
    CSVDataSource src(path);;
    DataPacket pkt{};

    ASSERT_TRUE(src.next(pkt));
    ASSERT_EQ(pkt.v1, uint8_t(5));
    ASSERT_EQ(pkt.v2, uint8_t(10));
    ASSERT_TRUE(src.next(pkt));
    ASSERT_EQ(pkt.v1, uint8_t(15));
    ASSERT_EQ(pkt.v2, uint8_t(20));
}

TEST(csv_boundary_values_0_and_255) {
    
    const auto path = writeTempCSV("0,255,0,255\n");
    CSVDataSource src(path);;

    DataPacket pkt{};
    ASSERT_TRUE(src.next(pkt));
    ASSERT_EQ(pkt.v1, uint8_t(0));
    ASSERT_EQ(pkt.v2, uint8_t(255));

    ASSERT_TRUE(src.next(pkt));
    ASSERT_EQ(pkt.v1, uint8_t(0));
    ASSERT_EQ(pkt.v2, uint8_t(255));
}

TEST(csv_row_index_increments_once_per_line) {
    // row_ must increment exactly once per CSV line — not once per packet.
    const auto path = writeTempCSV("1,2,3,4\n5,6,7,8\n9,10,11,12\n");
    CSVDataSource src(path);;

    DataPacket pkt{};
    for (uint64_t r = 0; r < 3; ++r) {
        for (uint64_t c = 0; c < 4; c += 2) {
            ASSERT_TRUE(src.next(pkt));
            ASSERT_EQ(pkt.row, r);
            ASSERT_EQ(pkt.col, c);
        }
    }
    ASSERT_FALSE(src.next(pkt));
}

TEST(csv_no_trailing_newline) {
    // Many real CSV files don't end with \n — must still be parsed correctly.
    const auto path = writeTempCSV("10,20,30,40");   // no \n at end
    CSVDataSource src(path);;
    DataPacket pkt{};

    ASSERT_TRUE(src.next(pkt));
    ASSERT_EQ(pkt.v1, uint8_t(10));
    ASSERT_EQ(pkt.v2, uint8_t(20));

    ASSERT_TRUE(src.next(pkt));
    ASSERT_EQ(pkt.v1, uint8_t(30));
    ASSERT_EQ(pkt.v2, uint8_t(40));

    ASSERT_FALSE(src.next(pkt));
}

TEST(csv_crlf_line_endings) {
    // Windows line endings (\r\n) — the \r must be stripped by the parser.
    const auto path = writeTempCSV("10,20,30,40\r\n50,60,70,80\r\n",
                                    "cynlr_crlf.csv");
    CSVDataSource src(path);;
    DataPacket pkt{};

    ASSERT_TRUE(src.next(pkt));
    ASSERT_EQ(pkt.v1, uint8_t(10));
    ASSERT_EQ(pkt.v2, uint8_t(20));

    ASSERT_TRUE(src.next(pkt));
    // col=2 in row 0
    ASSERT_EQ(pkt.v1, uint8_t(30));
    ASSERT_EQ(pkt.v2, uint8_t(40));

    ASSERT_TRUE(src.next(pkt));
    // row 1, col 0
    ASSERT_EQ(pkt.row, 1ULL);
    ASSERT_EQ(pkt.v1, uint8_t(50));
}

TEST(csv_empty_file_returns_false_immediately) {
    const auto path = writeTempCSV("", "cynlr_empty.csv");
    CSVDataSource src(path);;
    DataPacket pkt{};
    ASSERT_FALSE(src.next(pkt));
}

TEST(csv_single_row_two_columns_minimum) {
    const auto path = writeTempCSV("100,200\n");
    CSVDataSource src(path);;
    DataPacket pkt{};

    ASSERT_TRUE(src.next(pkt));
    ASSERT_EQ(pkt.v1,  uint8_t(100));
    ASSERT_EQ(pkt.v2,  uint8_t(200));
    ASSERT_EQ(pkt.row, 0ULL);
    ASSERT_EQ(pkt.col, 0ULL);

    ASSERT_FALSE(src.next(pkt));
}

TEST(csv_bad_file_path_throws) {
    ASSERT_THROWS(
        CSVDataSource("/nonexistent/path/no_such_file.csv"),
        std::runtime_error
    );
}
TEST(csv_zero_columns_throws) {
    // Columns are now auto-detected — zero columns is impossible unless
    // the file is empty, which throws runtime_error instead.
    const auto path = writeTempCSV("", "cynlr_empty2.csv");
    ASSERT_THROWS(CSVDataSource(path), std::runtime_error);
}
TEST(csv_single_row_two_columns) {
    

    const auto path = writeTempCSV("100,200\n");
    CSVDataSource src(path);

    DataPacket pkt{};
    ASSERT_TRUE(src.next(pkt));
    ASSERT_EQ(pkt.v1,  static_cast<uint8_t>(100));
    ASSERT_EQ(pkt.v2,  static_cast<uint8_t>(200));
    ASSERT_EQ(pkt.row, static_cast<uint64_t>(0));
    ASSERT_EQ(pkt.col, static_cast<uint64_t>(0));

    ASSERT_TRUE(!src.next(pkt));
}

TEST(csv_large_grid_exhaustive_verification) {

    const size_t ROWS = 5, COLS = 6;
    std::ostringstream ss;
    for (size_t r = 0; r < ROWS; ++r) {
        for (size_t c = 0; c < COLS; ++c) {
            if (c) ss << ',';
            ss << ((r * 10 + c) % 256);
        }
        ss << '\n';
    }
    const auto path = writeTempCSV(ss.str(), "cynlr_large.csv");
    CSVDataSource src(path);

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
    ASSERT_FALSE(src.next(pkt));
}


// Section 5 — SPSCQueue

TEST(spsc_empty_on_construction) {
    SPSCQueue<DataPacket, 8> q;
    ASSERT_TRUE(q.empty());
    ASSERT_EQ(q.size(), static_cast<size_t>(0));
}

TEST(spsc_single_push_pop_roundtrip) {
    SPSCQueue<DataPacket, 8> q;
    DataPacket in{ 42, 84, 7, 3 };
    ASSERT_TRUE(q.push(in));

    DataPacket out{};
    ASSERT_TRUE(q.pop(out));
    ASSERT_EQ(out.v1,  uint8_t(42));
    ASSERT_EQ(out.v2,  uint8_t(84));
    ASSERT_EQ(out.row, uint64_t(7));
    ASSERT_EQ(out.col, uint64_t(3));
    ASSERT_TRUE(q.empty());
}

TEST(spsc_fifo_ordering_preserved) {
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

TEST(spsc_pop_on_empty_returns_false) {
    SPSCQueue<DataPacket, 8> q;
    DataPacket pkt{};
    ASSERT_FALSE(q.pop(pkt));
    // Verify the output packet was not corrupted (still zero).
    ASSERT_EQ(pkt.v1,  uint8_t(0));
    ASSERT_EQ(pkt.row, uint64_t(0));
}

TEST(spsc_push_on_full_returns_false) {
    // CAPACITY=4, MASK=3.  The queue holds at most CAPACITY-1 = 3 items.
    // (One slot is sacrificed to distinguish full vs empty without a counter.)
    SPSCQueue<DataPacket, 4> q;
    DataPacket pkt{1, 2, 0, 0};
    ASSERT_TRUE(q.push(pkt));
    ASSERT_TRUE(q.push(pkt));
    ASSERT_TRUE(q.push(pkt));
    // 4th push — queue is now full (head - tail == CAPACITY == 4)
    ASSERT_FALSE(q.push(pkt));  // head+1=4, 4-0=4 > MASK=3 → full
    ASSERT_EQ(q.size(), static_cast<size_t>(3));
}

TEST(spsc_pop_after_full_makes_space) {
    SPSCQueue<DataPacket, 4> q;
    DataPacket pkt{ 1, 2, 0, 0 };
    q.push(pkt); q.push(pkt); q.push(pkt);  // fill to capacity-1

    DataPacket out{};
    ASSERT_TRUE(q.pop(out));                 // free one slot
    ASSERT_TRUE(q.push(pkt));               // must succeed now
}

TEST(spsc_wrap_around_over_many_cycles) {
    // Push and pop through the ring CAPACITY*4 times to exercise index wrap.
    SPSCQueue<DataPacket, 4> q;
    for (int round = 0; round < 16; ++round) {
        DataPacket in{ static_cast<uint8_t>(round & 0xFF), 0, 0, 0 };
        while (!q.push(in)) { /* spin — should not be needed here */ }

        DataPacket out{};
        ASSERT_TRUE(q.pop(out));
        ASSERT_EQ(out.v1, static_cast<uint8_t>(round & 0xFF));
    }
    ASSERT_TRUE(q.empty());
}

TEST(spsc_size_tracks_occupancy) {
    SPSCQueue<DataPacket, 8> q;
    DataPacket pkt{ 1, 2, 0, 0 };

    ASSERT_EQ(q.size(), 0u);
    q.push(pkt); ASSERT_EQ(q.size(), 1u);
    q.push(pkt); ASSERT_EQ(q.size(), 2u);

    DataPacket out{};
    q.pop(out);  ASSERT_EQ(q.size(), 1u);
    q.pop(out);  ASSERT_EQ(q.size(), 0u);
}

TEST(spsc_two_thread_1000_packets_ordered) {
    // Producer pushes 1000 packets; consumer verifies ordering end-to-end.
    static constexpr int N = 1000;
    SPSCQueue<DataPacket, 64> q;

    std::atomic<int>  consumed{ 0 };
    std::atomic<bool> order_ok{ true };

    std::thread producer([&]() {
        for (int i = 0; i < N; ++i) {
            DataPacket pkt{
                static_cast<uint8_t>(i & 0xFF),
                static_cast<uint8_t>((i >> 8) & 0xFF),
                static_cast<uint64_t>(i),
                0ULL
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
                    order_ok.store(false, std::memory_order_relaxed);
                }
                ++expected;
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(consumed.load(), N);
    ASSERT_TRUE(order_ok.load());
}


// Section 6 — createDataSource factory
TEST(factory_random_mode_returns_random_source) {
    auto cfg = makeConfig(4, 1'000'000ULL, 128, Mode::RANDOM);
    auto src = createDataSource(cfg);
    ASSERT_TRUE(src != nullptr);

    // RandomDataSource always returns true.
    DataPacket pkt{};
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(src->next(pkt));
    }
}

TEST(factory_csv_mode_returns_csv_source) {
    const auto path = writeTempCSV("1,2,3,4\n", "cynlr_factory.csv");
    auto cfg = makeConfig(4, 1'000'000ULL, 128, Mode::CSV, path);
    auto src = createDataSource(cfg);
    ASSERT_TRUE(src != nullptr);

    DataPacket pkt{};
    ASSERT_TRUE(src->next(pkt));   // row 0, col 0
    ASSERT_TRUE(src->next(pkt));   // row 0, col 2
    ASSERT_FALSE(src->next(pkt));  // EOF
}

TEST(factory_csv_bad_path_throws) {
    auto cfg = makeConfig(4, 1'000'000ULL, 128, Mode::CSV, "/no/such/file.csv");
    ASSERT_THROWS(createDataSource(cfg), std::runtime_error);
}


// =============================================================================
// Section 7 — GeneratorBlock
// =============================================================================

TEST(generator_random_mode_produces_packets) {
    auto cfg = makeConfig(4, 100'000ULL);  // T = 100 µs
    PipelineQueue q;

    GeneratorBlock gen(cfg, q, createDataSource(cfg));
    std::thread t([&gen]{ gen.run(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    gen.stop();
    t.join();

    // At T=100µs over 10ms expect ~100 packets; lower-bound 20 for safety.
    ASSERT_TRUE(q.size() >= 20);
}

TEST(generator_csv_mode_exact_values_and_order) {
    const auto path = writeTempCSV("10,20,30,40\n50,60,70,80\n",
                                    "cynlr_gen_csv.csv");
    auto cfg = makeConfig(4, 100'000ULL, 128, Mode::CSV, path);

    SimpleQueue<DataPacket> q;   // use SimpleQueue for easy single-thread drain
    GeneratorBlock gen(cfg, q, createDataSource(cfg));

    // Run synchronously (no thread) — CSV source will exhaust and run() returns.
    gen.run();

    struct E { uint8_t v1, v2; uint64_t row, col; };
    const E expected[4] = {
        { 10, 20, 0, 0 },
        { 30, 40, 0, 2 },
        { 50, 60, 1, 0 },
        { 70, 80, 1, 2 },
    };

    for (const auto& e : expected) {
        DataPacket pkt{};
        ASSERT_TRUE(q.pop(pkt));
        ASSERT_EQ(pkt.v1,  e.v1);
        ASSERT_EQ(pkt.v2,  e.v2);
        ASSERT_EQ(pkt.row, e.row);
        ASSERT_EQ(pkt.col, e.col);
    }

    DataPacket extra{};
    ASSERT_FALSE(q.pop(extra));  // queue must be exactly empty
}

TEST(generator_csv_mode_no_extra_packets) {
    // 1 row × 2 columns → exactly 1 packet.
    const auto path = writeTempCSV("7,13\n", "cynlr_1pkt.csv");
    auto cfg = makeConfig(2, 100'000ULL, 128, Mode::CSV, path);

    SimpleQueue<DataPacket> q;
    GeneratorBlock gen(cfg, q, createDataSource(cfg));
    gen.run();

    int count = 0;
    DataPacket pkt{};
    while (q.pop(pkt)) ++count;
    ASSERT_EQ(count, 1);
}

TEST(generator_stop_exits_within_grace_period) {
    // stop() must cause run() to return within a reasonable time.
    auto cfg = makeConfig(4, 50'000ULL);  // T = 50 µs
    PipelineQueue q;

    GeneratorBlock gen(cfg, q, createDataSource(cfg));
    std::thread t([&gen]{ gen.run(); });

    const auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    gen.stop();
    t.join();

    const auto elapsed = std::chrono::steady_clock::now() - t0;
    ASSERT_TRUE(elapsed < std::chrono::milliseconds(200));
}

TEST(generator_stop_before_run_exits_immediately) {
    // Calling stop() before run() — run() should exit on the first iteration.
    auto cfg = makeConfig(4, 1'000'000ULL);  // T = 1 ms
    PipelineQueue q;

    GeneratorBlock gen(cfg, q, createDataSource(cfg));
    gen.stop();  // set flag BEFORE run()

    const auto t0 = std::chrono::steady_clock::now();
    gen.run();   // single-threaded — should return almost immediately
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    // Should return in well under 1 cycle (T=1ms).
    ASSERT_TRUE(elapsed < std::chrono::milliseconds(10));
}

TEST(generator_back_pressure_handled) {
    // Use a tiny queue (capacity 4, holds 3 items) and a very fast T.
    // The generator must not crash or block forever when the queue is full.
    SPSCQueue<DataPacket, 4> tiny_q;
    auto cfg = makeConfig(4, 1'000ULL);  // T = 1 µs — fast producer

    GeneratorBlock gen(cfg, tiny_q, createDataSource(cfg));
    std::thread t([&gen]{ gen.run(); });

    // Let it run for 5 ms, slowly draining the queue to create back-pressure.
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        DataPacket pkt{};
        tiny_q.pop(pkt);  // drain at a slower rate than production
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    gen.stop();
    t.join();  // if this hangs, back-pressure handling is broken
    ASSERT_TRUE(true);  // reaching here means no deadlock
}

TEST(generator_timing_within_tolerance) {
    // Measure inter-packet intervals over 20 consecutive pops.
    // Acceptable range: [0.5 T, 4 T] — wide to avoid CI flakiness, tight
    // enough to catch gross sleeping errors.
    const uint64_t T_ns = 500'000ULL;  // 500 µs
    auto cfg = makeConfig(4, T_ns);

    PipelineQueue q;
    GeneratorBlock gen(cfg, q, createDataSource(cfg));
    std::thread t([&gen]{ gen.run(); });

    std::vector<std::chrono::steady_clock::time_point> ts;
    ts.reserve(22);

    while (ts.size() < 21) {
        DataPacket pkt{};
        if (q.pop(pkt)) {
            ts.push_back(std::chrono::steady_clock::now());
        }
    }

    gen.stop();
    t.join();

    for (size_t i = 1; i < ts.size(); ++i) {
        const int64_t interval = std::chrono::duration_cast<std::chrono::nanoseconds>(
		ts[i] - ts[i - 1]).count();

        ASSERT_TRUE(interval >= static_cast<int64_t>(T_ns / 2));
        ASSERT_TRUE(interval <= static_cast<int64_t>(T_ns * 4));
    }
}


int main() {
    std::cout << "======================================================\n"
              << "  CynLr Pipeline Test Suite\n"
              << "======================================================\n\n";

    std::cout << "--- Section 1: DataPacket ---\n";
    // (all tests run together; sections are printed for readability)

    return run_all_tests();
}