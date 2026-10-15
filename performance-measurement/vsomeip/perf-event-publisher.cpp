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
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include <vsomeip/vsomeip.hpp>

#include "common/LatencyStats.h"
#include "common/Options.h"
#include "common/Pacing.h"
#include "common/PerfPayload.h"
#include "common/Report.h"

namespace {

std::shared_ptr<vsomeip::application> application;
std::atomic<bool> stop_requested{false};
// Service instance this publisher offers; peer runs start one publisher per instance.
vsomeip::instance_t instance = perf::kEventInstanceId;

std::mutex registered_mutex;
std::condition_variable registered_condition;
bool registered = false;

std::mutex subscriber_mutex;
std::condition_variable subscriber_condition;
std::set<vsomeip::client_t> subscribers;

void handle_signal(int) {
    stop_requested = true;
}

bool on_subscription_impl(vsomeip::client_t client, bool subscribed) {
    {
        std::lock_guard<std::mutex> lock(subscriber_mutex);
        if (subscribed) {
            subscribers.insert(client);
        } else {
            subscribers.erase(client);
        }
    }
    subscriber_condition.notify_all();
    return true;
}

#ifdef PERF_VSOMEIP_LEGACY_API
bool on_subscription(vsomeip::client_t client, bool subscribed) {
    return on_subscription_impl(client, subscribed);
}
#else
bool on_subscription(vsomeip::client_t client, vsomeip::uid_t, vsomeip::gid_t, bool subscribed) {
    return on_subscription_impl(client, subscribed);
}
#endif

void offer_event(vsomeip::service_t service, vsomeip::instance_t instance, vsomeip::event_t event,
                 const std::set<vsomeip::eventgroup_t>& groups, bool is_field) {
#ifdef PERF_VSOMEIP_LEGACY_API
    application->offer_event(service, instance, event, groups, is_field);
#else
    application->offer_event(service, instance, event, groups,
                             is_field ? vsomeip::event_type_e::ET_FIELD : vsomeip::event_type_e::ET_EVENT);
#endif
}

std::shared_ptr<vsomeip::payload> make_payload(const perf::Payload& bytes) {
    auto payload = vsomeip::runtime::get()->create_payload();
    payload->set_data(bytes);
    return payload;
}

std::size_t subscriber_count() {
    std::lock_guard<std::mutex> lock(subscriber_mutex);
    return subscribers.size();
}

void on_state(vsomeip::state_type_e state) {
    if (state != vsomeip::state_type_e::ST_REGISTERED) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(registered_mutex);
        registered = true;
    }
    registered_condition.notify_all();
}

} // namespace

