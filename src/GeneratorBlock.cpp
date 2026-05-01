// ============================================================================
// GeneratorBlock.cpp
//
// Implements three cooperating classes that together form the first pipeline
// stage — the data acquisition block:
//
//   createDataSource()   — factory that selects the correct IDataSource
//                          implementation based on SystemConfig::mode.
//
//   RandomDataSource     — infinite pixel stream from a Mersenne Twister RNG.
//                          Used in stress-test / unlimited-run mode.
//
//   CSVDataSource        — finite pixel stream read row-by-row from a .csv
//                          file.  Used in evaluation / test mode where exact
//                          input values must be reproduced.
//
//   GeneratorBlock       — timing-controlled loop that calls source_->next(),
//                          pushes DataPackets into the pipeline queue, and
//                          paces itself to emit exactly one packet per cycle T.
//
// Threading model
// ---------------
//   GeneratorBlock::run() executes on its own thread (launched by main.cpp).
//   stop() is called from the supervisor thread and is the only cross-thread
//   interaction.  All other state is private to the generator thread.
//
// Timing strategy
// ---------------
//   Each cycle: sleep for (T - 20 µs) to avoid OS timer overshoot, then
//   spin-wait the remaining margin with _mm_pause for precise delivery.
//   This hybrid approach keeps CPU burn low for large T values while still
//   achieving sub-microsecond precision at the deadline.
// ============================================================================

#include <cstdint>
#include <vector>
#include <random>
#include <iostream>
#include <stdexcept>

#include "Queue.hpp"
#include "ConfigManager.hpp"
#include "GeneratorBlock.hpp"
#include <memory>

// Threshold below which the hybrid sleep+spin strategy collapses to pure spin.
static constexpr uint64_t SPIN_THRESHOLD_NS = 1'000'000ULL;


// ============================================================================
// createDataSource
// ----------------------------------------------------------------------------
// Factory function — selects RandomDataSource or CSVDataSource based on the
// mode field in SystemConfig and constructs it with the relevant parameters.
//
// Responsibility : decouple main.cpp and GeneratorBlock from the concrete
//                  source type.  Neither the block nor the supervisor need to
//                  know which source is active — they only hold IDataSource*.
//
// Ownership : returns a unique_ptr; the caller (GeneratorBlock ctor) takes
//             ownership and the source is destroyed when the block is destroyed.
// ============================================================================

std::unique_ptr<IDataSource> createDataSource(const SystemConfig& config) {
    if (config.mode == Mode::CSV) {
        return std::make_unique<CSVDataSource>(
            config.input_file, config.csv_mismatch_policy);
    }
    return std::make_unique<RandomDataSource>(config.columns);
}


// ============================================================================
// RandomDataSource
// ----------------------------------------------------------------------------
// Generates an infinite stream of random uint8 pixel pairs using a
//
// Responsibility : emulate the line-scan camera in stress-test mode.
//                  Produces statistically uniform pixel values with no
//                  repetition pattern that could bias filter or threshold
//                  measurements.
//
// Coordinate tracking:
//   col_ advances by 2 per call (two pixels per packet).
//   When col_ reaches columns_, it wraps to 0 and row_ increments —
//   matching the physical scan pattern of a line-scan camera.
//
// next() always returns true — random data is infinite by definition.
// ============================================================================

RandomDataSource::RandomDataSource(size_t columns)
    : columns_(columns), rng_(std::random_device{}()), dist_(0, 255)
{
    if (columns_ == 0)
        throw std::invalid_argument("RandomDataSource: columns must be > 0");
}

// Fills packet with two independent random values and the current coordinates,
// then advances the column/row counters.
bool RandomDataSource::next(DataPacket& packet) {
    packet.v1 = static_cast<uint8_t>(dist_(rng_));
    packet.v2 = static_cast<uint8_t>(dist_(rng_));

    packet.row = row_;
    packet.col = col_;

    advance();
    return true;
}

// Advances col_ by 2; wraps to the next row when the end of the scan line
// is reached.
void RandomDataSource::advance() {
    col_ += 2;
    if (col_ >= columns_) { col_ = 0; row_++; }
}


