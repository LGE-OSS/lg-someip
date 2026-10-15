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
#include <memory>
#include <thread>

#include <rpc/rpc_server.h>
#include <rpc/rpc_types.h>
#include <transport/endpoint.h>

#include "common/PerfPayload.h"
#include "common/Options.h"
#include "common/Report.h"

namespace {

std::unique_ptr<someip::rpc::RpcServer> server;
std::atomic<std::uint64_t> requests_served{0};
std::atomic<bool> stop_requested{false};
perf::TrafficWindow traffic;
bool echo_payload = true;
perf::Payload fixed_response;

void handle_signal(int) {
    stop_requested = true;
}

someip::rpc::RpcResult on_request(std::uint16_t, std::uint16_t, const someip::platform::ByteBuffer& input,
                                  someip::platform::ByteBuffer& output) {
    traffic.note_request();
    perf::Payload bytes(input.begin(), input.end());
    if (echo_payload) {
        output = bytes;
    } else {
        output = fixed_response;
        std::copy_n(bytes.begin(), std::min(bytes.size(), perf::kPayloadHeaderSize), output.begin());
    }
    ++requests_served;
    return someip::rpc::RpcResult::SUCCESS;
}

someip::transport::TransportProtocol parse_transport(const std::string& name) {
    if (name == "udp") {
        return someip::transport::TransportProtocol::UDP;
    }
    if (name == "tcp") {
        return someip::transport::TransportProtocol::TCP;
    }
    throw std::invalid_argument("transport must be udp or tcp");
}

} // namespace

int main(int argc, char* argv[]) {
    perf::Options options(argc, argv,
                          {
                              {"name", "perf-rpc-server"},
                              {"config", ""},
                              {"bind", "0.0.0.0"},
                              {"port", "30501"},
                              {"transport", "udp"},
                              {"response-size", "0"},
                              {"instance", "1"},
                          });
    if (!options.ok()) {
        options.print_usage(argv[0]);
        return 1;
    }

    try {
        auto protocol = parse_transport(options.str("transport"));
        auto response_size = options.u64("response-size");
        if (response_size != 0) {
            echo_payload = false;
            fixed_response = perf::make_payload(response_size);
        }

        const someip::transport::Endpoint endpoint(options.str("bind"), static_cast<std::uint16_t>(options.u64("port")),
                                                   protocol);
        server = std::make_unique<someip::rpc::RpcServer>(perf::kRpcServiceId, perf::kMajorVersion, endpoint);
        if (!server->register_method(perf::kRpcMethodId, on_request)) {
            std::cerr << "perf-rpc-server: method registration failed" << std::endl;
            return 1;
        }
        if (!server->initialize()) {
            std::cerr << "perf-rpc-server: init failed" << std::endl;
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "perf-rpc-server: " << error.what() << std::endl;
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    std::cout << "perf-rpc-server: offering service 0x" << std::hex << perf::kRpcServiceId << std::dec << " at "
              << options.str("bind") << ":" << options.u64("port") << " (direct endpoint)" << std::endl;

    while (!stop_requested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        traffic.poll(requests_served);
    }
    traffic.poll(requests_served);
    server->shutdown();

    perf::ResultRow row;
    // opensomeip identifies a responder by its endpoint (--port); the instance
    // is recorded so rows line up with the other stacks.
    row.add("stack", "opensomeip")
        .add("instance", options.u64("instance"))
        .add("requests_served", requests_served.load())
        .add("lifetime_cpu_ms", perf::process_cpu_ms())
        .add_resources(traffic.usage());
    std::cout << "perf-rpc-server summary:\n";
    row.print(std::cout);
    return 0;
}
