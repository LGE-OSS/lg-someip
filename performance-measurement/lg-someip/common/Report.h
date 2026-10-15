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

#ifndef LG_SOMEIP_PERF_REPORT_H
#define LG_SOMEIP_PERF_REPORT_H

#include <sys/resource.h>
#include <sys/time.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "LatencyStats.h"

#ifndef PERF_GIT_REVISION
#define PERF_GIT_REVISION "unknown"
#endif

namespace perf {

// CPU time the calling thread has spent busy-waiting for send schedules (see
// wait_until_ns() in Pacing.h). It is benchmark overhead, not stack cost, so
// ResourceUsage reports it separately. Read from the thread that paces sends.
inline std::uint64_t& pacing_cpu_ns() {
    static thread_local std::uint64_t total_ns = 0;
    return total_ns;
}

// CPU time consumed by the calling thread, from the scheduler's own accounting
// (nanosecond resolution, unlike the tick-based getrusage() totals).
inline std::uint64_t thread_cpu_ns() {
    timespec now{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(now.tv_nsec);
}

// CPU time consumed by all threads of this process, from the scheduler's own
// accounting (nanosecond resolution).
inline double process_cpu_ms() {
    timespec now{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
    return static_cast<double>(now.tv_sec) * 1000.0 + static_cast<double>(now.tv_nsec) / 1e6;
}

// Process CPU time, peak memory, and context switches. cpu_ms comes from the
// process CPU clock. getrusage() may charge CPU in whole scheduler ticks (4 to
// 10 ms), so the user/system split can be coarse or one-sided; compare cpu_ms
// (reported as cpu_total_ms) instead.
// pacing_ms is the part of that time the pacing thread spent waiting for the
// next scheduled send; cpu_net_ms removes it so runs at different rates and
// different stacks compare the work done for the messages themselves.
struct ResourceUsage {
    double user_ms{0};
    double system_ms{0};
    double cpu_ms{0};
    double pacing_ms{0};
    long max_rss_kb{0};
    long voluntary_switches{0};
    long involuntary_switches{0};

    static ResourceUsage sample() {
        struct rusage usage {};
        getrusage(RUSAGE_SELF, &usage);
        ResourceUsage result;
        result.user_ms = usage.ru_utime.tv_sec * 1000.0 + usage.ru_utime.tv_usec / 1000.0;
        result.system_ms = usage.ru_stime.tv_sec * 1000.0 + usage.ru_stime.tv_usec / 1000.0;
        result.cpu_ms = process_cpu_ms();
        result.pacing_ms = static_cast<double>(pacing_cpu_ns()) / 1e6;
        result.max_rss_kb = usage.ru_maxrss;
        result.voluntary_switches = usage.ru_nvcsw;
        result.involuntary_switches = usage.ru_nivcsw;
        return result;
    }

    // Counters are deltas; peak RSS stays the absolute process high-water mark.
    ResourceUsage since(const ResourceUsage& start) const {
        ResourceUsage delta(*this);
        delta.user_ms -= start.user_ms;
        delta.system_ms -= start.system_ms;
        delta.cpu_ms -= start.cpu_ms;
        delta.pacing_ms -= start.pacing_ms;
        delta.voluntary_switches -= start.voluntary_switches;
        delta.involuntary_switches -= start.involuntary_switches;
        return delta;
    }
};

// Resource usage of a provider between its first and its last observed
// request, so idle start-up and shut-down time stays out of the figure. Call
// note_request() from the request callback and poll() regularly from the main
// thread; the window ends at the last poll that saw new requests.
class TrafficWindow {
public:
    void note_request() {
        if (started_.load(std::memory_order_acquire)) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_.load(std::memory_order_relaxed)) {
            first_ = ResourceUsage::sample();
            started_.store(true, std::memory_order_release);
        }
    }

    void poll(std::uint64_t requests_served) {
        if (started_.load(std::memory_order_acquire) && requests_served != last_served_) {
            last_served_ = requests_served;
            last_ = ResourceUsage::sample();
        }
    }

    // Usage between the first request and the last poll that saw a new one.
    ResourceUsage usage() const {
        return started_.load(std::memory_order_acquire) ? last_.since(first_) : ResourceUsage();
    }

private:
    std::mutex mutex_;
    std::atomic<bool> started_{false};
    ResourceUsage first_;
    ResourceUsage last_;
    std::uint64_t last_served_{0};
};

// Ordered list of named result columns. Each scenario writes its own CSV file
// so the header stays stable across runs of the same scenario.
class ResultRow {
public:
    template <typename T> ResultRow& add(const std::string& name, const T& value) {
        std::ostringstream text;
        text << std::fixed << std::setprecision(3) << value;
        columns_.emplace_back(name, text.str());
        return *this;
    }

    ResultRow& add_latency(const LatencySummary& summary) {
        return add("samples", summary.count)
            .add("min_us", summary.min_us)
            .add("mean_us", summary.mean_us)
            .add("stddev_us", summary.stddev_us)
            .add("p50_us", summary.p50_us)
            .add("p90_us", summary.p90_us)
            .add("p99_us", summary.p99_us)
            .add("p999_us", summary.p999_us)
            .add("max_us", summary.max_us);
    }

    ResultRow& add_resources(const ResourceUsage& usage) {
        const double total_ms = usage.cpu_ms;
        return add("cpu_user_ms", usage.user_ms)
            .add("cpu_sys_ms", usage.system_ms)
            .add("cpu_total_ms", total_ms)
            .add("cpu_pacing_ms", usage.pacing_ms)
            .add("cpu_net_ms", std::max(total_ms - usage.pacing_ms, 0.0))
            .add("max_rss_kb", usage.max_rss_kb)
            .add("vol_ctx_switches", usage.voluntary_switches)
            .add("invol_ctx_switches", usage.involuntary_switches);
    }

    void print(std::ostream& out) const {
        std::size_t width = 0;
        for (const auto& column : columns_) {
            width = std::max(width, column.first.size());
        }
        for (const auto& column : columns_) {
            out << "  " << std::left << std::setw(static_cast<int>(width)) << column.first << " : " << column.second
                << '\n';
        }
    }

    // Append the row, writing the header first when the file is new or empty.
    bool append_csv(const std::string& path) const {
        bool need_header = false;
        {
            std::ifstream existing(path);
            need_header = !existing || existing.peek() == std::ifstream::traits_type::eof();
        }
        std::ofstream out(path, std::ios::app);
        if (!out) {
            return false;
        }
        if (need_header) {
            write_line(out, true);
        }
        write_line(out, false);
        return static_cast<bool>(out);
    }

private:
    // Quote a field that holds a comma, a quote, or a line break (RFC 4180),
    // so a free-text --label cannot shift the columns of its row.
    static std::string csv_field(const std::string& text) {
        if (text.find_first_of(",\"\r\n") == std::string::npos) {
            return text;
        }
        std::string quoted = "\"";
        for (char c : text) {
            quoted += c == '"' ? std::string("\"\"") : std::string(1, c);
        }
        return quoted + "\"";
    }

    void write_line(std::ostream& out, bool header) const {
        for (std::size_t i = 0; i < columns_.size(); ++i) {
            out << (i == 0 ? "" : ",") << csv_field(header ? columns_[i].first : columns_[i].second);
        }
        out << '\n';
    }

    std::vector<std::pair<std::string, std::string>> columns_;
};

inline std::string utc_timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

} // namespace perf

#endif // LG_SOMEIP_PERF_REPORT_H
