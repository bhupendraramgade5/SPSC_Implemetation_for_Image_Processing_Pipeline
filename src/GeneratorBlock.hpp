#ifndef GENERATOR_BLOCK_HPP
#define GENERATOR_BLOCK_HPP


#include <atomic>  
#include <chrono>
#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>


#include <vector>


#include "Queue.hpp"


#include "ConfigManager.hpp"  



class RandomDataSource : public IDataSource {
public:
    explicit RandomDataSource(size_t columns);

    bool next(DataPacket& packet) override;

private:
    void advance();

private:
    size_t columns_;
    uint64_t row_ = 0;
    uint64_t col_ = 0;

    std::mt19937 rng_;
    std::uniform_int_distribution<int> dist_;
};


class CSVDataSource : public IDataSource {
public:
    CSVDataSource(const std::string& file, size_t columns);

    bool next(DataPacket& packet) override;

private:
    bool loadNextRow();
    // void advance(); // Changing name here definition
    void advanceCol();

private:
    std::ifstream file_;
    std::deque<uint8_t> buffer_;

    size_t columns_;
    uint64_t row_ = 0;
    uint64_t col_ = 0;
};



class GeneratorBlock {
public:
    GeneratorBlock(const SystemConfig& config,
        IQueue<DataPacket>& queue,
        std::unique_ptr<IDataSource> source);
    void run();
    void stop();



private:
    void spinWaitUntil(std::chrono::steady_clock::time_point deadline) const;

    const SystemConfig& config_;
    IQueue<DataPacket>& queue_;
    std::unique_ptr<IDataSource> source_;

    std::atomic<bool>            stop_flag_; // added new

};

std::unique_ptr<IDataSource> createDataSource(const SystemConfig& config);

#endif