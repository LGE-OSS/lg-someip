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
#include <set>
#include <thread>

#include <vsomeip/vsomeip.hpp>

#include "common/Options.h"
#include "common/PerfPayload.h"

namespace {

std::shared_ptr<vsomeip::application> application;
std::atomic<bool> stop_requested{false};
std::mutex registered_mutex;
std::condition_variable registered_condition;
bool registered = false;
std::uint64_t offer_realtime_ns = 0;

void handle_signal(int) {
    stop_requested = true;
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

void offer_event(vsomeip::event_t event, const std::set<vsomeip::eventgroup_t>& groups, bool is_field) {
#ifdef PERF_VSOMEIP_LEGACY_API
    application->offer_event(perf::kSdServiceId, perf::kSdInstanceId, event, groups, is_field);
#else
    application->offer_event(perf::kSdServiceId, perf::kSdInstanceId, event, groups,
                             is_field ? vsomeip::event_type_e::ET_FIELD : vsomeip::event_type_e::ET_EVENT);
#endif
}

std::shared_ptr<vsomeip::payload> make_payload(const perf::Payload& bytes) {
    auto payload = vsomeip::runtime::get()->create_payload();
    payload->set_data(bytes);
    return payload;
}

void notify_stamped(vsomeip::event_t event, std::uint32_t sequence) {
    auto bytes = perf::make_payload(perf::kPayloadHeaderSize);
    perf::stamp_payload(bytes, sequence, offer_realtime_ns);
    application->notify(perf::kSdServiceId, perf::kSdInstanceId, event, make_payload(bytes), true);
}

void heartbeat_until(std::uint64_t deadline_ns, std::chrono::milliseconds period, std::uint32_t& sequence) {
    while (!stop_requested && (deadline_ns == 0 || perf::now_ns() < deadline_ns)) {
        notify_stamped(perf::kSdHeartbeatEventId, ++sequence);
        std::this_thread::sleep_for(period);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    perf::Options options(argc, argv,
                          {
                              {"name", "perf-sd-provider"},
                              {"config", ""},
                              {"cycles", "0"},
                              {"on-ms", "2000"},
                              {"off-ms", "2000"},
                              {"heartbeat-ms", "10"},
                          });
    if (!options.ok()) {
        options.print_usage(argv[0]);
        return 1;
    }
    const auto cycles = options.u64("cycles");
    const auto on_ns = options.u64("on-ms") * 1000000;
    const auto off = std::chrono::milliseconds(options.u64("off-ms"));
    const auto heartbeat = std::chrono::milliseconds(std::max<std::uint64_t>(options.u64("heartbeat-ms"), 1));

    if (!options.str("config").empty()) {
        setenv("VSOMEIP_CONFIGURATION", options.str("config").c_str(), 1);
    }
    setenv("VSOMEIP_APPLICATION_NAME", options.str("name").c_str(), 0);

    application = vsomeip::runtime::get()->create_application(options.str("name"));
    if (!application->init()) {
        std::cerr << "perf-sd-provider: init failed" << std::endl;
        return 1;
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
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

    const std::set<vsomeip::eventgroup_t> groups{perf::kSdEventGroupId};
    offer_event(perf::kSdOfferFieldId, groups, true);
    offer_event(perf::kSdHeartbeatEventId, groups, false);
    offer_event(perf::kSdStoppingEventId, groups, false);

    std::uint32_t heartbeat_sequence = 0;
    for (std::uint64_t cycle = 1; !stop_requested && (cycles == 0 || cycle <= cycles); ++cycle) {
        offer_realtime_ns = perf::realtime_ns();
        notify_stamped(perf::kSdOfferFieldId, static_cast<std::uint32_t>(cycle));
        application->offer_service(perf::kSdServiceId, perf::kSdInstanceId, perf::kMajorVersion);
        std::cout << "perf-sd-provider: cycle " << cycle << " offered" << std::endl;

        heartbeat_until(cycles == 0 ? 0 : perf::now_ns() + on_ns, heartbeat, heartbeat_sequence);

        notify_stamped(perf::kSdStoppingEventId, static_cast<std::uint32_t>(cycle));
        application->stop_offer_service(perf::kSdServiceId, perf::kSdInstanceId, perf::kMajorVersion);
        std::cout << "perf-sd-provider: cycle " << cycle << " withdrawn" << std::endl;
        if (cycles != 0 && cycle < cycles) {
            std::this_thread::sleep_for(off);
        }
    }

    application->stop();
    runner.join();
    return stop_requested ? 3 : 0;
}
