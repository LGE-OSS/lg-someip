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

# Prepare a Raspberry Pi 4/5 (64-bit Raspberry Pi OS or Debian/Ubuntu arm64)
# for the benchmarks: install packages, build a release build of lg-someip with
# the performance applications, and report settings that distort results.
#
#   pi-setup.sh [-n] [-j <jobs>] [-b <build dir>] [-r <revision>]
#
#   -n  skip package installation (no sudo needed)
#   -j  parallel build jobs (default: CPUs, limited to 2 below 3 GB of memory)
#   -b  build directory (default: <repo>/build-pi)
#   -r  revision recorded in results (default: from git, if this is a clone)

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "${script_dir}/../../.." && pwd)"
build_dir="${repo}/build-pi"
install_packages=1
jobs=""
revision_args=()

while getopts "nj:b:r:h" opt; do
    case "${opt}" in
        n) install_packages=0 ;;
        j) jobs="${OPTARG}" ;;
        b) build_dir="${OPTARG}" ;;
        r) revision_args=(-DPERF_GIT_REVISION="${OPTARG}") ;;
        *) sed -n '15,/^set -euo/p' "$0" | sed '$d'; exit 1 ;;
    esac
done

packages=(build-essential cmake git rapidjson-dev linuxptp ethtool rsync nftables)
if ((install_packages)); then
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends "${packages[@]}"
else
    for tool in g++ cmake git; do
        command -v "${tool}" >/dev/null || {
            echo "pi-setup.sh: ${tool} is missing; run without -n, or: sudo apt-get install ${packages[*]}" >&2
            exit 1
        }
    done
fi

if [[ -z "${jobs}" ]]; then
    jobs="$(nproc)"
    mem_kb="$(awk '/MemTotal/ { print $2 }' /proc/meminfo)"
    if ((mem_kb < 3000000 && jobs > 2)); then
        jobs=2
    fi
fi

cmake -S "${repo}" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release -DENABLE_EXAMPLES=ON -DENABLE_TOOLS=OFF \
    "${revision_args[@]}"
cmake --build "${build_dir}" -j "${jobs}" --target someip-daemon \
    perf-rpc-server perf-rpc-client perf-rpc-threads perf-event-publisher perf-event-subscriber perf-sd-provider perf-sd-probe

echo
echo "== host state =="
"${script_dir}/sysinfo.sh" | tee "${build_dir}/sysinfo.txt"

warnings=0
info="$(cat "${build_dir}/sysinfo.txt")"
field() { sed -n "s/^$1=//p" <<< "${info}"; }
if [[ "$(field arch)" != "aarch64" ]]; then
    echo "warning: $(field arch) system; a 64-bit OS is recommended for comparable results" >&2
    warnings=1
fi
if [[ -n "$(field governor)" && "$(field governor)" != "performance" ]]; then
    echo "warning: CPU governor is '$(field governor)'; for stable numbers run:" >&2
    echo "         echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor" >&2
    warnings=1
fi
if [[ -n "$(field throttled)" && "$(field throttled)" != "0x0" ]]; then
    echo "warning: throttled=$(field throttled): under-voltage or thermal throttling since boot; check power and cooling" >&2
    warnings=1
fi
echo
echo "pi-setup.sh: built into ${build_dir}$( ((warnings)) && echo ' (see warnings above)')"
