#include <cstdint>
#include <vector>
#include <random>
#include "Queue.hpp"
#include <fstream>
#include <sstream>
#include <thread>

//class IDataSource {
//public:
//    virtual bool next(DataPacket& packet) = 0;
//    virtual ~IDataSource() = default;
//};

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
    void advance();

private:
    std::ifstream file_;
    std::deque<uint8_t> buffer_;

    size_t columns_;
    uint64_t row_ = 0;
    uint64_t col_ = 0;
};


