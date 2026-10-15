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
#include <mutex>
#include <thread>
#include <vector>

#include <rpc/rpc_client.h>
#include <rpc/rpc_types.h>
#include <transport/endpoint.h>

#include "common/LatencyStats.h"
#include "common/Options.h"
#include "common/Pacing.h"
#include "common/PerfPayload.h"
#include "common/Report.h"

namespace {

enum class Mode { PingPong, Rate };

// One RpcClient per responder: opensomeip's client talks to one remote
// endpoint. Responder k listens on --port + k - 1; requests go to them in turn,
// so with --rate R every responder receives R requests per second.
std::vector<std::unique_ptr<someip::rpc::RpcClient>> clients;
Mode mode = Mode::PingPong;
std::atomic<bool> stop_requested{false};
std::atomic<std::uint64_t> late_responses{0};
std::atomic<std::uint64_t> invalid_responses{0};

std::mutex response_mutex;
std::condition_variable response_condition;
std::uint32_t pending_sequence = 0;
std::uint32_t completed_sequence = 0;
std::uint64_t completed_ns = 0;

std::unique_ptr<std::atomic<std::uint64_t>[]> rate_latency_ns;
std::uint64_t rate_total = 0;
std::atomic<std::uint64_t> rate_received{0};
std::atomic<std::uint64_t> rate_duplicates{0};
std::atomic<std::uint64_t> rate_last_receive_ns{0};

void handle_signal(int) {
    stop_requested = true;
    response_condition.notify_all();
}

void on_response(const someip::rpc::RpcResponse& response) {
    auto received_ns = perf::now_ns();
    if (response.result != someip::rpc::RpcResult::SUCCESS) {
        ++invalid_responses;
        return;
    }
    perf::Payload bytes(response.return_values.begin(), response.return_values.end());
    perf::PayloadHeader header;
    if (!perf::parse_payload(bytes, header)) {
        ++invalid_responses;
        return;
    }
    if (mode == Mode::PingPong) {
        std::lock_guard<std::mutex> lock(response_mutex);
        if (header.sequence != pending_sequence) {
            ++late_responses;
            return;
        }
        completed_ns = received_ns;
        completed_sequence = header.sequence;
        response_condition.notify_one();
        return;
    }

    if (header.sequence == 0 || header.sequence > rate_total) {
        ++invalid_responses;
        return;
    }
    auto latency = received_ns > header.timestamp_ns ? received_ns - header.timestamp_ns : 1;
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

someip::rpc::RpcCallHandle send_stamped(perf::Payload& bytes, std::uint32_t sequence, std::uint64_t timestamp_ns,
                                        const someip::rpc::RpcTimeout& timeout) {
    perf::stamp_payload(bytes, sequence, timestamp_ns);
    auto& client = clients[(sequence - 1) % clients.size()];
    return client->call_method_async(perf::kRpcServiceId, perf::kRpcMethodId, bytes, on_response, timeout);
}

RunResult run_pingpong(perf::Payload& bytes, const RunConfig& config, perf::ResultRow& row) {
    perf::LatencyStats stats;
    stats.reserve(config.count);
    someip::rpc::RpcTimeout timeout{config.timeout, config.timeout};
    std::uint64_t lost = 0;
    std::uint64_t sent = 0;
    perf::ResourceUsage usage_start;
    std::uint64_t measure_start_ns = 0;

    for (std::uint64_t i = 0; i < config.warmup + config.count && !stop_requested; ++i) {
        if (i == config.warmup) {
            usage_start = perf::ResourceUsage::sample();
            measure_start_ns = perf::now_ns();
        }
        auto sequence = static_cast<std::uint32_t>(i + 1);
        auto send_ns = perf::now_ns();
        {
            std::lock_guard<std::mutex> lock(response_mutex);
            pending_sequence = sequence;
            completed_sequence = 0;
        }
        if (send_stamped(bytes, sequence, send_ns, timeout) == 0) {
            if (i >= config.warmup) {
                ++sent;
                ++lost;
            }
        } else {
            std::unique_lock<std::mutex> lock(response_mutex);
            bool answered = response_condition.wait_for(
                lock, config.timeout, [&] { return completed_sequence == sequence || stop_requested; });
            if (i >= config.warmup) {
                ++sent;
                if (answered && completed_sequence == sequence) {
                    stats.add(completed_ns - send_ns);
                } else {
                    ++lost;
                }
            }
        }
        if (config.interval.count() > 0) {
            std::this_thread::sleep_for(config.interval);
        }
    }

    auto measure_end_ns = perf::now_ns();
    auto usage = perf::ResourceUsage::sample().since(usage_start);
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

RunResult run_rate(perf::Payload& bytes, const RunConfig& config, perf::ResultRow& row) {
    rate_total = config.warmup + config.count;
    rate_latency_ns.reset(new std::atomic<std::uint64_t>[rate_total]);
    for (std::uint64_t i = 0; i < rate_total; ++i) {
        rate_latency_ns[i] = 0;
    }

    someip::rpc::RpcTimeout timeout{config.timeout, config.timeout};
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
        auto send_ns = perf::now_ns();
        if (!paced) {
            scheduled_ns = send_ns;
        }
        max_lag_ns = std::max(max_lag_ns, send_ns - scheduled_ns);
        if (send_stamped(bytes, static_cast<std::uint32_t>(i + 1), scheduled_ns, timeout) == 0) {
            ++invalid_responses;
        }
        if (i >= config.warmup) {
            ++sent;
        }
    }
    auto send_end_ns = perf::now_ns();
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
    auto send_duration_s = static_cast<double>(send_end_ns - measure_start_ns) / 1e9;
    auto last_receive_ns = std::max(rate_last_receive_ns.load(), measure_start_ns);
    auto receive_duration_s = static_cast<double>(last_receive_ns - measure_start_ns) / 1e9;
    auto throughput = receive_duration_s > 0 ? static_cast<double>(summary.count) / receive_duration_s : 0.0;

    row.add("target_rate_msg_s", config.rate)
        .add("window", config.window)
        .add("sent", sent)
        .add("lost", lost)
        .add("loss_pct", sent == 0 ? 0.0 : 100.0 * static_cast<double>(lost) / static_cast<double>(sent))
        .add("duplicates", rate_duplicates.load())
        .add("invalid", invalid_responses.load())
        .add("send_rate_msg_s", send_duration_s > 0 ? static_cast<double>(sent) / send_duration_s : 0.0)
        .add("throughput_msg_s", throughput)
        .add("throughput_mbit_s", throughput * static_cast<double>(bytes.size()) * 8 / 1e6)
        .add("max_in_flight", max_in_flight)
        .add("window_waits", window_waits)
        .add("stalled", stalled ? 1 : 0)
        .add("max_send_lag_us", static_cast<double>(max_lag_ns) / 1000.0)
        .add("duration_s", receive_duration_s)
        .add_latency(summary)
        .add_resources(usage);
    return RunResult{lost == 0 && !stalled ? 0 : 3, std::move(stats)};
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
                              {"name", "perf-rpc-client"}, {"config", ""},       {"server", "127.0.0.1"},
                              {"port", "30501"},           {"transport", "udp"}, {"mode", "pingpong"},
                              {"payload", "64"},           {"count", "10000"},   {"warmup", "1000"},
                              {"interval-us", "0"},        {"rate", "1000"},     {"window", "1000"},
                              {"timeout-ms", "1000"},      {"label", ""},        {"csv", ""},
                              {"samples-out", ""},         {"responders", "1"},  {"client-id", "801"},
                              {"start-at", "0"},
                          });
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
    const auto responders = std::max<std::uint64_t>(options.u64("responders"), 1);
    // --rate is per responder; the requester offers responders times that rate in total.
    config.rate = std::strtod(options.str("rate").c_str(), nullptr) * static_cast<double>(responders);
    config.window = std::max<std::uint64_t>(options.u64("window"), 1);
    if (config.warmup + config.count > 0xFFFFFFFFull) {
        std::cerr << "perf-rpc-client: --warmup + --count must fit a 32-bit sequence number" << std::endl;
        return 1;
    }
    const auto payload_size = std::max<std::size_t>(options.u64("payload"), perf::kPayloadHeaderSize);

