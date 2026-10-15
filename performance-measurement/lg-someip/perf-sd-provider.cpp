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
#include <iostream>
#include <memory>
#include <set>
#include <thread>

#include <lgsomeip/LgsomeipApi.h>

#include "common/Options.h"
#include "common/PerfPayload.h"

// Provider for the Service Discovery probe.
//
// Each cycle stores the wall-clock offer time in a field (and in every other
// notification of the cycle), offers the service,
// sends heartbeat notifications while offered, announces the stop with a
// "stopping" notification, and withdraws the offer. With --cycles 0 the
// service stays offered until SIGINT, which is used for cold-start and crash
// measurements. The probe on the consumer side derives every metric from
// these notifications, so only offer_to_available needs synchronized clocks.
namespace {

std::shared_ptr<lgsomeip::api::Application> application;
std::atomic<bool> registered{false};
std::atomic<bool> stop_requested{false};
bool events_offered = false;

void handle_signal(int) {
    stop_requested = true;
}

void on_state(bool is_registered) {
    if (is_registered && !events_offered) {
        const std::set<lgsomeip::api::eventgroup_t> groups{perf::kSdEventGroupId};
        application->offer_event(perf::kSdServiceId, perf::kSdInstanceId, perf::kSdOfferFieldId, groups, true);
        application->offer_event(perf::kSdServiceId, perf::kSdInstanceId, perf::kSdHeartbeatEventId, groups);
        application->offer_event(perf::kSdServiceId, perf::kSdInstanceId, perf::kSdStoppingEventId, groups);
        events_offered = true;
    }
    registered = is_registered;
}

// Wall-clock time of the current cycle's offer_service(). Every notification
// carries it, so the probe learns the offer time from whichever arrives first.
std::uint64_t offer_realtime_ns = 0;

void notify_stamped(lgsomeip::api::event_t event, std::uint32_t sequence) {
    auto payload = perf::make_payload(perf::kPayloadHeaderSize);
    perf::stamp_payload(payload, sequence, offer_realtime_ns);
    application->notify(perf::kSdServiceId, perf::kSdInstanceId, event, payload, true);
}

// Send heartbeats until the deadline (0 = until SIGINT).
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

    application = lgsomeip::api::Runtime::instance().create_application(options.str("name"), options.str("config"));
    if (!application->init()) {
        std::cerr << "perf-sd-provider: init failed" << std::endl;
        return 1;
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    application->register_application_state_handler(on_state);
    application->start();

    while (!registered && !stop_requested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::uint32_t heartbeat_sequence = 0;
    for (std::uint64_t cycle = 1; !stop_requested && (cycles == 0 || cycle <= cycles); ++cycle) {
        // The field value is sent to every new subscriber as its initial
        // event, so the probe learns when this offer was made.
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
    application->join();
    return 0;
}
