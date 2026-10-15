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

# Same-host open-loop RPC sweep: for each payload size, step the offered rate up
# and record throughput, loss, and latency under load. Rate 0 means "as fast as
# the in-flight window allows" and gives the saturation throughput.
#
#   run-rpc-rate.sh [-b <build dir>] [-o <results dir>] [-p "<payload sizes>"]
#                   [-r "<rates msg/s>"] [-d <seconds per step>] [-W <window>]
#                   [-t udp|tcp] [-c <cpu lists>]
#
#   -d  measured duration of each step; the count is rate * duration
#       (rate 0 uses -n <count> instead)
#   -c  CPU lists for daemon,server,client (e.g. "1,2,3-4"); the client should
#       get at least two CPUs because the sender paces by yielding
#
# Results: <results dir>/rpc-rate.csv plus one samples file per step.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${script_dir}/common.sh"

build_dir="${script_dir}/../../../build"
results_dir="$(cd "${script_dir}/../.." && pwd)/results"
payloads="64 1024 16384"
rates="1000 5000 10000 20000 50000 0"
duration=5
count=100000
window=1000
transport="udp"
cpus=""

while getopts "b:o:p:r:d:n:W:t:c:h" opt; do
    case "${opt}" in
        b) build_dir="${OPTARG}" ;;
        o) results_dir="${OPTARG}" ;;
        p) payloads="${OPTARG}" ;;
        r) rates="${OPTARG}" ;;
        d) duration="${OPTARG}" ;;
        n) count="${OPTARG}" ;;
        W) window="${OPTARG}" ;;
        t) transport="${OPTARG}" ;;
        c) cpus="${OPTARG}" ;;
        *) sed -n '15,/^set -euo/p' "$0" | sed '$d'; exit 1 ;;
    esac
done

perf_resolve_binaries
server="${perf_bin_dir}/perf-rpc-server"
client="${perf_bin_dir}/perf-rpc-client"
perf_resolve_binaries "${server}" "${client}"
perf_setup_pinning
perf_start_stack "${server}"

status=0
for payload in ${payloads}; do
    for rate in ${rates}; do
        if [[ "${rate}" == "0" ]]; then
            step_count="${count}"
        else
            step_count=$((rate * duration))
        fi
        # Warm up for about a tenth of the step, at the same rate.
        step_warmup=$((step_count / 10))
        echo "=== rpc-rate transport=${transport} payload=${payload} rate=${rate} count=${step_count} ==="
        "${pin_client[@]}" "${client}" --config "${config}" --mode rate --rate "${rate}" --window "${window}" \
            --payload "${payload}" --count "${step_count}" --warmup "${step_warmup}" \
            --label "local-${transport}" --csv "${results_dir}/rpc-rate.csv" \
            --samples-out "${results_dir}/rpc-rate-${transport}-${payload}-${rate}.samples" \
            | grep -E "^  (throughput_msg_s|loss_pct|p50_us|p99_us|max_send_lag_us|stalled) " || status=1
    done
done

echo "results: ${results_dir}/rpc-rate.csv"
exit "${status}"
