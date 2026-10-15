#!/bin/bash
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

# Build and install vsomeip 3.7.6 and opensomeip v0.2.0 in the home directory of
# a benchmark board, as performance-measurement/docs/stack-comparison.md
# describes. Run it on the board (about 15 minutes on a Raspberry Pi 4). It
# needs build-essential, cmake, git, and
# libboost-{dev,system-dev,thread-dev,filesystem-dev}; it installs to
# ~/vsomeip-install and ~/opensomeip-install and leaves routingmanagerd in
# ~/vsomeip-src/build/examples/routingmanagerd/, where the agent looks for it.

set -euo pipefail
cd "${HOME}"
[[ -d vsomeip-src ]] || git clone -q --depth 1 --branch 3.7.6 https://github.com/COVESA/vsomeip.git vsomeip-src
[[ -d opensomeip-src ]] || git clone -q --depth 1 --branch v0.2.0 https://github.com/vtz/opensomeip.git opensomeip-src

echo "== vsomeip $(date +%T)"
cmake -S vsomeip-src -B vsomeip-src/build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${HOME}/vsomeip-install" -DENABLE_SIGNAL_HANDLING=1 > vsomeip-configure.log 2>&1
cmake --build vsomeip-src/build -j"$(nproc)" --target vsomeip3 routingmanagerd > vsomeip-build.log 2>&1
cmake --install vsomeip-src/build > vsomeip-install.log 2>&1
ls -l vsomeip-src/build/examples/routingmanagerd/routingmanagerd

echo "== opensomeip $(date +%T)"
cmake -S opensomeip-src -B opensomeip-src/build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${HOME}/opensomeip-install" -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF > opensomeip-configure.log 2>&1
cmake --build opensomeip-src/build -j"$(nproc)" > opensomeip-build.log 2>&1
cmake --install opensomeip-src/build > opensomeip-install.log 2>&1
ls "${HOME}/opensomeip-install"
echo "== done $(date +%T)"
