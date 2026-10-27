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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <lgsomeip/LgsomeipApi.h>

#include "InteropIds.h"
#include "InteropVectors.h"

// Method tests against interop-server.
//
//   interop-client [--tcp | --udp] [--app name] [--large] [--timeout seconds] [--wait seconds]
//
//   --tcp      send the requests over TCP, service instance 0x0001 (default)
//   --udp      send the requests over UDP, service instance 0x0002
//   --app      application name from the configuration (default interop-client)
//   --large    also send the 4000 and 30000 byte payloads (TCP only, UDP cannot carry them)
//   --timeout  wait per request (default 5)
//   --wait     wait for the service to become available (default 30)
//
// Run the client once per transport to test both. The selected instance is the
// only one the client requests, so every request travels over exactly one transport.
//
// Checks: every test vector goes to the server, which verifies it independently
// and answers with a payload it serialized itself; the answer is verified here
// as well. Then fire-and-forget requests, the return codes 0x01-0x0F (the codes
// above 0x0A are reserved and must pass through unchanged), client and
// session correlation of 20 outstanding requests, and the server's own verdict.
// Exit code: 0 when everything passes, 1 on a failure, 2 on a usage error and 3
// if the service never became available.
namespace {

using lgsomeip::api::Message;
using lgsomeip::api::MessageType;
using lgsomeip::api::Payload;
using lgsomeip::api::ReturnCode;
namespace interop = lgsomeip::interop;

struct Options {
    std::string app{"interop-client"};
    interop::Transport transport{interop::Transport::Tcp};
    bool transport_set{false};
    bool large{false};
    int timeout_s{5};
    int wait_s{30};
};

std::shared_ptr<lgsomeip::api::Application> application;
lgsomeip::api::instance_t instance = interop::kInstanceTcp;
bool service_requested = false;

std::mutex mutex;
std::condition_variable condition;
bool service_available = false;
std::deque<std::shared_ptr<Message>> inbox;

void on_message(const std::shared_ptr<Message>& message) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        inbox.push_back(message);
    }
    condition.notify_all();
}

void on_availability(lgsomeip::api::service_t, lgsomeip::api::instance_t, bool available) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        service_available = available;
    }
    condition.notify_all();
}

void on_state(bool registered) {
    if (registered && !service_requested) {
        application->request_service(interop::kServiceId, instance, interop::kMajorVersion);
        service_requested = true;
    }
}

bool wait_for_service(int seconds) {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, std::chrono::seconds(seconds), [] { return service_available; });
}

Message make_request(lgsomeip::api::method_t method, Payload payload = {}, MessageType type = MessageType::Request) {
    Message request;
    request.service = interop::kServiceId;
    request.instance = instance;
    request.method = method;
    request.interface_version = interop::kMajorVersion;
    request.type = type;
    request.payload = std::move(payload);
    return request;
}

// Collect count messages from the inbox, waiting up to the timeout.
std::vector<std::shared_ptr<Message>> collect(std::size_t count, int timeout_s) {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait_for(lock, std::chrono::seconds(timeout_s), [count] { return inbox.size() >= count; });
    std::vector<std::shared_ptr<Message>> messages;
    while (!inbox.empty() && messages.size() < count) {
        messages.push_back(inbox.front());
        inbox.pop_front();
    }
    return messages;
}

// Send one request and wait for its answer; null when none arrives in time.
std::shared_ptr<Message> call(const Message& request, int timeout_s) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        inbox.clear();
    }
    application->send(request);
    auto messages = collect(1, timeout_s);
    return messages.empty() ? nullptr : messages.front();
}

std::uint32_t read_u32(const Payload& payload, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; i++) {
        value = (value << 8) | payload[offset + i];
    }
    return value;
}

struct Counters {
    int passed{0};
    int failed{0};
    int skipped{0};
};

Counters counters;

void report(const std::string& name, bool ok, const std::string& detail = "") {
    const char* verdict = "PASS";
    if (ok) {
        counters.passed++;
    } else {
        verdict = "FAIL";
        counters.failed++;
    }
    std::cout << verdict << "  " << name;
    if (!ok && !detail.empty()) {
        std::cout << "  -- " << detail;
    }
    std::cout << std::endl;
}

