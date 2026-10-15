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

#ifndef LG_SOMEIP_PERF_EVENT_TALLY_H
#define LG_SOMEIP_PERF_EVENT_TALLY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "LatencyStats.h"
#include "PerfPayload.h"
#include "Report.h"

namespace perf {

// What an event subscriber records, per publisher, and the loss and latency
// analysis at the end of a run. Every stack's subscriber feeds it, so loss,
// gaps, duplicates, and reordering are computed by the same code for all.
//
// A publisher is identified by a key: the service instance where the stack
// reports it, or the sender endpoint where it does not (SOME/IP carries no
// instance in its header). Sequence numbers are per publisher. The class is
// not thread-safe; callers hold their own lock.
class EventTally {
public:
    explicit EventTally(Clock clock) : clock_(clock) {}

    Clock clock() const {
        return clock_;
    }

    // Create the state of a publisher that is known in advance, so that the
    // reservation does not happen inside a notification callback.
    void expect(std::uint64_t key) {
        state(key);
    }

    // A data notification. Take both timestamps first thing in the callback.
    void add_data(std::uint64_t key, const Payload& payload, std::uint64_t received_clock_ns,
                  std::uint64_t received_ns) {
        PayloadHeader header;
        if (!parse_payload(payload, header) || header.sequence == 0) {
            ++invalid_;
            return;
        }
        auto& publisher = state(key);
        // A clock offset between hosts can make a latency negative. It is
        // stored as 0 and counted, so a wrong clock shows up in the
        // negative_latency column instead of passing as a fast network.
        const auto latency = received_clock_ns > header.timestamp_ns ? received_clock_ns - header.timestamp_ns : 0;
        if (received_clock_ns < header.timestamp_ns) {
            ++negative_latency_;
        }
        publisher.arrivals.emplace_back(header.sequence, latency);
        if (header.sequence < publisher.highest) {
            ++reordered_;
        } else {
            publisher.highest = header.sequence;
        }
        if (first_receive_ns_ == 0) {
            first_receive_ns_ = received_ns;
            payload_size_ = payload.size();
        }
        last_receive_ns_ = received_ns;
        ++arrivals_;
    }

    void add_end(std::uint64_t key, const Payload& payload) {
        EndMarker marker;
        if (parse_end_marker(payload, marker)) {
            auto& publisher = state(key);
            publisher.marker = marker;
            publisher.ended = true;
        }
    }

    void add_invalid() {
        ++invalid_;
    }

    std::size_t arrivals() const {
        return arrivals_;
    }

    bool received_any() const {
        return first_receive_ns_ != 0;
    }

    std::uint64_t last_receive_ns() const {
        return last_receive_ns_;
    }

    // Publishers whose end marker has arrived.
    std::size_t ended() const {
        std::size_t count = 0;
        for (const auto& entry : publishers_) {
            count += entry.second.ended ? 1 : 0;
        }
        return count;
    }

