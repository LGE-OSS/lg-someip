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

# Run the opensomeip RPC and event benchmarks on two hosts from a third machine
# that reaches both over SSH. Host A is the provider, host B the consumer. It
# mirrors the lg-someip and vsomeip coordinators (same options, same payloads,
# rates, counts, CPU pinning, and CSV columns), so the results can be merged
# with performance-measurement/scripts/merge-results.py.
#
#   run-two-hosts.sh -A <ssh host A> -B <ssh host B> -a <A test IP> -b <B test IP>
#                    [-d <repo dir on hosts>] [-o <results dir>] [-s "<scenarios>"]
#                    [-t udp|tcp] [-p "<payloads>"] [-r "<rates>"] [-N <count>]
#                    [-D <seconds per rate step>] [-c <daemon CPUs>,<app CPUs>]
#                    [-y] [-q] [-n]
#
#   scenarios  pingpong rate event (default: all). opensomeip has no Service
#              Discovery integration in the benchmark, so there is no sd scenario.
#   -c         only the second field (app CPUs) is used: there is no daemon
#   -t         tcp applies to RPC only; the direct-event harness is UDP
#   -y         copy the opensomeip applications to both hosts and build them first
#   -q         quick smoke test with small counts (results are not meaningful)
#   -n         dry run: print the host commands without running them
#
# opensomeip has no routing daemon and no Service Discovery in this harness:
# the provider address and ports are passed explicitly, so the numbers exclude
# daemon IPC and discovery by design (see performance-measurement/docs/stack-comparison.md).

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "${script_dir}/../../.." && pwd)"

host_a=""
host_b=""
ip_a=""
ip_b=""
remote_dir="lg-someip"
remote_run_dir="perf-run-opensomeip"
results_dir="${repo}/performance-measurement/results/opensomeip-two-hosts-$(date +%Y%m%d-%H%M%S)"
scenarios="pingpong rate event"
transport="udp"
payloads=""
rates="1000 5000 10000 20000 0"
count=10000
duration=5
cpus="1,2-3"
rpc_port=30501
event_port=30611
sync=0
quick=0
dry_run=0
status=0
remote_started=0

while getopts "A:B:a:b:d:o:s:t:p:r:N:D:c:yqnh" opt; do
    case "${opt}" in
        A) host_a="${OPTARG}" ;;
        B) host_b="${OPTARG}" ;;
        a) ip_a="${OPTARG}" ;;
        b) ip_b="${OPTARG}" ;;
        d) remote_dir="${OPTARG}" ;;
        o) results_dir="${OPTARG}" ;;
        s) scenarios="${OPTARG}" ;;
        t) transport="${OPTARG}" ;;
        p) payloads="${OPTARG}" ;;
        r) rates="${OPTARG}" ;;
        N) count="${OPTARG}" ;;
        D) duration="${OPTARG}" ;;
        c) cpus="${OPTARG}" ;;
        y) sync=1 ;;
        q) quick=1 ;;
        n) dry_run=1 ;;
        *) sed -n '15,/^set -euo/p' "$0" | sed '$d'; exit 1 ;;
    esac
done

if [[ -z "${host_a}" || -z "${host_b}" || -z "${ip_a}" || -z "${ip_b}" ]]; then
    echo "run-two-hosts.sh: -A, -B, -a and -b are required" >&2
    sed -n '15,/^set -euo/p' "$0" | sed '$d'
    exit 1
fi

if ((quick)); then
    payloads="${payloads:-64 1024}"
    rates="1000 0"
    count=500
    duration=1
    results_dir="${results_dir}-quick"
fi
if [[ -z "${payloads}" ]]; then
    if [[ "${transport}" == udp ]]; then
        payloads="64 1024 1400"
    else
        payloads="4096 16384 65536"
    fi
fi
label="remote-${transport}"

agent() {
    local host="$1"
    shift
    local command
    command="cd $(printf %q "${remote_dir}") && PERF_CPUS=$(printf %q "${cpus}") \
PERF_RUN_DIR=$(printf %q "${remote_run_dir}") \
performance-measurement/opensomeip/scripts/remote-agent.sh $(printf '%q ' "$@")"
    if ((dry_run)); then
        echo "[${host}] ${command}"
        return 0
    fi
    ssh -o BatchMode=yes "${host}" "${command}"
}

