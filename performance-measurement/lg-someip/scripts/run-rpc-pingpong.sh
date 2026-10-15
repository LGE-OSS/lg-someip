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

# Same-host RPC ping-pong sweep: daemon, provider, and consumer on one machine.
#
#   run-rpc-pingpong.sh [-b <build dir>] [-o <results dir>] [-p "<payload sizes>"]
#                       [-n <count>] [-w <warmup>] [-t udp|tcp] [-s] [-c <cpu lists>]
#
#   -s  busy-wait for responses in the client instead of a condition variable
#   -c  CPU lists for daemon,server,client separated by commas (e.g. "1,2,3" or
#       "1,2,3-4"). taskset pins every thread of a process, so with -s the client
#       needs at least two CPUs or the spinning thread starves its receive thread.
#
# Results: <results dir>/rpc-pingpong.csv plus one samples file per payload size.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${script_dir}/common.sh"

build_dir="${script_dir}/../../../build"
results_dir="$(cd "${script_dir}/../.." && pwd)/results"
payloads="16 64 256 1024 4096 16384 65536"
count=10000
warmup=1000
transport="udp"
spin=""
cpus=""

while getopts "b:o:p:n:w:t:sc:h" opt; do
    case "${opt}" in
        b) build_dir="${OPTARG}" ;;
        o) results_dir="${OPTARG}" ;;
        p) payloads="${OPTARG}" ;;
        n) count="${OPTARG}" ;;
        w) warmup="${OPTARG}" ;;
        t) transport="${OPTARG}" ;;
        s) spin="--spin" ;;
        c) cpus="${OPTARG}" ;;
        *) sed -n '15,/^set -euo/p' "$0" | sed '$d'; exit 1 ;;
    esac
done

perf_resolve_binaries
server="${perf_bin_dir}/perf-rpc-server"
client="${perf_bin_dir}/perf-rpc-client"
perf_resolve_binaries "${server}" "${client}"
perf_setup_pinning
if [[ -n "${spin}" && "${cpu_client:-}" =~ ^[0-9]+$ ]]; then
    echo "warning: -s with a single client CPU measures scheduler timeslices, not latency" >&2
fi

perf_start_stack "${server}"

status=0
for payload in ${payloads}; do
    echo "=== rpc-pingpong transport=${transport} payload=${payload} ==="
    "${pin_client[@]}" "${client}" --config "${config}" --mode pingpong --payload "${payload}" \
        --count "${count}" --warmup "${warmup}" --label "local-${transport}" \
        --csv "${results_dir}/rpc-pingpong.csv" \
        --samples-out "${results_dir}/rpc-pingpong-${transport}-${payload}.samples" ${spin} || status=$?
done

echo "results: ${results_dir}/rpc-pingpong.csv"
exit "${status}"
