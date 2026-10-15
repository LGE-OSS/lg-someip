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
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include <lgsomeip/LgsomeipApi.h>

#include "common/EventTally.h"
#include "common/Options.h"
#include "common/PerfPayload.h"
#include "common/Report.h"

// Event consumer for the notification benchmarks.
//
// Subscribes to the benchmark event group of --publishers publishers (service
// instances 1..N) and records, for every data notification, its publisher,
// sequence number, and one-way latency: the receive time at the start of the
// callback minus the publisher's scheduled send time. With --clock monotonic
// both ends must run on the same host. With --clock realtime the hosts must
// synchronize CLOCK_REALTIME (for example with PTP), and the clock offset
// between them becomes part of the result.
//
// The run ends when the end marker of every publisher has arrived (it carries
// the total and warm-up counts, so loss is exact), after --idle-timeout-ms
// without traffic, or on SIGINT.
namespace {

std::shared_ptr<lgsomeip::api::Application> application;
std::uint16_t publishers = 1;
bool subscriptions_requested = false;
std::atomic<bool> stop_requested{false};

// Notifications arrive on several library worker threads.
std::mutex data_mutex;
std::condition_variable data_condition;
perf::EventTally tally(perf::Clock::Monotonic);

void handle_signal(int) {
    stop_requested = true;
}

void on_data(const std::shared_ptr<lgsomeip::api::Message>& message) {
    const auto received_clock_ns = perf::clock_ns(tally.clock());
    const auto received_ns = perf::now_ns();
    std::lock_guard<std::mutex> lock(data_mutex);
    if (!message) {
        tally.add_invalid();
        return;
    }
    tally.add_data(message->instance, message->payload, received_clock_ns, received_ns);
    data_condition.notify_all();
}

void on_end(const std::shared_ptr<lgsomeip::api::Message>& message) {
    if (!message) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(data_mutex);
        tally.add_end(message->instance, message->payload);
    }
    data_condition.notify_all();
}

void on_availability(lgsomeip::api::service_t, lgsomeip::api::instance_t instance, bool available) {
    if (available) {
        application->subscribe(perf::kEventServiceId, instance, perf::kEventGroupId, perf::kMajorVersion);
    } else {
        application->unsubscribe(perf::kEventServiceId, instance, perf::kEventGroupId);
    }
}

void on_state(bool registered) {
    if (registered && !subscriptions_requested) {
        const std::set<lgsomeip::api::eventgroup_t> groups{perf::kEventGroupId};
        for (lgsomeip::api::instance_t instance = 1; instance <= publishers; ++instance) {
            application->request_event(perf::kEventServiceId, instance, perf::kDataEventId, groups);
            application->request_event(perf::kEventServiceId, instance, perf::kEndEventId, groups);
            application->request_service(perf::kEventServiceId, instance, perf::kMajorVersion);
        }
        subscriptions_requested = true;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    perf::Options options(argc, argv,
                          {
                              {"name", "perf-event-sub-1"},
                              {"config", ""},
                              {"clock", "monotonic"},
                              {"publishers", "1"},
                              {"start-timeout-ms", "30000"},
                              {"idle-timeout-ms", "5000"},
                              {"drain-ms", "200"},
                              {"label", ""},
                              {"csv", ""},
                              {"samples-out", ""},
                          });
    perf::Clock clock_kind = perf::Clock::Monotonic;
    if (!options.ok() || !perf::parse_clock(options.str("clock"), clock_kind)) {
        options.print_usage(argv[0]);
        return 1;
    }
    tally = perf::EventTally(clock_kind);
    publishers = static_cast<std::uint16_t>(std::max<std::uint64_t>(options.u64("publishers"), 1));
    const auto start_timeout = std::chrono::milliseconds(options.u64("start-timeout-ms"));
    const auto idle_timeout = std::chrono::milliseconds(options.u64("idle-timeout-ms"));

    application = lgsomeip::api::Runtime::instance().create_application(options.str("name"), options.str("config"));
    if (!application->init()) {
        std::cerr << "perf-event-subscriber: init failed" << std::endl;
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    for (lgsomeip::api::instance_t instance = 1; instance <= publishers; ++instance) {
        // Reserve before traffic starts, so no callback reallocates under the lock.
        tally.expect(instance);
        application->register_message_handler(perf::kEventServiceId, instance, perf::kDataEventId, on_data);
        application->register_message_handler(perf::kEventServiceId, instance, perf::kEndEventId, on_end);
        application->register_availability_handler(perf::kEventServiceId, instance, on_availability,
                                                   perf::kMajorVersion);
    }
    application->register_application_state_handler(on_state);
    auto usage_start = perf::ResourceUsage::sample();
    application->start();

    // Wait for the first notification, then until every end marker or an idle gap.
    bool timed_out = false;
    {
        std::unique_lock<std::mutex> lock(data_mutex);
        auto start_deadline = std::chrono::steady_clock::now() + start_timeout;
        while (!stop_requested && tally.ended() < publishers) {
            auto seen = tally.arrivals();
            data_condition.wait_for(lock, std::chrono::milliseconds(100));
            if (tally.arrivals() != seen || tally.ended() >= publishers) {
                continue;
            }
            if (!tally.received_any()) {
                if (std::chrono::steady_clock::now() >= start_deadline) {
                    timed_out = true;
                    break;
                }
            } else if (perf::now_ns() - tally.last_receive_ns() >
                       static_cast<std::uint64_t>(idle_timeout.count()) * 1000000) {
                timed_out = true;
                break;
            }
        }
    }
    // An end marker can overtake data still queued in the callback threads.
    std::this_thread::sleep_for(std::chrono::milliseconds(options.u64("drain-ms")));
    auto usage = perf::ResourceUsage::sample().since(usage_start);

    for (lgsomeip::api::instance_t instance = 1; instance <= publishers; ++instance) {
        application->unsubscribe(perf::kEventServiceId, instance, perf::kEventGroupId);
        application->release_service(perf::kEventServiceId, instance);
    }
    application->stop();
    application->join();

    std::lock_guard<std::mutex> lock(data_mutex);
    if (!tally.received_any()) {
        std::cerr << "perf-event-subscriber: no notification received" << std::endl;
        return 2;
    }

    perf::ResultRow row;
    row.add("timestamp", perf::utc_timestamp())
        .add("revision", PERF_GIT_REVISION)
        .add("scenario", "event-subscribe")
        .add("label", options.str("label"))
        .add("subscriber", options.str("name"))
        .add("clock", options.str("clock"));
    const auto lost = tally.add_columns(row, publishers);
    row.add_resources(usage);

    std::cout << "perf-event-subscriber result (one-way):\n";
    row.print(std::cout);
    if (!options.str("csv").empty() && !row.append_csv(options.str("csv"))) {
        std::cerr << "perf-event-subscriber: cannot write " << options.str("csv") << std::endl;
    }
    if (!options.str("samples-out").empty() && !tally.samples().write_samples(options.str("samples-out"))) {
        std::cerr << "perf-event-subscriber: cannot write " << options.str("samples-out") << std::endl;
    }
    return (stop_requested || timed_out || lost != 0) ? 3 : 0;
}