pause() {
    if ((dry_run)); then
        echo "[pause] $1 ms"
    else
        sleep "$(awk "BEGIN { print $1 / 1000 }")"
    fi
}

cleanup() {
    agent "${host_a}" stop-all >/dev/null 2>&1 || true
    agent "${host_b}" stop-all >/dev/null 2>&1 || true
}

collect() {
    ((dry_run)) && return 0
    mkdir -p "${results_dir}/A-provider" "${results_dir}/B-consumer"
    rsync -az "${host_a}:${remote_dir}/${remote_run_dir}/" "${results_dir}/A-provider/" || true
    rsync -az "${host_b}:${remote_dir}/${remote_run_dir}/" "${results_dir}/B-consumer/" || true
}

# Print chosen columns of a key: value block that a benchmark printed.
show() {
    grep -E "^(  ($1) |\[)" | sed "/^\[/!s/^/  $2/" || true
}

sync_and_build() {
    ((sync)) || return 0
    local host
    for host in "${host_a}" "${host_b}"; do
        echo "##### sync and build on ${host} #####"
        if ((dry_run)); then
            echo "[${host}] rsync performance-measurement/opensomeip/ and the shared lg-someip common/ and scripts/ -> ${remote_dir}/"
            echo "[${host}] cmake -S performance-measurement/opensomeip -B performance-measurement/opensomeip/build"
            continue
        fi
        (cd "${repo}" && rsync -az --relative --exclude='build/' performance-measurement/opensomeip/ \
            performance-measurement/lg-someip/common/ performance-measurement/lg-someip/scripts/ \
            "${host}:${remote_dir}/")
        ssh -o BatchMode=yes "${host}" "cd $(printf %q "${remote_dir}/performance-measurement/opensomeip") && \
cmake -S . -B build -DCMAKE_PREFIX_PATH=\$HOME/opensomeip-install -DCMAKE_BUILD_TYPE=Release && \
cmake --build build -j\$(nproc)"
    done
}

start_remote_stack() {
    echo "=== network baseline: ping from B to A ==="
    if ((dry_run)); then
        agent "${host_b}" ping "${ip_a}"
    else
        agent "${host_b}" ping "${ip_a}" | tee "${results_dir}/ping-B-to-A.txt" | tail -n 2 | sed 's/^/  /'
    fi
}

scenario_pingpong() {
    agent "${host_a}" start rpc-server perf-rpc-server --bind 0.0.0.0 --port "${rpc_port}" --transport "${transport}"
    pause 1000
    local payload
    for payload in ${payloads}; do
        echo "=== remote opensomeip pingpong transport=${transport} payload=${payload} ==="
        agent "${host_b}" run perf-rpc-client --server "${ip_a}" --port "${rpc_port}" --transport "${transport}" \
            --mode pingpong --payload "${payload}" --count "${count}" --warmup $((count / 10)) --label "${label}" \
            --csv "${remote_run_dir}/rpc-pingpong.csv" \
            --samples-out "${remote_run_dir}/rpc-pingpong-${label}-${payload}.samples" |
            show "lost|p50_us|p99_us|p999_us" "" || status=1
    done
    agent "${host_a}" stop rpc-server
}

# Lost requests make perf-rpc-client exit with 3. At an unthrottled rate (rate 0)
# that is a measured result (the stack's saturation behavior), not a failure of
# the run; anything else, or any other exit code, is.
stress_status() {
    local rate="$1" rc="$2"
    if ((rate == 0 && rc == 3)); then
        echo "  (requests lost at maximum rate: recorded as a result)"
    else
        status=1
    fi
}

# After a maximum-rate step the server and the daemons still hold queued requests
# and responses of that step. The next step must not meet them: it would lose or
# delay its first messages and report the stress step's backlog as its own
# result. Restart the server and let the queues drain before going on.
restart_rpc_server() {
    agent "${host_a}" stop rpc-server
    pause 2000
    agent "${host_a}" start rpc-server perf-rpc-server --bind 0.0.0.0 --port "${rpc_port}" --transport "${transport}"
    pause 1000
}

