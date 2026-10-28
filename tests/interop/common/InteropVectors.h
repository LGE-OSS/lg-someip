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

#ifndef LG_SOMEIP_TESTS_INTEROP_VECTORS_H
#define LG_SOMEIP_TESTS_INTEROP_VECTORS_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <lgsomeip/LgsomeipApi.h>

namespace lgsomeip {
namespace interop {

using Bytes = std::vector<std::uint8_t>;

/**
 * One serialization test vector.
 *
 * golden is the expected SOME/IP wire encoding written out byte by byte. It is
 * independent of the serializer, so a missing byte swap is caught even when both
 * peers are little-endian (QNX aarch64 and Linux x86_64 are).
 *
 * encode() builds the payload from typed values with the library serializer.
 * decode() parses a payload with the library deserializer and returns true only
 * if every value equals the expected one and no bytes are left over.
 */
struct Vector {
    std::string name;
    api::method_t method{0};
    Bytes golden;
    std::function<Bytes()> encode;
    std::function<bool(const Bytes&)> decode;
    // Sizeable payloads that need a transport able to carry them; run on request.
    bool large{false};
};

/** An event notified by the server with the payload of one test vector. */
struct EventSpec {
    api::event_t event;
    api::eventgroup_t group;
    bool is_field;
    const char* vector_name;
};

const std::vector<Vector>& vectors();
const Vector* find_vector_by_method(api::method_t method);
const Vector* find_vector_by_name(const std::string& name);

const std::vector<EventSpec>& events();

/** Result of exercising one vector against the serializer without a network. */
struct LocalResult {
    bool encode_matches_golden{false};
    bool decode_golden{false};
    bool roundtrip{false};
    bool ok() const {
        return encode_matches_golden && decode_golden && roundtrip;
    }
};

LocalResult run_local(const Vector& vector);

/** Hex dump of the first bytes, for failure messages. */
std::string hex(const Bytes& bytes, std::size_t max_bytes = 32);

} // namespace interop
} // namespace lgsomeip

#endif // LG_SOMEIP_TESTS_INTEROP_VECTORS_H
