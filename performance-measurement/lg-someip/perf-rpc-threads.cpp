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
#include <type_traits>
#include <vector>

#include <lgsomeip/LgsomeipApi.h>

#include "common/Options.h"
#include "common/Pacing.h"
#include "common/PerfPayload.h"
#include "common/Report.h"
#include "common/ThreadStress.h"

// Multi-threaded consumer: one process, --threads worker threads, each sending
// a request and waiting for its response. The responder echoes the payload, so
// request and response have the same size. See common/ThreadStress.h.
static_assert(std::is_same<perf::Payload, lgsomeip::api::Payload>::value,
              "perf::Payload must be lgsomeip::api::Payload");

namespace {

std::shared_ptr<lgsomeip::api::Application> application;
std::unique_ptr<perf::ResponseSlots> slots;
std::atomic<bool> stop_requested{false};
std::atomic<std::uint64_t> late_responses{0};
std::atomic<std::uint64_t> invalid_responses{0};

bool service_requested = false;
std::uint16_t responders = 1;
std::mutex availability_mutex;
std::condition_variable availability_condition;
std::set<lgsomeip::api::instance_t> available_instances;

void handle_signal(int) {
    stop_requested = true;
}

void on_response(const std::shared_ptr<lgsomeip::api::Message>& message) {
    perf::PayloadHeader header;
    if (!message || message->type != lgsomeip::api::MessageType::Response ||
        !perf::parse_payload(message->payload, header)) {
        ++invalid_responses;
        return;
    }
    if (!slots->complete(header.sequence, message->payload.size())) {
        ++late_responses;
    }
}

void on_availability(lgsomeip::api::service_t, lgsomeip::api::instance_t instance, bool available) {
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

void on_state(bool registered) {
    if (registered && !service_requested) {
        for (lgsomeip::api::instance_t instance = 1; instance <= responders; ++instance) {
            application->request_service(perf::kRpcServiceId, instance, perf::kMajorVersion);
        }
        service_requested = true;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    perf::Options options(argc, argv,
                          {
                              {"name", "perf-rpc-threads"},
                              {"config", ""},
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
    application = lgsomeip::api::Runtime::instance().create_application(options.str("name"), options.str("config"));
    if (!application->init()) {
        std::cerr << "perf-rpc-threads: init failed" << std::endl;
        return 1;
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    application->register_application_state_handler(on_state);
    for (lgsomeip::api::instance_t instance = 1; instance <= responders; ++instance) {
        application->register_message_handler(perf::kRpcServiceId, instance, perf::kRpcMethodId, on_response);
        application->register_availability_handler(perf::kRpcServiceId, instance, on_availability, perf::kMajorVersion);
    }

    auto discovery_start = perf::now_ns();
    application->start();
    {
        std::unique_lock<std::mutex> lock(availability_mutex);
        if (!availability_condition.wait_for(lock, std::chrono::milliseconds(options.u64("discovery-timeout-ms")),
                                             [] { return available_instances.size() >= responders; })) {
            std::cerr << "perf-rpc-threads: only " << available_instances.size() << " of " << responders
                      << " responder(s) available before discovery timeout" << std::endl;
            application->stop();
            application->join();
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
        .add("stack", "lgsomeip")
        .add("scenario", "rpc-threads")
        .add("label", options.str("label"))
        .add("payload_bytes", config.payload_size)
        .add("responders", responders)
        .add("discovery_ms", discovery_ms);
    row.add("start_late_ms", perf::wait_until_realtime_ms(options.u64("start-at")));

    // Per-worker request message: the instance changes per request so that every
    // worker visits all responders in turn.
    std::vector<lgsomeip::api::Message> requests(config.threads);
    for (auto& request : requests) {
        request.service = perf::kRpcServiceId;
        request.method = perf::kRpcMethodId;
        request.interface_version = perf::kMajorVersion;
        request.type = lgsomeip::api::MessageType::Request;
    }
    auto result = perf::run_thread_stress(
        config, [&](std::size_t thread, std::uint32_t sequence, std::uint64_t, perf::Payload& payload) -> std::size_t {
            auto& request = requests[thread];
            request.instance = static_cast<lgsomeip::api::instance_t>((thread + sequence) % responders + 1);
            request.payload = payload;
            slots->arm(thread, sequence);
            application->send(request);
            return slots->wait(thread, config.timeout);
        });
    row.add("late", late_responses.load()).add("invalid", invalid_responses.load());
    perf::add_thread_stress_columns(row, config, result);

    for (lgsomeip::api::instance_t instance = 1; instance <= responders; ++instance) {
        application->release_service(perf::kRpcServiceId, instance);
    }
    application->stop();
    application->join();

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
