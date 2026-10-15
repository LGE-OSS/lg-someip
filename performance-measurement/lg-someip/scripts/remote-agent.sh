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

# Host-side helper that run-two-hosts.sh drives over SSH. Every call is short:
# background processes are started with nohup and tracked by PID files, so an
# SSH session never has to stay open while a process runs.
#
#   remote-agent.sh config <gen-config.sh options>    write ${run_dir}/perf.json
#   remote-agent.sh daemon-start | daemon-kill        (re)start or kill -9 the daemon
#   remote-agent.sh start <name> <app> [args...]      start an app in the background
#   remote-agent.sh stop <name> [signal]              signal it and wait (default INT)
#   remote-agent.sh wait <name> <seconds>             wait for it to exit by itself
#   remote-agent.sh run <app> [args...]               run an app in the foreground
#   remote-agent.sh stop-all | clean | sysinfo [<ip>] | ping <ip>
#   remote-agent.sh daemon-usage                     daemon CPU time and RSS as key=value lines
#   remote-agent.sh build-size                       daemon and app binary sizes as key=value lines
#   remote-agent.sh merge-csv <output> <inputs...>
#   remote-agent.sh summarize <csv> <key column> <value>
#   remote-agent.sh local-suite [run-local-suite.sh options]
#
# Environment: PERF_CPUS="<daemon CPUs>,<app CPUs>" (e.g. "1,2-3"),
# PERF_BUILD_DIR (default <repo>/build-pi), PERF_RUN_DIR (default <repo>/perf-run).
# Apps get --config ${run_dir}/perf.json; relative paths in their arguments are
# relative to the repository root.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "${script_dir}/../../.." && pwd)"
build_dir="${PERF_BUILD_DIR:-${repo}/build-pi}"
run_dir="${PERF_RUN_DIR:-${repo}/perf-run}"
bin_dir="${build_dir}/performance-measurement/lg-someip"
config="${run_dir}/perf.json"
IFS=, read -r cpu_daemon cpu_app <<< "${PERF_CPUS:-}"
cd "${repo}"
mkdir -p "${run_dir}"

prefix_for() {
    # prefix_for <cpu list>: print a taskset prefix, or nothing.
    [[ -n "$1" ]] && echo "taskset -c $1"
    return 0
}

start_bg() {
    # start_bg <name> <cpu list> <command...>. nohup and taskset exec the
    # command, so the PID file holds the program itself and signals reach it.
    local name="$1" cpus="$2"; shift 2
    local prefix
    read -r -a prefix <<< "$(prefix_for "${cpus}")"
    # Append, so restarts and later phases keep the earlier output.
    echo "===== $(date -u +%Y-%m-%dT%H:%M:%S.%NZ) start: $*" >> "${run_dir}/${name}.log"
    nohup "${prefix[@]}" "$@" >> "${run_dir}/${name}.log" 2>&1 < /dev/null &
    echo $! > "${run_dir}/${name}.pid"
}

pid_of() {
    [[ -f "${run_dir}/$1.pid" ]] && cat "${run_dir}/$1.pid"
    return 0
}

stop_one() {
    # stop_one <name> <signal>: signal, wait up to 10 s, then kill -9.
    local name="$1" signal="$2" pid i=0
    pid="$(pid_of "${name}")"
    [[ -n "${pid}" ]] || return 0
    kill "-${signal}" "${pid}" 2>/dev/null || true
    while ((i++ < 100)) && kill -0 "${pid}" 2>/dev/null; do
        sleep 0.1
    done
    kill -9 "${pid}" 2>/dev/null || true
    rm -f "${run_dir}/${name}.pid"
}

require_app() {
    [[ -x "${bin_dir}/$1" ]] || { echo "remote-agent.sh: ${bin_dir}/$1 not built; run pi-setup.sh" >&2; exit 1; }
}

daemon_usage() {
    source "${script_dir}/common.sh"
    perf_daemon_usage "$(pid_of daemon)"
}

command="${1:-}"
shift || true
case "${command}" in
    config)
        "${script_dir}/gen-config.sh" -o "${config}" "$@"
        ;;
    daemon-start)
        stop_one daemon INT
        # The daemon owns the local IPC sockets; stale ones from a killed daemon block startup.
        rm -rf /tmp/someip/
        start_bg daemon "${cpu_daemon}" "${build_dir}/daemon/someip-daemon" "${config}"
        # Ready once its local socket exists. A fixed long sleep would let a
        # cyclic offer arrive before the first app starts, which hides the
        # discovery time in cold-start measurements.
        for ((i = 0; i < 300; i++)); do
            [[ -S /tmp/someip/someip-0000 ]] && break
            sleep 0.01
        done
        sleep 0.05
        kill -0 "$(pid_of daemon)" 2>/dev/null && [[ -S /tmp/someip/someip-0000 ]] ||
            { echo "remote-agent.sh: daemon did not start" >&2; tail "${run_dir}/daemon.log" >&2; exit 1; }
        ;;
    daemon-kill)
        stop_one daemon KILL
        ;;
    start)
        name="$1" app="$2"; shift 2
        require_app "${app}"
        start_bg "${name}" "${cpu_app}" "${bin_dir}/${app}" --config "${config}" "$@"
        ;;
    stop)
        stop_one "$1" "${2:-INT}"
        ;;
    wait)
        pid="$(pid_of "$1")"
        for ((i = 0; i < $2 * 10; i++)); do
            [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null || break
            sleep 0.1
        done
        if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
            echo "remote-agent.sh: $1 still running after $2 s; stopping it" >&2
            stop_one "$1" INT
            exit 1
        fi
        rm -f "${run_dir}/$1.pid"
        ;;
    run)
        app="$1"; shift
        require_app "${app}"
        read -r -a prefix <<< "$(prefix_for "${cpu_app}")"
        "${prefix[@]}" "${bin_dir}/${app}" --config "${config}" "$@"
        ;;
    stop-all)
        for pidfile in "${run_dir}"/*.pid; do
            [[ -f "${pidfile}" ]] || continue
            name="$(basename "${pidfile}" .pid)"
            [[ "${name}" == daemon ]] || stop_one "${name}" INT
        done
        stop_one daemon INT
        ;;
    ping)
        # Network round-trip baseline without lg-someip. Intervals below
        # 200 ms need root on some systems, so fall back to a slower run.
        ping -q -c 500 -i 0.01 "$1" 2>/dev/null || ping -q -c 50 -i 0.2 "$1"
        ;;
    clean)
        rm -rf "${run_dir}"
        mkdir -p "${run_dir}"
        ;;
    sysinfo)
        "${script_dir}/sysinfo.sh" "$@"
        ;;
    daemon-usage)
        daemon_usage
        ;;
    build-size)
        # Byte size of the daemon and every built perf-* binary, as key=value
        # lines, so a run can report its storage (ROM) footprint.
        for f in "${build_dir}/daemon/someip-daemon" "${bin_dir}"/perf-*; do
            [[ -f "${f}" ]] && printf '%s_bytes=%s\n' "$(basename "${f}")" "$(stat -c%s "${f}")"
        done
        ;;
    merge-csv)
        source "${script_dir}/common.sh"
        perf_merge_csv "$@"
        ;;
    summarize)
        source "${script_dir}/common.sh"
        perf_summarize_ms "$@"
        ;;
    local-suite)
        "${script_dir}/run-local-suite.sh" -b "${build_dir}" -o "${run_dir}/local" "$@"
        ;;
    *)
        sed -n '15,/^set -euo/p' "$0" | sed '$d'
        exit 1
        ;;
esac
