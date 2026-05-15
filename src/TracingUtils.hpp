#ifndef TRACING_UTILS_HPP
#define TRACING_UTILS_HPP

// ============================================================================
// TracingUtils.hpp
//
// Data structures and output sinks for the Tracing & Computation pipeline stage.
// Mirrors the role of FilterUtils.hpp (Filter stage) and LabellingUtils.hpp
// (Labelling stage).
//
// Contains:
//   CompletedBlob     — packet emitted when a label is recycled (component done).
//   BlobAccumulator   — per-label working statistics (NOT sent over a queue).
//   ITracingOutput    — abstract sink for completed blobs.
//   NullTracingOutput — zero-overhead sink (write_output = false).
//   CSVTracingOutput  — buffered CSV file sink.
//   StdoutTracingOutput — debug print sink.
//   makeTracingOutput — factory matching the pattern of makeOutputWriter.
//
// Design constraints:
//   - BlobAccumulator is NOT trivially copyable (contains bool and sentinel
//     uint64_t); it is only stored in a flat vector, never in a queue.
//   - CompletedBlob IS trivially copyable (sent over a future queue if needed).
//   - All hot-path methods (update, merge) are inline and branch-predictor
//     friendly — four independent min/max comparisons, no virtual calls.
//   - CSVTracingOutput uses a 256-entry internal buffer so file I/O is
//     amortised across many blob completions, keeping the Tracing hot path
//     free of per-blob I/O latency.
// ============================================================================

#include "ConfigManager.hpp"  // SystemConfig

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>


// ============================================================================
// CompletedBlob
// ----------------------------------------------------------------------------
// Immutable record emitted once per label lifecycle, carrying all statistics
// accumulated over the label's lifetime.
//
// Layout (8-byte aligned, sizeof == 48):
//   [ 0] label        uint16  — the canonical label ID (after find())
//   [ 2] _pad[6]      uint8   — explicit pad → 8-byte alignment for uint64s
//   [ 8] pixel_count  uint64  — total foreground pixels under this label
//   [16] top_row      uint64  — minimum scan row (earliest row seen)
//   [24] bottom_row   uint64  — maximum scan row (latest row seen)
//   [32] left_col     uint64  — minimum column (leftmost pixel)
//   [40] right_col    uint64  — maximum column (rightmost pixel)
//
// Derived fields (not stored; computed by output sinks):
//   width  = right_col  - left_col  + 1
//   height = bottom_row - top_row   + 1
//
// A CompletedBlob with pixel_count == 0 is emitted only when a label is
// recycled before any pixel was assigned to it (should not occur under
// normal labelling; treated as a no-op by output sinks).
// ============================================================================

struct CompletedBlob {
    uint16_t label       = 0;
    uint8_t  _pad[6]     = {};
    uint64_t pixel_count = 0;
    uint64_t top_row     = 0;
    uint64_t bottom_row  = 0;
    uint64_t left_col    = 0;
    uint64_t right_col   = 0;
};

static_assert(std::is_trivially_copyable<CompletedBlob>::value,
              "CompletedBlob must be trivially copyable for future queue use");

static_assert(sizeof(CompletedBlob) == 48,
              "CompletedBlob layout changed — verify _pad and field order");


// ============================================================================
// BlobAccumulator
// ----------------------------------------------------------------------------
// Per-label working statistics maintained by TracingBlock.
// Stored in a flat array indexed by label ID (0-based, slot 0 unused).
//
// NOT sent over any queue — internal state only.
//
// active flag:
//   false → slot has never been used, or was reset after a recycle.
//           Calling update() on an inactive slot implicitly activates it.
//   true  → at least one pixel has been assigned to this label.
//
// Sentinel values:
//   top_row  = UINT64_MAX  → no row seen yet (min sentinel)
//   left_col = UINT64_MAX  → no col seen yet (min sentinel)
//   bottom_row = 0         → no row seen yet (max sentinel, safe because rows
//                            are always >= 0)
//   right_col  = 0         → no col seen yet (same)
//
// Thread safety: none. All methods must be called from TracingBlock's thread.
// ============================================================================