int main(int argc, char* argv[]) {
    perf::Options options(argc, argv,
                          {
                              {"name", "perf-event-pub"},
                              {"config", ""},
                              {"payload", "64"},
                              {"count", "10000"},
                              {"warmup", "1000"},
                              {"rate", "1000"},
                              {"subscribers", "1"},
                              {"subscribe-timeout-ms", "10000"},
                              {"settle-ms", "500"},
                              {"end-repeat", "5"},
                              {"clock", "monotonic"},
                              {"label", ""},
                              {"csv", ""},
                              {"instance", "1"},
                              {"start-at", "0"},
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
    const auto wanted_subscribers = options.u64("subscribers");
    instance = static_cast<vsomeip::instance_t>(options.u64("instance"));
    if (total > 0xFFFFFFFFull) {
        std::cerr << "perf-event-publisher: --warmup + --count must fit a 32-bit sequence number" << std::endl;
        return 1;
    }

    if (!options.str("config").empty()) {
        setenv("VSOMEIP_CONFIGURATION", options.str("config").c_str(), 1);
    }
    setenv("VSOMEIP_APPLICATION_NAME", options.str("name").c_str(), 0);

    application = vsomeip::runtime::get()->create_application(options.str("name"));
    if (!application->init()) {
        std::cerr << "perf-event-publisher: init failed" << std::endl;
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    application->register_subscription_handler(perf::kEventServiceId, instance, perf::kEventGroupId, on_subscription);
    application->register_state_handler(on_state);
    std::thread runner([] { application->start(); });

    {
        std::unique_lock<std::mutex> lock(registered_mutex);
        registered_condition.wait(lock, [] { return registered || stop_requested; });
    }
    if (stop_requested) {
        application->stop();
        runner.join();
        return 3;
    }

    const std::set<vsomeip::eventgroup_t> groups{perf::kEventGroupId};
    offer_event(perf::kEventServiceId, instance, perf::kDataEventId, groups, false);
    offer_event(perf::kEventServiceId, instance, perf::kEndEventId, groups, false);
    application->offer_service(perf::kEventServiceId, instance, perf::kMajorVersion);

    {
        std::unique_lock<std::mutex> lock(subscriber_mutex);
        if (!subscriber_condition.wait_for(lock, std::chrono::milliseconds(options.u64("subscribe-timeout-ms")), [&] {
                return subscribers.size() >= wanted_subscribers || stop_requested;
            })) {
            std::cerr << "perf-event-publisher: only " << subscribers.size() << " of " << wanted_subscribers
                      << " subscribers before timeout" << std::endl;
            application->stop_offer_service(perf::kEventServiceId, instance, perf::kMajorVersion);
            application->stop();
            runner.join();
            return 2;
        }
    }
    if (stop_requested) {
        application->stop_offer_service(perf::kEventServiceId, instance, perf::kMajorVersion);
        application->stop();
        runner.join();
        return 3;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(options.u64("settle-ms")));
    std::cout << "perf-event-publisher: " << subscriber_count() << " subscriber(s); publishing " << warmup
              << " warm-up + " << count << " notifications of " << payload_size << " bytes at "
              << (rate > 0 ? std::to_string(rate) + " msg/s" : std::string("maximum rate")) << std::endl;

    // Start barrier: several publishers begin publishing at the same moment.
    const double start_late_ms = perf::wait_until_realtime_ms(options.u64("start-at"));

    auto payload = perf::make_payload(payload_size);
    const std::uint64_t clock_offset_ns = clock == perf::Clock::Realtime ? perf::realtime_ns() - perf::now_ns() : 0;
    const perf::Schedule schedule(rate, perf::now_ns());

    perf::LatencyStats notify_cost;
    notify_cost.reserve(count);
    std::uint64_t max_lag_ns = 0;
    std::uint64_t measure_start_ns = perf::now_ns();
    std::uint64_t sent = 0;
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
        // vsomeip needs a new payload object per notification; build it first
        // so notify_*_us covers the notify() call only, as in the other stacks.
        auto message_payload = make_payload(payload);
        auto notify_start_ns = perf::now_ns();
        application->notify(perf::kEventServiceId, instance, perf::kDataEventId, message_payload, true);
        auto done_ns = perf::now_ns();
        if (i >= warmup) {
            ++sent;
            notify_cost.add(done_ns - notify_start_ns);
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
        application->notify(perf::kEventServiceId, instance, perf::kEndEventId, make_payload(end_payload), true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    application->stop_offer_service(perf::kEventServiceId, instance, perf::kMajorVersion);
    application->stop();
    runner.join();

    auto duration_s = static_cast<double>(send_end_ns - measure_start_ns) / 1e9;
    auto cost = notify_cost.summarize();
    perf::ResultRow row;
    row.add("timestamp", perf::utc_timestamp())
        .add("stack", "vsomeip")
        .add("revision", PERF_GIT_REVISION)
        .add("scenario", "event-publish")
        .add("label", options.str("label"))
        .add("payload_bytes", payload_size)
        .add("instance", instance)
        .add("start_late_ms", start_late_ms)
        .add("subscribers", subscriber_count())
        .add("target_rate_msg_s", rate)
        .add("sent", sent)
        .add("duration_s", duration_s)
        .add("send_rate_msg_s", duration_s > 0 ? static_cast<double>(sent) / duration_s : 0.0)
        .add("max_send_lag_us", static_cast<double>(max_lag_ns) / 1000.0)
        .add("notify_mean_us", cost.mean_us)
        .add("notify_p50_us", cost.p50_us)
        .add("notify_p99_us", cost.p99_us)
        .add("notify_max_us", cost.max_us)
        .add_resources(usage);

    std::cout << "perf-event-publisher result:\n";
    row.print(std::cout);
    if (!options.str("csv").empty() && !row.append_csv(options.str("csv"))) {
        std::cerr << "perf-event-publisher: cannot write " << options.str("csv") << std::endl;
    }
    return stop_requested ? 3 : 0;
}
