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

#ifndef LG_SOMEIP_PERF_THREAD_STRESS_H
#define LG_SOMEIP_PERF_THREAD_STRESS_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common/LatencyStats.h"
#include "common/PerfPayload.h"
#include "common/Report.h"

// Stack-agnostic part of the multi-threaded requester ("thread stress").
//
// One process runs N worker threads. Each worker repeatedly sends one request
// and blocks until its response arrives, as an application with many request
// threads would. The stack adapter supplies a single function, call(), that
// sends the payload and waits for the matching response. Everything else (the
// pacing, the counters, the CPU accounting, the result columns) is shared, so
// the stacks differ only in the code under test.
//
// The response is the same size as the request: the providers echo the
// payload, and call() checks that the size came back unchanged.
namespace perf {

// A sequence number carries the index of the worker that owns the request in
// its top bits, so a response callback finds the waiting worker without a map.
constexpr unsigned kSequenceThreadShift = 22;
constexpr std::uint32_t kSequenceCounterMask = (1u << kSequenceThreadShift) - 1;
constexpr std::size_t kMaxStressThreads = 1u << (32 - kSequenceThreadShift);

inline std::uint32_t make_sequence(std::size_t thread, std::uint64_t counter) {
    // The counter is never zero: sequence 0 means "nothing pending" elsewhere.
    return (static_cast<std::uint32_t>(thread) << kSequenceThreadShift) |
           (static_cast<std::uint32_t>(counter % kSequenceCounterMask) + 1);
}

inline std::size_t sequence_thread(std::uint32_t sequence) {
    return sequence >> kSequenceThreadShift;
}

// One slot per worker. A response that arrives after its request timed out is
// recognized by its sequence number and counted as late instead of completing
// the worker's next request.
class ResponseSlots {
public:
    explicit ResponseSlots(std::size_t count) : slots_(count) {
    }

    void arm(std::size_t thread, std::uint32_t sequence) {
        auto& slot = slots_[thread];
        std::lock_guard<std::mutex> lock(slot.mutex);
        slot.expected = sequence;
        slot.done = false;
    }

    // Called from the stack's response callback with the size of the response
    // payload. Returns false for a response nobody waits for.
    bool complete(std::uint32_t sequence, std::size_t size) {
        auto thread = sequence_thread(sequence);
        if (thread >= slots_.size()) {
            return false;
        }
        auto& slot = slots_[thread];
        {
            std::lock_guard<std::mutex> lock(slot.mutex);
            if (slot.expected != sequence || slot.done) {
                return false;
            }
            slot.done = true;
            slot.size = size;
        }
        slot.condition.notify_one();
        return true;
    }

