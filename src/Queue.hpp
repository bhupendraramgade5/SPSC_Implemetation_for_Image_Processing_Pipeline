

#ifndef IQUEUE_HPP
#define IQUEUE_HPP


#include <queue>
#include <mutex>


template <typename T>
class IQueue {
public:
    virtual bool push(const T& item) = 0;
    virtual bool pop(T& item) = 0;
    virtual ~IQueue() = default;
};



template <typename T>
class SimpleQueue : public IQueue<T> {
public:
    bool push(const T& item) override {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(item);
        return true;
    }

    bool pop(T& item) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        item = queue_.front();
        queue_.pop();
        return true;
    }

private:
    std::queue<T> queue_;
    std::mutex mutex_;
};


#include <cstdint>

struct DataPacket {
    uint8_t v1;
    uint8_t v2;
    uint64_t row;
    uint64_t col;
};

class IDataSource {
public:
    virtual bool next(DataPacket& packet) = 0;
    virtual ~IDataSource() = default;
};

#endif // SIMPLE_QUEUE_HPP