// ============================================================================
// CSVDataSource
// ----------------------------------------------------------------------------
// Reads pixel data row-by-row from a comma-separated file where each line
// contains exactly m values in [0, 255].
//
// Responsibility : provide deterministic, reproducible input for evaluation
//                  and unit testing.  The output must match the file contents
//                  exactly so test assertions can verify pixel values.
//
// Buffering:
//   loadNextRow() reads one full CSV line into buffer_ (a std::deque<uint8_t>).
//   next() consumes two values per call from the front of the buffer.
//   This decouples file I/O (per-row) from packet emission (per-pair).
//
// Mismatch handling (CSVMismatchPolicy):
//   REJECT   — throw on any row whose column count differs from m (default).
//   TRUNCATE — silently drop extra columns; skip short rows with a warning.
//   ZERO_PAD — pad short rows with 0; truncate long rows silently.
//
// Coordinate tracking:
//   row_ is incremented inside loadNextRow() immediately after reading,
//   so next() assigns row_ - 1 to packet.row to compensate.
//   col_ tracks the column of v1 within the current row.
// ============================================================================
CSVDataSource::CSVDataSource(const std::string& file,
                             CSVMismatchPolicy  mismatch_policy)
    : file_(file), columns_(0), mismatch_policy_(mismatch_policy)
{
    if (!file_.is_open())
        throw std::runtime_error(
            "CSVDataSource: cannot open file '" + file + "'");

    // Always auto-detect columns from first row — file is the source of truth.
    std::string line;
    if (std::getline(file_, line)) {
        std::stringstream ss(line);
        std::string val;
        size_t count = 0;
        while (std::getline(ss, val, ',')) {
            const auto first = val.find_first_not_of(" \t\r\n");
            if (first != std::string::npos) ++count;
        }
        columns_ = count;
    }

    if (columns_ == 0)
        throw std::runtime_error(
            "CSVDataSource: file is empty or first row has no values");

    // Pipeline contract: columns must be even (2 pixels per packet).
    // Truncate silently if odd — warn the user.
    if (columns_ % 2 != 0) {
        std::cerr << "[CSVDataSource] Warning: detected " << columns_
                  << " columns (odd) — truncating to " << (columns_ - 1)
                  << " to satisfy 2-pixels-per-packet contract.\n";
        --columns_;
    }

    std::cout << "[CSVDataSource] Auto-detected columns from file: "
              << columns_ << "\n";

    // Rewind so loadNextRow() re-reads from row 0.
    file_.seekg(0);
}

// Returns the next pair of pixels from the buffer, loading a new CSV row
// when fewer than 2 values remain.  Returns false at EOF.
bool CSVDataSource::next(DataPacket& packet) {
    if (!file_.is_open()) return false;

    if (buffer_.size() < 2) {
        if (!loadNextRow()) return false;
    }

    if (buffer_.size() < 2) return false;

    packet.v1 = buffer_.front(); buffer_.pop_front();
    packet.v2 = buffer_.front(); buffer_.pop_front();


    packet.row = row_ - 1;   // row_ already incremented by loadNextRow()
    packet.col = col_;

    advanceCol();
    return true;
}

// Reads one line from the CSV file into buffer_.
// Trims whitespace around each comma-separated token before parsing.
// Applies CSVMismatchPolicy if the parsed column count != columns_.
// Increments row_ and resets col_ after a successful read.
bool CSVDataSource::loadNextRow() {
    std::string line;
    if (!std::getline(file_, line)) return false;
    buffer_.clear();

    std::stringstream ss(line);
    std::string val;


    while (std::getline(ss, val, ',')) {
        const auto first = val.find_first_not_of(" \t\r\n");
        const auto last  = val.find_last_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
 
        const int value = std::stoi(val.substr(first, last - first + 1));
        buffer_.push_back(static_cast<uint8_t>(value));
    }

    const size_t actual = buffer_.size();

    if (actual != columns_) {
        switch (mismatch_policy_) {
            case CSVMismatchPolicy::REJECT:
                throw std::runtime_error(
                    "CSVDataSource: row " + std::to_string(row_) +
                    " has " + std::to_string(actual) +
                    " values, expected " + std::to_string(columns_) +
                    ". Set csv_mismatch_policy = truncate or zero_pad.");

            case CSVMismatchPolicy::TRUNCATE:
                if (actual > columns_) {
                    buffer_.resize(columns_);
                } else {
                    std::cerr << "[CSVDataSource] Warning: row " << row_
                              << " has only " << actual
                              << " values (expected " << columns_
                              << "), row skipped (TRUNCATE).\n";
                    buffer_.clear();
                }
                break;

            case CSVMismatchPolicy::ZERO_PAD:
                if (actual > columns_) {
                    buffer_.resize(columns_);
                } else {
                    while (buffer_.size() < columns_)
                        buffer_.push_back(0);
                    std::cerr << "[CSVDataSource] Warning: row " << row_
                              << " padded from " << actual
                              << " to " << columns_ << " (ZERO_PAD).\n";
                }
                break;
        }
    }

    // --- Odd column count after mismatch resolution ------------------------
    // Pipeline always emits 2 pixels per packet — a trailing unpaired pixel
    // has no valid partner and must be dropped.
    if (buffer_.size() % 2 != 0) {
        std::cerr << "[CSVDataSource] Warning: row " << row_
                  << " has odd column count (" << buffer_.size()
                  << ") after policy application — dropping last pixel.\n";
        buffer_.pop_back();
    }

    row_++;
    col_ = 0;

    // Return false if the row ended up empty (e.g. TRUNCATE on a short row).
    return !buffer_.empty();
}