struct BlobAccumulator {
    uint64_t pixel_count = 0;
    uint64_t top_row     = std::numeric_limits<uint64_t>::max();
    uint64_t bottom_row  = 0;
    uint64_t left_col    = std::numeric_limits<uint64_t>::max();
    uint64_t right_col   = 0;
    bool     active      = false;

    // ------------------------------------------------------------------
    // Update with one pixel at (row, col).
    // Implicitly activates the accumulator on first call.
    // All six fields update unconditionally — no branch on active.
    // ------------------------------------------------------------------
    void update(uint64_t row, uint64_t col) noexcept {
        ++pixel_count;
        if (row < top_row)    top_row    = row;
        if (row > bottom_row) bottom_row = row;
        if (col < left_col)   left_col   = col;
        if (col > right_col)  right_col  = col;
        active = true;
    }

    // ------------------------------------------------------------------
    // Merge `other` (absorbed label) into `this` (surviving label).
    // Called when a merge event fires: absorbed → surviving.
    // After merging, `other` should be reset() by the caller.
    // ------------------------------------------------------------------
    void merge(const BlobAccumulator& other) noexcept {
        pixel_count += other.pixel_count;
        if (other.top_row    < top_row)    top_row    = other.top_row;
        if (other.bottom_row > bottom_row) bottom_row = other.bottom_row;
        if (other.left_col   < left_col)   left_col   = other.left_col;
        if (other.right_col  > right_col)  right_col  = other.right_col;
        active = active || other.active;
    }

    // ------------------------------------------------------------------
    // Produce an immutable CompletedBlob snapshot.
    // Caller must pass the canonical root label after find().
    // ------------------------------------------------------------------
    CompletedBlob finalise(uint16_t label) const noexcept {
        CompletedBlob b;
        b.label       = label;
        b.pixel_count = pixel_count;
        // Collapse sentinel values to 0 for blobs that saw no pixels.
        b.top_row     = (top_row  == std::numeric_limits<uint64_t>::max()) ? 0 : top_row;
        b.bottom_row  = bottom_row;
        b.left_col    = (left_col == std::numeric_limits<uint64_t>::max()) ? 0 : left_col;
        b.right_col   = right_col;
        return b;
    }

    // ------------------------------------------------------------------
    // Reset to construction state.
    // Called after a label is recycled or when an absorbed accumulator
    // has been merged into the surviving label's accumulator.
    // ------------------------------------------------------------------
    void reset() noexcept {
        pixel_count = 0;
        top_row     = std::numeric_limits<uint64_t>::max();
        bottom_row  = 0;
        left_col    = std::numeric_limits<uint64_t>::max();
        right_col   = 0;
        active      = false;
    }
};


// ============================================================================
// ITracingOutput
// ----------------------------------------------------------------------------
// Abstract sink for CompletedBlob records.  Mirrors IOutputWriter in its role.
//
// emit(blob)  — called for each completed blob.  May buffer internally.
// flush()     — flush all buffered blobs.  Called once at pipeline shutdown.
//
// Implementations must be safe to call from a single thread (TracingBlock's
// run() thread).  No cross-thread safety is required.
// ============================================================================

class ITracingOutput {
public:
    virtual void emit(const CompletedBlob& blob) = 0;
    virtual void flush() = 0;
    virtual ~ITracingOutput() = default;
};


// ============================================================================
// NullTracingOutput
// ----------------------------------------------------------------------------
// Zero-overhead sink used when write_output = false.
// No heap allocation, no virtual function body.
// ============================================================================

class NullTracingOutput : public ITracingOutput {
public:
    void emit(const CompletedBlob&) override {}
    void flush()                    override {}
};


// ============================================================================
// CSVTracingOutput
// ----------------------------------------------------------------------------
// Buffered CSV file sink.  Accumulates blobs in an internal vector and flushes
// to disk every 256 entries (or on explicit flush()).
//
// CSV columns: label, pixel_count, top_row, bottom_row, left_col, right_col,
//              width (derived), height (derived)
//
// Buffering strategy:
//   256 entries × 48 bytes ≈ 12 KB buffer — small enough to be L1-resident.
//   Flush is called every 256 entries or at shutdown, so for a run producing
//   10,000 blobs only ~40 file write calls are made — negligible I/O overhead.
// ============================================================================

