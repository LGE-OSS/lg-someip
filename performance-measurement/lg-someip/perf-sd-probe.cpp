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
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <lgsomeip/LgsomeipApi.h>

#include "common/LatencyStats.h"
#include "common/Options.h"
#include "common/PerfPayload.h"
#include "common/Report.h"

// Consumer side of the Service Discovery probe (see perf-sd-provider).
//
// One "episode" is one period in which the service is available. For each
// episode the probe records:
//   start_to_available_ms        first episode, service already offered when
//                                the probe started: start() -> availability
//   offer_to_available_ms        service offered after the probe started:
//                                provider offer_service() -> availability;
//                                needs synchronized CLOCK_REALTIME across hosts
//   subscribe_to_field_ms        subscribe() -> initial field event
//   subscribe_to_first_event_ms  subscribe() -> first notification of any kind
//   stop_to_unavailable_ms       "stopping" notification -> unavailability
//                                (graceful StopOffer); negative when the
//                                notification arrived after unavailability
//   last_event_to_unavailable_ms last notification -> unavailability; for a
//                                crashed provider this is the detection time,
//                                with an error of up to one heartbeat period
namespace {

struct Episode {
    std::uint64_t available_ns{0};
    std::uint64_t available_realtime_ns{0};
    std::uint64_t offer_realtime_ns{0};
    std::uint64_t field_ns{0};
    std::uint64_t first_event_ns{0};
    std::uint64_t last_event_ns{0};
    std::uint64_t stopping_ns{0};
    std::uint64_t unavailable_ns{0};
};

std::shared_ptr<lgsomeip::api::Application> application;
bool requested = false;
std::atomic<bool> stop_requested{false};

std::mutex episode_mutex;
std::condition_variable episode_condition;
std::vector<Episode> episodes;

void handle_signal(int) {
    stop_requested = true;
}

// Callers hold episode_mutex. Returns the open episode, or nullptr.
Episode* open_episode() {
    if (episodes.empty() || episodes.back().unavailable_ns != 0) {
        return nullptr;
    }
    return &episodes.back();
}

void on_event(const std::shared_ptr<lgsomeip::api::Message>& message) {
    auto received_ns = perf::now_ns();
    perf::PayloadHeader header;
    if (!message || !perf::parse_payload(message->payload, header)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(episode_mutex);
        auto* episode = open_episode();
        if (episode == nullptr) {
            // The "stopping" notification can arrive after StopOffer has
            // already been reported. Keep it for the episode that just ended.
            if (message->method == perf::kSdStoppingEventId && !episodes.empty() && episodes.back().stopping_ns == 0 &&
                received_ns - episodes.back().unavailable_ns < 1000000000) {
                episodes.back().stopping_ns = received_ns;
            }
            return;
        }
        if (episode->first_event_ns == 0) {
            episode->first_event_ns = received_ns;
        }
        episode->last_event_ns = received_ns;
        if (episode->offer_realtime_ns == 0) {
            // Every notification carries the provider's offer time.
            episode->offer_realtime_ns = header.timestamp_ns;
        }
        if (message->method == perf::kSdOfferFieldId && episode->field_ns == 0) {
            episode->field_ns = received_ns;
        } else if (message->method == perf::kSdStoppingEventId) {
            episode->stopping_ns = received_ns;
        }
    }
    episode_condition.notify_all();
}

void on_availability(lgsomeip::api::service_t, lgsomeip::api::instance_t, bool available) {
    auto now = perf::now_ns();
    auto now_realtime = perf::realtime_ns();
    {
        std::lock_guard<std::mutex> lock(episode_mutex);
        auto* episode = open_episode();
        if (available && episode == nullptr) {
            Episode next;
            next.available_ns = now;
            next.available_realtime_ns = now_realtime;
            episodes.push_back(next);
        } else if (!available && episode != nullptr) {
            episode->unavailable_ns = now;
        }
    }
    episode_condition.notify_all();
    // Subscribe as an application would: as soon as the service is available.
    if (available) {
        application->subscribe(perf::kSdServiceId, perf::kSdInstanceId, perf::kSdEventGroupId, perf::kMajorVersion);
    } else {
        application->unsubscribe(perf::kSdServiceId, perf::kSdInstanceId, perf::kSdEventGroupId);
    }
}

void on_state(bool registered) {
    if (registered && !requested) {
        const std::set<lgsomeip::api::eventgroup_t> groups{perf::kSdEventGroupId};
        for (auto event : {perf::kSdOfferFieldId, perf::kSdHeartbeatEventId, perf::kSdStoppingEventId}) {
            application->request_event(perf::kSdServiceId, perf::kSdInstanceId, event, groups);
        }
        application->request_service(perf::kSdServiceId, perf::kSdInstanceId, perf::kMajorVersion);
        requested = true;
    }
}

// Milliseconds between two steady-clock stamps, or empty if either is missing.
std::string span_ms(std::uint64_t from_ns, std::uint64_t to_ns) {
    if (from_ns == 0 || to_ns == 0 || to_ns < from_ns) {
        return "";
    }
    return std::to_string(static_cast<double>(to_ns - from_ns) / 1e6);
}

// Signed difference for timestamps from two clocks that may disagree slightly.
std::string signed_ms(std::uint64_t from_ns, std::uint64_t to_ns) {
    if (from_ns == 0 || to_ns == 0) {
        return "";
    }
    return std::to_string((static_cast<double>(to_ns) - static_cast<double>(from_ns)) / 1e6);
}

} // namespace

