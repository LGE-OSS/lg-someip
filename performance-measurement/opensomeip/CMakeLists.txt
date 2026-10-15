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

cmake_minimum_required(VERSION 3.20)
project(someip-perf-opensomeip CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(opensomeip CONFIG REQUIRED)
find_package(Threads REQUIRED)

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
    target_compile_definitions(${name} PRIVATE PERF_GIT_REVISION="${PERF_GIT_REVISION}")
    target_link_libraries(${name} PRIVATE opensomeip::opensomeip Threads::Threads)
endfunction()

add_perf_app(perf-rpc-server)
add_perf_app(perf-rpc-client)
add_perf_app(perf-rpc-threads)
add_perf_app(perf-event-publisher)
add_perf_app(perf-event-subscriber)