    // Returns the response payload size, or 0 when the timeout expired first.
    std::size_t wait(std::size_t thread, std::chrono::milliseconds timeout) {
        auto& slot = slots_[thread];
        std::unique_lock<std::mutex> lock(slot.mutex);
        return slot.condition.wait_for(lock, timeout, [&slot] { return slot.done; }) ? slot.size : 0;
    }

private:
    struct alignas(64) Slot {
        std::mutex mutex;
        std::condition_variable condition;
        std::uint32_t expected{0};
        std::size_t size{0};
        bool done{false};
    };
    std::vector<Slot> slots_;
};

struct ThreadStressConfig {
    std::size_t threads{1};
    std::size_t payload_size{64};
    double warmup_s{1.0};
    double duration_s{5.0};
    // Requests per second of one worker; 0 sends the next request as soon as
    // the previous response arrived. A paced worker sleeps between requests,
    // so the pacing costs no CPU: the CPU figure is the cost of the stack.
    double thread_rate{0};
    std::chrono::milliseconds timeout{1000};
};

struct ThreadStressResult {
    LatencyStats stats;
    // Every call of the run, warm-up and timed-out calls included. Host-level CPU
    // figures (daemon, provider) cover the whole run, so they divide by this.
    std::uint64_t calls{0};
    std::uint64_t sent{0};
    std::uint64_t lost{0};
    std::uint64_t bad_size{0};
    double duration_s{0};
    ResourceUsage usage;
    std::size_t threads_started{0};
};

// call(thread, sequence, send_ns, payload) sends the payload (already stamped
// with the sequence and the send time) and waits for its response. It returns
// the size of the response payload, or 0 when no matching response arrived in
// time. It runs on the worker thread.
template <typename CallFn> ThreadStressResult run_thread_stress(const ThreadStressConfig& config, CallFn&& call) {
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> measure_start_ns{0};
    std::atomic<std::uint64_t> measure_end_ns{0};
    std::atomic<std::size_t> ready{0};

    struct WorkerResult {
        LatencyStats stats;
        std::uint64_t calls{0};
        std::uint64_t sent{0};
        std::uint64_t lost{0};
        std::uint64_t bad_size{0};
    };
    std::vector<std::unique_ptr<WorkerResult>> results;
    std::vector<std::thread> workers;
    for (std::size_t index = 0; index < config.threads; ++index) {
        results.push_back(std::make_unique<WorkerResult>());
    }

    for (std::size_t index = 0; index < config.threads; ++index) {
        workers.emplace_back([&, index] {
            auto& result = *results[index];
            Payload payload = make_payload(config.payload_size);
            ++ready;
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const auto period_ns =
                config.thread_rate > 0 ? static_cast<std::uint64_t>(1e9 / config.thread_rate) : std::uint64_t{0};
            // Spread paced workers over one period so they do not all send in the same instant.
            std::uint64_t next_ns = now_ns() + (period_ns == 0 ? 0 : period_ns * index / config.threads);
            for (std::uint64_t counter = 0; !stop.load(std::memory_order_relaxed); ++counter) {
                if (period_ns != 0) {
                    std::this_thread::sleep_for(std::chrono::nanoseconds(
                        next_ns > now_ns() ? static_cast<std::int64_t>(next_ns - now_ns()) : 0));
                    next_ns += period_ns;
                    if (stop.load(std::memory_order_relaxed)) {
                        break;
                    }
                }
                auto sequence = make_sequence(index, counter);
                auto send_ns = now_ns();
                stamp_payload(payload, sequence, send_ns);
                auto response_size = call(index, sequence, send_ns, payload);
                auto end_ns = now_ns();
                ++result.calls;
                auto start = measure_start_ns.load(std::memory_order_acquire);
                auto end = measure_end_ns.load(std::memory_order_acquire);
                // Count a request when it was sent inside the measured window.
                if (start == 0 || send_ns < start || (end != 0 && send_ns >= end)) {
                    continue;
                }
                ++result.sent;
                if (response_size == 0) {
                    ++result.lost;
                } else if (response_size != config.payload_size) {
                    ++result.bad_size;
                } else {
                    result.stats.add(end_ns - send_ns);
                }
            }
        });
    }
    while (ready.load() < config.threads) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    go.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::duration<double>(config.warmup_s));
    ResourceUsage usage_start = ResourceUsage::sample();
    auto start_ns = now_ns();
    measure_start_ns.store(start_ns, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::duration<double>(config.duration_s));
    auto end_ns = now_ns();
    measure_end_ns.store(end_ns, std::memory_order_release);
    ResourceUsage usage = ResourceUsage::sample().since(usage_start);
    stop.store(true, std::memory_order_relaxed);
    for (auto& worker : workers) {
        worker.join();
    }

    ThreadStressResult total;
    total.usage = usage;
    total.duration_s = static_cast<double>(end_ns - start_ns) / 1e9;
    total.threads_started = workers.size();
    for (auto& result : results) {
        total.calls += result->calls;
        total.sent += result->sent;
        total.lost += result->lost;
        total.bad_size += result->bad_size;
        total.stats.append(result->stats);
    }
    return total;
}

// Result columns shared by every stack. cpu_pct is the process CPU time over
// the measured window as a percentage of one core (200 = two cores busy).
inline void add_thread_stress_columns(ResultRow& row, const ThreadStressConfig& config,
                                      const ThreadStressResult& result) {
    auto summary = result.stats.summarize();
    const double completed = static_cast<double>(summary.count);
    const double throughput = result.duration_s > 0 ? completed / result.duration_s : 0.0;
    const double cpu_ms = result.usage.cpu_ms;
    row.add("threads", config.threads)
        .add("thread_rate", config.thread_rate)
        .add("duration_s", result.duration_s)
        .add("calls_total", result.calls)
        .add("sent", result.sent)
        .add("completed", summary.count)
        .add("lost", result.lost)
        .add("bad_size", result.bad_size)
        .add("loss_pct", result.sent == 0 ? 0.0 : 100.0 * static_cast<double>(result.lost) / static_cast<double>(result.sent))
        .add("throughput_msg_s", throughput)
        .add_latency(summary)
        .add("cpu_pct", result.duration_s > 0 ? cpu_ms / (result.duration_s * 10.0) : 0.0)
        .add("cpu_per_req_us", completed > 0 ? cpu_ms * 1000.0 / completed : 0.0)
        .add_resources(result.usage);
}

} // namespace perf

#endif // LG_SOMEIP_PERF_THREAD_STRESS_H
