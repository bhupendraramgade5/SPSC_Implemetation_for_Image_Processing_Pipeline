


#include <cstdint>
#include <vector>
#include <random>
#include <iostream>
#include <stdexcept>

#include "Queue.hpp"
#include "ConfigManager.hpp"
#include "GeneratorBlock.hpp"
#include <memory>

static constexpr uint64_t SPIN_THRESHOLD_NS = 1'000'000ULL;

std::unique_ptr<IDataSource> createDataSource(const SystemConfig& config) {
    if (config.mode == Mode::CSV) {
        return std::make_unique<CSVDataSource>(config.input_file, config.columns);
    }
    return std::make_unique<RandomDataSource>(config.columns);
}



RandomDataSource::RandomDataSource(size_t columns)
    : columns_(columns),
      rng_(std::random_device{}()),
      dist_(0, 255) 
{
    if(columns_ == 0) {
        throw std::invalid_argument("RandomDataSource: columns must be > 0");
    }
}

bool RandomDataSource::next(DataPacket& packet) {
    packet.v1 = static_cast<uint8_t>(dist_(rng_));
    packet.v2 = static_cast<uint8_t>(dist_(rng_));

    packet.row = row_;
    packet.col = col_;

    advance();
    return true;
}

void RandomDataSource::advance() {
    col_ += 2;
    if (col_ >= columns_) {
        col_ = 0;
        row_++;
    }
}


CSVDataSource::CSVDataSource(const std::string& file, size_t columns)
    : file_(file), columns_(columns) 
{
    // Check if file opened successfully
    if(!file_.is_open()){
        throw std::runtime_error(
            "CSVDataSource: cannot open file '" + file + "'");

    }
    // Check if columns is valid
    if(columns_ == 0) {
        throw std::invalid_argument("CSVDataSource: columns must be > 0");
    }
}

bool CSVDataSource::next(DataPacket& packet) {
    if (!file_.is_open()) return false;

    if (buffer_.size() < 2) {
        if (!loadNextRow()) return false;
    }

    if (buffer_.size() < 2) return false;

    packet.v1 = buffer_.front(); buffer_.pop_front();
    packet.v2 = buffer_.front(); buffer_.pop_front();

    // packet.row = row_; // Bug??

    packet.row = row_ - 1;   // row_ was already incremented by loadNextRow()
    packet.col = col_;

    advanceCol();
    return true;
}

bool CSVDataSource::loadNextRow() {
    std::string line;
    if (!std::getline(file_, line)) return false;
    buffer_.clear();

    std::stringstream ss(line);
    std::string val;
    // Current : Not Tolerant to white Spaces in CSV
    // while (std::getline(ss, val, ',')) {
    //     buffer_.push_back(static_cast<uint8_t>(std::stoi(val)));
    // }

    while (std::getline(ss, val, ',')) {
        const auto first = val.find_first_not_of(" \t\r\n");
        const auto last  = val.find_last_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
 
        const int value = std::stoi(val.substr(first, last - first + 1));
        buffer_.push_back(static_cast<uint8_t>(value));
    }

    if (buffer_.size() != columns_) {
        std::cerr << "CSVDataSource: row " << row_
                  << " has " << buffer_.size()
                  << " values, expected " << columns_ << '\n';
    }


    row_++;
    col_ = 0;
    return true;
}

void CSVDataSource::advanceCol() {
    col_ += 2;
    if (col_ >= columns_) {
        col_ = 0;
    }
}

GeneratorBlock::GeneratorBlock(const SystemConfig&          config,
                               IQueue<DataPacket>&           queue,
                               std::unique_ptr<IDataSource>  source)
    : config_(config)
    , queue_(queue)
    , source_(std::move(source))
    , stop_flag_(false)
{}


void GeneratorBlock::run() {
    const auto cycle = std::chrono::nanoseconds(config_.cycle_time_ns);
    const bool use_spin = (config_.cycle_time_ns < SPIN_THRESHOLD_NS);

    while (!stop_flag_.load(std::memory_order_relaxed)) {

        const auto deadline = std::chrono::steady_clock::now() + cycle;

        DataPacket packet{};
        if (!source_->next(packet)) {
            break;
        }
        
        queue_.push(packet);

        if (use_spin) {
            spinWaitUntil(deadline);
        } else {
            const auto now = std::chrono::steady_clock::now();
            if (now < deadline) {
                std::this_thread::sleep_for(deadline - now);
            }
        }
    }
}

void GeneratorBlock::stop() {
    stop_flag_.store(true, std::memory_order_relaxed);
}

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