class CSVTracingOutput : public ITracingOutput {
public:
    explicit CSVTracingOutput(const std::string& path)
        : path_(path)
    {
        file_.open(path, std::ios::out | std::ios::trunc);
        if (!file_.is_open())
            throw std::runtime_error(
                "CSVTracingOutput: cannot open '" + path + "'");

        std::cout << "[TracingOutput] Writing blobs to: " << path << '\n';

        file_ << "label,pixel_count,top_row,bottom_row,"
                 "left_col,right_col,width,height\n";

        buffer_.reserve(FLUSH_THRESHOLD);
    }

    ~CSVTracingOutput() {
        if (file_.is_open()) {
            flush();
            file_.close();
            std::cout << "[TracingOutput] " << total_emitted_
                      << " blobs written to " << path_ << '\n';
        }
    }

    void emit(const CompletedBlob& b) override {
        buffer_.push_back(b);
        if (buffer_.size() >= FLUSH_THRESHOLD) flush();
    }

    void flush() override {
        for (const auto& b : buffer_) {
            // Derived dimensions — guard against sentinel values
            const uint64_t w = (b.right_col  >= b.left_col)
                             ? (b.right_col  - b.left_col  + 1) : 0;
            const uint64_t h = (b.bottom_row >= b.top_row)
                             ? (b.bottom_row - b.top_row   + 1) : 0;

            file_ << b.label        << ','
                  << b.pixel_count  << ','
                  << b.top_row      << ','
                  << b.bottom_row   << ','
                  << b.left_col     << ','
                  << b.right_col    << ','
                  << w              << ','
                  << h              << '\n';
            ++total_emitted_;
        }
        file_.flush();
        buffer_.clear();
    }

private:
    static constexpr size_t FLUSH_THRESHOLD = 256;

    std::string               path_;
    std::ofstream             file_;
    std::vector<CompletedBlob> buffer_;
    uint64_t                  total_emitted_ = 0;
};


// ============================================================================
// StdoutTracingOutput
// ----------------------------------------------------------------------------
// Debug sink that prints each completed blob to stdout.
// Useful for interactive testing and evaluation demos.
// ============================================================================

class StdoutTracingOutput : public ITracingOutput {
public:
    void emit(const CompletedBlob& b) override {
        const uint64_t w = (b.right_col  >= b.left_col)
                         ? (b.right_col  - b.left_col  + 1) : 0;
        const uint64_t h = (b.bottom_row >= b.top_row)
                         ? (b.bottom_row - b.top_row   + 1) : 0;

        std::cout << "[Blob] label="  << b.label
                  << " pixels="       << b.pixel_count
                  << " rows=["        << b.top_row    << "–" << b.bottom_row << "]"
                  << " cols=["        << b.left_col   << "–" << b.right_col  << "]"
                  << " bbox="         << w << "×" << h
                  << '\n';
        ++total_emitted_;
    }

    void flush() override {
        std::cout.flush();
        std::cout << "[TracingOutput] " << total_emitted_ << " blobs emitted.\n";
    }

private:
    uint64_t total_emitted_ = 0;
};


// ============================================================================
// makeTracingOutput
// ----------------------------------------------------------------------------
// Factory function — selects the appropriate ITracingOutput implementation
// based on SystemConfig.
//
// If write_output is false : NullTracingOutput (no I/O overhead).
// If write_output is true  : CSVTracingOutput writing to a derived path.
//
// The blob output file is named <output_file>_blobs.csv to avoid clobbering
// the labelled-pixel output CSV produced by the label stage.
// ============================================================================

inline std::unique_ptr<ITracingOutput>
makeTracingOutput(const SystemConfig& cfg) {
    if (!cfg.write_output)
        return std::make_unique<NullTracingOutput>();

    const std::string path = cfg.output_file.empty()
                           ? "blobs.csv"
                           : cfg.output_file + "_blobs.csv";
    return std::make_unique<CSVTracingOutput>(path);
}

#endif // TRACING_UTILS_HPP
