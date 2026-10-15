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
#include <thread>

#include <lgsomeip/LgsomeipApi.h>

#include "common/Options.h"
#include "common/PerfPayload.h"
#include "common/Report.h"

// Provider for the RPC benchmarks: answers every request as fast as possible.
// By default the request payload is echoed so the client measures the same
// size in both directions; --response-size sets a fixed response size instead.
namespace {

std::shared_ptr<lgsomeip::api::Application> application;
lgsomeip::api::Payload fixed_response;
bool echo_payload = true;
std::atomic<std::uint64_t> requests_served{0};
bool service_offered = false;
std::atomic<bool> stop_requested{false};
// Service instance this responder offers; peer runs start one responder per instance.
lgsomeip::api::instance_t instance = perf::kRpcInstanceId;
perf::TrafficWindow traffic;

void handle_signal(int) {
    // Only set a lock-free flag here; the main thread performs the shutdown.
    stop_requested = true;
}

void on_request(const std::shared_ptr<lgsomeip::api::Message>& request) {
    if (!request || request->type != lgsomeip::api::MessageType::Request) {
        return;
    }

    traffic.note_request();

    lgsomeip::api::Message response;
    response.service = request->service;
    response.instance = request->instance;
    response.method = request->method;
    response.client = request->client;
    response.session = request->session;
    response.interface_version = request->interface_version;
    response.type = lgsomeip::api::MessageType::Response;
    if (echo_payload) {
        response.payload = request->payload;
    } else {
        // Copy the request header so the client can still match the sequence.
        response.payload = fixed_response;
        std::copy_n(request->payload.begin(), std::min(request->payload.size(), perf::kPayloadHeaderSize),
                    response.payload.begin());
    }
    application->send(response);
    ++requests_served;
}

void on_state(bool registered) {
    if (!registered || service_offered) {
        return;
    }
    application->offer_service(perf::kRpcServiceId, instance, perf::kMajorVersion);
    service_offered = true;
    std::cout << "perf-rpc-server: offering service 0x" << std::hex << perf::kRpcServiceId << " instance 0x" << instance
              << std::dec << std::endl;
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

    instance = static_cast<lgsomeip::api::instance_t>(options.u64("instance"));
    auto response_size = options.u64("response-size");
    if (response_size != 0) {
        echo_payload = false;
        fixed_response = perf::make_payload(response_size);
    }

    application = lgsomeip::api::Runtime::instance().create_application(options.str("name"), options.str("config"));
    if (!application->init()) {
        std::cerr << "perf-rpc-server: init failed" << std::endl;
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    application->register_message_handler(perf::kRpcServiceId, instance, perf::kRpcMethodId, on_request, true);
    application->register_application_state_handler(on_state);

    application->start();

    while (!stop_requested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        traffic.poll(requests_served);
    }
    traffic.poll(requests_served);

    application->stop_offer_service(perf::kRpcServiceId, instance, perf::kMajorVersion);
    application->stop();
    application->join();

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
