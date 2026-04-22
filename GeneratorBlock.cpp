#include <cstdint>
#include <vector>
#include <random>
#include "Queue.hpp"
#include "ConfigManager.hpp"
#include "GeneratorBlock.hpp"

class GeneratorBlock {
public:
    GeneratorBlock(const SystemConfig& config,
                   IQueue<DataPacket>& queue,
                   std::unique_ptr<IDataSource> source)
        : config_(config), queue_(queue), source_(std::move(source)) {}

    void run() {
        while (true) {
            auto start = std::chrono::steady_clock::now();

            DataPacket packet;
            if (!source_->next(packet)) break;

            queue_.push(packet);

            auto end = std::chrono::steady_clock::now();
            auto elapsed = end - start;

            auto target = std::chrono::nanoseconds(config_.cycle_time_ns);
            if (elapsed < target)
                std::this_thread::sleep_for(target - elapsed);
        }
    }

private:
    const SystemConfig& config_;
    IQueue<DataPacket>& queue_;
    std::unique_ptr<IDataSource> source_;
};


RandomDataSource::RandomDataSource(size_t columns)
    : columns_(columns),
      rng_(std::random_device{}()),
      dist_(0, 255) {}

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
    : file_(file), columns_(columns) {}

bool CSVDataSource::next(DataPacket& packet) {
    if (!file_.is_open()) return false;

    if (buffer_.size() < 2) {
        if (!loadNextRow()) return false;
    }

    if (buffer_.size() < 2) return false;

    packet.v1 = buffer_.front(); buffer_.pop_front();
    packet.v2 = buffer_.front(); buffer_.pop_front();

    packet.row = row_;
    packet.col = col_;

    advance();
    return true;
}

bool CSVDataSource::loadNextRow() {
    std::string line;
    if (!std::getline(file_, line)) return false;

    std::stringstream ss(line);
    std::string val;

    while (std::getline(ss, val, ',')) {
        buffer_.push_back(static_cast<uint8_t>(std::stoi(val)));
    }

    row_++;
    col_ = 0;
    return true;
}

void CSVDataSource::advance() {
    col_ += 2;
    if (col_ >= columns_) {
        col_ = 0;
    }
}