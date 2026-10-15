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

#ifndef LG_SOMEIP_PERF_PACING_H
#define LG_SOMEIP_PERF_PACING_H

#include <chrono>
#include <cstdint>
#include <thread>

#include "PerfPayload.h"
#include "Report.h"

namespace perf {

// Below this distance to the target, wait_until_ns() yields instead of sleeping.
constexpr std::uint64_t kSpinWindowNs = 200000;

// Wait until the steady clock reaches target_ns. Sleep while far away and
// yield for the last stretch, because sleep_for() overshoots by tens of
// microseconds, which matters at high rates. The yielding is a busy-wait that
// would otherwise show up as stack CPU, so the CPU time spent here is added to
// pacing_cpu_ns() and reported as cpu_pacing_ms.
inline void wait_until_ns(std::uint64_t target_ns) {
    auto now = now_ns();
    if (now >= target_ns) {
        return;
    }
    const auto cpu_start = thread_cpu_ns();
    for (; now < target_ns; now = now_ns()) {
        if (target_ns - now > kSpinWindowNs) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(target_ns - now - kSpinWindowNs));
        } else {
            std::this_thread::yield();
        }
    }
    pacing_cpu_ns() += thread_cpu_ns() - cpu_start;
}

// Start barrier for several peers: wait until CLOCK_REALTIME reaches start_ms
// (milliseconds since the epoch), so that all requesters or publishers of one
// measurement begin together, on one host or on PTP-synchronized hosts.
// 0 means "no barrier". Returns how late the caller arrived, in milliseconds
// (0 when it had to wait); a late peer started after the others.
inline double wait_until_realtime_ms(std::uint64_t start_ms) {
    if (start_ms == 0) {
        return 0.0;
    }
    const auto target_ns = start_ms * 1000000ull;
    const auto now = realtime_ns();
    if (now >= target_ns) {
        return static_cast<double>(now - target_ns) / 1e6;
    }
    std::this_thread::sleep_for(std::chrono::nanoseconds(target_ns - now));
    while (realtime_ns() < target_ns) {
        std::this_thread::yield();
    }
    return 0.0;
}

// Fixed-rate schedule: the i-th send is due at start + i / rate.
class Schedule {
public:
    Schedule(double rate, std::uint64_t start_ns) : period_ns_(rate > 0 ? 1e9 / rate : 0.0), start_ns_(start_ns) {}

    bool paced() const {
        return period_ns_ > 0;
    }

    std::uint64_t due_ns(std::uint64_t index) const {
        return start_ns_ + static_cast<std::uint64_t>(static_cast<double>(index) * period_ns_);
    }

private:
    double period_ns_;
    std::uint64_t start_ns_;
};

} // namespace perf

#endif // LG_SOMEIP_PERF_PACING_H
