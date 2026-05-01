#ifndef PERF_HPP
#define PERF_HPP


#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdint>

#include "FilterUtils.hpp"

struct PerfStats {
    uint64_t min_gap = UINT64_MAX;
    uint64_t max_gap = 0;
    double   avg_gap = 0.0;
    uint64_t p99_gap = 0;
    size_t   count   = 0;
};

inline PerfStats computePerfStats(std::vector<uint64_t>& gaps) {
    PerfStats stats;

    if (gaps.empty()) return stats;

    uint64_t sum = 0;

    for (uint64_t g : gaps) {
        if (g < stats.min_gap) stats.min_gap = g;
        if (g > stats.max_gap) stats.max_gap = g;
        sum += g;
    }

    stats.count   = gaps.size();
    stats.avg_gap = static_cast<double>(sum) / static_cast<double>(stats.count);

    std::sort(gaps.begin(), gaps.end());
    const size_t idx = static_cast<size_t>(0.99 * static_cast<double>(stats.count));
    stats.p99_gap = gaps[(idx < stats.count) ? idx : stats.count - 1];

    return stats;
}
struct Stats {
    uint64_t min = UINT64_MAX;
    uint64_t max = 0;
    double   avg = 0.0;
    uint64_t p99 = 0;
};

inline Stats computeStats(std::vector<uint64_t>& gaps) {
    Stats s;
    if (gaps.empty()) return s;

    uint64_t sum = 0;
    for (uint64_t g : gaps) {
        if (g < s.min) s.min = g;
        if (g > s.max) s.max = g;
        sum += g;
    }
    s.avg = static_cast<double>(sum) / static_cast<double>(gaps.size());

    std::sort(gaps.begin(), gaps.end());
    const size_t idx = static_cast<size_t>(0.99 * static_cast<double>(gaps.size()));
    s.p99 = gaps[(idx < gaps.size()) ? idx : gaps.size() - 1];

    return s;
}

struct LinearStats {
    uint64_t min_ns = UINT64_MAX;
    uint64_t max_ns = 0;
    uint64_t p50_ns = 0;
    uint64_t p99_ns = 0;
    double   avg_ns = 0.0;
    size_t   count  = 0;
};
inline LinearStats computeLinearStats(const std::vector<uint64_t>& timestamps) {
    LinearStats s;
    if (timestamps.size() < 2) return s;

    std::vector<uint64_t> gaps;
    gaps.reserve(timestamps.size() - 1);
    for (size_t i = 1; i < timestamps.size(); ++i) {
        if (timestamps[i] >= timestamps[i - 1])
            gaps.push_back(timestamps[i] - timestamps[i - 1]);
    }

    if (gaps.empty()) return s;

    uint64_t sum = 0;
    for (uint64_t g : gaps) {
        if (g < s.min_ns) s.min_ns = g;
        if (g > s.max_ns) s.max_ns = g;
        sum += g;
    }
    s.count  = gaps.size();
    s.avg_ns = static_cast<double>(sum) / static_cast<double>(s.count);

    std::sort(gaps.begin(), gaps.end());
    s.p50_ns = gaps[s.count / 2];
    const size_t idx = static_cast<size_t>(0.99 * static_cast<double>(s.count));
    s.p99_ns = gaps[(idx < s.count) ? idx : s.count - 1];

    return s;
}

inline void printLinearStats(const LinearStats& s, uint64_t budget_ns) {
    std::cout << "========================================\n"
              << " Performance Report  (single thread)\n"
              << "========================================\n"
              << " Samples      : " << s.count   << "\n"
              << " Min gap (ns) : " << s.min_ns  << "\n"
              << " Max gap (ns) : " << s.max_ns  << "\n"
              << " Avg gap (ns) : " << s.avg_ns  << "\n"
              << " P50 gap (ns) : " << s.p50_ns  << "\n"
              << " P99 gap (ns) : " << s.p99_ns  << "\n"
              << " Budget T(ns) : " << budget_ns << "\n";

    if (s.max_ns <= budget_ns)
        std::cout << " RESULT       : PASS\n";
    else if (s.avg_ns <= static_cast<double>(budget_ns))
        std::cout << " RESULT       : AVG PASS / MAX FAIL  (OS jitter)\n";
    else
        std::cout << " RESULT       : FAIL\n";

    std::cout << "========================================\n\n";
}

#endif