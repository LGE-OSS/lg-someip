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

# Host-side helper that run-two-hosts.sh drives over SSH for the opensomeip
# benchmarks. opensomeip has no routing daemon and no configuration file, so
# there is nothing to start besides the applications themselves; each call is
# short and background processes are tracked by PID files, as in the lg-someip
# and vsomeip agents.
#
#   remote-agent.sh start <name> <app> [args...]   start an app in the background
#   remote-agent.sh stop <name> [signal]           signal it and wait (default INT)
#   remote-agent.sh wait <name> <seconds>          wait for it to exit by itself
#   remote-agent.sh run <app> [args...]            run an app in the foreground
#   remote-agent.sh stop-all | clean | sysinfo [<ip>] | ping <ip>
#   remote-agent.sh build-size                     app binary sizes as key=value lines
#   remote-agent.sh versions                       library versions as key=value lines
#   remote-agent.sh merge-csv <output> <inputs...>
#
# Environment: PERF_CPUS="<unused>,<app CPUs>" (same "1,2-3" form as the other
# stacks; only the second field is used), PERF_BUILD_DIR, PERF_RUN_DIR,
# OPENSOMEIP_PREFIX (default ~/opensomeip-install).

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "${script_dir}/../../.." && pwd)"
build_dir="${PERF_BUILD_DIR:-${repo}/performance-measurement/opensomeip/build}"
run_dir="${PERF_RUN_DIR:-${repo}/perf-run-opensomeip}"
install_dir="${OPENSOMEIP_PREFIX:-${HOME}/opensomeip-install}"
IFS=, read -r _ cpu_app <<< "${PERF_CPUS:-}"

mkdir -p "${run_dir}"
export LD_LIBRARY_PATH="${install_dir}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

prefix_for() {
    [[ -n "$1" ]] && echo "taskset -c $1"
    return 0
}

pid_of() {
    [[ -f "${run_dir}/$1.pid" ]] && cat "${run_dir}/$1.pid"
    return 0
}

start_bg() {
    # start_bg <name> <cpu list> <command...>
    local name="$1" cpus="$2"
    shift 2
    local prefix
    read -r -a prefix <<< "$(prefix_for "${cpus}")"
    echo "===== $(date -u +%Y-%m-%dT%H:%M:%S.%NZ) start: $*" >> "${run_dir}/${name}.log"
    nohup "${prefix[@]}" "$@" >> "${run_dir}/${name}.log" 2>&1 < /dev/null &
    echo $! > "${run_dir}/${name}.pid"
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
    rm -f "${run_dir:?}/${name:?}.pid"
}

require_app() {
    [[ -x "${build_dir}/$1" ]] || { echo "opensomeip remote-agent: ${build_dir}/$1 not built" >&2; exit 1; }
}

command="${1:-}"
shift || true
case "${command}" in
    start)
        name="$1" app="$2"
        shift 2
        require_app "${app}"
        start_bg "${name}" "${cpu_app}" "${build_dir}/${app}" "$@"
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
            echo "opensomeip remote-agent: $1 still running after $2 s; stopping it" >&2
            stop_one "$1" INT
            exit 1
        fi
        rm -f "${run_dir:?}/${1:?}.pid"
        ;;
    run)
        app="$1"
        shift
        require_app "${app}"
        read -r -a prefix <<< "$(prefix_for "${cpu_app}")"
        "${prefix[@]}" "${build_dir}/${app}" "$@"
        ;;
    stop-all)
        for pidfile in "${run_dir}"/*.pid; do
            [[ -f "${pidfile}" ]] || continue
            stop_one "$(basename "${pidfile}" .pid)" INT
        done
        ;;
    clean)
        rm -rf "${run_dir:?}"
        mkdir -p "${run_dir}"
        ;;
    sysinfo)
        "${repo}/performance-measurement/lg-someip/scripts/sysinfo.sh" "$@"
        ;;
    ping)
        ping -q -c 500 -i 0.01 "$1" 2>/dev/null || ping -q -c 50 -i 0.2 "$1"
        ;;
    build-size)
        for f in "${build_dir}"/perf-*; do
            [[ -f "${f}" ]] && printf '%s_bytes=%s\n' "$(basename "${f}")" "$(stat -c%s "${f}")"
        done
        ;;
    versions)
        # Best effort: the source checkout the library was built from.
        for dir in "${HOME}/opensomeip-src" "${install_dir}/src"; do
            if [[ -d "${dir}/.git" ]]; then
                echo "opensomeip_version=$(git -C "${dir}" describe --tags --always 2>/dev/null || echo unknown)"
                exit 0
            fi
        done
        echo "opensomeip_version=unknown"
        ;;
    merge-csv)
        source "${repo}/performance-measurement/lg-someip/scripts/common.sh"
        perf_merge_csv "$@"
        ;;
    *)
        sed -n '15,/^set -euo/p' "$0" | sed '$d'
        exit 1
        ;;
esac
