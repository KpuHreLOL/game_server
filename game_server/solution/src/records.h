#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace records {


struct Record {
    std::string name;
    int score{0};
    std::chrono::milliseconds play_time{0};
};

inline constexpr std::size_t kMaxRecordsPerRequest = 100;

class RecordsStorage {
public:
    virtual void Add(const Record& record) = 0;
    virtual std::vector<Record> Get(std::size_t start, std::size_t max_items) = 0;

protected:
    ~RecordsStorage() = default;
};

}  // namespace records