// Advances col_ by 2 within the current row.
// Wraps to 0 at the row boundary — loadNextRow() will reset it explicitly,
// but this keeps the counter consistent for mid-row position queries.
void CSVDataSource::advanceCol() {
    col_ += 2;
    if (col_ >= columns_) col_ = 0;
}


// ============================================================================
// GeneratorBlock
// ----------------------------------------------------------------------------
// The first pipeline stage.  Owns an IDataSource and a reference to the
// inter-block queue.  Calls source_->next() once per cycle T, pushes the
// resulting DataPacket, then paces itself so the next iteration starts
// exactly T nanoseconds after the previous one.
//
// Back-pressure handling:
//   If the downstream queue is full, the block spin-retries the push until
//   either it succeeds or the cycle deadline expires.  On deadline expiry the
//   packet is counted as dropped and the cycle moves on.  This prevents the
//   generator from blocking the supervisor thread but records the event so
//   it is visible in the pipeline summary.
//
// Row counting:
//   rows_emitted_ is incremented each time packet.row transitions to a new
//   value.  The supervisor thread reads this atomically to enforce max_rows.
//
// Shutdown:
//   stop_flag_ is set by stop() (called from the supervisor thread).
//   run() checks it at the top of every iteration so shutdown latency is
//   bounded by one cycle T.
// ============================================================================

GeneratorBlock::GeneratorBlock(const SystemConfig&          config,
                               IQueue<DataPacket>&           queue,
                               std::unique_ptr<IDataSource>  source)
    : config_(config)
    , queue_(queue)
    , source_(std::move(source))
    , stop_flag_(false)
    , dropped_packets_(0)
{}

// Main pipeline loop.  Runs on a dedicated thread until the source is
// exhausted, stop() is called, or max_rows is reached.
void GeneratorBlock::run() {
    const auto cycle = std::chrono::nanoseconds(config_.cycle_time_ns);
    // const bool use_spin = (config_.cycle_time_ns < SPIN_THRESHOLD_NS);

    uint64_t prev_row = UINT64_MAX;  // sentinel: no row seen yet
    uint64_t rows_completed = 0;

    while (!stop_flag_.load(std::memory_order_relaxed)) {

        const auto deadline = std::chrono::steady_clock::now() + cycle;

        // --- 1. Generate -----------------------------------------------------
        DataPacket packet{};
        if (!source_->next(packet)) break;  // CSV exhausted
        // --- 2. Push with back-pressure retry --------------------------------
        // Spin until the queue accepts the packet or the deadline expires.
        bool pushed = false;
        while (!pushed) {
            pushed = queue_.push(packet);
            if (!pushed) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    dropped_packets_.fetch_add(1, std::memory_order_relaxed);
                    break;  // deadline missed — drop and move on
                }
#if defined(_MSC_VER)
                _mm_pause();
#elif defined(__GNUC__) || defined(__clang__)
                __builtin_ia32_pause();
#endif
            }
        }

        // --- 3. Row accounting -----------------------------------------------
        if (prev_row != UINT64_MAX && packet.row != prev_row) {
            ++rows_completed;
            rows_emitted_.store(rows_completed, std::memory_order_relaxed);
            if (config_.max_rows > 0 && rows_completed >= config_.max_rows)
                break;
        }
        prev_row = packet.row;

        // --- 4. Cycle pacing -------------------------------------------------
        // Sleep most of the remaining budget to yield the core, then
        // spin-wait the final 20 µs margin for sub-microsecond precision.
        auto now       = std::chrono::steady_clock::now();
        auto remaining = deadline - now;

        if (remaining > std::chrono::microseconds(50)) {
            std::this_thread::sleep_for(remaining - std::chrono::microseconds(20));
        }

        spinWaitUntil(deadline);   // precise delivery at the cycle boundary
    }
}

// Sets stop_flag_ — safe to call from any thread.
// run() will exit within one cycle T of this call.
void GeneratorBlock::stop() {
    stop_flag_.store(true, std::memory_order_relaxed);
}

// Busy-waits until steady_clock reaches deadline.
// Uses _mm_pause / __builtin_ia32_pause to reduce pipeline pressure and
// power consumption while spinning, without yielding the OS time slice.
void GeneratorBlock::spinWaitUntil(
        std::chrono::steady_clock::time_point deadline) const
{
    while (std::chrono::steady_clock::now() < deadline) {
#if defined(_MSC_VER)
        _mm_pause();
#elif defined(__GNUC__) || defined(__clang__)
        __builtin_ia32_pause();
#endif
    }
}
