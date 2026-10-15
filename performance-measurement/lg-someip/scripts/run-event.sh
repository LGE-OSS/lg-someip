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

# Same-host event sweep: for each subscriber count, payload size, and offered
# rate, start N subscribers and one publisher, then record one-way latency,
# loss, and reordering per subscriber. Rate 0 publishes as fast as possible.
#
#   run-event.sh [-b <build dir>] [-o <results dir>] [-S "<subscriber counts>"]
#                [-p "<payload sizes>"] [-r "<rates msg/s>"] [-d <seconds per step>]
#                [-n <count for rate 0>] [-t udp|tcp] [-c <cpu lists>]
#
#   -S  subscriber counts to test, each from 1 to 8 (default "1 4")
#   -c  CPU lists for daemon,publisher,subscribers (e.g. "1,2,3-6"); all
#       subscribers share the third list
#
# Results: <results dir>/event-subscribe.csv (one row per subscriber and step),
#          <results dir>/event-publish.csv (one row per step), samples files.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${script_dir}/common.sh"

build_dir="${script_dir}/../../../build"
results_dir="$(cd "${script_dir}/../.." && pwd)/results"
subscriber_counts="1 4"
payloads="64 1024 16384"
rates="1000 10000 0"
duration=5
count=100000
transport="udp"
cpus=""

while getopts "b:o:S:p:r:d:n:t:c:h" opt; do
    case "${opt}" in
        b) build_dir="${OPTARG}" ;;
        o) results_dir="${OPTARG}" ;;
        S) subscriber_counts="${OPTARG}" ;;
        p) payloads="${OPTARG}" ;;
        r) rates="${OPTARG}" ;;
        d) duration="${OPTARG}" ;;
        n) count="${OPTARG}" ;;
        t) transport="${OPTARG}" ;;
        c) cpus="${OPTARG}" ;;
        *) sed -n '15,/^set -euo/p' "$0" | sed '$d'; exit 1 ;;
    esac
done

perf_resolve_binaries
publisher="${perf_bin_dir}/perf-event-publisher"
subscriber="${perf_bin_dir}/perf-event-subscriber"
perf_resolve_binaries "${publisher}" "${subscriber}"
perf_setup_pinning
perf_prepare
perf_start_daemon

status=0
for subscribers in ${subscriber_counts}; do
    if ((subscribers < 1 || subscribers > 8)); then
        echo "subscriber count ${subscribers} is outside 1..8" >&2
        exit 1
    fi
    for payload in ${payloads}; do
        for rate in ${rates}; do
            if [[ "${rate}" == "0" ]]; then
                step_count="${count}"
            else
                step_count=$((rate * duration))
            fi
            step_warmup=$((step_count / 10))
            step="${transport}-${subscribers}sub-${payload}-${rate}"
            label="local-${transport}-${subscribers}sub"
            echo "=== event transport=${transport} subscribers=${subscribers} payload=${payload} rate=${rate} count=${step_count} ==="

            sub_pids=()
            sub_csvs=()
            for ((i = 1; i <= subscribers; i++)); do
                sub_csv="${results_dir}/.event-subscribe-${i}.csv"
                rm -f "${sub_csv}"
                sub_csvs+=("${sub_csv}")
                "${pin_client[@]}" "${subscriber}" --config "${config}" --name "perf-event-sub-${i}" \
                    --label "${label}" --csv "${sub_csv}" \
                    --samples-out "${results_dir}/event-${step}-sub${i}.samples" \
                    > "${results_dir}/event-sub-${i}.log" 2>&1 &
                sub_pids+=($!)
                perf_pids+=($!)
            done

            "${pin_server[@]}" "${publisher}" --config "${config}" --subscribers "${subscribers}" \
                --rate "${rate}" --payload "${payload}" --count "${step_count}" --warmup "${step_warmup}" \
                --label "${label}" --csv "${results_dir}/event-publish.csv" \
                > "${results_dir}/event-pub.log" 2>&1 || { status=1; echo "publisher failed; see event-pub.log" >&2; }
            grep -E "^  (send_rate_msg_s|notify_p99_us) " "${results_dir}/event-pub.log" | sed 's/^/  pub /' || true

            for ((i = 1; i <= subscribers; i++)); do
                wait "${sub_pids[i - 1]}" || status=1
                grep -E "^  (loss_pct|reordered|throughput_msg_s|p50_us|p99_us) " \
                    "${results_dir}/event-sub-${i}.log" | sed "s/^/  sub${i} /" || true
            done
            perf_merge_csv "${results_dir}/event-subscribe.csv" "${sub_csvs[@]}"
        done
    done
done

echo "results: ${results_dir}/event-subscribe.csv, ${results_dir}/event-publish.csv"
exit "${status}"