std::string hex8(unsigned value) {
    static const char digits[] = "0123456789ABCDEF";
    return std::string("0x") + digits[(value >> 4) & 0xF] + digits[value & 0xF];
}

// Checks shared by every response: type, return code and request identifiers.
bool response_ok(const std::shared_ptr<Message>& response, MessageType type, ReturnCode code, std::string& why) {
    if (!response) {
        why = "no response";
    } else if (response->type != type) {
        why = "message type " + hex8(static_cast<unsigned>(response->type));
    } else if (response->return_code != code) {
        why = "return code " + hex8(static_cast<unsigned>(response->return_code));
    } else if (response->client != application->client_id()) {
        why = "client id mismatch";
    } else if (response->session == 0) {
        why = "session is zero";
    } else {
        return true;
    }
    return false;
}

int run_vectors(const Options& options) {
    int executed = 0;
    for (const auto& vector : interop::vectors()) {
        if (vector.large && !options.large) {
            counters.skipped++;
            std::cout << "SKIP  " << vector.name << " (use --tcp --large)" << std::endl;
            continue;
        }
        executed++;

        auto response = call(make_request(vector.method, vector.encode()), options.timeout_s);
        std::string why;
        if (!response_ok(response, MessageType::Response, ReturnCode::Ok, why)) {
            report("vector " + vector.name, false, why);
        } else if (response->payload != vector.golden) {
            report("vector " + vector.name, false,
                   "response != golden: " + interop::hex(response->payload) + " vs " + interop::hex(vector.golden));
        } else if (!vector.decode(response->payload)) {
            report("vector " + vector.name, false, "response does not decode");
        } else {
            report("vector " + vector.name, true);
        }
    }
    return executed;
}

