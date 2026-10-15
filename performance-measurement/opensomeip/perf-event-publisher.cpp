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

#include <signal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <set>
#include <thread>

#include <events/event_publisher.h>
#include <events/event_types.h>

#include "common/LatencyStats.h"
#include "common/Options.h"
#include "common/Pacing.h"
#include "common/PerfPayload.h"
#include "common/Report.h"

namespace {

constexpr std::uint16_t kClientId = 0x0421;

std::atomic<bool> stop_requested{false};

void handle_signal(int) {
    stop_requested = true;
}

someip::events::EventConfig event_config(std::uint16_t event_id) {
    someip::events::EventConfig config;
    config.event_id = event_id;
    config.eventgroup_id = perf::kEventGroupId;
    config.reliability = someip::events::Reliability::UNRELIABLE;
    config.notification_type = someip::events::NotificationType::ON_CHANGE;
    config.cycle_time = std::chrono::milliseconds(0);
    return config;
}

} // namespace

int main(int argc, char* argv[]) {
    perf::Options options(argc, argv,
                          {
                              {"name", "perf-event-pub"},
                              {"subscriber", "127.0.0.1"},
                              {"port", "30611"},
                              {"payload", "64"},
                              {"count", "10000"},
                              {"warmup", "1000"},
                              {"rate", "1000"},
                              {"settle-ms", "100"},
                              {"subscribers", "1"},
                              {"instance", "1"},
                              {"start-at", "0"},
                              {"end-repeat", "5"},
                              {"clock", "monotonic"},
                              {"label", ""},
                              {"csv", ""},
                          });
    perf::Clock clock = perf::Clock::Monotonic;
    if (!options.ok() || !perf::parse_clock(options.str("clock"), clock)) {
        options.print_usage(argv[0]);
        return 1;
    }

    const auto payload_size = std::max<std::size_t>(options.u64("payload"), perf::kPayloadHeaderSize);
    const auto count = options.u64("count");
    const auto warmup = options.u64("warmup");
    const auto total = warmup + count;
    const auto rate = std::strtod(options.str("rate").c_str(), nullptr);
    if (total > 0xFFFFFFFFull) {
        std::cerr << "perf-event-publisher: --warmup + --count must fit a 32-bit sequence number" << std::endl;
        return 1;
    }

    const auto instance = static_cast<std::uint16_t>(options.u64("instance"));
    const auto subscribers = std::max<std::uint64_t>(options.u64("subscribers"), 1);
    someip::events::EventPublisher publisher(perf::kEventServiceId, instance);
    if (!publisher.register_event(event_config(perf::kDataEventId)) ||
        !publisher.register_event(event_config(perf::kEndEventId)) || !publisher.initialize()) {
        std::cerr << "perf-event-publisher: event publisher initialization failed" << std::endl;
        return 1;
    }

    // Subscriber k listens on --port + k - 1. handle_subscription() takes the
    // endpoint from the default client endpoint at the moment it is called, so
    // pointing the default at each subscriber in turn registers all of them.
    for (std::uint64_t k = 0; k < subscribers; ++k) {
        publisher.set_default_client_endpoint(options.str("subscriber"),
                                              static_cast<std::uint16_t>(options.u64("port") + k));
        if (!publisher.handle_subscription(perf::kEventGroupId, static_cast<std::uint16_t>(kClientId + k), 0xFFFFFFu)) {
            std::cerr << "perf-event-publisher: manual subscription setup failed for subscriber " << k + 1 << std::endl;
            publisher.shutdown();
            return 2;
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(options.u64("settle-ms")));

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    std::cout << "perf-event-publisher: direct endpoint " << options.str("subscriber") << ":" << options.u64("port")
              << "; publishing " << warmup << " warm-up + " << count << " notifications of " << payload_size
              << " bytes at " << (rate > 0 ? std::to_string(rate) + " msg/s" : std::string("maximum rate"))
              << std::endl;

    // Start barrier: several publishers begin publishing at the same moment.
    const double start_late_ms = perf::wait_until_realtime_ms(options.u64("start-at"));

    auto payload = perf::make_payload(payload_size);
    const auto clock_offset_ns = clock == perf::Clock::Realtime ? perf::realtime_ns() - perf::now_ns() : 0;
    const perf::Schedule schedule(rate, perf::now_ns());
    perf::LatencyStats publish_cost;
    publish_cost.reserve(count);
    std::uint64_t max_lag_ns = 0;
    std::uint64_t sent = 0;
    std::uint64_t measure_start_ns = perf::now_ns();
    auto usage_start = perf::ResourceUsage::sample();

    for (std::uint64_t i = 0; i < total && !stop_requested; ++i) {
        if (i == warmup) {
            usage_start = perf::ResourceUsage::sample();
            measure_start_ns = perf::now_ns();
        }
        std::uint64_t scheduled_ns = 0;
        if (schedule.paced()) {
            scheduled_ns = schedule.due_ns(i);
            perf::wait_until_ns(scheduled_ns);
        }
        auto send_ns = perf::now_ns();
        if (!schedule.paced()) {
            scheduled_ns = send_ns;
        }
        perf::stamp_payload(payload, static_cast<std::uint32_t>(i + 1), scheduled_ns + clock_offset_ns);
        auto publish_start_ns = perf::now_ns();
        if (!publisher.publish_event(perf::kDataEventId, payload)) {
            std::cerr << "perf-event-publisher: publish failed at sequence " << i + 1 << std::endl;
            stop_requested = true;
            break;
        }
        auto done_ns = perf::now_ns();
        if (i >= warmup) {
            ++sent;
            publish_cost.add(done_ns - publish_start_ns);
            max_lag_ns = std::max(max_lag_ns, send_ns - scheduled_ns);
        }
    }
    auto send_end_ns = perf::now_ns();
    auto usage = perf::ResourceUsage::sample().since(usage_start);

    perf::EndMarker marker;
    marker.total = static_cast<std::uint32_t>(warmup + sent);
    marker.warmup = warmup;
    marker.rate = rate;
    auto end_payload = perf::make_end_marker(marker);
    for (std::uint64_t i = 0; i < options.u64("end-repeat"); ++i) {
        publisher.publish_event(perf::kEndEventId, end_payload);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    publisher.shutdown();
    auto duration_s = static_cast<double>(send_end_ns - measure_start_ns) / 1e9;
    auto publish_summary = publish_cost.summarize();
    perf::ResultRow row;
    row.add("timestamp", perf::utc_timestamp())
        .add("stack", "opensomeip")
        .add("architecture", "direct-event-no-sd")
        .add("scenario", "event-publish")
        .add("label", options.str("label"))
        .add("payload_bytes", payload_size)
        .add("instance", instance)
        .add("start_late_ms", start_late_ms)
        .add("subscribers", subscribers)
        .add("target_rate_msg_s", rate)
        .add("sent", sent)
        .add("duration_s", duration_s)
        .add("send_rate_msg_s", duration_s > 0 ? static_cast<double>(sent) / duration_s : 0.0)
        .add("max_send_lag_us", static_cast<double>(max_lag_ns) / 1000.0)
        .add("publish_mean_us", publish_summary.mean_us)
        .add("publish_p50_us", publish_summary.p50_us)
        .add("publish_p99_us", publish_summary.p99_us)
        .add("publish_max_us", publish_summary.max_us)
        .add_resources(usage);
    std::cout << "perf-event-publisher result:\n";
    row.print(std::cout);
    if (!options.str("csv").empty() && !row.append_csv(options.str("csv"))) {
        std::cerr << "perf-event-publisher: cannot write " << options.str("csv") << std::endl;
    }
    return stop_requested ? 3 : 0;
}
