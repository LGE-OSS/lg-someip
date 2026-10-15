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
#include <mutex>
#include <thread>

#include <someip/message.h>
#include <transport/endpoint.h>
#include <transport/transport.h>
#include <transport/udp_transport.h>

#include "common/EventTally.h"
#include "common/Options.h"
#include "common/PerfPayload.h"
#include "common/Report.h"

// Direct-event subscriber for opensomeip: listens on one UDP endpoint, to
// which --publishers publishers send their notifications, and feeds the shared
// EventTally. SOME/IP carries no instance in its header, so publishers are told
// apart by their sender port (each publisher process binds its own port).
namespace {

std::atomic<bool> stop_requested{false};

class EventReceiver final : public someip::transport::ITransportListener {
public:
    EventReceiver(const someip::transport::Endpoint& endpoint, perf::Clock clock)
        : transport_(endpoint), tally_(clock) {
        transport_.set_listener(this);
    }

    bool start() {
        return transport_.start() == someip::Result::SUCCESS;
    }

    void stop() {
        transport_.stop();
    }

    void on_message_received(someip::MessagePtr message, const someip::transport::Endpoint& sender) override {
        // Take the receive timestamps before any copying, as the other stacks do.
        const auto received_clock_ns = perf::clock_ns(tally_.clock());
        const auto received_ns = perf::now_ns();
        if (!message || message->get_service_id() != perf::kEventServiceId ||
            message->get_message_type() != someip::MessageType::NOTIFICATION) {
            return;
        }
        const auto event_id = message->get_method_id();
        perf::Payload bytes(message->get_payload().begin(), message->get_payload().end());
        const std::uint64_t publisher = sender.get_port();
        std::lock_guard<std::mutex> lock(mutex_);
        if (event_id == perf::kDataEventId) {
            tally_.add_data(publisher, bytes, received_clock_ns, received_ns);
        } else if (event_id == perf::kEndEventId) {
            tally_.add_end(publisher, bytes);
        }
        condition_.notify_all();
    }

    // Wait for the end marker of every publisher. Gives up (false) when no
    // notification arrives within start_timeout, or when traffic stops for
    // idle_timeout before all end markers, as the other subscribers do.
    bool wait_for_completion(std::size_t publishers, std::chrono::milliseconds start_timeout,
                             std::chrono::milliseconds idle_timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto start_deadline = std::chrono::steady_clock::now() + start_timeout;
        const auto idle_ns = static_cast<std::uint64_t>(idle_timeout.count()) * 1000000;
        while (!stop_requested && tally_.ended() < publishers) {
            const auto seen = tally_.arrivals();
            condition_.wait_for(lock, std::chrono::milliseconds(100));
            if (tally_.arrivals() != seen || tally_.ended() >= publishers) {
                continue;
            }
            if (!tally_.received_any()) {
                if (std::chrono::steady_clock::now() >= start_deadline) {
                    return false;
                }
            } else if (perf::now_ns() - tally_.last_receive_ns() > idle_ns) {
                return false;
            }
        }
        return true;
    }

    // Callers must not hold the lock; the transport is stopped by then.
    perf::EventTally& tally() {
        return tally_;
    }

    void on_connection_lost(const someip::transport::Endpoint&) override {}
    void on_connection_established(const someip::transport::Endpoint&) override {}
    void on_error(someip::Result) override {}

private:
    someip::transport::UdpTransport transport_;
    std::mutex mutex_;
    std::condition_variable condition_;
    perf::EventTally tally_;
};

void handle_signal(int) {
    stop_requested = true;
}

} // namespace

int main(int argc, char* argv[]) {
    perf::Options options(argc, argv,
                          {
                              {"bind", "0.0.0.0"},
                              {"port", "30611"},
                              {"clock", "monotonic"},
                              {"publishers", "1"},
                              {"start-timeout-ms", "30000"},
                              {"idle-timeout-ms", "5000"},
                              {"drain-ms", "200"},
                              {"label", ""},
                              {"csv", ""},
                              {"samples-out", ""},
                          });
    perf::Clock clock = perf::Clock::Monotonic;
    if (!options.ok() || !perf::parse_clock(options.str("clock"), clock)) {
        options.print_usage(argv[0]);
        return 1;
    }
    const auto publishers = std::max<std::uint64_t>(options.u64("publishers"), 1);

    const auto usage_start = perf::ResourceUsage::sample();
    EventReceiver receiver(
        someip::transport::Endpoint(options.str("bind"), static_cast<std::uint16_t>(options.u64("port"))), clock);
    if (!receiver.start()) {
        std::cerr << "perf-event-subscriber: transport initialization failed" << std::endl;
        return 1;
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    const bool completed =
        receiver.wait_for_completion(publishers, std::chrono::milliseconds(options.u64("start-timeout-ms")),
                                     std::chrono::milliseconds(options.u64("idle-timeout-ms")));
    std::this_thread::sleep_for(std::chrono::milliseconds(options.u64("drain-ms")));
    const auto usage = perf::ResourceUsage::sample().since(usage_start);
    receiver.stop();

    auto& tally = receiver.tally();
    if (!tally.received_any()) {
        std::cerr << "perf-event-subscriber: no event notifications received" << std::endl;
        return 2;
    }

    perf::ResultRow row;
    row.add("timestamp", perf::utc_timestamp())
        .add("stack", "opensomeip")
        .add("architecture", "direct-event-no-sd")
        .add("scenario", "event-subscribe")
        .add("label", options.str("label"))
        .add("bind", options.str("bind"))
        .add("port", options.u64("port"))
        .add("clock", options.str("clock"));
    const auto lost = tally.add_columns(row, publishers);
    row.add_resources(usage);
    std::cout << "perf-event-subscriber result (one-way, direct event transport):\n";
    row.print(std::cout);
    if (!options.str("csv").empty() && !row.append_csv(options.str("csv"))) {
        std::cerr << "perf-event-subscriber: cannot write " << options.str("csv") << std::endl;
    }
    if (!options.str("samples-out").empty() && !tally.samples().write_samples(options.str("samples-out"))) {
        std::cerr << "perf-event-subscriber: cannot write " << options.str("samples-out") << std::endl;
    }
    return (stop_requested || !completed || lost != 0) ? 3 : 0;
}
