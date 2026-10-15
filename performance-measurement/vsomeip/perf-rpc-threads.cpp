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
#include <vector>

#include <vsomeip/vsomeip.hpp>

#include "common/Options.h"
#include "common/Pacing.h"
#include "common/PerfPayload.h"
#include "common/Report.h"
#include "common/ThreadStress.h"

// Multi-threaded consumer: one process, --threads worker threads, each sending
// a request and waiting for its response. The responder echoes the payload, so
// request and response have the same size. See common/ThreadStress.h.
//
// vsomeip's application::start() blocks until stop(), so it runs on its own
// thread. Every worker owns its request message and builds a new payload object
// for each request, which vsomeip requires; that work is part of the timed call.

namespace {

std::shared_ptr<vsomeip::application> application;
std::unique_ptr<perf::ResponseSlots> slots;
std::atomic<bool> stop_requested{false};
std::atomic<std::uint64_t> late_responses{0};
std::atomic<std::uint64_t> invalid_responses{0};

std::atomic<bool> service_requested{false};
std::uint16_t responders = 1;
std::mutex availability_mutex;
std::condition_variable availability_condition;
std::set<vsomeip::instance_t> available_instances;

void handle_signal(int) {
    stop_requested = true;
}

void on_response(const std::shared_ptr<vsomeip::message>& message) {
    auto response_payload = message->get_payload();
    perf::Payload bytes(response_payload->get_data(), response_payload->get_data() + response_payload->get_length());
    perf::PayloadHeader header;
    if (message->get_message_type() != vsomeip::message_type_e::MT_RESPONSE || !perf::parse_payload(bytes, header)) {
        ++invalid_responses;
        return;
    }
    if (!slots->complete(header.sequence, bytes.size())) {
        ++late_responses;
    }
}

void on_availability(vsomeip::service_t, vsomeip::instance_t instance, bool available) {
    {
        std::lock_guard<std::mutex> lock(availability_mutex);
        if (available) {
            available_instances.insert(instance);
        } else {
            available_instances.erase(instance);
        }
    }
    availability_condition.notify_all();
}

void on_state(vsomeip::state_type_e state) {
    if (state == vsomeip::state_type_e::ST_REGISTERED && !service_requested.exchange(true)) {
        for (vsomeip::instance_t instance = 1; instance <= responders; ++instance) {
            application->request_service(perf::kRpcServiceId, instance, perf::kMajorVersion);
        }
    }
}

void send_request(const std::shared_ptr<vsomeip::message>& request) {
#ifdef PERF_VSOMEIP_LEGACY_API
    application->send(request, true);
#else
    application->send(request);
#endif
}

} // namespace

