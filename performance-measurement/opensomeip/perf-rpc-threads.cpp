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
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <rpc/rpc_client.h>
#include <rpc/rpc_types.h>
#include <transport/endpoint.h>

#include "common/Options.h"
#include "common/Pacing.h"
#include "common/PerfPayload.h"
#include "common/Report.h"
#include "common/ThreadStress.h"

// Multi-threaded requester: one process, --threads worker threads, each sending
// a request and waiting for its response. The responder echoes the payload, so
// request and response have the same size. See common/ThreadStress.h.
//
// opensomeip has no daemon and no Service Discovery in this harness: one
// RpcClient per responder is shared by all workers and talks to the responder's
// fixed endpoint. Workers use call_method_async plus the same condition-variable
// wait as the other stacks; call_method_sync is not used because it polls with a
// 1 ms sleep, which would add latency and wake-ups that do not belong to the
// network path.
//
// Known defect worked around here: opensomeip 0.2.0's RpcClient creates a SOME/IP
// session for every call and never releases it. After 65,535 calls the session
// table holds every id, and SessionManager::get_next_session_id() loops forever
// while it holds the table's mutex, so every calling thread hangs (and the table
// grows until then). A requester that runs longer than that has to replace its
// RpcClient; ClientPool does so after kCallsPerClient calls, counts the
// replacements (client_renewals in the result row). The replacement happens
// inside one timed call, so that call's latency includes the new client's setup;
// it is one call in 50,000 per responder.
namespace {

constexpr std::uint64_t kCallsPerClient = 50000;

using ClientPtr = std::shared_ptr<someip::rpc::RpcClient>;

// One RpcClient per responder, shared by all workers and renewed as described above.
class ClientPool {
public:
    ClientPool(std::size_t responders, std::string server, std::uint16_t port,
               someip::transport::TransportProtocol protocol, std::uint16_t client_id)
        : server_(std::move(server)), port_(port), protocol_(protocol), client_id_(client_id), entries_(responders) {
    }

    bool initialize() {
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            entries_[index].client = create(index);
            if (!entries_[index].client) {
                return false;
            }
        }
        return true;
    }

    // The client for the next call to responder `index`; renews it when it has made its share.
    ClientPtr acquire(std::size_t index) {
        auto& entry = entries_[index];
        std::lock_guard<std::mutex> lock(entry.mutex);
        if (entry.calls >= kCallsPerClient) {
            if (auto fresh = create(index)) {
                entry.client = std::move(fresh);
                entry.calls = 0;
                ++renewals_;
            }
        }
        ++entry.calls;
        return entry.client;
    }

    std::size_t size() const {
        return entries_.size();
    }

    std::uint64_t renewals() const {
        return renewals_.load();
    }

    void shutdown() {
        for (auto& entry : entries_) {
            std::lock_guard<std::mutex> lock(entry.mutex);
            entry.client.reset();
        }
    }

private:
    struct Entry {
        std::mutex mutex;
        ClientPtr client;
        std::uint64_t calls{0};
    };

    ClientPtr create(std::size_t index) const {
        // The deleter shuts the client down when the last worker using it lets go.
        ClientPtr client(new someip::rpc::RpcClient(client_id_, perf::kMajorVersion,
                                                    someip::transport::Endpoint("0.0.0.0", 0, protocol_)),
                         [](someip::rpc::RpcClient* released) {
                             released->shutdown();
                             delete released;
                         });
        if (!client->initialize()) {
            return nullptr;
        }
        client->set_remote_endpoint(
            someip::transport::Endpoint(server_, static_cast<std::uint16_t>(port_ + index), protocol_));
        return client;
    }

    std::string server_;
    std::uint16_t port_;
    someip::transport::TransportProtocol protocol_;
    std::uint16_t client_id_;
    std::vector<Entry> entries_;
    std::atomic<std::uint64_t> renewals_{0};
};

std::unique_ptr<ClientPool> clients;
std::unique_ptr<perf::ResponseSlots> slots;
std::atomic<bool> stop_requested{false};
std::atomic<std::uint64_t> late_responses{0};
std::atomic<std::uint64_t> invalid_responses{0};

void handle_signal(int) {
    stop_requested = true;
}

void on_response(const someip::rpc::RpcResponse& response) {
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
    if (!slots->complete(header.sequence, bytes.size())) {
        ++late_responses;
    }
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
                              {"name", "perf-rpc-threads"},
                              {"server", "127.0.0.1"},
                              {"port", "30501"},
                              {"transport", "udp"},
                              {"payload", "64"},
                              {"threads", "8"},
                              {"responders", "1"},
                              {"warmup-s", "1"},
                              {"duration-s", "5"},
                              {"thread-rate", "0"},
                              {"timeout-ms", "1000"},
                              {"label", ""},
                              {"csv", ""},
                              {"samples-out", ""},
                              {"client-id", "801"},
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
    const auto responders = std::max<std::uint64_t>(options.u64("responders"), 1);
    if (config.threads > perf::kMaxStressThreads) {
        std::cerr << "perf-rpc-threads: --threads must be at most " << perf::kMaxStressThreads << std::endl;
        return 1;
    }
    slots = std::make_unique<perf::ResponseSlots>(config.threads);

    try {
        auto protocol = parse_transport(options.str("transport"));
        clients = std::make_unique<ClientPool>(responders, options.str("server"),
                                               static_cast<std::uint16_t>(options.u64("port")), protocol,
                                               static_cast<std::uint16_t>(options.u64("client-id")));
        if (!clients->initialize()) {
            std::cerr << "perf-rpc-threads: init failed" << std::endl;
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "perf-rpc-threads: " << error.what() << std::endl;
        return 1;
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    std::cout << "perf-rpc-threads: direct endpoint " << options.str("server") << ":" << options.u64("port") << "; "
              << config.threads << " threads, " << responders << " responder(s), " << config.payload_size
              << " bytes each way" << std::endl;

    perf::ResultRow row;
    row.add("timestamp", perf::utc_timestamp())
        .add("revision", PERF_GIT_REVISION)
        .add("stack", "opensomeip")
        .add("scenario", "rpc-threads")
        .add("label", options.str("label"))
        .add("payload_bytes", config.payload_size)
        .add("responders", responders)
        .add("discovery_ms", 0.0);
    row.add("start_late_ms", perf::wait_until_realtime_ms(options.u64("start-at")));

    const someip::rpc::RpcTimeout timeout{config.timeout, config.timeout};
    auto result = perf::run_thread_stress(
        config, [&](std::size_t thread, std::uint32_t sequence, std::uint64_t, perf::Payload& payload) -> std::size_t {
            slots->arm(thread, sequence);
            auto client = clients->acquire((thread + sequence) % clients->size());
            if (client->call_method_async(perf::kRpcServiceId, perf::kRpcMethodId, payload, on_response, timeout) == 0) {
                return 0;
            }
            return slots->wait(thread, config.timeout);
        });
    row.add("late", late_responses.load())
        .add("invalid", invalid_responses.load())
        .add("client_renewals", clients->renewals());
    perf::add_thread_stress_columns(row, config, result);

    clients->shutdown();
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
