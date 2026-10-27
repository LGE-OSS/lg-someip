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

#include "InteropVectors.h"

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include "InteropIds.h"
#include "serialization/someip/SomeIPParameterSerialization.h"

namespace ser = lgsomeip::serialization;

namespace lgsomeip {
namespace interop {
namespace {

using Len8 = ser::LengthFieldConfig<std::uint8_t>;
using Len16 = ser::LengthFieldConfig<std::uint16_t>;
using Len32 = ser::LengthFieldConfig<std::uint32_t>;
using NoLength = ser::LengthFieldConfig<void>;

float float_from_bits(std::uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

double double_from_bits(std::uint64_t bits) {
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Floating point values are compared bit for bit so that NaN and -0.0 are checked too.
bool same(float a, float b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

bool same(double a, double b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

template <typename T> bool same(T a, T b) {
    return a == b;
}

Bytes take(ser::Serializer& writer) {
    return *writer.finish();
}

// Thin wrapper around the library Deserializer that also checks for leftover bytes.
class Reader {
public:
    explicit Reader(const Bytes& bytes)
        : deserializer_(std::unique_ptr<ser::PacketBuffer>(new ser::PacketBuffer(bytes))) {}

    template <typename T> bool get(T& value) {
        return deserializer_.pop_front(value) == sizeof(T);
    }

    template <typename T> bool expect(T expected) {
        T actual{};
        return get(actual) && same(actual, expected);
    }

    bool finished() {
        return deserializer_.get_remaining_length() == 0;
    }

    ser::Deserializer& raw() {
        return deserializer_;
    }

private:
    ser::Deserializer deserializer_;
};

// ---------------------------------------------------------------------------
// Scalars: every fixed-size base type in one struct. The 64-bit members start at
// an odd offset on purpose, to exercise unaligned access on the target CPU.
// ---------------------------------------------------------------------------
struct Scalars {
    bool b;
    std::uint8_t u8;
    std::int8_t i8;
    std::uint16_t u16;
    std::int16_t i16;
    std::uint32_t u32;
    std::int32_t i32;
    std::uint64_t u64;
    std::int64_t i64;
    float f;
    double d;
};

Bytes encode_scalars(const Scalars& s) {
    ser::Serializer w;
    w.push_back(s.b);
    w.push_back(s.u8);
    w.push_back(s.i8);
    w.push_back(s.u16);
    w.push_back(s.i16);
    w.push_back(s.u32);
    w.push_back(s.i32);
    w.push_back(s.u64);
    w.push_back(s.i64);
    w.push_back(s.f);
    w.push_back(s.d);
    return take(w);
}

bool decode_scalars(const Bytes& in, const Scalars& e) {
    Reader r(in);
    return r.expect(e.b) && r.expect(e.u8) && r.expect(e.i8) && r.expect(e.u16) && r.expect(e.i16) && r.expect(e.u32) &&
           r.expect(e.i32) && r.expect(e.u64) && r.expect(e.i64) && r.expect(e.f) && r.expect(e.d) && r.finished();
}

const Scalars kScalarsMin{true,
                          0xA5,
                          static_cast<std::int8_t>(-128),
                          0x1234,
                          static_cast<std::int16_t>(-2),
                          0x89ABCDEFU,
                          static_cast<std::int32_t>(0x80000000U),
                          0x0123456789ABCDEFULL,
                          static_cast<std::int64_t>(-2),
                          1.5F,
                          -2.5};

const Scalars kScalarsMax{false,
                          0xFF,
                          127,
                          0xFFFF,
                          32767,
                          0xFFFFFFFFU,
                          2147483647,
                          0xFFFFFFFFFFFFFFFFULL,
                          static_cast<std::int64_t>(0x7FFFFFFFFFFFFFFFULL),
                          FLT_MAX,
                          DBL_MAX};

// ---------------------------------------------------------------------------
// Floating point special values.
// ---------------------------------------------------------------------------
constexpr std::uint32_t kFloatBits[4] = {0x7F800000U, 0xFF800000U, 0x80000000U, 0x7FC00000U};
constexpr std::uint64_t kDoubleBits[3] = {0x7FF0000000000000ULL, 0x8000000000000000ULL, 0x7FF8000000000000ULL};

Bytes encode_float_special() {
    ser::Serializer w;
    for (const auto bits : kFloatBits) {
        w.push_back(float_from_bits(bits));
    }
    for (const auto bits : kDoubleBits) {
        w.push_back(double_from_bits(bits));
    }
    return take(w);
}

bool decode_float_special(const Bytes& in) {
    Reader r(in);
    for (const auto bits : kFloatBits) {
        if (!r.expect(float_from_bits(bits))) {
            return false;
        }
    }
    for (const auto bits : kDoubleBits) {
        if (!r.expect(double_from_bits(bits))) {
            return false;
        }
    }
    return r.finished();
}

// ---------------------------------------------------------------------------
// Structs with a length field in front (8, 16 and 32 bit).
// ---------------------------------------------------------------------------
Bytes encode_struct_len32() {
    ser::Serializer w;
    ser::ComplexTypeSerializer<Len32> s(&w);
    s.push_back(static_cast<std::uint16_t>(0x0102));
    s.push_back(static_cast<std::uint8_t>(0x03));
    s.push_back(static_cast<std::uint32_t>(0x04050607));
    s.finish();
    return take(w);
}

bool decode_struct_len32(const Bytes& in) {
    Reader r(in);
    ser::ComplexTypeDeserializer<Len32> s(&r.raw());
    std::uint16_t a{};
    std::uint8_t b{};
    std::uint32_t c{};
    return s.pop_front(a) == 2 && a == 0x0102 && s.pop_front(b) == 1 && b == 0x03 && s.pop_front(c) == 4 &&
           c == 0x04050607 && !s.has_remaining_data() && r.finished();
}

Bytes encode_struct_len8() {
    ser::Serializer w;
    ser::ComplexTypeSerializer<Len8> s(&w);
    s.push_back(static_cast<std::uint8_t>(0xAA));
    s.push_back(static_cast<std::uint8_t>(0xBB));
    s.finish();
    return take(w);
}

bool decode_struct_len8(const Bytes& in) {
    Reader r(in);
    ser::ComplexTypeDeserializer<Len8> s(&r.raw());
    std::uint8_t a{};
    std::uint8_t b{};
    return s.pop_front(a) == 1 && a == 0xAA && s.pop_front(b) == 1 && b == 0xBB && !s.has_remaining_data() &&
           r.finished();
}

Bytes encode_struct_len16() {
    ser::Serializer w;
    ser::ComplexTypeSerializer<Len16> s(&w);
    s.push_back(static_cast<std::uint32_t>(0xDEADBEEFU));
    s.finish();
    return take(w);
}

bool decode_struct_len16(const Bytes& in) {
    Reader r(in);
    ser::ComplexTypeDeserializer<Len16> s(&r.raw());
    std::uint32_t a{};
    return s.pop_front(a) == 4 && a == 0xDEADBEEFU && !s.has_remaining_data() && r.finished();
}

// A struct with a 32-bit length field that contains a struct with a 16-bit one.
Bytes encode_nested() {
    ser::Serializer w;
    ser::ComplexTypeSerializer<Len32> outer(&w);
    outer.push_back(static_cast<std::uint8_t>(0x7E));
    ser::ComplexTypeSerializer<Len16> inner(&w);
    inner.push_back(static_cast<std::uint16_t>(0x0A0B));
    inner.finish();
    outer.push_back(static_cast<std::uint8_t>(0x7F));
    outer.finish();
    return take(w);
}

bool decode_nested(const Bytes& in) {
    Reader r(in);
    ser::ComplexTypeDeserializer<Len32> outer(&r.raw());
    std::uint8_t head{};
    std::uint8_t tail{};
    std::uint16_t value{};
    if (outer.pop_front(head) != 1 || head != 0x7E) {
        return false;
    }
    ser::ComplexTypeDeserializer<Len16> inner(&r.raw());
    if (inner.pop_front(value) != 2 || value != 0x0A0B || inner.has_remaining_data()) {
        return false;
    }
    // The inner struct reads from the shared deserializer; tell the outer one.
    outer.consumed(2 + 2);
    return outer.pop_front(tail) == 1 && tail == 0x7F && !outer.has_remaining_data() && r.finished();
}

// ---------------------------------------------------------------------------
// Deeply nested structs. Every level has members in front of and behind its child
// struct, and the width of the length field changes from level to level.
//   L1 (32 bit) { u8 0x11, L2, u8 0x1F }
//   L2 (16 bit) { u16 0x2122, L3, u16 0x2F2F }
//   L3 ( 8 bit) { u8 0x31, L4, u8 0x3F }
//   L4 (32 bit) { u32 0x41424344, L5, u8 0x4F }
//   L5 (16 bit) { u8 0x51, L6, u8 0x5F }
//   L6 ( 8 bit) { u64 0x6162636465666768 }
// ---------------------------------------------------------------------------
Bytes encode_nested_mixed() {
    ser::Serializer w;
    ser::ComplexTypeSerializer<Len32> l1(&w);
    l1.push_back(static_cast<std::uint8_t>(0x11));
    ser::ComplexTypeSerializer<Len16> l2(&w);
    l2.push_back(static_cast<std::uint16_t>(0x2122));
    ser::ComplexTypeSerializer<Len8> l3(&w);
    l3.push_back(static_cast<std::uint8_t>(0x31));
    ser::ComplexTypeSerializer<Len32> l4(&w);
    l4.push_back(static_cast<std::uint32_t>(0x41424344U));
    ser::ComplexTypeSerializer<Len16> l5(&w);
    l5.push_back(static_cast<std::uint8_t>(0x51));
    ser::ComplexTypeSerializer<Len8> l6(&w);
    l6.push_back(static_cast<std::uint64_t>(0x6162636465666768ULL));
    l6.finish();
    l5.push_back(static_cast<std::uint8_t>(0x5F));
    l5.finish();
    l4.push_back(static_cast<std::uint8_t>(0x4F));
    l4.finish();
    l3.push_back(static_cast<std::uint8_t>(0x3F));
    l3.finish();
    l2.push_back(static_cast<std::uint16_t>(0x2F2F));
    l2.finish();
    l1.push_back(static_cast<std::uint8_t>(0x1F));
    l1.finish();
    return take(w);
}

// The inner structs read from the shared deserializer, so every parent is told how many bytes its child
// occupied (length field included): L6 9, L5 13, L4 22, L3 25, L2 31.
bool decode_nested_mixed(const Bytes& in) {
    Reader r(in);
    std::uint8_t u8{};
    std::uint16_t u16{};
    std::uint32_t u32{};
    std::uint64_t u64{};

    ser::ComplexTypeDeserializer<Len32> l1(&r.raw());
    if (l1.pop_front(u8) != 1 || u8 != 0x11) {
        return false;
    }
    ser::ComplexTypeDeserializer<Len16> l2(&r.raw());
    if (l2.pop_front(u16) != 2 || u16 != 0x2122) {
        return false;
    }
    ser::ComplexTypeDeserializer<Len8> l3(&r.raw());
    if (l3.pop_front(u8) != 1 || u8 != 0x31) {
        return false;
    }
    ser::ComplexTypeDeserializer<Len32> l4(&r.raw());
    if (l4.pop_front(u32) != 4 || u32 != 0x41424344U) {
        return false;
    }
    ser::ComplexTypeDeserializer<Len16> l5(&r.raw());
    if (l5.pop_front(u8) != 1 || u8 != 0x51) {
        return false;
    }
    ser::ComplexTypeDeserializer<Len8> l6(&r.raw());
    if (l6.pop_front(u64) != 8 || u64 != 0x6162636465666768ULL || l6.has_remaining_data()) {
        return false;
    }

    l5.consumed(9);
    if (l5.pop_front(u8) != 1 || u8 != 0x5F || l5.has_remaining_data()) {
        return false;
    }
    l4.consumed(13);
    if (l4.pop_front(u8) != 1 || u8 != 0x4F || l4.has_remaining_data()) {
        return false;
    }
    l3.consumed(22);
    if (l3.pop_front(u8) != 1 || u8 != 0x3F || l3.has_remaining_data()) {
        return false;
    }
    l2.consumed(25);
    if (l2.pop_front(u16) != 2 || u16 != 0x2F2F || l2.has_remaining_data()) {
        return false;
    }
    l1.consumed(31);
    if (l1.pop_front(u8) != 1 || u8 != 0x1F || l1.has_remaining_data()) {
        return false;
    }
    return r.finished();
}

// A chain of structs, each with a 16-bit length field, one member in front of the child and one behind it.
// The innermost struct holds a 32-bit value instead of a child. The golden bytes are built with plain
// byte arithmetic, not with the serializer.
constexpr int kDeepLevels = 32;

std::uint8_t deep_lead(int level) {
    return static_cast<std::uint8_t>(level * 7 + 1);
}

std::uint8_t deep_trail(int level) {
    return static_cast<std::uint8_t>(level * 13 + 5);
}

Bytes golden_deep() {
    Bytes inner;
    for (int level = kDeepLevels - 1; level >= 0; level--) {
        Bytes body{deep_lead(level)};
        if (level == kDeepLevels - 1) {
            body.insert(body.end(), {0xCA, 0xFE, 0xBA, 0xBE});
        } else {
            body.insert(body.end(), inner.begin(), inner.end());
        }
        body.push_back(deep_trail(level));

        Bytes next{static_cast<std::uint8_t>(body.size() >> 8), static_cast<std::uint8_t>(body.size() & 0xFF)};
        next.insert(next.end(), body.begin(), body.end());
        inner = std::move(next);
    }
    return inner;
}

void encode_deep_level(ser::Serializer& w, int level) {
    ser::ComplexTypeSerializer<Len16> s(&w);
    s.push_back(deep_lead(level));
    if (level + 1 < kDeepLevels) {
        encode_deep_level(w, level + 1);
    } else {
        s.push_back(static_cast<std::uint32_t>(0xCAFEBABEU));
    }
    s.push_back(deep_trail(level));
    s.finish();
}

Bytes encode_deep() {
    ser::Serializer w;
    encode_deep_level(w, 0);
    return take(w);
}

// Returns the number of bytes the level occupies (length field included), or 0 on a mismatch.
std::size_t decode_deep_level(Reader& r, int level) {
    ser::ComplexTypeDeserializer<Len16> s(&r.raw());
    std::uint8_t lead{};
    std::uint8_t trail{};
    if (s.pop_front(lead) != 1 || lead != deep_lead(level)) {
        return 0;
    }

    std::size_t child = 4;
    if (level + 1 < kDeepLevels) {
        child = decode_deep_level(r, level + 1);
        if (child == 0) {
            return 0;
        }
        s.consumed(child);
    } else {
        std::uint32_t value{};
        if (s.pop_front(value) != 4 || value != 0xCAFEBABEU) {
            return 0;
        }
    }

    if (s.pop_front(trail) != 1 || trail != deep_trail(level) || s.has_remaining_data()) {
        return 0;
    }
    return 2 + 1 + child + 1;
}

bool decode_deep(const Bytes& in) {
    Reader r(in);
    return decode_deep_level(r, 0) != 0 && r.finished();
}

// ---------------------------------------------------------------------------
// An array (32-bit length) of structs. Each element holds a struct (16-bit length) that in turn holds
// an array (8-bit length), with members in front of and behind the nested ones.
//   Sensor { u8 id, Pos { i16 x, i16 y, Tags (8 bit) { u8... } }, u32 trailer }
// ---------------------------------------------------------------------------
struct Sensor {
    std::uint8_t id;
    std::int16_t x;
    std::int16_t y;
    Bytes tags;
    std::uint32_t trailer;
};

const std::vector<Sensor> kSensors{{1, -2, 3, {0xA1, 0xA2, 0xA3}, 0x01020304U}, {2, 0x7FFF, -32768, {}, 0x05060708U}};

Bytes encode_sensors() {
    ser::Serializer w;
    ser::ComplexTypeSerializer<Len32> list(&w);
    for (const auto& sensor : kSensors) {
        list.push_back(sensor.id);
        ser::ComplexTypeSerializer<Len16> pos(&w);
        pos.push_back(sensor.x);
        pos.push_back(sensor.y);
        ser::ComplexTypeSerializer<Len8> tags(&w);
        for (const auto tag : sensor.tags) {
            tags.push_back(tag);
        }
        tags.finish();
        pos.finish();
        list.push_back(sensor.trailer);
    }
    list.finish();
    return take(w);
}

bool decode_sensors(const Bytes& in) {
    Reader r(in);
    ser::ComplexTypeDeserializer<Len32> list(&r.raw());
    std::size_t index = 0;
    while (list.has_remaining_data()) {
        if (index >= kSensors.size()) {
            return false;
        }
        const Sensor& expected = kSensors[index];

        std::uint8_t id{};
        if (list.pop_front(id) != 1 || id != expected.id) {
            return false;
        }

        ser::ComplexTypeDeserializer<Len16> pos(&r.raw());
        std::int16_t x{};
        std::int16_t y{};
        if (pos.pop_front(x) != 2 || x != expected.x || pos.pop_front(y) != 2 || y != expected.y) {
            return false;
        }

        ser::ComplexTypeDeserializer<Len8> tags(&r.raw());
        Bytes actual_tags;
        while (tags.has_remaining_data()) {
            std::uint8_t tag{};
            if (tags.pop_front(tag) != 1) {
                return false;
            }
            actual_tags.push_back(tag);
        }
        if (actual_tags != expected.tags) {
            return false;
        }

        // The tags occupy their length field plus the tags; Pos occupies its length field, x, y and the tags.
        const std::size_t tags_size = 1 + expected.tags.size();
        pos.consumed(tags_size);
        if (pos.has_remaining_data()) {
            return false;
        }
        list.consumed(2 + 2 + 2 + tags_size);

        std::uint32_t trailer{};
        if (list.pop_front(trailer) != 4 || trailer != expected.trailer) {
            return false;
        }
        index++;
    }
    return index == kSensors.size() && r.finished();
}

// ---------------------------------------------------------------------------
// Arrays.
// ---------------------------------------------------------------------------
Bytes encode_array_u16(const std::vector<std::uint16_t>& values) {
    ser::Serializer w;
    ser::ComplexTypeSerializer<Len32> a(&w);
    for (const auto value : values) {
        a.push_back(value);
    }
    a.finish();
    return take(w);
}

bool decode_array_u16(const Bytes& in, const std::vector<std::uint16_t>& expected) {
    Reader r(in);
    ser::ComplexTypeDeserializer<Len32> a(&r.raw());
    std::vector<std::uint16_t> actual;
    while (a.has_remaining_data()) {
        std::uint16_t value{};
        if (a.pop_front(value) != 2) {
            return false;
        }
        actual.push_back(value);
    }
    return actual == expected && r.finished();
}

struct Item {
    std::uint8_t id;
    std::uint16_t value;
};

const std::vector<Item> kItems{{1, 0x1111}, {2, 0x2222}};

Bytes encode_array_of_struct() {
    ser::Serializer w;
    ser::ComplexTypeSerializer<Len32> a(&w);
    for (const auto& item : kItems) {
        a.push_back(item.id);
        a.push_back(item.value);
    }
    a.finish();
    return take(w);
}

bool decode_array_of_struct(const Bytes& in) {
    Reader r(in);
    ser::ComplexTypeDeserializer<Len32> a(&r.raw());
    std::size_t index = 0;
    while (a.has_remaining_data()) {
        Item item{};
        if (index >= kItems.size() || a.pop_front(item.id) != 1 || a.pop_front(item.value) != 2 ||
            item.id != kItems[index].id || item.value != kItems[index].value) {
            return false;
        }
        index++;
    }
    return index == kItems.size() && r.finished();
}

// ---------------------------------------------------------------------------
// Strings (32-bit length field, BOM and terminating zero included in the length).
// ---------------------------------------------------------------------------
Bytes encode_string8(const Bytes& text) {
    ser::Serializer w;
    ser::ComplexTypeSerializer<Len32> s(&w);
    for (const auto byte : text) {
        s.push_back(byte);
    }
    s.finish();
    return take(w);
}

bool decode_string8(const Bytes& in, const Bytes& expected) {
    Reader r(in);
    ser::ComplexTypeDeserializer<Len32> s(&r.raw());
    Bytes actual;
    while (s.has_remaining_data()) {
        std::uint8_t byte{};
        if (s.pop_front(byte) != 1) {
            return false;
        }
        actual.push_back(byte);
    }
    return actual == expected && r.finished();
}

const Bytes kUtf8Ascii{0xEF, 0xBB, 0xBF, 'H', 'i', 0x00};
// "한글" in UTF-8, written as bytes so the source file encoding does not matter.
const Bytes kUtf8Korean{0xEF, 0xBB, 0xBF, 0xED, 0x95, 0x9C, 0xEA, 0xB8, 0x80, 0x00};
const std::vector<std::uint16_t> kUtf16Hi{0xFEFF, 0x0048, 0x0069, 0x0000};

// ---------------------------------------------------------------------------
// Payload sizes: a repeatable pattern of n bytes.
// ---------------------------------------------------------------------------
Bytes pattern(std::size_t size) {
    Bytes bytes(size);
    for (std::size_t i = 0; i < size; i++) {
        bytes[i] = static_cast<std::uint8_t>((i * 7 + 3) & 0xFF);
    }
    return bytes;
}

Bytes encode_pattern(std::size_t size) {
    ser::Serializer w;
    for (const auto byte : pattern(size)) {
        w.push_back(byte);
    }
    return take(w);
}

bool decode_pattern(const Bytes& in, std::size_t size) {
    Reader r(in);
    const Bytes expected = pattern(size);
    for (std::size_t i = 0; i < size; i++) {
        if (!r.expect(expected[i])) {
            return false;
        }
    }
    return r.finished();
}

// ---------------------------------------------------------------------------
// TLV: 32-bit base member, 16-bit-length member and 8-bit member. Data IDs above
// 0xFF make sure the high nibble of the tag is checked too.
// ---------------------------------------------------------------------------
Bytes encode_tlv() {
    ser::Serializer w;
    {
        ser::TlvSerializer<Len16> t(&w, ser::WireTypeEnum::BaseType32Bit, 0x0001);
        t.push_back(static_cast<std::uint32_t>(0x11223344));
        t.finish();
    }
    {
        ser::TlvSerializer<Len16> t(&w, ser::WireTypeEnum::ComplexDataType16BitLengthField, 0x0102);
        t.push_back(static_cast<std::uint8_t>(1));
        t.push_back(static_cast<std::uint8_t>(2));
        t.push_back(static_cast<std::uint8_t>(3));
        t.finish();
    }
    {
        ser::TlvSerializer<Len16> t(&w, ser::WireTypeEnum::BaseType8Bit, 0x0003);
        t.push_back(static_cast<std::uint8_t>(0xEE));
        t.finish();
    }
    return take(w);
}

bool decode_tlv(const Bytes& in) {
    Reader r(in);
    ser::TlvDeserializer<Len16> t(&r.raw());

    ser::TlvTag tag = t.consume_tag();
    std::uint32_t word{};
    if (tag.data_id_ != 0x0001 || tag.len_ != 4 || t.pop_front(word) != 4 || word != 0x11223344) {
        return false;
    }

    tag = t.consume_tag();
    if (tag.data_id_ != 0x0102 || tag.len_ != 3) {
        return false;
    }
    for (std::uint8_t expected = 1; expected <= 3; expected++) {
        std::uint8_t byte{};
        if (t.pop_front(byte) != 1 || byte != expected) {
            return false;
        }
    }

    tag = t.consume_tag();
    std::uint8_t last{};
    if (tag.data_id_ != 0x0003 || tag.len_ != 1 || t.pop_front(last) != 1 || last != 0xEE) {
        return false;
    }
    return r.finished();
}

// ---------------------------------------------------------------------------
// TLV: every remaining wire type. The 16 bit and 64 bit base types, the length field taken from the data
// definition and the 8 bit and 32 bit length fields. The last member uses data ID 0x0FFF so that all
// twelve bits of the ID are checked.
// ---------------------------------------------------------------------------
Bytes encode_tlv_wire_types() {
    ser::Serializer w;
    {
        ser::TlvSerializer<Len16> t(&w, ser::WireTypeEnum::BaseType16Bit, 0x0010);
        t.push_back(static_cast<std::uint16_t>(0x1234));
        t.finish();
    }
    {
        ser::TlvSerializer<Len16> t(&w, ser::WireTypeEnum::BaseType64Bit, 0x0011);
        t.push_back(static_cast<std::uint64_t>(0x0102030405060708ULL));
        t.finish();
    }
    {
        ser::TlvSerializer<Len16> t(&w, ser::WireTypeEnum::ComplexDataTypeFromDataDefinition, 0x0112);
        t.push_back(static_cast<std::uint8_t>(0xAA));
        t.push_back(static_cast<std::uint8_t>(0xBB));
        t.finish();
    }
    {
        ser::TlvSerializer<Len16> t(&w, ser::WireTypeEnum::ComplexDataType8BitLengthField, 0x0013);
        t.push_back(static_cast<std::uint8_t>(0xC1));
        t.push_back(static_cast<std::uint8_t>(0xC2));
        t.push_back(static_cast<std::uint8_t>(0xC3));
        t.finish();
    }
    {
        ser::TlvSerializer<Len16> t(&w, ser::WireTypeEnum::ComplexDataType32BitLengthField, 0x0FFF);
        t.push_back(static_cast<std::uint8_t>(0xD1));
        t.push_back(static_cast<std::uint8_t>(0xD2));
        t.finish();
    }
    return take(w);
}

// Reads the bytes of the current TLV member and compares them with the expected ones.
bool tlv_bytes_are(ser::TlvDeserializer<Len16>& t, const Bytes& expected) {
    for (const auto want : expected) {
        std::uint8_t byte{};
        if (t.pop_front(byte) != 1 || byte != want) {
            return false;
        }
    }
    return !t.has_remaining_data();
}

bool decode_tlv_wire_types(const Bytes& in) {
    Reader r(in);
    ser::TlvDeserializer<Len16> t(&r.raw());

    ser::TlvTag tag = t.consume_tag();
    std::uint16_t half{};
    if (tag.data_id_ != 0x0010 || tag.len_ != 2 || t.pop_front(half) != 2 || half != 0x1234) {
        return false;
    }

    tag = t.consume_tag();
    std::uint64_t wide{};
    if (tag.data_id_ != 0x0011 || tag.len_ != 8 || t.pop_front(wide) != 8 || wide != 0x0102030405060708ULL) {
        return false;
    }

    tag = t.consume_tag();
    if (tag.data_id_ != 0x0112 || tag.len_ != 2 || !tlv_bytes_are(t, {0xAA, 0xBB})) {
        return false;
    }

    tag = t.consume_tag();
    if (tag.data_id_ != 0x0013 || tag.len_ != 3 || !tlv_bytes_are(t, {0xC1, 0xC2, 0xC3})) {
        return false;
    }

    tag = t.consume_tag();
    if (tag.data_id_ != 0x0FFF || tag.len_ != 2 || !tlv_bytes_are(t, {0xD1, 0xD2})) {
        return false;
    }
    return r.finished();
}

// ---------------------------------------------------------------------------
// A struct without a length field inside a struct with a 16-bit one.
//   Outer (16 bit) { Inner (no length field) { u8 0x01, u16 0x0203 }, u8 0x04 }
// ---------------------------------------------------------------------------
Bytes encode_struct_no_length_nested() {
    ser::Serializer w;
    ser::ComplexTypeSerializer<Len16> outer(&w);
    ser::ComplexTypeSerializer<NoLength> inner(&w);
    inner.push_back(static_cast<std::uint8_t>(0x01));
    inner.push_back(static_cast<std::uint16_t>(0x0203));
    inner.finish();
    outer.push_back(static_cast<std::uint8_t>(0x04));
    outer.finish();
    return take(w);
}

bool decode_struct_no_length_nested(const Bytes& in) {
    Reader r(in);
    ser::ComplexTypeDeserializer<Len16> outer(&r.raw());
    ser::ComplexTypeDeserializer<NoLength> inner(&r.raw());
    std::uint8_t a{};
    std::uint16_t b{};
    std::uint8_t c{};
    if (inner.pop_front(a) != 1 || a != 0x01 || inner.pop_front(b) != 2 || b != 0x0203) {
        return false;
    }
    // The inner struct has no length field, so only its members count.
    outer.consumed(1 + 2);
    return outer.pop_front(c) == 1 && c == 0x04 && !outer.has_remaining_data() && r.finished();
}

// ---------------------------------------------------------------------------
// Strings: dynamic length UTF-16 little endian, fixed length UTF-8.
// ---------------------------------------------------------------------------
Bytes encode_string_utf16le() {
    ser::Serializer w;
    return ser::push_back_string(w, std::u16string(u"Hi"), ser::StringEncoding::Utf16LE) ? take(w) : Bytes{};
}

bool decode_string_utf16le(const Bytes& in) {
    Reader r(in);
    std::u16string text;
    return ser::pop_front_string(r.raw(), text, ser::StringEncoding::Utf16LE) == in.size() && text == u"Hi" &&
           r.finished();
}

Bytes encode_string_fixed_utf8() {
    ser::Serializer w;
    return ser::push_back_fixed_string(w, 8, std::string("Hi")) ? take(w) : Bytes{};
}

bool decode_string_fixed_utf8(const Bytes& in) {
    Reader r(in);
    std::string text;
    return ser::pop_front_fixed_string(r.raw(), 8, text) == 8 && text == "Hi" && r.finished();
}

// ---------------------------------------------------------------------------
// Union (feat_req_someip_274 example): uint8 (type 1) and uint16 (type 2) padded to 4 bytes.
// ---------------------------------------------------------------------------
Bytes encode_union_padded() {
    ser::Serializer w;
    ser::UnionSerializer<> first(&w, 1);
    first.push_back(static_cast<std::uint8_t>(0xAB));
    bool ok = first.finish(4);
    ser::UnionSerializer<> second(&w, 2);
    second.push_back(static_cast<std::uint16_t>(0xABCD));
    ok = second.finish(4) && ok;
    return ok ? take(w) : Bytes{};
}

bool decode_union_padded(const Bytes& in) {
    Reader r(in);
    ser::UnionDeserializer<> first(&r.raw());
    std::uint8_t a{};
    if (!first.valid() || first.type() != 1 || first.pop_front(a) != 1 || a != 0xAB || !first.skip_remaining()) {
        return false;
    }
    ser::UnionDeserializer<> second(&r.raw());
    std::uint16_t b{};
    return second.valid() && second.type() == 2 && second.pop_front(b) == 2 && b == 0xABCD && second.skip_remaining() &&
           r.finished();
}

// ---------------------------------------------------------------------------
// A struct that is longer than the receiver knows (feat_req_someip_601): the extra bytes are skipped.
//   Struct (16 bit) { u8 0x01, extra 0x02 0x03 0x04 0x05 }, u8 0x09
// ---------------------------------------------------------------------------
Bytes encode_struct_extended() {
    ser::Serializer w;
    ser::ComplexTypeSerializer<Len16> s(&w);
    for (std::uint8_t byte = 1; byte <= 5; byte++) {
        s.push_back(byte);
    }
    const bool ok = s.finish();
    w.push_back(static_cast<std::uint8_t>(0x09));
    return ok ? take(w) : Bytes{};
}

bool decode_struct_extended(const Bytes& in) {
    Reader r(in);
    ser::ComplexTypeDeserializer<Len16> s(&r.raw());
    std::uint8_t known{};
    std::uint8_t after{};
    return s.pop_front(known) == 1 && known == 0x01 && s.skip_remaining() && !s.has_remaining_data() && r.get(after) &&
           after == 0x09 && r.finished();
}

// ---------------------------------------------------------------------------
// Table.
// ---------------------------------------------------------------------------
void add(std::vector<Vector>& table, const char* name, Bytes golden, std::function<Bytes()> encode,
         std::function<bool(const Bytes&)> decode, bool large = false) {
    Vector vector;
    vector.name = name;
    vector.method = static_cast<api::method_t>(kVectorMethodBase + table.size());
    vector.golden = std::move(golden);
    vector.encode = std::move(encode);
    vector.decode = std::move(decode);
    vector.large = large;
    table.push_back(std::move(vector));
}

std::vector<Vector> build_vectors() {
    std::vector<Vector> table;

    add(
        table, "scalars_min", {0x01, 0xA5, 0x80, 0x12, 0x34, 0xFF, 0xFE, 0x89, 0xAB, 0xCD, 0xEF, 0x80, 0x00, 0x00, 0x00,
                               0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                               0xFE, 0x3F, 0xC0, 0x00, 0x00, 0xC0, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        [] { return encode_scalars(kScalarsMin); }, [](const Bytes& b) { return decode_scalars(b, kScalarsMin); });

    add(
        table, "scalars_max", {0x00, 0xFF, 0x7F, 0xFF, 0xFF, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0xFF, 0xFF, 0xFF,
                               0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                               0xFF, 0x7F, 0x7F, 0xFF, 0xFF, 0x7F, 0xEF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
        [] { return encode_scalars(kScalarsMax); }, [](const Bytes& b) { return decode_scalars(b, kScalarsMax); });

    add(table, "float_special", {0x7F, 0x80, 0x00, 0x00, 0xFF, 0x80, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x7F, 0xC0,
                                 0x00, 0x00, 0x7F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
                                 0x00, 0x00, 0x00, 0x00, 0x7F, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        encode_float_special, decode_float_special);

    add(table, "struct_len32", {0x00, 0x00, 0x00, 0x07, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07}, encode_struct_len32,
        decode_struct_len32);
    add(table, "struct_len16", {0x00, 0x04, 0xDE, 0xAD, 0xBE, 0xEF}, encode_struct_len16, decode_struct_len16);
    add(table, "struct_len8", {0x02, 0xAA, 0xBB}, encode_struct_len8, decode_struct_len8);
    add(table, "struct_nested", {0x00, 0x00, 0x00, 0x06, 0x7E, 0x00, 0x02, 0x0A, 0x0B, 0x7F}, encode_nested,
        decode_nested);
    add(table, "struct_nested_mixed", {0x00, 0x00, 0x00, 0x21, 0x11, 0x00, 0x1D, 0x21, 0x22, 0x18, 0x31, 0x00, 0x00,
                                       0x00, 0x12, 0x41, 0x42, 0x43, 0x44, 0x00, 0x0B, 0x51, 0x08, 0x61, 0x62, 0x63,
                                       0x64, 0x65, 0x66, 0x67, 0x68, 0x5F, 0x4F, 0x3F, 0x2F, 0x2F, 0x1F},
        encode_nested_mixed, decode_nested_mixed);
    add(table, "struct_nested_32_levels", golden_deep(), encode_deep, decode_deep);
    add(table, "array_nested_struct_array",
        {0x00, 0x00, 0x00, 0x1B, 0x01, 0x00, 0x08, 0xFF, 0xFE, 0x00, 0x03, 0x03, 0xA1, 0xA2, 0xA3, 0x01,
         0x02, 0x03, 0x04, 0x02, 0x00, 0x05, 0x7F, 0xFF, 0x80, 0x00, 0x00, 0x05, 0x06, 0x07, 0x08},
        encode_sensors, decode_sensors);

    add(
        table, "array_u16", {0x00, 0x00, 0x00, 0x06, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03},
        [] { return encode_array_u16({1, 2, 3}); }, [](const Bytes& b) { return decode_array_u16(b, {1, 2, 3}); });
    add(
        table, "array_empty", {0x00, 0x00, 0x00, 0x00}, [] { return encode_array_u16({}); },
        [](const Bytes& b) { return decode_array_u16(b, {}); });
    add(table, "array_of_struct", {0x00, 0x00, 0x00, 0x06, 0x01, 0x11, 0x11, 0x02, 0x22, 0x22}, encode_array_of_struct,
        decode_array_of_struct);

    add(
        table, "utf8_ascii", {0x00, 0x00, 0x00, 0x06, 0xEF, 0xBB, 0xBF, 0x48, 0x69, 0x00},
        [] { return encode_string8(kUtf8Ascii); }, [](const Bytes& b) { return decode_string8(b, kUtf8Ascii); });
    add(
        table, "utf8_korean", {0x00, 0x00, 0x00, 0x0A, 0xEF, 0xBB, 0xBF, 0xED, 0x95, 0x9C, 0xEA, 0xB8, 0x80, 0x00},
        [] { return encode_string8(kUtf8Korean); }, [](const Bytes& b) { return decode_string8(b, kUtf8Korean); });
    add(
        table, "utf16_be", {0x00, 0x00, 0x00, 0x08, 0xFE, 0xFF, 0x00, 0x48, 0x00, 0x69, 0x00, 0x00},
        [] { return encode_array_u16(kUtf16Hi); }, [](const Bytes& b) { return decode_array_u16(b, kUtf16Hi); });

    add(table, "tlv_members",
        {0x20, 0x01, 0x11, 0x22, 0x33, 0x44, 0x61, 0x02, 0x00, 0x03, 0x01, 0x02, 0x03, 0x00, 0x03, 0xEE}, encode_tlv,
        decode_tlv);

    for (const std::size_t size : {0U, 1U, 255U, 1000U}) {
        add(
            table, ("payload_" + std::to_string(size)).c_str(), pattern(size), [size] { return encode_pattern(size); },
            [size](const Bytes& b) { return decode_pattern(b, size); });
    }
    for (const std::size_t size : {4000U, 30000U}) {
        add(
            table, ("payload_" + std::to_string(size)).c_str(), pattern(size), [size] { return encode_pattern(size); },
            [size](const Bytes& b) { return decode_pattern(b, size); }, true);
    }

    // Appended last so that the method IDs of the vectors above stay the same.
    add(table, "tlv_wire_types",
        {0x10, 0x10, 0x12, 0x34, 0x30, 0x11, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x41, 0x12, 0x00,
         0x02, 0xAA, 0xBB, 0x50, 0x13, 0x03, 0xC1, 0xC2, 0xC3, 0x7F, 0xFF, 0x00, 0x00, 0x00, 0x02, 0xD1, 0xD2},
        encode_tlv_wire_types, decode_tlv_wire_types);
    add(table, "struct_no_length_nested", {0x00, 0x04, 0x01, 0x02, 0x03, 0x04}, encode_struct_no_length_nested,
        decode_struct_no_length_nested);
    add(table, "string_utf16_le", {0x00, 0x00, 0x00, 0x08, 0xFF, 0xFE, 0x48, 0x00, 0x69, 0x00, 0x00, 0x00},
        encode_string_utf16le, decode_string_utf16le);
    add(table, "string_fixed_utf8", {0xEF, 0xBB, 0xBF, 0x48, 0x69, 0x00, 0x00, 0x00}, encode_string_fixed_utf8,
        decode_string_fixed_utf8);
    add(table, "union_padded", {0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0xAB, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0xAB, 0xCD, 0x00, 0x00},
        encode_union_padded, decode_union_padded);
    add(table, "struct_extended", {0x00, 0x05, 0x01, 0x02, 0x03, 0x04, 0x05, 0x09}, encode_struct_extended,
        decode_struct_extended);

    return table;
}

} // namespace

const std::vector<Vector>& vectors() {
    static const std::vector<Vector> table = build_vectors();
    return table;
}

const Vector* find_vector_by_method(api::method_t method) {
    for (const auto& vector : vectors()) {
        if (vector.method == method) {
            return &vector;
        }
    }
    return nullptr;
}

const Vector* find_vector_by_name(const std::string& name) {
    for (const auto& vector : vectors()) {
        if (vector.name == name) {
            return &vector;
        }
    }
    return nullptr;
}

const std::vector<EventSpec>& events() {
    static const std::vector<EventSpec> table = {
        {0x8001, kGroupUnicast, false, "scalars_min"},    {0x8002, kGroupUnicast, false, "array_of_struct"},
        {0x8003, kGroupUnicast, true, "utf8_korean"},     {0x8011, kGroupMulticast, false, "scalars_max"},
        {0x8012, kGroupMulticast, false, "struct_len32"},
    };
    return table;
}

LocalResult run_local(const Vector& vector) {
    LocalResult result;
    const Bytes encoded = vector.encode();
    result.encode_matches_golden = encoded == vector.golden;
    result.decode_golden = vector.decode(vector.golden);
    result.roundtrip = vector.decode(encoded);
    return result;
}

std::string hex(const Bytes& bytes, std::size_t max_bytes) {
    std::string text;
    char buffer[4];
    for (std::size_t i = 0; i < bytes.size() && i < max_bytes; i++) {
        std::snprintf(buffer, sizeof(buffer), "%02X", bytes[i]);
        text += buffer;
        text += ' ';
    }
    if (bytes.size() > max_bytes) {
        text += "... (" + std::to_string(bytes.size()) + " bytes)";
    }
    return text;
}

} // namespace interop
} // namespace lgsomeip