scenario_rate() {
    agent "${host_a}" start rpc-server perf-rpc-server --bind 0.0.0.0 --port "${rpc_port}" --transport "${transport}"
    pause 1000
    local payload rate step_count
    for payload in ${payloads}; do
        for rate in ${rates}; do
            step_count=$((rate == 0 ? count * 5 : rate * duration))
            echo "=== remote opensomeip rate transport=${transport} payload=${payload} rate=${rate} ==="
            agent "${host_b}" run perf-rpc-client --server "${ip_a}" --port "${rpc_port}" --transport "${transport}" \
                --mode rate --rate "${rate}" --payload "${payload}" --count "${step_count}" \
                --warmup $((step_count / 10)) --label "${label}" --csv "${remote_run_dir}/rpc-rate.csv" \
                --samples-out "${remote_run_dir}/rpc-rate-${label}-${payload}-${rate}.samples" |
                show "throughput_msg_s|loss_pct|p50_us|p99_us|stalled" "" || stress_status "${rate}" "$?"
            if ((rate == 0)); then
                restart_rpc_server
            fi
        done
    done
    agent "${host_a}" stop rpc-server
}

# The harness delivers notifications to one fixed UDP endpoint: the subscriber
# listens on B, the publisher sends to it from A. There is no subscription
# handshake, so the subscriber must be listening first.
scenario_event() {
    if [[ "${transport}" != udp ]]; then
        echo "event scenario: the opensomeip direct-event harness is UDP only; skipping" >&2
        return 0
    fi
    local payload rate step_count
    for payload in ${payloads}; do
        for rate in ${rates}; do
            step_count=$((rate == 0 ? count * 5 : rate * duration))
            echo "=== remote opensomeip event payload=${payload} rate=${rate} ==="
            agent "${host_b}" start event-sub perf-event-subscriber --bind 0.0.0.0 --port "${event_port}" \
                --clock realtime --label "${label}-1sub" --csv "${remote_run_dir}/event-subscribe.csv" \
                --samples-out "${remote_run_dir}/event-${label}-1sub-${payload}-${rate}-sub1.samples"
            pause 1000
            agent "${host_a}" run perf-event-publisher --subscriber "${ip_b}" --port "${event_port}" \
                --settle-ms 1000 --clock realtime --rate "${rate}" --payload "${payload}" --count "${step_count}" \
                --warmup $((step_count / 10)) --label "${label}-1sub" --csv "${remote_run_dir}/event-publish.csv" |
                show "send_rate_msg_s|publish_p99_us" "pub " || status=1
            agent "${host_b}" wait event-sub 60 || status=1
        done
    done
}

sync_and_build
trap cleanup EXIT
cleanup
agent "${host_a}" clean
agent "${host_b}" clean

if ((!dry_run)); then
    mkdir -p "${results_dir}"
    revision="$(git -C "${repo}" describe --always --dirty 2>/dev/null || echo unknown)"
    {
        echo "revision=${revision}"
        echo "command=$0 $*"
        echo "host_a=${host_a} ip_a=${ip_a} host_b=${host_b} ip_b=${ip_b} transport=${transport}"
        echo "stack=opensomeip architecture=direct-endpoint-no-daemon-no-sd"
    } > "${results_dir}/run-info.txt"
    agent "${host_a}" versions > "${results_dir}/A-versions.txt"
    agent "${host_b}" versions > "${results_dir}/B-versions.txt"
    agent "${host_a}" sysinfo "${ip_a}" > "${results_dir}/A-sysinfo-before.txt"
    agent "${host_b}" sysinfo "${ip_b}" > "${results_dir}/B-sysinfo-before.txt"
    agent "${host_a}" build-size > "${results_dir}/A-build-size.txt"
    agent "${host_b}" build-size > "${results_dir}/B-build-size.txt"
fi

for scenario in ${scenarios}; do
    echo "##### ${scenario} #####"
    case "${scenario}" in
        pingpong | rate | event)
            if ((!remote_started)); then
                start_remote_stack
                remote_started=1
            fi
            "scenario_${scenario}"
            ;;
        *)
            echo "unknown scenario '${scenario}'" >&2
            status=1
            ;;
    esac
done

cleanup
if ((!dry_run)); then
    agent "${host_a}" sysinfo "${ip_a}" > "${results_dir}/A-sysinfo-after.txt"
    agent "${host_b}" sysinfo "${ip_b}" > "${results_dir}/B-sysinfo-after.txt"
fi
collect
((dry_run)) || echo "results: ${results_dir}"
exit "${status}"
