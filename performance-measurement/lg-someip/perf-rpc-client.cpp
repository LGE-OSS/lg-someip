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

#include "common/LatencyStats.h"
#include "common/Options.h"
#include "common/Pacing.h"
#include "common/PerfPayload.h"
#include "common/Report.h"

// Consumer for the RPC benchmarks.
//
// pingpong (closed loop) keeps exactly one request outstanding and records the
// round-trip time of each one: from just before send() to the start of the
// response callback. It shows the latency floor: both library paths, the daemon
// hops, and the provider, but not the wakeup of this program's main thread.
//
// rate (open loop) sends on a fixed schedule regardless of responses, with many
// requests in flight. Each request carries its *scheduled* send time, and the
// latency is measured from that time, so a sender that falls behind (because
// send() blocks or the in-flight window is full) adds to the latency instead
// of hiding it. With --rate 0 requests are sent as fast as the window allows,
// which finds the saturation throughput.
// The shared measurement kit is stack-agnostic; it must stay interchangeable
// with the library's own payload type.
static_assert(std::is_same<perf::Payload, lgsomeip::api::Payload>::value,
              "perf::Payload must be lgsomeip::api::Payload");

namespace {

enum class Mode { PingPong, Rate };

std::shared_ptr<lgsomeip::api::Application> application;
Mode mode = Mode::PingPong;
bool service_requested = false;
std::atomic<bool> stop_requested{false};
std::atomic<std::uint64_t> late_responses{0};
std::atomic<std::uint64_t> invalid_responses{0};

// Responders: one service instance each, numbered 1..responders. Requests go
// to them in turn, so with --rate R every responder receives R requests per second.
std::uint16_t responders = 1;
std::mutex availability_mutex;
std::condition_variable availability_condition;
std::set<lgsomeip::api::instance_t> available_instances;

// Ping-pong handshake. The pending sequence and the completion are changed
// under one mutex so a late response to a timed-out request can never be
// taken for the response to the current one.
std::mutex response_mutex;
std::condition_variable response_condition;
std::uint32_t pending_sequence = 0;
std::atomic<std::uint32_t> completed_sequence{0};
std::uint64_t completed_ns = 0;
bool spin_wait = false;

// Rate mode bookkeeping. Responses arrive on several library worker threads,
// so each sequence has its own slot and every counter is atomic.
std::unique_ptr<std::atomic<std::uint64_t>[]> rate_latency_ns;
std::uint64_t rate_total = 0;
std::uint64_t rate_warmup = 0;
std::atomic<std::uint64_t> rate_received{0};
std::atomic<std::uint64_t> rate_duplicates{0};
std::atomic<std::uint64_t> rate_last_receive_ns{0};

void handle_signal(int) {
    stop_requested = true;
}

void on_pingpong_response(const perf::PayloadHeader& header, std::uint64_t received_ns) {
    {
        std::lock_guard<std::mutex> lock(response_mutex);
        if (header.sequence != pending_sequence) {
            // The sender already gave up on this request after its timeout.
            ++late_responses;
            return;
        }
        completed_ns = received_ns;
        completed_sequence.store(header.sequence, std::memory_order_release);
    }
    if (!spin_wait) {
        response_condition.notify_one();
    }
}

void on_rate_response(const perf::PayloadHeader& header, std::uint64_t received_ns) {
    if (header.sequence == 0 || header.sequence > rate_total) {
        ++invalid_responses;
        return;
    }
    // Zero marks "not received", so a real latency is stored as at least 1 ns.
    std::uint64_t latency = received_ns > header.timestamp_ns ? received_ns - header.timestamp_ns : 1;
    std::uint64_t expected = 0;
    if (!rate_latency_ns[header.sequence - 1].compare_exchange_strong(expected, latency)) {
        ++rate_duplicates;
        return;
    }
    ++rate_received;

    auto last = rate_last_receive_ns.load();
    while (received_ns > last && !rate_last_receive_ns.compare_exchange_weak(last, received_ns)) {
    }
}

void on_response(const std::shared_ptr<lgsomeip::api::Message>& message) {
    auto received_ns = perf::now_ns();
    perf::PayloadHeader header;
    if (!message || message->type != lgsomeip::api::MessageType::Response ||
        !perf::parse_payload(message->payload, header)) {
        ++invalid_responses;
        return;
    }
    if (mode == Mode::PingPong) {
        on_pingpong_response(header, received_ns);
    } else {
        on_rate_response(header, received_ns);
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

bool wait_for_pingpong_response(std::uint32_t sequence, std::chrono::milliseconds timeout) {
    if (spin_wait) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (completed_sequence.load(std::memory_order_acquire) != sequence) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
        }
        return true;
    }
    std::unique_lock<std::mutex> lock(response_mutex);
    return response_condition.wait_for(lock, timeout, [sequence] { return completed_sequence.load() == sequence; });
}

struct RunConfig {
    std::uint64_t count{0};
    std::uint64_t warmup{0};
    std::chrono::microseconds interval{0};
    std::chrono::milliseconds timeout{0};
    double rate{0};
    std::uint64_t window{0};
};

struct RunResult {
    int exit_code{0};
    perf::LatencyStats samples;
};

RunResult run_pingpong(std::vector<lgsomeip::api::Message>& requests, const RunConfig& config, perf::ResultRow& row) {
    perf::LatencyStats stats;
    stats.reserve(config.count);
    std::uint64_t lost = 0;
    std::uint64_t sent = 0;
    perf::ResourceUsage usage_start;
    std::uint64_t measure_start_ns = 0;

    for (std::uint64_t i = 0; i < config.warmup + config.count && !stop_requested; ++i) {
        if (i == config.warmup) {
            usage_start = perf::ResourceUsage::sample();
            measure_start_ns = perf::now_ns();
        }

        // Sequence 0 is reserved for "nothing pending". Responders take turns.
        auto sequence = static_cast<std::uint32_t>(i + 1);
        auto& request = requests[i % requests.size()];
        auto send_ns = perf::now_ns();
        perf::stamp_payload(request.payload, sequence, send_ns);
        {
            std::lock_guard<std::mutex> lock(response_mutex);
            pending_sequence = sequence;
        }
        application->send(request);

        bool answered = wait_for_pingpong_response(sequence, config.timeout);
        if (i >= config.warmup) {
            ++sent;
            if (answered) {
                std::lock_guard<std::mutex> lock(response_mutex);
                stats.add(completed_ns - send_ns);
            } else {
                ++lost;
            }
        }
        if (config.interval.count() > 0) {
            std::this_thread::sleep_for(config.interval);
        }
    }

    auto measure_end_ns = perf::now_ns();
    auto usage = perf::ResourceUsage::sample().since(usage_start);
    {
        std::lock_guard<std::mutex> lock(response_mutex);
        pending_sequence = 0;
    }

    auto duration_s = sent == 0 ? 0.0 : static_cast<double>(measure_end_ns - measure_start_ns) / 1e9;
    auto summary = stats.summarize();
    row.add("interval_us", config.interval.count())
        .add("sent", sent)
        .add("lost", lost)
        .add("late", late_responses.load())
        .add("invalid", invalid_responses.load())
        .add("duration_s", duration_s)
        .add("rate_msg_s", duration_s > 0 ? static_cast<double>(summary.count) / duration_s : 0.0)
        .add_latency(summary)
        .add_resources(usage);
    return RunResult{lost == 0 ? 0 : 3, std::move(stats)};
}

RunResult run_rate(std::vector<lgsomeip::api::Message>& requests, const RunConfig& config, perf::ResultRow& row) {
    rate_total = config.warmup + config.count;
    rate_warmup = config.warmup;
    rate_latency_ns.reset(new std::atomic<std::uint64_t>[rate_total]);
    for (std::uint64_t i = 0; i < rate_total; ++i) {
        rate_latency_ns[i] = 0;
    }

    std::uint64_t sent = 0;
    std::uint64_t max_in_flight = 0;
    std::uint64_t window_waits = 0;
    std::uint64_t max_lag_ns = 0;
    const perf::Schedule schedule(config.rate, perf::now_ns());
    const bool paced = schedule.paced();
    std::uint64_t measure_start_ns = perf::now_ns();
    bool stalled = false;
    perf::ResourceUsage usage_start = perf::ResourceUsage::sample();

    for (std::uint64_t i = 0; i < rate_total && !stop_requested; ++i) {
        if (i == config.warmup) {
            usage_start = perf::ResourceUsage::sample();
            measure_start_ns = perf::now_ns();
        }

        std::uint64_t scheduled_ns = 0;
        if (paced) {
            scheduled_ns = schedule.due_ns(i);
            perf::wait_until_ns(scheduled_ns);
        }

        // Bound the requests in flight so a provider that falls behind cannot
        // grow queues without limit; waiting here shows up as schedule lag.
        // If the window stays full for the whole timeout, responses are being
        // lost and the run stops as "stalled" instead of waiting per request.
        auto in_flight = i - rate_received.load();
        if (in_flight >= config.window) {
            ++window_waits;
            auto deadline = perf::now_ns() + static_cast<std::uint64_t>(config.timeout.count()) * 1000000;
            while (i - rate_received.load() >= config.window && perf::now_ns() < deadline && !stop_requested) {
                std::this_thread::yield();
            }
            in_flight = i - rate_received.load();
            if (in_flight >= config.window) {
                stalled = true;
                std::cerr << "perf-rpc-client: in-flight window stayed full for " << config.timeout.count()
                          << " ms; stopping after " << i << " requests" << std::endl;
                break;
            }
        }
        max_in_flight = std::max(max_in_flight, in_flight + 1);

        auto& request = requests[i % requests.size()];
        auto send_ns = perf::now_ns();
        if (!paced) {
            scheduled_ns = send_ns;
        }
        max_lag_ns = std::max(max_lag_ns, send_ns - scheduled_ns);
        perf::stamp_payload(request.payload, static_cast<std::uint32_t>(i + 1), scheduled_ns);
        application->send(request);
        if (i >= config.warmup) {
            ++sent;
        }
    }
    auto send_end_ns = perf::now_ns();

    // Drain: give outstanding responses up to the timeout to arrive.
    auto sent_total = config.warmup + sent;
    auto drain_deadline = send_end_ns + static_cast<std::uint64_t>(config.timeout.count()) * 1000000;
    while (rate_received.load() < sent_total && perf::now_ns() < drain_deadline && !stop_requested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto usage = perf::ResourceUsage::sample().since(usage_start);

    perf::LatencyStats stats;
    stats.reserve(sent);
    for (std::uint64_t i = config.warmup; i < sent_total; ++i) {
        auto latency = rate_latency_ns[i].load();
        if (latency != 0) {
            stats.add(latency);
        }
    }
    auto summary = stats.summarize();
    auto lost = sent - summary.count;

    // Throughput: measured responses over the time from the first measured
    // send to the last response. Offered rate: measured sends over send time.
    auto send_duration_s = static_cast<double>(send_end_ns - measure_start_ns) / 1e9;
    auto last_receive_ns = std::max(rate_last_receive_ns.load(), measure_start_ns);
    auto receive_duration_s = static_cast<double>(last_receive_ns - measure_start_ns) / 1e9;
    auto throughput = receive_duration_s > 0 ? static_cast<double>(summary.count) / receive_duration_s : 0.0;
    auto payload_bytes = static_cast<double>(requests.front().payload.size());

    row.add("target_rate_msg_s", config.rate)
        .add("window", config.window)
        .add("sent", sent)
        .add("lost", lost)
        .add("loss_pct", sent == 0 ? 0.0 : 100.0 * static_cast<double>(lost) / static_cast<double>(sent))
        .add("duplicates", rate_duplicates.load())
        .add("invalid", invalid_responses.load())
        .add("send_rate_msg_s", send_duration_s > 0 ? static_cast<double>(sent) / send_duration_s : 0.0)
        .add("throughput_msg_s", throughput)
        .add("throughput_mbit_s", throughput * payload_bytes * 8 / 1e6)
        .add("max_in_flight", max_in_flight)
        .add("window_waits", window_waits)
        .add("stalled", stalled ? 1 : 0)
        .add("max_send_lag_us", static_cast<double>(max_lag_ns) / 1000.0)
        .add("duration_s", receive_duration_s)
        .add_latency(summary)
        .add_resources(usage);
    return RunResult{lost == 0 && !stalled ? 0 : 3, std::move(stats)};
}

} // namespace

int main(int argc, char* argv[]) {
    perf::Options options(argc, argv,
                          {
                              {"name", "perf-rpc-client"},
                              {"config", ""},
                              {"mode", "pingpong"},
                              {"payload", "64"},
                              {"count", "10000"},
                              {"warmup", "1000"},
                              {"interval-us", "0"},
                              {"rate", "1000"},
                              {"window", "1000"},
                              {"timeout-ms", "1000"},
                              {"discovery-timeout-ms", "10000"},
                              {"label", ""},
                              {"csv", ""},
                              {"samples-out", ""},
                              {"responders", "1"},
                              {"start-at", "0"},
                          },
                          {"spin"});
    if (!options.ok()) {
        options.print_usage(argv[0]);
        return 1;
    }
    if (options.str("mode") == "pingpong") {
        mode = Mode::PingPong;
    } else if (options.str("mode") == "rate") {
        mode = Mode::Rate;
    } else {
        std::cerr << "perf-rpc-client: unsupported --mode '" << options.str("mode") << "'" << std::endl;
        return 1;
    }

    RunConfig config;
    config.count = options.u64("count");
    config.warmup = options.u64("warmup");
    config.interval = std::chrono::microseconds(options.u64("interval-us"));
    config.timeout = std::chrono::milliseconds(options.u64("timeout-ms"));
    responders = static_cast<std::uint16_t>(std::max<std::uint64_t>(options.u64("responders"), 1));
    // --rate is per responder; the requester offers responders times that rate in total.
    config.rate = std::strtod(options.str("rate").c_str(), nullptr) * responders;
    config.window = std::max<std::uint64_t>(options.u64("window"), 1);
    if (config.warmup + config.count > 0xFFFFFFFFull) {
        std::cerr << "perf-rpc-client: --warmup + --count must fit a 32-bit sequence number" << std::endl;
        return 1;
    }

    spin_wait = options.flag("spin");
    const auto payload_size = std::max<std::size_t>(options.u64("payload"), perf::kPayloadHeaderSize);
    if (payload_size > SOMEIP_UDP_MAX_PAYLOAD_SIZE) {
        std::cout << "perf-rpc-client: note: payload exceeds SOMEIP_UDP_MAX_PAYLOAD_SIZE ("
                  << SOMEIP_UDP_MAX_PAYLOAD_SIZE << "); remote UDP needs SOME/IP-TP or a TCP service" << std::endl;
    }

    application = lgsomeip::api::Runtime::instance().create_application(options.str("name"), options.str("config"));
    if (!application->init()) {
        std::cerr << "perf-rpc-client: init failed" << std::endl;
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
            std::cerr << "perf-rpc-client: only " << available_instances.size() << " of " << responders
                      << " responder(s) available before discovery timeout" << std::endl;
            application->stop();
            application->join();
            return 2;
        }
    }
    auto discovery_ms = static_cast<double>(perf::now_ns() - discovery_start) / 1e6;

