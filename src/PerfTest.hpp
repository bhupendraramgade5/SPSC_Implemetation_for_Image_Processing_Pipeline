#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdint>

#include "FilterUtils.hpp"

struct Stats {
    uint64_t min = UINT64_MAX;
    uint64_t max = 0;
    double avg = 0.0;
    uint64_t p99 = 0;
};


struct PerfStats {
    uint64_t min_gap = UINT64_MAX;
    uint64_t max_gap = 0;
    double   avg_gap = 0.0;
    uint64_t p99_gap = 0;
    size_t   count   = 0;
};

PerfStats computePerfStats(std::vector<uint64_t>& gaps) {
    PerfStats stats;

    if (gaps.empty()) return stats;

    uint64_t sum = 0;

    for (uint64_t g : gaps) {
        if (g < stats.min_gap) stats.min_gap = g;
        if (g > stats.max_gap) stats.max_gap = g;
        sum += g;
    }

    stats.count = gaps.size();
    stats.avg_gap = static_cast<double>(sum) / gaps.size();

    std::sort(gaps.begin(), gaps.end());
    size_t idx = static_cast<size_t>(0.99 * gaps.size());
    if (idx >= gaps.size()) idx = gaps.size() - 1;
    stats.p99_gap = gaps[idx];

    return stats;
}