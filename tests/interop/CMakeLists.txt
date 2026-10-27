# *******************************************************************************
# Copyright (C) 2026 LG Electronics Inc.
#
# See the NOTICE file(s) distributed with this work for additional
# information regarding copyright ownership.
#
# This program and the accompanying materials are made available under the
# terms of the Apache License Version 2.0 which is available at
# https://www.apache.org/licenses/LICENSE-2.0
#
# SPDX-License-Identifier: Apache-2.0
# *******************************************************************************

# Interoperability test applications. They need no test framework, so they build
# on QNX as well as on Linux. Enable them with -DENABLE_INTEROP_TESTS=ON.
#
#   interop-serialization  offline check of the serializer against golden bytes
#   interop-server         service provider (methods and events)
#   interop-client         method tests; prints PASS/FAIL, exit code 0 on success
#   interop-subscriber     event tests including the multicast event group
#
# The service has one instance per transport (0x0001 TCP, 0x0002 UDP), so a run
# tests exactly one transport: pass --tcp (default) or --udp to client/subscriber.
#
# Use config/interop_config.json on every host and set "unicast" to the address
# of that host (and "is-provider" to true on the server host only).
#
# Run (the daemon must run on every host, see examples/config/README.md):
#   export LGSOMEIP_CONFIGURATION=<path>/interop_config.json
#   host A: interop-server
#   host B: interop-client --tcp --large
#           interop-client --udp
#           interop-subscriber --tcp --app interop-sub-1 --count 5
#           interop-subscriber --udp --app interop-sub-1 --count 5
#           interop-subscriber --udp --app interop-sub-2 --count 5   (a second UDP
#             subscriber reaches the multicast threshold of 2)
# Multicast on the wire (host A or B):
#   tcpdump -ni <interface> 'udp and host 224.225.226.235'

add_library(interop-common STATIC common/InteropVectors.cpp)
lgsomeip_configure_target(interop-common)
set_target_properties(interop-common PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(interop-common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/common
        ${LGSOMEIP_SOURCE_DIR}/src
)
target_link_libraries(interop-common PUBLIC lgsomeip::api)

foreach(INTEROP_APP IN ITEMS
        interop-serialization
        interop-server
        interop-client
        interop-subscriber)
    add_executable(${INTEROP_APP} ${INTEROP_APP}.cpp)
    lgsomeip_configure_target(${INTEROP_APP})
    target_link_libraries(${INTEROP_APP} PRIVATE interop-common lgsomeip::api ${ADDITIONAL_LIBS})
endforeach()

# The serializer check needs no daemon, so it can run as a regular test.
if(BUILD_TESTING)
    add_test(NAME interop.serialization COMMAND interop-serialization)
    set_tests_properties(interop.serialization PROPERTIES LABELS interop TIMEOUT 60)
endif()
