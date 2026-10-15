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

# Shared helpers for the same-host benchmark scripts. Source this file; it
# expects build_dir, results_dir, transport, and cpus to be set by the caller.

perf_resolve_binaries() {
    build_dir="$(cd "${build_dir}" && pwd)"
    daemon="${build_dir}/daemon/someip-daemon"
    perf_bin_dir="${build_dir}/performance-measurement/lg-someip"
    local binary
    for binary in "${daemon}" "$@"; do
        [[ -x "${binary}" ]] || { echo "missing ${binary}; build with -DENABLE_EXAMPLES=ON" >&2; exit 1; }
    done
}

# CPU pinning prefixes for daemon, server, and client from "-c d,s,c". The
# binaries are started directly (not through a shell function) so "$!" is the
# real process and the cleanup signal reaches it; background subshells of a
# script ignore SIGINT. taskset pins every thread of a process.
perf_setup_pinning() {
    pin_daemon=()
    pin_server=()
    pin_client=()
    if [[ -n "${cpus}" ]]; then
        IFS=, read -r cpu_daemon cpu_server cpu_client <<< "${cpus}"
        pin_daemon=(taskset -c "${cpu_daemon}")
        pin_server=(taskset -c "${cpu_server}")
        pin_client=(taskset -c "${cpu_client}")
    fi
}

perf_pids=()
perf_cleanup() {
    # Stop in reverse start order so the server withdraws its offer before the daemon exits.
    local i
    for ((i = ${#perf_pids[@]} - 1; i >= 0; i--)); do
        kill -INT "${perf_pids[i]}" 2>/dev/null || true
        wait "${perf_pids[i]}" 2>/dev/null || true
    done
}

# Generate the local configuration into ${config} and arm the cleanup trap.
perf_prepare() {
    mkdir -p "${results_dir}"
    config="${results_dir}/perf_local_${transport}.json"
    "$(dirname "${BASH_SOURCE[0]}")/gen-config.sh" -o "${config}" -t "${transport}" -r local
    trap perf_cleanup EXIT
}

perf_start_daemon() {
    # The daemon owns the local IPC sockets; stale ones from a crashed run block startup.
    rm -rf /tmp/someip/
    "${pin_daemon[@]}" "${daemon}" "${config}" > "${results_dir}/daemon.log" 2>&1 &
    perf_pids+=($!)
    sleep 1
}

# Start the daemon and one server process with a generated local configuration.
# Usage: perf_start_stack <server binary> [server args...]
perf_start_stack() {
    perf_prepare
    perf_start_daemon
    local server="$1"; shift
    "${pin_server[@]}" "${server}" --config "${config}" "$@" > "${results_dir}/server.log" 2>&1 &
    perf_pids+=($!)
    sleep 1
}

# Merge CSV files that share one header into <output>, appending to it.
# Usage: perf_merge_csv <output> <inputs...>
perf_merge_csv() {
    local output="$1"; shift
    local input
    for input in "$@"; do
        [[ -s "${input}" ]] || continue
        if [[ -s "${output}" ]]; then
            tail -n +2 "${input}" >> "${output}"
        else
            cat "${input}" > "${output}"
        fi
        rm -f "${input}"
    done
}

# Print n/min/p50/max of every *_ms column of <csv> for rows whose <key column>
# equals <value>. Empty cells are skipped. Uses only awk and sort.
# Usage: perf_summarize_ms <csv> <key column> <value>
perf_summarize_ms() {
    local csv="$1" key="$2" value="$3"
    [[ -s "${csv}" ]] || return 0
    local columns
    columns="$(head -1 "${csv}" | tr , '\n' | grep -n '_ms$' || true)"
    local key_index
    key_index="$(head -1 "${csv}" | tr , '\n' | grep -nx "${key}" | cut -d: -f1)"
    local entry index name
    for entry in ${columns}; do
        index="${entry%%:*}"
        name="${entry#*:}"
        awk -F, -v k="${key_index}" -v v="${value}" -v c="${index}" \
            'NR > 1 && $k == v && $c != "" { print $c }' "${csv}" | sort -g |
            awk -v name="${name}" '{ x[NR] = $1 } END {
                if (NR > 0) printf "  %-30s n=%-4d min=%-10.3f p50=%-10.3f max=%.3f\n",
                    name, NR, x[1], x[int((NR + 1) / 2)], x[NR] }'
    done
}

# Print the CPU time, memory, and context switches of process <pid> as
# key=value lines. CPU time and context switches are summed over every thread
# (/proc/<pid>/status alone reports only the main thread). Run time comes from
# schedstat in nanoseconds, because utime/stime in /proc/<pid>/stat advance in
# whole ticks (10 ms at 100 Hz); the tick values are the fallback when the
# kernel has no schedstat, and daemon_cpu_source says which one was used. Time
# of threads that already exited is not included. CPU time is cumulative, so
# two calls around a scenario and their difference give that scenario's cost;
# VmHWM is the peak RSS since start and needs no polling in between.
# Usage: perf_daemon_usage <pid>
perf_daemon_usage() {
    local pid="$1"
    if [[ -z "${pid}" ]] || ! kill -0 "${pid}" 2>/dev/null; then
        echo "daemon_pid=0"
        return 0
    fi
    echo "daemon_pid=${pid}"
    if cat "/proc/${pid}/task/${pid}/schedstat" >/dev/null 2>&1; then
        awk '{ ns += $1 } END { printf "daemon_cpu_ms=%.3f\ndaemon_cpu_source=schedstat\n", ns / 1e6 }' \
            /proc/"${pid}"/task/*/schedstat 2>/dev/null
    else
        # Field 2 is "(comm)", which may itself contain spaces or parentheses;
        # skip past its closing ")" instead of splitting on whitespace.
        awk -v c="$(getconf CLK_TCK)" '{ i = match($0, /\)[^)]*$/); n = split(substr($0, i + 2), f, " ");
            printf "daemon_cpu_ms=%.3f\ndaemon_cpu_source=ticks\n", (f[12] + f[13]) * 1000 / c }' \
            "/proc/${pid}/stat" 2>/dev/null
    fi
    echo "daemon_threads=$(find "/proc/${pid}/task" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)"
    awk '
        /^VmRSS:/ { print "daemon_rss_kb=" $2 }
        /^VmHWM:/ { print "daemon_peak_rss_kb=" $2 }
    ' "/proc/${pid}/status" 2>/dev/null
    awk '
        /^voluntary_ctxt_switches:/ { vol += $2 }
        /^nonvoluntary_ctxt_switches:/ { invol += $2 }
        END { print "daemon_vol_ctx_switches=" vol + 0; print "daemon_invol_ctx_switches=" invol + 0 }
    ' /proc/"${pid}"/task/*/status 2>/dev/null
}
