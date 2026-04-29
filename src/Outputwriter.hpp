#pragma once

// OutputWriter.hpp
// Adapter pattern: decouples the filter-output sink from the pipeline logic.
//
// The filter produces FilteredPacket (b1, b2, row, col).
// Each packet carries two filtered pixels emitted at the same moment.
//
// Three implementations:
//   NullOutputWriter   – no-op; used when write_output = false
//   CSVOutputWriter    – writes row,col,b1,b2 to a CSV file (buffered)
//   StdoutOutputWriter – prints to console (debugging)
//
// NOTE ON LATENCY
//   File I/O is intentionally outside the 100 ns timing budget.
//   The pipeline measures filter latency separately; the writer is a
//   post-processing sink, not part of the critical path.

#include <cstdint>
#include <string>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <stdexcept>

#include "FilterUtils.hpp"   // FilteredPacket

// ---------------------------------------------------------------------------
// Interface
// ---------------------------------------------------------------------------
class IOutputWriter {
public:
    virtual ~IOutputWriter() = default;

    /// Write one FilteredPacket (two pixels, same row, adjacent columns).
    virtual void write(const FilteredPacket& fp) = 0;

    /// Flush and close.  Called once when the pipeline is done.
    virtual void finalize() = 0;
};

// ---------------------------------------------------------------------------
// NullOutputWriter  –  zero overhead, used when write_output = false
// ---------------------------------------------------------------------------
class NullOutputWriter : public IOutputWriter {
public:
    void write(const FilteredPacket&) override {}
    void finalize()                   override {}
};
class CSVOutputWriter : public IOutputWriter {
public:
    explicit CSVOutputWriter(const std::string& path)
        : path_(path), written_(0)
    {
        // Create parent directories if missing
        namespace fs = std::filesystem;
        fs::path p(path);
        if (p.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
            if (ec)
                throw std::runtime_error(
                    "OutputWriter: cannot create directory '" +
                    p.parent_path().string() + "': " + ec.message());
        }

        file_.open(path, std::ios::out | std::ios::trunc);
        if (!file_.is_open())
            throw std::runtime_error(
                "OutputWriter: cannot open file '" + path + "'");

        file_ << "row,col,b1,b2\n";
    }

    ~CSVOutputWriter() { if (file_.is_open()) file_.close(); }

    void write(const FilteredPacket& fp) override {
        file_ << fp.row << ','
              << fp.col << ','
              << static_cast<int>(fp.b1) << ','
              << static_cast<int>(fp.b2) << '\n';
        ++written_;
        if (written_ % 1000 == 0) file_.flush();
    }

    void finalize() override {
        if (file_.is_open()) {
            file_.flush();
            file_.close();
            std::cout << "[OutputWriter] " << written_
                      << " packets written to " << path_ << '\n';
        }
    }

private:
    std::string   path_;
    std::ofstream file_;
    uint64_t      written_;
};
class StdoutOutputWriter : public IOutputWriter {
public:
    void write(const FilteredPacket& fp) override {
        std::cout << "row=" << fp.row
                  << " col=" << fp.col
                  << " b1=" << static_cast<int>(fp.b1)
                  << " b2=" << static_cast<int>(fp.b2) << '\n';
    }
    void finalize() override {}
};

#include "ConfigManager.hpp"  // SystemConfig

inline std::unique_ptr<IOutputWriter>
makeOutputWriter(const SystemConfig& cfg) {
    if (!cfg.write_output)
        return std::make_unique<NullOutputWriter>();
    return std::make_unique<CSVOutputWriter>(cfg.output_file);
}