void run_no_return(const Options& options) {
    constexpr int kCalls = 5;
    for (int i = 0; i < kCalls; i++) {
        application->send(make_request(interop::kMethodNoReturn, {}, MessageType::RequestNoReturn));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto response = call(make_request(interop::kMethodNoReturnCount), options.timeout_s);
    std::string why;
    if (!response_ok(response, MessageType::Response, ReturnCode::Ok, why) || response->payload.size() != 4) {
        report("request-no-return", false, why.empty() ? "bad payload size" : why);
    } else {
        const auto count = read_u32(response->payload, 0);
        report("request-no-return", count == kCalls, "server counted " + std::to_string(count));
    }
}

void run_return_codes(const Options& options) {
    for (std::uint8_t code = 0x01; code <= 0x0F; code++) {
        auto response = call(make_request(interop::kMethodError, {code}), options.timeout_s);
        std::string why;
        report("return-code " + hex8(code),
               response_ok(response, MessageType::Error, static_cast<ReturnCode>(code), why), why);
    }
}

// Send many requests without waiting, then match the answers by payload index.
void run_correlation(const Options& options) {
    constexpr std::uint32_t kRequests = 20;
    {
        std::lock_guard<std::mutex> lock(mutex);
        inbox.clear();
    }
    for (std::uint32_t i = 0; i < kRequests; i++) {
        Payload payload{static_cast<std::uint8_t>(i >> 24), static_cast<std::uint8_t>(i >> 16),
                        static_cast<std::uint8_t>(i >> 8), static_cast<std::uint8_t>(i)};
        application->send(make_request(interop::kMethodEcho, payload));
    }

    const auto responses = collect(kRequests, options.timeout_s);
    std::set<std::uint32_t> indexes;
    std::set<std::uint16_t> sessions;
    std::string why;
    for (const auto& response : responses) {
        std::string reason;
        if (!response_ok(response, MessageType::Response, ReturnCode::Ok, reason)) {
            why = reason;
        } else if (response->payload.size() != 4) {
            why = "echo payload size";
        } else {
            indexes.insert(read_u32(response->payload, 0));
            sessions.insert(response->session);
        }
    }
    if (why.empty() && (responses.size() != kRequests || indexes.size() != kRequests)) {
        why = "got " + std::to_string(responses.size()) + " responses, " + std::to_string(indexes.size()) +
              " distinct payloads";
    }
    if (why.empty() && sessions.size() != kRequests) {
        why = "sessions are not unique (" + std::to_string(sessions.size()) + " distinct)";
    }
    report("client/session correlation (" + std::to_string(kRequests) + " outstanding)", why.empty(), why);
}

void run_server_report(const Options& options, int executed) {
    auto response = call(make_request(interop::kMethodReport), options.timeout_s);
    std::string why;
    if (!response_ok(response, MessageType::Response, ReturnCode::Ok, why) || response->payload.size() != 8) {
        report("server-side verification", false, why.empty() ? "bad payload size" : why);
        return;
    }
    const auto pass = static_cast<int>(read_u32(response->payload, 0));
    const auto fail = static_cast<int>(read_u32(response->payload, 4));
    std::cout << "server saw: pass=" << pass << " fail=" << fail << " of " << executed << " vectors" << std::endl;
    const bool ok = fail == 0 && pass == executed;
    report("server-side verification", ok, "server counters do not add up to the vectors sent");
}

bool parse(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--tcp" || arg == "--udp") {
            const auto transport = arg == "--tcp" ? interop::Transport::Tcp : interop::Transport::Udp;
            if (options.transport_set && options.transport != transport) {
                return false; // --tcp and --udp exclude each other
            }
            options.transport = transport;
            options.transport_set = true;
        } else if (arg == "--large") {
            options.large = true;
        } else if (arg == "--app" && has_value) {
            options.app = argv[++i];
        } else if (arg == "--timeout" && has_value) {
            options.timeout_s = std::atoi(argv[++i]);
        } else if (arg == "--wait" && has_value) {
            options.wait_s = std::atoi(argv[++i]);
        } else {
            return false;
        }
    }
    const bool large_ok = !options.large || options.transport == interop::Transport::Tcp;
    return large_ok && options.timeout_s > 0 && options.wait_s > 0;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        std::cerr << "usage: " << argv[0] << " [--tcp | --udp] [--app name] [--large (TCP only)] [--timeout s]"
                  << " [--wait s]" << std::endl;
        return 2;
    }
    instance = interop::instance_of(options.transport);

    application = lgsomeip::api::Runtime::instance().create_application(options.app);
    if (!application->init()) {
        return 1;
    }

    // Install callbacks before start() so no response can arrive unhandled.
    application->register_application_state_handler(on_state);
    application->register_availability_handler(interop::kServiceId, instance, on_availability,
                                               interop::kMajorVersion);
    for (const auto& vector : interop::vectors()) {
        application->register_message_handler(interop::kServiceId, instance, vector.method, on_message);
    }
    for (const auto method : {interop::kMethodReset, interop::kMethodReport, interop::kMethodNoReturnCount,
                              interop::kMethodError, interop::kMethodEcho}) {
        application->register_message_handler(interop::kServiceId, instance, method, on_message);
    }
    application->start();

    if (!wait_for_service(options.wait_s)) {
        std::cerr << "service 0x" << std::hex << interop::kServiceId << " not available" << std::endl;
        application->stop();
        return 3;
    }

    std::string why;
    auto reset = call(make_request(interop::kMethodReset), options.timeout_s);
    if (!response_ok(reset, MessageType::Response, ReturnCode::Ok, why)) {
        std::cerr << "reset failed: " << why << std::endl;
        application->stop();
        return 1;
    }

    std::cout << "transport: " << (options.transport == interop::Transport::Tcp ? "TCP" : "UDP") << " (instance 0x"
              << std::hex << instance << std::dec << ")" << std::endl;

    const int executed = run_vectors(options);
    run_no_return(options);
    run_return_codes(options);
    run_correlation(options);
    run_server_report(options, executed);

    std::cout << "result: passed=" << counters.passed << " failed=" << counters.failed
              << " skipped=" << counters.skipped << std::endl;
    application->stop();
    return counters.failed == 0 ? 0 : 1;
}
