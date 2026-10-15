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

# Performance measurement applications. They use only the public lgsomeip::api
# so the numbers reflect what an application sees.

# Record the source revision in every result row. A copy without .git (such
# as the one run-two-hosts.sh makes) passes it with -DPERF_GIT_REVISION=<rev>.
if(NOT DEFINED PERF_GIT_REVISION)
    execute_process(
        COMMAND git describe --always --dirty
        WORKING_DIRECTORY ${LGSOMEIP_SOURCE_DIR}
        OUTPUT_VARIABLE PERF_GIT_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    # An unpacked make-bundle.sh archive carries the revision in REVISION.
    if(NOT PERF_GIT_REVISION AND EXISTS ${LGSOMEIP_SOURCE_DIR}/REVISION)
        file(STRINGS ${LGSOMEIP_SOURCE_DIR}/REVISION PERF_GIT_REVISION LIMIT_COUNT 1)
    endif()
endif()
if(NOT PERF_GIT_REVISION)
    set(PERF_GIT_REVISION "unknown")
endif()

function(add_perf_app name)
    add_executable(${name} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp)
    lgsomeip_configure_target(${name})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${name} PRIVATE PERF_GIT_REVISION="${PERF_GIT_REVISION}")
    target_link_libraries(${name} PRIVATE lgsomeip::api ${ADDITIONAL_LIBS})
endfunction()

add_perf_app(perf-rpc-server)
add_perf_app(perf-rpc-client)
add_perf_app(perf-rpc-threads)
add_perf_app(perf-event-publisher)
add_perf_app(perf-event-subscriber)
add_perf_app(perf-sd-provider)
add_perf_app(perf-sd-probe)