    // Analyze the run and add the result columns. expected_publishers is how
    // many publishers the subscriber was told to expect. Returns the number of
    // lost notifications; the latency samples are available from samples().
    std::uint64_t add_columns(ResultRow& row, std::size_t expected_publishers) {
        samples_ = LatencyStats();
        samples_.reserve(arrivals_);
        std::uint64_t expected = 0;
        std::uint64_t duplicates = 0;
        std::uint64_t gap_runs = 0;
        std::uint64_t first_missing = 0;
        std::uint64_t last_missing = 0;
        std::size_t seen = 0;
        double rate = 0;
        for (auto& entry : publishers_) {
            auto& publisher = entry.second;
            if (publisher.arrivals.empty() && !publisher.ended) {
                continue;
            }
            ++seen;
            // Without an end marker the run's configuration is unknown: use
            // the highest sequence seen and treat nothing as warm-up.
            const std::uint64_t total = publisher.ended ? publisher.marker.total : publisher.highest;
            const std::uint64_t warmup = publisher.ended ? publisher.marker.warmup : 0;
            if (publisher.ended) {
                rate = publisher.marker.rate;
            }
            expected += total > warmup ? total - warmup : 0;
            std::sort(publisher.arrivals.begin(), publisher.arrivals.end());
            std::uint64_t next_expected = warmup + 1;
            auto note_gap = [&](std::uint64_t from, std::uint64_t to) {
                if (from >= to) {
                    return;
                }
                ++gap_runs;
                if (first_missing == 0) {
                    first_missing = from;
                }
                last_missing = to - 1;
            };
            std::uint32_t previous = 0;
            for (const auto& arrival : publisher.arrivals) {
                if (arrival.first == previous) {
                    ++duplicates;
                    continue;
                }
                previous = arrival.first;
                if (arrival.first > warmup && arrival.first <= total) {
                    samples_.add(arrival.second);
                    note_gap(next_expected, arrival.first);
                    next_expected = static_cast<std::uint64_t>(arrival.first) + 1;
                }
            }
            note_gap(next_expected, total + 1);
        }
        const auto summary = samples_.summarize();
        const auto lost = expected > summary.count ? expected - summary.count : 0;
        // Throughput covers every notification received, warm-up included,
        // from the first to the last arrival, over all publishers.
        const auto duration_s = static_cast<double>(last_receive_ns_ - first_receive_ns_) / 1e9;
        const auto throughput = duration_s > 0 ? static_cast<double>(arrivals_) / duration_s : 0.0;
        // first/last_missing are sequence numbers of one publisher; with
        // several publishers they would mix numbering spaces, so they are 0.
        const bool single = seen <= 1;
        row.add("publishers", seen)
            .add("publishers_expected", expected_publishers)
            .add("payload_bytes", payload_size_)
            .add("target_rate_msg_s", rate)
            .add("end_marker", ended() >= expected_publishers ? 1 : 0)
            .add("expected", expected)
            .add("lost", lost)
            .add("loss_pct", expected == 0 ? 0.0 : 100.0 * static_cast<double>(lost) / static_cast<double>(expected))
            .add("gap_runs", gap_runs)
            .add("first_missing", single ? first_missing : 0)
            .add("last_missing", single ? last_missing : 0)
            .add("duplicates", duplicates)
            .add("reordered", reordered_)
            .add("invalid", invalid_)
            .add("negative_latency", negative_latency_)
            .add("throughput_msg_s", throughput)
            .add("throughput_mbit_s", throughput * static_cast<double>(payload_size_) * 8 / 1e6)
            .add_latency(summary);
        return lost;
    }

    const LatencyStats& samples() const {
        return samples_;
    }

private:
    struct Publisher {
        std::vector<std::pair<std::uint32_t, std::uint64_t>> arrivals; // sequence, latency ns
        std::uint32_t highest{0};
        bool ended{false};
        EndMarker marker;
    };

    // Capacity reserved per publisher: 16 MB of address space, used on demand.
    static constexpr std::size_t kReservedArrivals = 1u << 20;

    Publisher& state(std::uint64_t key) {
        auto it = publishers_.find(key);
        if (it == publishers_.end()) {
            it = publishers_.emplace(key, Publisher()).first;
            it->second.arrivals.reserve(kReservedArrivals);
        }
        return it->second;
    }

    Clock clock_;
    std::map<std::uint64_t, Publisher> publishers_;
    std::uint64_t arrivals_{0};
    std::uint64_t reordered_{0};
    std::uint64_t invalid_{0};
    std::uint64_t negative_latency_{0};
    std::uint64_t first_receive_ns_{0};
    std::uint64_t last_receive_ns_{0};
    std::uint64_t payload_size_{0};
    LatencyStats samples_;
};

} // namespace perf

#endif // LG_SOMEIP_PERF_EVENT_TALLY_H