    try {
        auto protocol = parse_transport(options.str("transport"));
        // Default client ID 801 = 0x0321, the ID of perf-rpc-client in the other stacks' configurations.
        const auto client_id = static_cast<std::uint16_t>(options.u64("client-id"));
        for (std::uint64_t k = 0; k < responders; ++k) {
            auto client = std::make_unique<someip::rpc::RpcClient>(client_id, perf::kMajorVersion,
                                                                   someip::transport::Endpoint("0.0.0.0", 0, protocol));
            if (!client->initialize()) {
                std::cerr << "perf-rpc-client: init failed" << std::endl;
                return 1;
            }
            client->set_remote_endpoint(someip::transport::Endpoint(
                options.str("server"), static_cast<std::uint16_t>(options.u64("port") + k), protocol));
            clients.push_back(std::move(client));
        }
    } catch (const std::exception& error) {
        std::cerr << "perf-rpc-client: " << error.what() << std::endl;
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    perf::Payload bytes = perf::make_payload(payload_size);
    std::cout << "perf-rpc-client: direct endpoint " << options.str("server") << ":" << options.u64("port")
              << "; mode=" << options.str("mode") << ", " << config.warmup << " warm-up + " << config.count
              << " measured requests of " << payload_size << " bytes" << std::endl;

    perf::ResultRow row;
    row.add("timestamp", perf::utc_timestamp())
        .add("stack", "opensomeip")
        .add("scenario", mode == Mode::PingPong ? "rpc-pingpong" : "rpc-rate")
        .add("label", options.str("label"))
        .add("payload_bytes", payload_size)
        .add("server", options.str("server"))
        .add("port", options.u64("port"))
        .add("responders", responders);
    // Start barrier: several requesters begin their run at the same moment.
    row.add("start_late_ms", perf::wait_until_realtime_ms(options.u64("start-at")));
    RunResult result;
    if (mode == Mode::PingPong) {
        row.add("wait", "condvar");
        result = run_pingpong(bytes, config, row);
    } else {
        result = run_rate(bytes, config, row);
    }

    for (auto& client : clients) {
        client->shutdown();
    }
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
