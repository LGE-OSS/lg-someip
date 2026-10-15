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

# Service Discovery timing. Runs up to three phases, K iterations each:
#
#   cold    provider already offered; start a fresh probe K times and measure
#           start() -> availability and subscribe() -> initial field event
#   toggle  provider offers and withdraws K times (-T on/off ms); measure offer
#           -> availability and graceful StopOffer -> unavailability
#   crash   start the provider, kill -9 it, K times; measure how long the
#           consumer takes to notice (last notification -> unavailability)
#
#   run-sd.sh [-b <build dir>] [-o <results dir>] [-k <iterations>] [-P "<phases>"]
#             [-T <on ms>,<off ms>] [-t udp|tcp] [-c <cpu lists>]
#             [-- <gen-config.sh SD options, e.g. -I 50 -C 1000>]
#
# On one host the daemon delivers local offers directly, so these numbers show
# the local path only. SD timing (initial delay, repetitions, cyclic offers,
# TTL expiry) applies between hosts; see performance-measurement/README.md for
# the two-host procedure.
#
# Results: <results dir>/sd-probe.csv (one row per episode).

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${script_dir}/common.sh"

build_dir="${script_dir}/../../../build"
results_dir="$(cd "${script_dir}/../.." && pwd)/results"
iterations=20
phases="cold toggle crash"
on_off="1000,500"
transport="udp"
cpus=""

while getopts "b:o:k:P:T:t:c:h" opt; do
    case "${opt}" in
        b) build_dir="${OPTARG}" ;;
        o) results_dir="${OPTARG}" ;;
        k) iterations="${OPTARG}" ;;
        P) phases="${OPTARG}" ;;
        T) on_off="${OPTARG}" ;;
        t) transport="${OPTARG}" ;;
        c) cpus="${OPTARG}" ;;
        *) sed -n '15,/^set -euo/p' "$0" | sed '$d'; exit 1 ;;
    esac
done
shift $((OPTIND - 1))
IFS=, read -r on_ms off_ms <<< "${on_off}"

perf_resolve_binaries
provider="${perf_bin_dir}/perf-sd-provider"
probe="${perf_bin_dir}/perf-sd-probe"
perf_resolve_binaries "${provider}" "${probe}"
perf_setup_pinning

mkdir -p "${results_dir}"
config="${results_dir}/perf_local_${transport}_sd.json"
"${script_dir}/gen-config.sh" -o "${config}" -t "${transport}" -r local "$@"
trap perf_cleanup EXIT
perf_start_daemon
rm -f "${results_dir}"/sd-probe-*.log "${results_dir}/sd-provider.log"

csv="${results_dir}/sd-probe.csv"
label="local-${transport}"
status=0

start_provider() {
    "${pin_server[@]}" "${provider}" --config "${config}" "$@" >> "${results_dir}/sd-provider.log" 2>&1 &
    provider_pid=$!
    perf_pids+=("${provider_pid}")
}

stop_provider() {
    kill "-$1" "${provider_pid}" 2>/dev/null || true
    wait "${provider_pid}" 2>/dev/null || true
}

for phase in ${phases}; do
    echo "=== sd phase=${phase} iterations=${iterations} ==="
    case "${phase}" in
        cold)
            start_provider --cycles 0
            sleep 1
            for ((i = 1; i <= iterations; i++)); do
                "${pin_client[@]}" "${probe}" --config "${config}" --episodes 1 --phase cold --label "${label}" \
                    --csv "${csv}" >> "${results_dir}/sd-probe-cold.log" 2>&1 || status=1
            done
            stop_provider INT
            ;;
        toggle)
            "${pin_client[@]}" "${probe}" --config "${config}" --episodes "${iterations}" --wait-down \
                --phase toggle --label "${label}" --csv "${csv}" > "${results_dir}/sd-probe-toggle.log" 2>&1 &
            probe_pid=$!
            perf_pids+=("${probe_pid}")
            sleep 1
            start_provider --cycles "${iterations}" --on-ms "${on_ms}" --off-ms "${off_ms}"
            wait "${provider_pid}" || status=1
            wait "${probe_pid}" || status=1
            ;;
        crash)
            "${pin_client[@]}" "${probe}" --config "${config}" --episodes "${iterations}" --wait-down \
                --phase crash --label "${label}" --csv "${csv}" > "${results_dir}/sd-probe-crash.log" 2>&1 &
            probe_pid=$!
            perf_pids+=("${probe_pid}")
            sleep 1
            for ((i = 1; i <= iterations; i++)); do
                start_provider --cycles 0
                sleep "$(awk "BEGIN { print ${on_ms} / 1000 }")"
                stop_provider KILL
                sleep "$(awk "BEGIN { print ${off_ms} / 1000 }")"
            done
            wait "${probe_pid}" || status=1
            ;;
        *)
            echo "unknown phase '${phase}'" >&2
            exit 1
            ;;
    esac
    perf_summarize_ms "${csv}" phase "${phase}"
done

echo "results: ${csv}"
exit "${status}"
