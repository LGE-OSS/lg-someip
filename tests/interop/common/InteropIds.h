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

#ifndef LG_SOMEIP_TESTS_INTEROP_IDS_H
#define LG_SOMEIP_TESTS_INTEROP_IDS_H

#include <lgsomeip/LgsomeipApi.h>

namespace lgsomeip {
namespace interop {

// Identifiers shared by interop-server, interop-client and interop-subscriber.
// They must match tests/interop/config/interop_config.json.
//
// The library sends a request on every transport configured for a service, so
// a service with a reliable and an unreliable endpoint would receive each
// request twice. The service therefore has one instance per transport: a client
// or subscriber requests the instance of the transport it wants to test.
constexpr api::service_t kServiceId = 0x2001;
constexpr api::instance_t kInstanceTcp = 0x0001;
constexpr api::instance_t kInstanceUdp = 0x0002;
constexpr api::major_version_t kMajorVersion = 0x01;

enum class Transport { Tcp, Udp };

constexpr api::instance_t instance_of(Transport transport) {
    return transport == Transport::Tcp ? kInstanceTcp : kInstanceUdp;
}

// Control methods. Test-vector methods are assigned from kVectorMethodBase upward.
constexpr api::method_t kVectorMethodBase = 0x0100;
constexpr api::method_t kMethodReset = 0x0F00;         // clear the server's counters
constexpr api::method_t kMethodReport = 0x0F01;        // returns u32 pass, u32 fail
constexpr api::method_t kMethodNoReturn = 0x0F10;      // fire-and-forget; the server counts calls
constexpr api::method_t kMethodNoReturnCount = 0x0F11; // returns the u32 call count
constexpr api::method_t kMethodError = 0x0F20;         // replies with Error and the return code in payload[0]
constexpr api::method_t kMethodEcho = 0x0F30;          // echoes the payload unchanged

// Event groups. The multicast group exists on the UDP instance only and is
// configured with a threshold of two subscribers: one subscriber is served by
// unicast, two or more by multicast.
constexpr api::eventgroup_t kGroupUnicast = 0x0100;
constexpr api::eventgroup_t kGroupMulticast = 0x0200;

} // namespace interop
} // namespace lgsomeip

#endif // LG_SOMEIP_TESTS_INTEROP_IDS_H