int main(int argc, char* argv[]) {
    perf::Options options(argc, argv,
                          {
                              {"name", "perf-rpc-threads"},
                              {"config", ""},
                              {"transport", "udp"},
                              {"payload", "64"},
                              {"threads", "8"},
                              {"responders", "1"},
                              {"warmup-s", "1"},
                              {"duration-s", "5"},
                              {"thread-rate", "0"},
                              {"timeout-ms", "1000"},
                              {"discovery-timeout-ms", "10000"},
                              {"label", ""},
                              {"csv", ""},
                              {"samples-out", ""},
                              {"start-at", "0"},
                          });
    if (!options.ok()) {
        options.print_usage(argv[0]);
        return 1;
    }

    perf::ThreadStressConfig config;
    config.threads = std::max<std::uint64_t>(options.u64("threads"), 1);
    config.payload_size = std::max<std::size_t>(options.u64("payload"), perf::kPayloadHeaderSize);
    config.warmup_s = std::strtod(options.str("warmup-s").c_str(), nullptr);
    config.duration_s = std::strtod(options.str("duration-s").c_str(), nullptr);
    config.thread_rate = std::strtod(options.str("thread-rate").c_str(), nullptr);
    config.timeout = std::chrono::milliseconds(options.u64("timeout-ms"));
    responders = static_cast<std::uint16_t>(std::max<std::uint64_t>(options.u64("responders"), 1));
    if (config.threads > perf::kMaxStressThreads) {
        std::cerr << "perf-rpc-threads: --threads must be at most " << perf::kMaxStressThreads << std::endl;
        return 1;
    }

    slots = std::make_unique<perf::ResponseSlots>(config.threads);
    if (!options.str("config").empty()) {
        setenv("VSOMEIP_CONFIGURATION", options.str("config").c_str(), 1);
    }
    setenv("VSOMEIP_APPLICATION_NAME", options.str("name").c_str(), 0);
    application = vsomeip::runtime::get()->create_application(options.str("name"));
    if (!application->init()) {
        std::cerr << "perf-rpc-threads: init failed" << std::endl;
        return 1;
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    application->register_state_handler(on_state);
    for (vsomeip::instance_t instance = 1; instance <= responders; ++instance) {
        application->register_message_handler(perf::kRpcServiceId, instance, perf::kRpcMethodId, on_response);
        application->register_availability_handler(perf::kRpcServiceId, instance, on_availability, perf::kMajorVersion);
    }

    auto discovery_start = perf::now_ns();
    std::thread runner([] { application->start(); });
    {
        std::unique_lock<std::mutex> lock(availability_mutex);
        if (!availability_condition.wait_for(lock, std::chrono::milliseconds(options.u64("discovery-timeout-ms")),
                                             [] { return available_instances.size() >= responders; })) {
            std::cerr << "perf-rpc-threads: only " << available_instances.size() << " of " << responders
                      << " responder(s) available before discovery timeout" << std::endl;
            application->stop();
            runner.join();
            return 2;
        }
    }
    auto discovery_ms = static_cast<double>(perf::now_ns() - discovery_start) / 1e6;
    std::cout << "perf-rpc-threads: service available after " << discovery_ms << " ms; " << config.threads
              << " threads, " << responders << " responder(s), " << config.payload_size << " bytes each way"
              << std::endl;

    perf::ResultRow row;
    row.add("timestamp", perf::utc_timestamp())
        .add("revision", PERF_GIT_REVISION)
        .add("stack", "vsomeip")
        .add("scenario", "rpc-threads")
        .add("label", options.str("label"))
        .add("payload_bytes", config.payload_size)
        .add("responders", responders)
        .add("discovery_ms", discovery_ms);
    row.add("start_late_ms", perf::wait_until_realtime_ms(options.u64("start-at")));

    // vsomeip sends a request over TCP only when it is created as reliable; the
    // service must offer the matching endpoint (gen-config.sh -t).
    const bool reliable = options.str("transport") == "tcp";
    std::vector<std::shared_ptr<vsomeip::message>> requests(config.threads);
    for (auto& request : requests) {
        request = vsomeip::runtime::get()->create_request(reliable);
        request->set_service(perf::kRpcServiceId);
        request->set_method(perf::kRpcMethodId);
        request->set_interface_version(perf::kMajorVersion);
    }
    auto result = perf::run_thread_stress(
        config, [&](std::size_t thread, std::uint32_t sequence, std::uint64_t, perf::Payload& payload) -> std::size_t {
            auto& request = requests[thread];
            request->set_instance(static_cast<vsomeip::instance_t>((thread + sequence) % responders + 1));
            auto request_payload = vsomeip::runtime::get()->create_payload();
            request_payload->set_data(payload);
            request->set_payload(request_payload);
            slots->arm(thread, sequence);
            send_request(request);
            return slots->wait(thread, config.timeout);
        });
    row.add("late", late_responses.load()).add("invalid", invalid_responses.load());
    perf::add_thread_stress_columns(row, config, result);

    for (vsomeip::instance_t instance = 1; instance <= responders; ++instance) {
        application->release_service(perf::kRpcServiceId, instance);
    }
    application->stop();
    runner.join();

    std::cout << "perf-rpc-threads result:\n";
    row.print(std::cout);
    if (!options.str("csv").empty() && !row.append_csv(options.str("csv"))) {
        std::cerr << "perf-rpc-threads: cannot write " << options.str("csv") << std::endl;
    }
    if (!options.str("samples-out").empty() && !result.stats.write_samples(options.str("samples-out"))) {
        std::cerr << "perf-rpc-threads: cannot write " << options.str("samples-out") << std::endl;
    }
    return stop_requested || result.lost != 0 || result.bad_size != 0 ? 3 : 0;
}
