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

#include <cstdint>
#include <iostream>

#include "InteropVectors.h"

// Offline serializer check: no daemon and no network are needed. Every test
// vector is encoded, compared with its golden bytes, decoded from the golden
// bytes and round-tripped. Run it on each target (Linux, QNX) before the
// network tests.
//
//   interop-serialization
//
// Exit code: 0 when every vector passes, 1 on a failure, 2 on a usage error.
int main(int argc, char** argv) {
    if (argc > 1) {
        std::cerr << "usage: " << argv[0] << std::endl;
        return 2;
    }

    const std::uint32_t probe = 1;
    std::cout << "host byte order: " << (*reinterpret_cast<const std::uint8_t*>(&probe) == 1 ? "little" : "big")
              << "-endian" << std::endl;

    int failed = 0;
    int passed = 0;
    for (const auto& vector : lgsomeip::interop::vectors()) {
        const auto result = lgsomeip::interop::run_local(vector);
        const char* verdict = "PASS";
        if (result.ok()) {
            passed++;
        } else {
            verdict = "FAIL";
            failed++;
        }

        std::cout << verdict << "  " << vector.name << "  (encode==golden:" << result.encode_matches_golden
                  << " decode(golden):" << result.decode_golden << " roundtrip:" << result.roundtrip << ")"
                  << std::endl;
        if (!result.ok()) {
            std::cout << "      golden : " << lgsomeip::interop::hex(vector.golden) << std::endl;
            std::cout << "      encoded: " << lgsomeip::interop::hex(vector.encode()) << std::endl;
        }
    }

    std::cout << "serialization: passed=" << passed << " failed=" << failed << std::endl;
    return failed == 0 ? 0 : 1;
}
