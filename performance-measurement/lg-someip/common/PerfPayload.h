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

#ifndef LG_SOMEIP_PERF_PAYLOAD_H
#define LG_SOMEIP_PERF_PAYLOAD_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Shared by the lg-someip benchmark applications and by the vsomeip and
// opensomeip applications in performance-measurement/, so every stack sends the same
// wire layout and IDs. It only deals in plain std::vector<uint8_t>, which is
// also what lgsomeip::api::Payload is, so it depends on no SOME/IP stack.
// Each application converts to and from its own payload type at the call site.

namespace perf {

using byte_t = std::uint8_t;
using Payload = std::vector<byte_t>;

// Shared identifiers for every performance application and configuration file
// (see scripts/gen-config.sh). They are the same for every stack, so the same
// service/instance/method/event IDs appear on the wire whichever stack is
// under test.
constexpr std::uint16_t kRpcServiceId = 0x2001;
constexpr std::uint16_t kRpcInstanceId = 0x0001;
constexpr std::uint16_t kRpcMethodId = 0x0001;
constexpr std::uint8_t kMajorVersion = 0x01;

constexpr std::uint16_t kEventServiceId = 0x2002;
constexpr std::uint16_t kEventInstanceId = 0x0001;
constexpr std::uint16_t kEventGroupId = 0x0001;
// Measurement notifications.
constexpr std::uint16_t kDataEventId = 0x8001;
// End-of-run marker; see make_end_marker().
constexpr std::uint16_t kEndEventId = 0x8002;

// Service Discovery probe: the provider stamps its offer time into a field
// (sent to each new subscriber), sends heartbeats while offered, and announces
// a graceful stop just before withdrawing the offer.
constexpr std::uint16_t kSdServiceId = 0x2003;
constexpr std::uint16_t kSdInstanceId = 0x0001;
constexpr std::uint16_t kSdEventGroupId = 0x0001;
constexpr std::uint16_t kSdOfferFieldId = 0x8001;
constexpr std::uint16_t kSdHeartbeatEventId = 0x8002;
constexpr std::uint16_t kSdStoppingEventId = 0x8003;

// Every measurement payload starts with this header, encoded in big-endian
// order so the same layout is valid between hosts of different endianness:
//   [0..3]  magic
//   [4..7]  sequence number
//   [8..15] sender timestamp in nanoseconds (steady clock of the sender)
// The rest of the payload is filler that only sets the message size.
constexpr std::uint32_t kPayloadMagic = 0x50455246; // "PERF"
constexpr std::size_t kPayloadHeaderSize = 16;

struct PayloadHeader {
    std::uint32_t sequence{0};
    std::uint64_t timestamp_ns{0};
};

inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Wall-clock time for one-way latency between hosts. It is only meaningful
// when both hosts synchronize CLOCK_REALTIME, for example with PTP (phc2sys).
inline std::uint64_t realtime_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

enum class Clock { Monotonic, Realtime };

inline std::uint64_t clock_ns(Clock clock) {
    return clock == Clock::Realtime ? realtime_ns() : now_ns();
}

inline bool parse_clock(const std::string& name, Clock& clock) {
    if (name == "monotonic") {
        clock = Clock::Monotonic;
    } else if (name == "realtime") {
        clock = Clock::Realtime;
    } else {
        return false;
    }
    return true;
}

inline void put_be(Payload& payload, std::size_t offset, std::uint64_t value, std::size_t bytes) {
    for (std::size_t i = 0; i < bytes; ++i) {
        payload[offset + i] = static_cast<byte_t>(value >> (8 * (bytes - 1 - i)));
    }
}

inline std::uint64_t get_be(const Payload& payload, std::size_t offset, std::size_t bytes) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes; ++i) {
        value = (value << 8) | payload[offset + i];
    }
    return value;
}

// Build a payload of the requested size; sizes below the header are raised to it.
inline Payload make_payload(std::size_t size) {
    Payload payload(size < kPayloadHeaderSize ? kPayloadHeaderSize : size);
    for (std::size_t i = kPayloadHeaderSize; i < payload.size(); ++i) {
        payload[i] = static_cast<byte_t>(i);
    }
    put_be(payload, 0, kPayloadMagic, 4);
    return payload;
}

// Rewrite only the header so a preallocated payload can be reused per message.
inline void stamp_payload(Payload& payload, std::uint32_t sequence, std::uint64_t timestamp_ns) {
    put_be(payload, 4, sequence, 4);
    put_be(payload, 8, timestamp_ns, 8);
}

inline bool parse_payload(const Payload& payload, PayloadHeader& header) {
    if (payload.size() < kPayloadHeaderSize || get_be(payload, 0, 4) != kPayloadMagic) {
        return false;
    }
    header.sequence = static_cast<std::uint32_t>(get_be(payload, 4, 4));
    header.timestamp_ns = get_be(payload, 8, 8);
    return true;
}

// The end marker tells subscribers how the run was configured, so each one can
// compute loss without being told the count in advance:
//   header.sequence      total notifications sent (warm-up included)
//   [16..23]             warm-up notifications (sequences 1..warmup)
//   [24..31]             offered rate in milli-messages per second (0 = maximum)
struct EndMarker {
    std::uint32_t total{0};
    std::uint64_t warmup{0};
    double rate{0};
};

constexpr std::size_t kEndMarkerSize = 32;

inline Payload make_end_marker(const EndMarker& marker) {
    auto payload = make_payload(kEndMarkerSize);
    stamp_payload(payload, marker.total, now_ns());
    put_be(payload, 16, marker.warmup, 8);
    put_be(payload, 24, static_cast<std::uint64_t>(marker.rate * 1000.0), 8);
    return payload;
}

inline bool parse_end_marker(const Payload& payload, EndMarker& marker) {
    PayloadHeader header;
    if (payload.size() < kEndMarkerSize || !parse_payload(payload, header)) {
        return false;
    }
    marker.total = header.sequence;
    marker.warmup = get_be(payload, 16, 8);
    marker.rate = static_cast<double>(get_be(payload, 24, 8)) / 1000.0;
    return true;
}

} // namespace perf

#endif // LG_SOMEIP_PERF_PAYLOAD_H
