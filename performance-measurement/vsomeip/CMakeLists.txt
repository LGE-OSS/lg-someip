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

# Performance measurement applications written against the public vsomeip API,
# so they can be built against either real upstream vsomeip (COVESA) or
# lg-someip's bundled vsomeip-compatible shim with no source changes. See
# performance-measurement/docs/stack-comparison.md for why this exists and how to pick a backend.
cmake_minimum_required(VERSION 3.16)
project(someip-perf-vsomeip CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# OFF (default): link lg-someip's bundled vsomeip-compatible shim
#   (find_package(vsomeip), target "vsomeip"). Point CMAKE_PREFIX_PATH at an
#   `lg-someip` install (e.g. cmake --install build --prefix <dir>).
# ON: link real upstream COVESA vsomeip. Its CMake package and target are both
#   named "vsomeip3" (confirmed from vsomeip3Config.cmake.in upstream), not
#   "vsomeip" and not namespaced.
option(USE_REAL_VSOMEIP "Link against upstream COVESA vsomeip (vsomeip3) instead of lg-someip's bundled shim" OFF)

if(USE_REAL_VSOMEIP)
    find_package(vsomeip3 CONFIG REQUIRED)
    set(VSOMEIP_TARGET vsomeip3)
else()
    find_package(vsomeip CONFIG REQUIRED)
    set(VSOMEIP_TARGET vsomeip)
    # lg-someip bundles an older vsomeip API snapshot: application::send() still
    # takes a flush flag, and register_message_handler() still takes an
    # is_provider flag. Both were dropped in real vsomeip by 3.7.6.
    set(PERF_VSOMEIP_LEGACY_API_DEFINE PERF_VSOMEIP_LEGACY_API)
endif()

if(NOT DEFINED PERF_GIT_REVISION)
    execute_process(
        COMMAND git describe --always --dirty
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE PERF_GIT_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(NOT PERF_GIT_REVISION)
    set(PERF_GIT_REVISION "unknown")
endif()

function(add_perf_app name)
    add_executable(${name} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp)
    # common/ (payload layout, statistics, pacing, reports) is shared with the
    # lg-someip applications so that every stack runs the same measurement code.
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                               ${CMAKE_CURRENT_SOURCE_DIR}/../lg-someip)
    target_compile_definitions(${name} PRIVATE PERF_GIT_REVISION="${PERF_GIT_REVISION}" ${PERF_VSOMEIP_LEGACY_API_DEFINE})
    target_link_libraries(${name} PRIVATE ${VSOMEIP_TARGET} Threads::Threads)
endfunction()

find_package(Threads REQUIRED)

add_perf_app(perf-rpc-server)
add_perf_app(perf-rpc-client)
add_perf_app(perf-rpc-threads)
add_perf_app(perf-event-publisher)
add_perf_app(perf-event-subscriber)
add_perf_app(perf-sd-provider)
add_perf_app(perf-sd-probe)
