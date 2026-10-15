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

# Host-side helper that run-two-hosts.sh drives over SSH for the vsomeip
# benchmarks. Every call is short: background processes are started with nohup
# and tracked by PID files, as in the lg-someip and opensomeip agents.
#
#   remote-agent.sh config <gen-config.sh options>    write the configuration
#   remote-agent.sh route-check                       check the multicast route
#   remote-agent.sh daemon-start | daemon-kill        (re)start or kill -9 the routing daemon
#   remote-agent.sh start <name> <app> [args...]      start an app in the background
#   remote-agent.sh stop <name> [signal]              signal it and wait (default INT)
#   remote-agent.sh wait <name> <seconds>             wait for it to exit by itself
#   remote-agent.sh run <app> [args...]               run an app in the foreground
#   remote-agent.sh stop-all | clean | sysinfo [<ip>] | ping <ip>
#   remote-agent.sh daemon-usage | build-size | versions   key=value lines
#   remote-agent.sh merge-csv <output> <inputs...>
#   remote-agent.sh summarize <csv> <key column> <value>
#
# Environment: PERF_CPUS="<daemon CPUs>,<app CPUs>", PERF_BUILD_DIR,
# PERF_RUN_DIR, and PERF_VSOMEIP_BACKEND (see below).

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "${script_dir}/../../.." && pwd)"
# PERF_VSOMEIP_BACKEND selects what the same vsomeip-API applications run on:
#   vsomeip  (default) upstream COVESA vsomeip and routingmanagerd
#   lgsomeip lg-someip's vsomeip API layer (build-lgsomeip, linked against an
#            lg-someip install in PERF_LG_INSTALL_DIR) and someip-daemon from
#            PERF_LG_BUILD_DIR, with an lg-someip configuration
backend="${PERF_VSOMEIP_BACKEND:-vsomeip}"
case "${backend}" in
    vsomeip)
        build_dir="${PERF_BUILD_DIR:-${repo}/performance-measurement/vsomeip/build}"
        run_dir="${PERF_RUN_DIR:-${repo}/perf-run-vsomeip}"
        config="${run_dir}/vs-perf.json"
        daemon="${HOME}/vsomeip-src/build/examples/routingmanagerd/routingmanagerd"
        vsomeip_lib="${HOME}/vsomeip-install/lib"
        ;;
    lgsomeip)
        lg_build_dir="${PERF_LG_BUILD_DIR:-${repo}/build-pi}"
        build_dir="${PERF_BUILD_DIR:-${repo}/performance-measurement/vsomeip/build-lgsomeip}"
        run_dir="${PERF_RUN_DIR:-${repo}/perf-run-vsomeip-lg}"
        config="${run_dir}/perf.json"
        daemon="${lg_build_dir}/daemon/someip-daemon"
        vsomeip_lib="${PERF_LG_INSTALL_DIR:-${HOME}/lgsomeip-install}/lib"
        ;;
    *)
        echo "vsomeip remote-agent: unknown PERF_VSOMEIP_BACKEND '${backend}'" >&2
        exit 1
        ;;
esac
bin_dir="${build_dir}"
IFS=, read -r cpu_daemon cpu_app <<< "${PERF_CPUS:-}"

mkdir -p "${run_dir}"
export LD_LIBRARY_PATH="${vsomeip_lib}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

prefix_for() {
    [[ -n "$1" ]] && echo "taskset -c $1"
    return 0
}

pid_of() {
    [[ -f "${run_dir}/$1.pid" ]] && cat "${run_dir}/$1.pid"
    return 0
}

start_bg() {
    local name="$1" cpus="$2" application_name="$3"
    shift 3
    local prefix
    read -r -a prefix <<< "$(prefix_for "${cpus}")"
    echo "===== $(date -u +%Y-%m-%dT%H:%M:%S.%NZ) start: $*" >> "${run_dir}/${name}.log"
    nohup env "LD_LIBRARY_PATH=${LD_LIBRARY_PATH}" "VSOMEIP_CONFIGURATION=${config}" \
        "VSOMEIP_APPLICATION_NAME=${application_name}" "${prefix[@]}" "$@" \
        >> "${run_dir}/${name}.log" 2>&1 < /dev/null &
    echo $! > "${run_dir}/${name}.pid"
}

stop_one() {
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
    [[ -x "${bin_dir}/$1" ]] || {
        echo "vsomeip remote-agent: ${bin_dir}/$1 not built" >&2
        exit 1
    }
}

application_name_for() {
    local app="$1"
    shift
    local name previous=""
    case "${app}" in
        perf-event-publisher) name="perf-event-pub" ;;
        perf-event-subscriber) name="perf-event-sub-1" ;;
        *) name="${app}" ;;
    esac
    for argument in "$@"; do
        if [[ "${previous}" == "--name" ]]; then
            name="${argument}"
        fi
        previous="${argument}"
    done
    printf '%s\n' "${name}"
}

run_app() {
    local app="$1" application_name="$2"
    shift 2
    require_app "${app}"
    local prefix
    read -r -a prefix <<< "$(prefix_for "${cpu_app}")"
    env "LD_LIBRARY_PATH=${LD_LIBRARY_PATH}" "VSOMEIP_CONFIGURATION=${config}" \
        "VSOMEIP_APPLICATION_NAME=${application_name}" "${prefix[@]}" "${bin_dir}/${app}" "$@"
}

