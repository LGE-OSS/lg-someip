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

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <lgsomeip/LgsomeipApi.h>

#include "InteropIds.h"
#include "InteropVectors.h"

// Provider for the interoperability tests (service 0x2001).
//
//   interop-server [application-name]      default application name: interop-server
//
// The service is offered twice: instance 0x0001 over TCP and instance 0x0002 over
// UDP (see InteropIds.h), so one server run serves clients of either transport.
//
// Methods: one method per test vector (see InteropVectors.cpp) plus a few control
// methods (InteropIds.h). For each vector request the server checks the received
// bytes against the golden bytes, decodes them with the library deserializer, and
// answers with a payload it serialized itself. interop-client asks for the verdict
// with kMethodReport. The counters are shared by both instances.
//
// Events: every EventSpec is notified twice a second with the encoding of its
// vector; interop-subscriber verifies them. The multicast group is offered by the
// UDP instance only.
namespace {

using lgsomeip::api::Message;
using lgsomeip::api::MessageType;
using lgsomeip::api::Payload;
using lgsomeip::api::ReturnCode;
namespace interop = lgsomeip::interop;

const lgsomeip::api::instance_t kInstances[] = {interop::kInstanceTcp, interop::kInstanceUdp};

std::shared_ptr<lgsomeip::api::Application> application;
std::atomic<bool> registered{false};
// The core retains offers and restores them after a daemon reconnect. The flag is
// read by the notifying main thread and written by the state callback.
std::atomic<bool> offered{false};

std::atomic<std::uint32_t> pass_count{0};
std::atomic<std::uint32_t> fail_count{0};
std::atomic<std::uint32_t> no_return_count{0};

void append_u32(Payload& payload, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        payload.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

void respond(const Message& request, Payload payload, MessageType type = MessageType::Response,
             ReturnCode return_code = ReturnCode::Ok) {
    // Keep the request's identifiers so the requester can correlate the answer.
    Message response;
    response.service = request.service;
    response.instance = request.instance;
    response.method = request.method;
    response.client = request.client;
    response.session = request.session;
    response.interface_version = request.interface_version;
    response.type = type;
    response.return_code = return_code;
    response.payload = std::move(payload);
    application->send(response);
}

bool is_request(const std::shared_ptr<Message>& message) {
    return message && (message->type == MessageType::Request || message->type == MessageType::RequestNoReturn);
}

void on_vector(const std::shared_ptr<Message>& request) {
    if (!is_request(request)) {
        return;
    }
    const interop::Vector* vector = interop::find_vector_by_method(request->method);
    if (vector == nullptr) {
        return;
    }

    const bool wire_ok = request->payload == vector->golden;
    const bool decode_ok = vector->decode(request->payload);
    if (wire_ok && decode_ok) {
        pass_count++;
    } else {
        fail_count++;
        std::cout << "FAIL  " << vector->name << " wire==golden:" << wire_ok << " decode:" << decode_ok << std::endl
                  << "      golden  : " << interop::hex(vector->golden) << std::endl
                  << "      received: " << interop::hex(request->payload) << std::endl;
    }

    if (request->type == MessageType::Request) {
        respond(*request, vector->encode());
    }
}

void on_reset(const std::shared_ptr<Message>& request) {
    if (!is_request(request)) {
        return;
    }
    pass_count = 0;
    fail_count = 0;
    no_return_count = 0;
    respond(*request, {});
}

void on_report(const std::shared_ptr<Message>& request) {
    if (!is_request(request)) {
        return;
    }
    Payload payload;
    append_u32(payload, pass_count);
    append_u32(payload, fail_count);
    std::cout << "report: pass=" << pass_count << " fail=" << fail_count << std::endl;
    respond(*request, std::move(payload));
}

void on_no_return(const std::shared_ptr<Message>& request) {
    if (is_request(request)) {
        no_return_count++;
    }
}

void on_no_return_count(const std::shared_ptr<Message>& request) {
    if (!is_request(request)) {
        return;
    }
    Payload payload;
    append_u32(payload, no_return_count);
    respond(*request, std::move(payload));
}

// The first payload byte selects the return code of the Error response.
void on_error(const std::shared_ptr<Message>& request) {
    if (!is_request(request) || request->type != MessageType::Request) {
        return;
    }
    const auto code = request->payload.empty() ? ReturnCode::NotOk : static_cast<ReturnCode>(request->payload[0]);
    respond(*request, {}, MessageType::Error, code);
}

void on_echo(const std::shared_ptr<Message>& request) {
    if (is_request(request) && request->type == MessageType::Request) {
        respond(*request, request->payload);
    }
}

// Multicast is not available over TCP, so only the UDP instance has the multicast group.
bool offers_event(lgsomeip::api::instance_t instance, const interop::EventSpec& spec) {
    return instance == interop::kInstanceUdp || spec.group != interop::kGroupMulticast;
}

void on_state(bool is_registered) {
    registered = is_registered;
    if (!is_registered || offered) {
        return;
    }

    for (const auto instance : kInstances) {
        for (const auto& spec : interop::events()) {
            if (offers_event(instance, spec)) {
                application->offer_event(interop::kServiceId, instance, spec.event, {spec.group}, spec.is_field);
            }
        }
        application->offer_service(interop::kServiceId, instance, interop::kMajorVersion);
    }
    offered = true;
}

void register_provider_handler(lgsomeip::api::method_t method, const lgsomeip::api::MessageHandler& handler) {
    for (const auto instance : kInstances) {
        application->register_message_handler(interop::kServiceId, instance, method, handler, true);
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string name = argc > 1 ? argv[1] : "interop-server";
    application = lgsomeip::api::Runtime::instance().create_application(name);
    if (!application->init()) {
        return 1;
    }

    for (const auto& vector : interop::vectors()) {
        register_provider_handler(vector.method, on_vector);
    }
    register_provider_handler(interop::kMethodReset, on_reset);
    register_provider_handler(interop::kMethodReport, on_report);
    register_provider_handler(interop::kMethodNoReturn, on_no_return);
    register_provider_handler(interop::kMethodNoReturnCount, on_no_return_count);
    register_provider_handler(interop::kMethodError, on_error);
    register_provider_handler(interop::kMethodEcho, on_echo);
    application->register_application_state_handler(on_state);
    application->start();

    while (true) {
        if (registered && offered) {
            for (const auto instance : kInstances) {
                for (const auto& spec : interop::events()) {
                    const interop::Vector* vector = interop::find_vector_by_name(spec.vector_name);
                    if (vector != nullptr && offers_event(instance, spec)) {
                        // Update on change (spec 6.4.1) skips a payload equal to the previous one,
                        // for an event and a field alike. Events repeat the same bytes on purpose,
                        // so force them; a field is sent once and again only when its value changes.
                        application->notify(interop::kServiceId, instance, spec.event, vector->encode(),
                                            !spec.is_field);
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}