    std::vector<lgsomeip::api::Message> requests(responders);
    for (std::uint16_t index = 0; index < responders; ++index) {
        auto& request = requests[index];
        request.service = perf::kRpcServiceId;
        request.instance = static_cast<lgsomeip::api::instance_t>(index + 1);
        request.method = perf::kRpcMethodId;
        request.interface_version = perf::kMajorVersion;
        request.type = lgsomeip::api::MessageType::Request;
        request.payload = perf::make_payload(payload_size);
    }

    std::cout << "perf-rpc-client: service available after " << discovery_ms << " ms; mode=" << options.str("mode")
              << ", " << config.warmup << " warm-up + " << config.count << " measured requests of " << payload_size
              << " bytes" << std::endl;

    perf::ResultRow row;
    row.add("timestamp", perf::utc_timestamp())
        .add("revision", PERF_GIT_REVISION)
        .add("scenario", mode == Mode::PingPong ? "rpc-pingpong" : "rpc-rate")
        .add("label", options.str("label"))
        .add("payload_bytes", payload_size)
        .add("discovery_ms", discovery_ms)
        .add("responders", responders);
    // Start barrier: several requesters begin their run at the same moment.
    row.add("start_late_ms", perf::wait_until_realtime_ms(options.u64("start-at")));
    RunResult result;
    if (mode == Mode::PingPong) {
        row.add("wait", spin_wait ? "spin" : "condvar");
        result = run_pingpong(requests, config, row);
    } else {
        result = run_rate(requests, config, row);
    }

    for (lgsomeip::api::instance_t instance = 1; instance <= responders; ++instance) {
        application->release_service(perf::kRpcServiceId, instance);
    }
    application->stop();
    application->join();

    std::cout << "perf-rpc-client result (round-trip):\n";
    row.print(std::cout);

    if (!options.str("csv").empty() && !row.append_csv(options.str("csv"))) {
        std::cerr << "perf-rpc-client: cannot write " << options.str("csv") << std::endl;
    }
    if (!options.str("samples-out").empty() && !result.samples.write_samples(options.str("samples-out"))) {
        std::cerr << "perf-rpc-client: cannot write " << options.str("samples-out") << std::endl;
    }
    return stop_requested ? 3 : result.exit_code;
}