int main(int argc, char* argv[]) {
    perf::Options options(argc, argv,
                          {
                              {"name", "perf-sd-probe"},
                              {"config", ""},
                              {"episodes", "1"},
                              {"timeout-ms", "30000"},
                              {"event-timeout-ms", "3000"},
                              {"phase", ""},
                              {"label", ""},
                              {"csv", ""},
                          },
                          {"wait-down"});
    if (!options.ok()) {
        options.print_usage(argv[0]);
        return 1;
    }
    const auto wanted = options.u64("episodes");
    const bool wait_down = options.flag("wait-down");
    const auto timeout = std::chrono::milliseconds(options.u64("timeout-ms"));
    const auto event_timeout = std::chrono::milliseconds(options.u64("event-timeout-ms"));

    application = lgsomeip::api::Runtime::instance().create_application(options.str("name"), options.str("config"));
    if (!application->init()) {
        std::cerr << "perf-sd-probe: init failed" << std::endl;
        return 1;
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    for (auto event : {perf::kSdOfferFieldId, perf::kSdHeartbeatEventId, perf::kSdStoppingEventId}) {
        application->register_message_handler(perf::kSdServiceId, perf::kSdInstanceId, event, on_event);
    }
    application->register_availability_handler(perf::kSdServiceId, perf::kSdInstanceId, on_availability,
                                               perf::kMajorVersion);
    application->register_application_state_handler(on_state);

    const auto start_ns = perf::now_ns();
    const auto start_realtime_ns = perf::realtime_ns();
    application->start();

    // Wait for each episode to be complete: available, initial field received
    // (or event timeout), and, with --wait-down, unavailable again.
    bool timed_out = false;
    std::vector<Episode> done;
    {
        std::unique_lock<std::mutex> lock(episode_mutex);
        for (std::size_t index = 0; index < wanted && !stop_requested && !timed_out; ++index) {
            auto has_episode = [&] { return stop_requested || episodes.size() > index; };
            if (!episode_condition.wait_for(lock, timeout, has_episode) || stop_requested) {
                timed_out = !stop_requested;
                break;
            }
            episode_condition.wait_for(lock, event_timeout, [&] {
                return stop_requested || episodes[index].field_ns != 0 || episodes[index].unavailable_ns != 0;
            });
            if (wait_down && !episode_condition.wait_for(lock, timeout, [&] {
                    return stop_requested || episodes[index].unavailable_ns != 0;
                })) {
                timed_out = true;
            }
            done.push_back(episodes[index]);
        }
    }

    // A "stopping" notification can still arrive just after unavailability;
    // wait briefly and take the final state of each recorded episode.
    if (wait_down && !done.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::lock_guard<std::mutex> lock(episode_mutex);
        for (std::size_t index = 0; index < done.size(); ++index) {
            done[index] = episodes[index];
        }
    }

    application->release_service(perf::kSdServiceId, perf::kSdInstanceId);
    application->stop();
    application->join();

    std::map<std::string, perf::LatencyStats> summary;
    auto add_sample = [&](const char* name, std::uint64_t from_ns, std::uint64_t to_ns) {
        if (from_ns != 0 && to_ns >= from_ns) {
            summary[name].add(to_ns - from_ns);
        }
    };

    for (std::size_t index = 0; index < done.size(); ++index) {
        const auto& episode = done[index];
        // Which start-up metric applies depends on whether the offer was
        // already in place when the probe started.
        const bool offered_before_start =
            episode.offer_realtime_ns == 0 || episode.offer_realtime_ns <= start_realtime_ns;
        const bool cold_start = index == 0 && offered_before_start;
        // The classification compares the provider's wall clock with this
        // host's. Within the clock offset of the start time it can be wrong;
        // offer_minus_start (milliseconds) records the margin so such rows can be audited.
        if (episode.offer_realtime_ns != 0 && (episode.offer_realtime_ns > start_realtime_ns
                                                   ? episode.offer_realtime_ns - start_realtime_ns
                                                   : start_realtime_ns - episode.offer_realtime_ns) < 200000000) {
            std::cerr << "perf-sd-probe: warning: episode " << index + 1
                      << " was offered within 200 ms of the probe start; cold-start and offer metrics may be swapped"
                      << " if the clocks differ" << std::endl;
        }
        perf::ResultRow row;
        row.add("timestamp", perf::utc_timestamp())
            .add("revision", PERF_GIT_REVISION)
            .add("scenario", "sd-probe")
            .add("phase", options.str("phase"))
            .add("label", options.str("label"))
            .add("episode", index + 1)
            .add("offer_minus_start",
                 episode.offer_realtime_ns == 0 ? "" : signed_ms(start_realtime_ns, episode.offer_realtime_ns))
            .add("start_to_available_ms", cold_start ? span_ms(start_ns, episode.available_ns) : "")
            .add("offer_to_available_ms",
                 offered_before_start ? "" : signed_ms(episode.offer_realtime_ns, episode.available_realtime_ns))
            .add("subscribe_to_field_ms", span_ms(episode.available_ns, episode.field_ns))
            .add("subscribe_to_first_event_ms", span_ms(episode.available_ns, episode.first_event_ns))
            .add("stopping_seen", episode.stopping_ns != 0 ? 1 : 0)
            .add("stopping_after_unavailable",
                 episode.stopping_ns != 0 && episode.stopping_ns > episode.unavailable_ns ? 1 : 0)
            // Negative when unavailability was reported before the "stopping"
            // notification that the provider sent first.
            .add("stop_to_unavailable_ms", signed_ms(episode.stopping_ns, episode.unavailable_ns))
            .add("last_event_to_unavailable_ms", span_ms(episode.last_event_ns, episode.unavailable_ns));
        if (!options.str("csv").empty() && !row.append_csv(options.str("csv"))) {
            std::cerr << "perf-sd-probe: cannot write " << options.str("csv") << std::endl;
        }

        if (cold_start) {
            add_sample("start_to_available", start_ns, episode.available_ns);
        }
        add_sample("subscribe_to_field", episode.available_ns, episode.field_ns);
        add_sample("subscribe_to_first_event", episode.available_ns, episode.first_event_ns);
        add_sample("stop_to_unavailable", episode.stopping_ns, episode.unavailable_ns);
        add_sample("last_event_to_unavailable", episode.last_event_ns, episode.unavailable_ns);
        if (!offered_before_start && episode.available_realtime_ns >= episode.offer_realtime_ns) {
            summary["offer_to_available"].add(episode.available_realtime_ns - episode.offer_realtime_ns);
        }
    }

    std::cout << "perf-sd-probe: " << done.size() << " of " << wanted << " episode(s)"
              << (timed_out ? " (timed out)" : "") << "\n";
    for (const auto& entry : summary) {
        auto stats = entry.second.summarize();
        std::cout << "  " << entry.first << " ms: n=" << stats.count << " min=" << stats.min_us / 1000.0
                  << " p50=" << stats.p50_us / 1000.0 << " max=" << stats.max_us / 1000.0 << "\n";
    }
    std::cout << std::flush;
    if (done.empty()) {
        return 2;
    }
    return (timed_out || stop_requested) ? 3 : 0;
}
