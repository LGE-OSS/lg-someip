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
        interop-serialization)
    add_executable(${INTEROP_APP} ${INTEROP_APP}.cpp)
    lgsomeip_configure_target(${INTEROP_APP})
    target_link_libraries(${INTEROP_APP} PRIVATE interop-common lgsomeip::api ${ADDITIONAL_LIBS})
endforeach()

# The serializer check needs no daemon, so it can run as a regular test.
if(BUILD_TESTING)
    add_test(NAME interop.serialization COMMAND interop-serialization)
    set_tests_properties(interop.serialization PROPERTIES LABELS interop TIMEOUT 60)
endif()
