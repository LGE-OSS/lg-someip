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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

#include <vsomeip/vsomeip.hpp>

#include "common/Options.h"
#include "common/PerfPayload.h"
#include "common/Report.h"

// vsomeip twin of performance-measurement/lg-someip/perf-rpc-server.cpp: answers
// every request as fast as possible. By default the request payload is echoed
// so the client measures the same size in both directions; --response-size
// sets a fixed response size instead. See performance-measurement/docs/stack-comparison.md for how
// this app is meant to be compared against the lgsomeip-native one.
namespace {

std::shared_ptr<vsomeip::application> application;
bool echo_payload = true;
perf::Payload fixed_response;
std::atomic<std::uint64_t> requests_served{0};
std::atomic<bool> stop_requested{false};
// Service instance this responder offers; peer runs start one responder per instance.
vsomeip::instance_t instance = perf::kRpcInstanceId;
perf::TrafficWindow traffic;

// application::offer_service() blocks until application::start() has set its
// internal "enabled" flag, which happens on the thread running start() right
// after it calls into the routing manager. Calling offer_service() straight
// from the state-handler callback can run on that very same thread (the
// callback fires synchronously from inside start()), which would deadlock.
// Signal readiness here and do the actual offer from a different thread.
std::mutex registered_mutex;
std::condition_variable registered_condition;
bool registered = false;

void handle_signal(int) {
    stop_requested = true;
}

void on_request(const std::shared_ptr<vsomeip::message>& request) {
    traffic.note_request();
    auto request_payload = request->get_payload();
    perf::Payload bytes(request_payload->get_data(), request_payload->get_data() + request_payload->get_length());

    auto response = vsomeip::runtime::get()->create_response(request);
    auto response_payload = vsomeip::runtime::get()->create_payload();
    if (echo_payload) {
        response_payload->set_data(bytes);
    } else {
        // Copy the request header so the client can still match the sequence.
        perf::Payload out = fixed_response;
        std::copy_n(bytes.begin(), std::min(bytes.size(), perf::kPayloadHeaderSize), out.begin());
        response_payload->set_data(out);
    }
    response->set_payload(response_payload);
#ifdef PERF_VSOMEIP_LEGACY_API
    application->send(response, true);
#else
    application->send(response);
#endif
    ++requests_served;
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
                              {"name", "perf-rpc-server"},
                              {"config", ""},
                              {"response-size", "0"},
                              {"instance", "1"},
                          });
    if (!options.ok()) {
        options.print_usage(argv[0]);
        return 1;
    }

    instance = static_cast<vsomeip::instance_t>(options.u64("instance"));
    auto response_size = options.u64("response-size");
    if (response_size != 0) {
        echo_payload = false;
        fixed_response = perf::make_payload(response_size);
    }

    // vsomeip has no --config argument of its own; it reads VSOMEIP_CONFIGURATION.
    if (!options.str("config").empty()) {
        setenv("VSOMEIP_CONFIGURATION", options.str("config").c_str(), 1);
    }
    setenv("VSOMEIP_APPLICATION_NAME", options.str("name").c_str(), 0);

    application = vsomeip::runtime::get()->create_application(options.str("name"));
    if (!application->init()) {
        std::cerr << "perf-rpc-server: init failed" << std::endl;
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    application->register_state_handler(on_state);
#ifdef PERF_VSOMEIP_LEGACY_API
    application->register_message_handler(perf::kRpcServiceId, instance, perf::kRpcMethodId, on_request, true);
#else
    application->register_message_handler(perf::kRpcServiceId, instance, perf::kRpcMethodId, on_request);
#endif

    // application::start() blocks until stop(), so it needs its own thread.
    std::thread runner([] { application->start(); });

    {
        std::unique_lock<std::mutex> lock(registered_mutex);
        registered_condition.wait(lock, [] { return registered; });
    }
    application->offer_service(perf::kRpcServiceId, instance, perf::kMajorVersion);
    std::cout << "perf-rpc-server: offering service 0x" << std::hex << perf::kRpcServiceId << std::dec << std::endl;

    while (!stop_requested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        traffic.poll(requests_served);
    }
    traffic.poll(requests_served);

    application->stop_offer_service(perf::kRpcServiceId, instance, perf::kMajorVersion);
    application->stop();
    runner.join();

    // cpu_* cover the traffic window (first to last request, warm-up included);
    // lifetime_cpu_ms covers the whole process including start-up and idle time.
    perf::ResultRow row;
    row.add("instance", instance)
        .add("requests_served", requests_served.load())
        .add("lifetime_cpu_ms", perf::process_cpu_ms())
        .add_resources(traffic.usage());
    std::cout << "perf-rpc-server summary:\n";
    row.print(std::cout);
    return 0;
}
