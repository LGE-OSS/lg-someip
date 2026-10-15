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

# Run every same-host benchmark on this machine with defaults sized for a
# 4-core board such as a Raspberry Pi 4/5, and record the host state before
# and after (temperature and throttling can change during a run).
#
#   run-local-suite.sh [-b <build dir>] [-o <results dir>] [-s "<scenarios>"]
#                      [-c <daemon>,<server>,<client> CPUs] [-q]
#
#   -s  any of: pingpong rate event sd (default: all)
#   -c  default "1,2,3": CPU 0 is left for interrupts and other work
#   -q  quick smoke test with small counts (results are not meaningful)
#
# Stops when a benchmark daemon from another run is using /tmp/someip.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${script_dir}/../../../build-pi"
results_dir="$(cd "${script_dir}/../.." && pwd)/results/local"
scenarios="pingpong rate event sd"
cpus="1,2,3"
quick=0

while getopts "b:o:s:c:qh" opt; do
    case "${opt}" in
        b) build_dir="${OPTARG}" ;;
        o) results_dir="${OPTARG}" ;;
        s) scenarios="${OPTARG}" ;;
        c) cpus="${OPTARG}" ;;
        q) quick=1 ;;
        *) sed -n '15,/^set -euo/p' "$0" | sed '$d'; exit 1 ;;
    esac
done

if pgrep -x someip-daemon >/dev/null; then
    echo "run-local-suite.sh: someip-daemon is already running; stop it first (the suite starts its own)" >&2
    exit 1
fi
mkdir -p "${results_dir}"
"${script_dir}/sysinfo.sh" > "${results_dir}/sysinfo-before.txt"

if ((quick)); then
    pingpong_args=(-p "64 1024" -n 500 -w 50)
    rate_args=(-p "64" -r "1000 0" -d 1 -n 5000)
    event_args=(-S "1 2" -p "64" -r "1000 0" -d 1 -n 5000)
    sd_args=(-k 2 -T 300,300)
else
    pingpong_args=()
    rate_args=(-p "64 1024 16384" -r "1000 5000 10000 20000 0" -d 3 -n 50000)
    event_args=(-S "1 4" -p "64 1024 16384" -r "1000 10000 0" -d 3 -n 50000)
    sd_args=(-k 10 -T 1000,500)
fi

status=0
for scenario in ${scenarios}; do
    echo "##### local ${scenario} #####"
    case "${scenario}" in
        pingpong) "${script_dir}/run-rpc-pingpong.sh" -b "${build_dir}" -o "${results_dir}" -c "${cpus}" "${pingpong_args[@]}" || status=1 ;;
        rate) "${script_dir}/run-rpc-rate.sh" -b "${build_dir}" -o "${results_dir}" -c "${cpus}" "${rate_args[@]}" || status=1 ;;
        event) "${script_dir}/run-event.sh" -b "${build_dir}" -o "${results_dir}" -c "${cpus}" "${event_args[@]}" || status=1 ;;
        sd) "${script_dir}/run-sd.sh" -b "${build_dir}" -o "${results_dir}" -c "${cpus}" "${sd_args[@]}" || status=1 ;;
        *) echo "unknown scenario '${scenario}'" >&2; status=1 ;;
    esac
done

"${script_dir}/sysinfo.sh" > "${results_dir}/sysinfo-after.txt"
grep -E "^(temp_c|throttled)=" "${results_dir}/sysinfo-after.txt" | sed 's/^/after run: /'
echo "results: ${results_dir}"
exit "${status}"
