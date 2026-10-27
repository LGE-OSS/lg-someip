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

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <lgsomeip/LgsomeipApi.h>

#include "InteropIds.h"
#include "InteropVectors.h"

// Event consumer test against interop-server.
//
//   interop-subscriber [--app name] [--group unicast|multicast|all] [--count N] [--timeout seconds]
//
//   --app      application name from the configuration (default interop-sub-1)
//   --group    event groups to subscribe to (default all)
//   --count    notifications required per event (default 5); a field event
//              needs one, because its value only changes when the server changes it
//   --timeout  total wait for the notifications (default 30)
//
// Every notification must be of type Notification, equal to the vector's golden
// bytes and decode with the library deserializer. The unicast group carries
// events 0x8001-0x8003 (0x8003 is a field). The multicast group carries
// 0x8011-0x8012; the subscriber cannot see whether it arrived by unicast or
// multicast. With the configuration's threshold of 2, run two subscribers and
// look at the wire:
//   tcpdump -ni <interface> 'udp and host 224.225.226.235'
// Exit code: 0 when every event arrived intact, 1 otherwise, 2 on a usage error.
namespace {

using lgsomeip::api::Message;
using lgsomeip::api::MessageType;
namespace interop = lgsomeip::interop;

struct Options {
    std::string app{"interop-sub-1"};
    std::string group{"all"};
    int count{5};
    int timeout_s{30};
};

struct EventState {
    int good{0};
    int bad{0};
    bool first_seen{false};
    std::chrono::steady_clock::time_point first_at;
    std::string last_problem;
};

std::shared_ptr<lgsomeip::api::Application> application;
std::vector<interop::EventSpec> selected;
bool events_requested = false;

std::mutex mutex;
std::condition_variable condition;
std::map<lgsomeip::api::event_t, EventState> states;
std::chrono::steady_clock::time_point subscribed_at;

void on_message(const std::shared_ptr<Message>& message) {
    const interop::EventSpec* spec = nullptr;
    for (const auto& candidate : selected) {
        if (candidate.event == message->method) {
            spec = &candidate;
        }
    }
    if (spec == nullptr) {
        return;
    }
    const interop::Vector* vector = interop::find_vector_by_name(spec->vector_name);

    std::string problem;
    if (message->type != MessageType::Notification) {
        problem = "message type is not Notification";
    } else if (vector == nullptr || message->payload != vector->golden) {
        problem = "payload != golden: " + interop::hex(message->payload);
    } else if (!vector->decode(message->payload)) {
        problem = "payload does not decode";
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        auto& state = states[spec->event];
        if (!state.first_seen) {
            state.first_seen = true;
            state.first_at = std::chrono::steady_clock::now();
        }
        if (problem.empty()) {
            state.good++;
        } else {
            state.bad++;
            state.last_problem = problem;
        }
    }
    condition.notify_all();
}

void on_availability(lgsomeip::api::service_t, lgsomeip::api::instance_t, bool available) {
    if (!available) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        subscribed_at = std::chrono::steady_clock::now();
    }
    std::vector<lgsomeip::api::eventgroup_t> groups;
    for (const auto& spec : selected) {
        if (std::find(groups.begin(), groups.end(), spec.group) == groups.end()) {
            groups.push_back(spec.group);
        }
    }
    for (const auto group : groups) {
        application->subscribe(interop::kServiceId, interop::kInstanceId, group, interop::kMajorVersion);
    }
}

void on_state(bool registered) {
    if (registered && !events_requested) {
        for (const auto& spec : selected) {
            application->request_event(interop::kServiceId, interop::kInstanceId, spec.event, {spec.group});
        }
        application->request_service(interop::kServiceId, interop::kInstanceId, interop::kMajorVersion);
        events_requested = true;
    }
}

int required(const interop::EventSpec& spec, const Options& options) {
    return spec.is_field ? 1 : options.count;
}

bool all_received(const Options& options) {
    for (const auto& spec : selected) {
        if (states[spec.event].good < required(spec, options)) {
            return false;
        }
    }
    return true;
}

bool parse(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--app" && has_value) {
            options.app = argv[++i];
        } else if (arg == "--group" && has_value) {
            options.group = argv[++i];
        } else if (arg == "--count" && has_value) {
            options.count = std::atoi(argv[++i]);
        } else if (arg == "--timeout" && has_value) {
            options.timeout_s = std::atoi(argv[++i]);
        } else {
            return false;
        }
    }
    const bool group_ok = options.group == "all" || options.group == "unicast" || options.group == "multicast";
    return group_ok && options.count > 0 && options.timeout_s > 0;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        std::cerr << "usage: " << argv[0] << " [--app name] [--group unicast|multicast|all] [--count N] [--timeout s]"
                  << std::endl;
        return 2;
    }

    for (const auto& spec : interop::events()) {
        const bool multicast = spec.group == interop::kGroupMulticast;
        if (options.group == "all" || (options.group == "multicast") == multicast) {
            selected.push_back(spec);
        }
    }

    application = lgsomeip::api::Runtime::instance().create_application(options.app);
    if (!application->init()) {
        return 1;
    }
    for (const auto& spec : selected) {
        application->register_message_handler(interop::kServiceId, interop::kInstanceId, spec.event, on_message);
    }
    application->register_availability_handler(interop::kServiceId, interop::kInstanceId, on_availability,
                                               interop::kMajorVersion);
    application->register_application_state_handler(on_state);
    application->start();

    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait_for(lock, std::chrono::seconds(options.timeout_s), [&options] { return all_received(options); });
    }

    int failed = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& spec : selected) {
            const auto& state = states[spec.event];
            const bool ok = state.good >= required(spec, options) && state.bad == 0;
            std::cout << (ok ? "PASS  " : "FAIL  ") << "event 0x" << std::hex << spec.event << std::dec << " "
                      << spec.vector_name << (spec.is_field ? " (field)" : "") << "  good=" << state.good
                      << " bad=" << state.bad;
            if (state.first_seen) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(state.first_at - subscribed_at);
                std::cout << "  first after " << ms.count() << " ms";
            }
            if (!state.last_problem.empty()) {
                std::cout << "  -- " << state.last_problem;
            }
            std::cout << std::endl;
            failed += ok ? 0 : 1;
        }
    }

    application->stop();
    return failed == 0 ? 0 : 1;
}