daemon_usage() {
    source "${repo}/performance-measurement/lg-someip/scripts/common.sh"
    perf_daemon_usage "$(pid_of daemon)"
}

command="${1:-}"
shift || true
case "${command}" in
    config)
        if [[ "${backend}" == lgsomeip ]]; then
            "${repo}/performance-measurement/lg-someip/scripts/gen-config.sh" -o "${config}" "$@"
        else
            "${script_dir}/gen-config.sh" -o "${config}" "$@"
        fi
        ;;
    route-check)
        ip route show | grep -q '^224\.0\.0\.0/4 ' || {
            echo "vsomeip remote-agent: missing 224.0.0.0/4 multicast route on $(hostname)" >&2
            exit 1
        }
        ip route show | grep '^224\.0\.0\.0/4 '
        ;;
    daemon-start)
        stop_one daemon INT
        if [[ "${backend}" == lgsomeip ]]; then
            [[ -x "${daemon}" ]] || { echo "vsomeip remote-agent: missing ${daemon}" >&2; exit 1; }
            # Same start-up as performance-measurement/lg-someip/scripts/remote-agent.sh.
            rm -rf /tmp/someip/
            start_bg daemon "${cpu_daemon}" someip-daemon "${daemon}" "${config}"
            for ((i = 0; i < 300; i++)); do
                [[ -S /tmp/someip/someip-0000 ]] && break
                sleep 0.01
            done
            sleep 0.05
            kill -0 "$(pid_of daemon)" 2>/dev/null && [[ -S /tmp/someip/someip-0000 ]] || {
                echo "vsomeip remote-agent: someip-daemon did not start" >&2
                tail "${run_dir}/daemon.log" >&2
                exit 1
            }
            exit 0
        fi
        # Stale sockets and lock files of a killed routing manager block start-up.
        rm -f /tmp/vsomeip-* /tmp/vsomeip.lck
        [[ -x "${daemon}" ]] || { echo "vsomeip remote-agent: missing ${daemon}" >&2; exit 1; }
        start_bg daemon "${cpu_daemon}" routingmanagerd "${daemon}"
        for ((i = 0; i < 300; i++)); do
            kill -0 "$(pid_of daemon)" 2>/dev/null || break
            sleep 0.01
        done
        kill -0 "$(pid_of daemon)" 2>/dev/null || {
            echo "vsomeip remote-agent: routingmanagerd did not start" >&2
            tail "${run_dir}/daemon.log" >&2
            exit 1
        }
        ;;
    daemon-kill)
        stop_one daemon KILL
        ;;
    start)
        name="$1"
        app="$2"
        shift 2
        application_name="$(application_name_for "${app}" "$@")"
        start_bg "${name}" "${cpu_app}" "${application_name}" "${bin_dir}/${app}" --config "${config}" "$@"
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
            echo "vsomeip remote-agent: $1 still running after $2 s; stopping it" >&2
            stop_one "$1" INT
            exit 1
        fi
        rm -f "${run_dir}/$1.pid"
        ;;
    run)
        app="$1"
        shift
        application_name="$(application_name_for "${app}" "$@")"
        run_app "${app}" "${application_name}" --config "${config}" "$@"
        ;;
    stop-all)
        for pidfile in "${run_dir}"/*.pid; do
            [[ -f "${pidfile}" ]] || continue
            name="$(basename "${pidfile}" .pid)"
            [[ "${name}" == daemon ]] || stop_one "${name}" INT
        done
        stop_one daemon INT
        ;;
    clean)
        rm -rf "${run_dir}"
        mkdir -p "${run_dir}"
        ;;
    sysinfo)
        "${repo}/performance-measurement/lg-someip/scripts/sysinfo.sh" "$@"
        ;;
    ping)
        ping -q -c 500 -i 0.01 "$1" 2>/dev/null || ping -q -c 50 -i 0.2 "$1"
        ;;
    daemon-usage)
        daemon_usage
        ;;
    versions)
        # Best effort: the source checkout and package versions the build used.
        echo "backend=${backend}"
        if [[ "${backend}" == lgsomeip ]]; then
            echo "lgsomeip_revision=$(sed -n 's/^PERF_GIT_REVISION[^=]*=//p' "${lg_build_dir}/CMakeCache.txt" 2>/dev/null || echo unknown)"
            exit 0
        fi
        echo "vsomeip_version=$(git -C "${HOME}/vsomeip-src" describe --tags --always 2>/dev/null || echo unknown)"
        echo "boost_version=$(dpkg-query -W -f='${Version}' libboost-dev 2>/dev/null || echo unknown)"
        ;;
    build-size)
        for f in "${daemon}" "${bin_dir}"/perf-*; do
            [[ -f "${f}" ]] && printf '%s_bytes=%s\n' "$(basename "${f}")" "$(stat -c%s "${f}")"
        done
        ;;
    merge-csv)
        source "${repo}/performance-measurement/lg-someip/scripts/common.sh"
        perf_merge_csv "$@"
        ;;
    summarize)
        source "${repo}/performance-measurement/lg-someip/scripts/common.sh"
        perf_summarize_ms "$@"
        ;;
    *)
        sed -n '15,/^set -euo/p' "$0" | sed '$d'
        exit 1
        ;;
esac
