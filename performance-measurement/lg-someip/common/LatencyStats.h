/********************************************************************************
 * Copyright (C) 2026 LG Electronics Inc.
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ********************************************************************************/

#ifndef LG_SOMEIP_PERF_LATENCY_STATS_H
#define LG_SOMEIP_PERF_LATENCY_STATS_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace perf {

struct LatencySummary {
    std::size_t count{0};
    double min_us{0};
    double mean_us{0};
    double stddev_us{0};
    double p50_us{0};
    double p90_us{0};
    double p99_us{0};
    double p999_us{0};
    double max_us{0};
};

// Keeps every sample so percentiles are exact. One million samples cost 8 MB,
// which is acceptable for benchmark runs and avoids histogram bucket error.
class LatencyStats {
public:
    void reserve(std::size_t count) {
        samples_ns_.reserve(count);
    }

    void add(std::uint64_t sample_ns) {
        samples_ns_.push_back(sample_ns);
    }

    // Merge the samples of another collector, for per-thread collection.
    void append(const LatencyStats& other) {
        samples_ns_.insert(samples_ns_.end(), other.samples_ns_.begin(), other.samples_ns_.end());
    }

    std::size_t count() const {
        return samples_ns_.size();
    }

    LatencySummary summarize() const {
        LatencySummary summary;
        summary.count = samples_ns_.size();
        if (summary.count == 0) {
            return summary;
        }

        std::vector<std::uint64_t> sorted(samples_ns_);
        std::sort(sorted.begin(), sorted.end());

        double sum = 0;
        for (auto sample : sorted) {
            sum += static_cast<double>(sample);
        }
        double mean = sum / static_cast<double>(summary.count);
        double squares = 0;
        for (auto sample : sorted) {
            double diff = static_cast<double>(sample) - mean;
            squares += diff * diff;
        }

        summary.min_us = to_us(sorted.front());
        summary.max_us = to_us(sorted.back());
        summary.mean_us = mean / 1000.0;
        summary.stddev_us = std::sqrt(squares / static_cast<double>(summary.count)) / 1000.0;
        summary.p50_us = to_us(percentile(sorted, 50.0));
        summary.p90_us = to_us(percentile(sorted, 90.0));
        summary.p99_us = to_us(percentile(sorted, 99.0));
        summary.p999_us = to_us(percentile(sorted, 99.9));
        return summary;
    }

    // Write one sample per line in microseconds, in arrival order, for plotting.
    bool write_samples(const std::string& path) const {
        std::ofstream out(path);
        if (!out) {
            return false;
        }
        for (auto sample : samples_ns_) {
            out << to_us(sample) << '\n';
        }
        return static_cast<bool>(out);
    }

private:
    static double to_us(std::uint64_t ns) {
        return static_cast<double>(ns) / 1000.0;
    }

    // Nearest-rank percentile on an ascending vector.
    static std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, double pct) {
        auto rank = static_cast<std::size_t>(std::ceil(pct / 100.0 * static_cast<double>(sorted.size())));
        rank = std::max<std::size_t>(rank, 1);
        return sorted[std::min(rank, sorted.size()) - 1];
    }

    std::vector<std::uint64_t> samples_ns_;
};

} // namespace perf

#endif // LG_SOMEIP_PERF_LATENCY_STATS_H
