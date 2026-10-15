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

# Run the benchmarks on two hosts (for example two Raspberry Pis on a direct
# Ethernet link) from a third machine that reaches both over SSH. Host A is the
# provider and host B the consumer; all result rows are written on B, except
# the event publisher's rows, which are written on A.
#
#   run-two-hosts.sh -A <ssh host A> -B <ssh host B> -a <A test IP> -b <B test IP>
#                    [-d <repo dir on hosts>] [-o <results dir>] [-s "<scenarios>"]
#                    [-t udp|tcp] [-p "<payloads>"] [-r "<rates>"] [-N <count>]
#                    [-D <seconds per rate step>] [-S "<subscriber counts>"]
#                    [-k <SD iterations>] [-c <daemon CPUs>,<app CPUs>] [-L <log level>]
#                    [-y] [-I] [-q] [-n]
#
#   scenarios  local pingpong rate event sd (default: all). "local" runs
#              run-local-suite.sh on both hosts before the two-host tests.
#   -a, -b     addresses on the test link; SSH may use another network
#   -d         repository path on both hosts, relative to the SSH home
#              directory (default: lg-someip)
#   -p         default "64 1024 1400" for UDP (1400 B is the largest SOME/IP
#              payload in one datagram without SOME/IP-TP) and "4096 16384 65536"
#              for TCP
#   -c         default "1,2-3"
#   -L         log level of the two-host daemons and apps (default: warning);
#              "debug" helps to diagnose a failing phase but slows every run
#   -y         copy the source to both hosts and build it first. From a git
#              checkout: the files git tracks plus untracked files in
#              performance-measurement/. From an unpacked
#              make-bundle.sh archive: every file except build and result
#              directories.
#   -I         with -y: also install the packages (asks for the sudo password
#              of each host in this terminal)
#   -q         quick smoke test with small counts (results are not meaningful)
#   -n         dry run: print the host commands without running them
#
# One-way event latency and offer_to_available use CLOCK_REALTIME on both
# hosts; synchronize them with PTP first (see performance-measurement/README.md).

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "${script_dir}/../../.." && pwd)"

host_a=""
host_b=""
ip_a=""
ip_b=""
remote_dir="lg-someip"
results_dir="${repo}/performance-measurement/results/two-hosts-$(date +%Y%m%d-%H%M%S)"
scenarios="local pingpong rate event sd"
transport="udp"
payloads=""
rates="1000 5000 10000 20000 0"
count=10000
duration=5
subscriber_counts="1 4"
sd_iterations=10
cpus="1,2-3"
log_level="warning"
sync=0
install=0
quick=0
dry_run=0

while getopts "A:B:a:b:d:o:s:t:p:r:N:D:S:k:c:L:yIqnh" opt; do
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
        S) subscriber_counts="${OPTARG}" ;;
        k) sd_iterations="${OPTARG}" ;;
        c) cpus="${OPTARG}" ;;
        L) log_level="${OPTARG}" ;;
        y) sync=1 ;;
        I) install=1 ;;
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
    subscriber_counts="1 2"
    sd_iterations=2
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
local_suite_args=()
((quick)) && local_suite_args=(-q)

# agent <host> <remote-agent.sh arguments...>
agent() {
    local host="$1"; shift
    local command
    command="cd $(printf %q "${remote_dir}") && PERF_CPUS=$(printf %q "${cpus}") \
performance-measurement/lg-someip/scripts/remote-agent.sh $(printf '%q ' "$@")"
    if ((dry_run)); then
        echo "[${host}] remote-agent.sh $*"
        return 0
    fi
    ssh -o BatchMode=yes "${host}" "${command}"
}

pause() {
    # pause <milliseconds>
    ((dry_run)) || sleep "$(awk "BEGIN { print $1 / 1000 }")"
}

cleanup() {
    agent "${host_a}" stop-all >/dev/null 2>&1 || true
    agent "${host_b}" stop-all >/dev/null 2>&1 || true
}

collect() {
    ((dry_run)) && return 0
    mkdir -p "${results_dir}/A-provider" "${results_dir}/B-consumer"
    rsync -az "${host_a}:${remote_dir}/perf-run/" "${results_dir}/A-provider/" || true
    rsync -az "${host_b}:${remote_dir}/perf-run/" "${results_dir}/B-consumer/" || true
}

# capture_daemon_usage <before|after>: snapshot each daemon's cumulative CPU
# time and peak RSS. Called once right after the daemons start, and once more
# before they are next restarted (sd restarts them during its crash phases),
# so the "after" snapshot covers exactly the pingpong/rate/event scenarios.
capture_daemon_usage() {
    if ((dry_run)); then
        agent "${host_a}" daemon-usage
        agent "${host_b}" daemon-usage
        return 0
    fi
    agent "${host_a}" daemon-usage > "${results_dir}/A-daemon-usage-$1.txt"
    agent "${host_b}" daemon-usage > "${results_dir}/B-daemon-usage-$1.txt"
}

start_remote_stack() {
    agent "${host_a}" config -r provider -u "${ip_a}" -t "${transport}" -l "${log_level}"
    agent "${host_b}" config -r consumer -u "${ip_b}" -t "${transport}" -l "${log_level}"
    agent "${host_a}" daemon-start
    agent "${host_b}" daemon-start
    capture_daemon_usage before
    # Round-trip baseline of the link itself, without lg-someip.
    echo "=== network baseline: ping from B to A ==="
    if ((dry_run)); then
        agent "${host_b}" ping "${ip_a}"
    else
        agent "${host_b}" ping "${ip_a}" | tee "${results_dir}/ping-B-to-A.txt" | tail -n 2 | sed 's/^/  /'
    fi
}

# Print chosen columns of a key: value block that a benchmark printed.
show() {
    # Dry-run lines ("[host] ...") pass through unchanged.
    grep -E "^(  ($1) |\[)" | sed "/^\[/!s/^/  $2/" || true
}

scenario_pingpong() {
    agent "${host_a}" start rpc-server perf-rpc-server
    local payload
    for payload in ${payloads}; do
        echo "=== remote pingpong transport=${transport} payload=${payload} ==="
        agent "${host_b}" run perf-rpc-client --mode pingpong --payload "${payload}" --count "${count}" \
            --warmup $((count / 10)) --label "${label}" --csv perf-run/rpc-pingpong.csv \
            --samples-out "perf-run/rpc-pingpong-${label}-${payload}.samples" | show "lost|p50_us|p99_us|p999_us" "" || status=1
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
    agent "${host_a}" start rpc-server perf-rpc-server
    pause 1000
}

scenario_rate() {
    agent "${host_a}" start rpc-server perf-rpc-server
    local payload rate step_count
    for payload in ${payloads}; do
        for rate in ${rates}; do
            step_count=$((rate == 0 ? count * 5 : rate * duration))
            echo "=== remote rate transport=${transport} payload=${payload} rate=${rate} ==="
            agent "${host_b}" run perf-rpc-client --mode rate --rate "${rate}" --payload "${payload}" \
                --count "${step_count}" --warmup $((step_count / 10)) --label "${label}" \
                --csv perf-run/rpc-rate.csv --samples-out "perf-run/rpc-rate-${label}-${payload}-${rate}.samples" |
                show "throughput_msg_s|loss_pct|p50_us|p99_us|stalled" "" || stress_status "${rate}" "$?"
            if ((rate == 0)); then
                restart_rpc_server
            fi
        done
    done
    agent "${host_a}" stop rpc-server
}

scenario_event() {
    local subscribers payload rate step_count i
    for subscribers in ${subscriber_counts}; do
        for payload in ${payloads}; do
            for rate in ${rates}; do
                step_count=$((rate == 0 ? count * 5 : rate * duration))
                echo "=== remote event transport=${transport} subscribers=${subscribers} payload=${payload} rate=${rate} ==="
                for ((i = 1; i <= subscribers; i++)); do
                    agent "${host_b}" start "event-sub-${i}" perf-event-subscriber --name "perf-event-sub-${i}" \
                        --clock realtime --label "${label}-${subscribers}sub" --csv "perf-run/.event-subscribe-${i}.csv" \
                        --samples-out "perf-run/event-${label}-${subscribers}sub-${payload}-${rate}-sub${i}.samples"
                done
                pause 1000
                # Host B's daemon subscribes once for all its local subscribers,
                # so the publisher waits for one remote subscription and then
                # gives the other local subscribers time to attach.
                agent "${host_a}" run perf-event-publisher --subscribers 1 --settle-ms 1000 --clock realtime \
                    --rate "${rate}" --payload "${payload}" --count "${step_count}" --warmup $((step_count / 10)) \
                    --label "${label}-${subscribers}sub" --csv perf-run/event-publish.csv |
                    show "send_rate_msg_s|notify_p99_us" "pub " || status=1
                for ((i = 1; i <= subscribers; i++)); do
                    agent "${host_b}" wait "event-sub-${i}" 60 || status=1
                done
                local csvs=()
                for ((i = 1; i <= subscribers; i++)); do
                    csvs+=("perf-run/.event-subscribe-${i}.csv")
                done
                agent "${host_b}" merge-csv perf-run/event-subscribe.csv "${csvs[@]}"
            done
        done
    done
    ((dry_run)) || ssh -o BatchMode=yes "${host_b}" "tail -n +2 ${remote_dir}/perf-run/event-subscribe.csv | wc -l" |
        sed 's/^/  subscriber rows: /'
}

scenario_sd() {
    local i on_ms=2000
    local probe_args=(--wait-down --timeout-ms 30000 --label "${label}" --csv perf-run/sd-probe.csv)

    echo "=== remote sd phase=toggle (graceful StopOffer) ==="
    agent "${host_b}" start sd-probe perf-sd-probe --episodes "${sd_iterations}" --phase toggle "${probe_args[@]}"
    pause 1000
    agent "${host_a}" run perf-sd-provider --cycles "${sd_iterations}" --on-ms "${on_ms}" --off-ms 2000 >/dev/null
    agent "${host_b}" wait sd-probe 30 || status=1
    agent "${host_b}" summarize perf-run/sd-probe.csv phase toggle

    echo "=== remote sd phase=app-crash (provider app killed, daemon alive) ==="
    agent "${host_b}" start sd-probe perf-sd-probe --episodes "${sd_iterations}" --phase app-crash "${probe_args[@]}"
    pause 1000
    for ((i = 1; i <= sd_iterations; i++)); do
        agent "${host_a}" start sd-provider perf-sd-provider --cycles 0
        pause "${on_ms}"
        agent "${host_a}" stop sd-provider KILL
        pause 6000
    done
    agent "${host_b}" wait sd-probe 30 || status=1
    agent "${host_b}" summarize perf-run/sd-probe.csv phase app-crash

    echo "=== remote sd phase=node-crash (daemon and provider killed; detected by TTL) ==="
    agent "${host_b}" start sd-probe perf-sd-probe --episodes "${sd_iterations}" --phase node-crash "${probe_args[@]}"
    pause 1000
    for ((i = 1; i <= sd_iterations; i++)); do
        # Each start is also a provider cold start: offer_to_available includes
        # the SD initial wait and the first offer on the link.
        agent "${host_a}" daemon-start
        agent "${host_a}" start sd-provider perf-sd-provider --cycles 0
        pause "${on_ms}"
        # Kill the daemon first; a dying app alone would let it send StopOffer.
        agent "${host_a}" daemon-kill
        agent "${host_a}" stop sd-provider KILL
        pause 7000
    done
    agent "${host_b}" wait sd-probe 30 || status=1
    agent "${host_a}" daemon-start
    agent "${host_b}" summarize perf-run/sd-probe.csv phase node-crash

    echo "=== remote sd phase=consumer-cold (consumer daemon and probe restarted) ==="
    agent "${host_a}" start sd-provider perf-sd-provider --cycles 0
    pause 3000
    for ((i = 1; i <= sd_iterations; i++)); do
        agent "${host_b}" daemon-start
        agent "${host_b}" run perf-sd-probe --episodes 1 --phase consumer-cold --timeout-ms 30000 --label "${label}" \
            --csv perf-run/sd-probe.csv >/dev/null || status=1
    done
    agent "${host_a}" stop sd-provider
    agent "${host_b}" summarize perf-run/sd-probe.csv phase consumer-cold
}

# Every host command runs with ssh -o BatchMode=yes, so key-based login must
# work before anything else.
if ((!dry_run)); then
    for host in "${host_a}" "${host_b}"; do
        if ! error="$(ssh -o BatchMode=yes -o ConnectTimeout=10 "${host}" true 2>&1)"; then
            echo "run-two-hosts.sh: cannot log in to '${host}' without a password:" >&2
            echo "  ${error}" >&2
            echo "  - use an address or an SSH config entry (Host/HostName/User) that resolves, e.g. -A pi@192.168.1.103" >&2
            echo "  - install your key once: ssh-copy-id ${host}" >&2
            echo "  - test with: ssh -o BatchMode=yes ${host} true" >&2
            exit 1
        fi
    done
fi

# A git checkout wins over a REVISION file, as in CMake: the file is only
# meaningful in an unpacked make-bundle.sh archive and goes stale in a clone.
revision="$(git -C "${repo}" describe --always --dirty 2>/dev/null || true)"
if [[ -z "${revision}" && -f "${repo}/REVISION" ]]; then
    revision="$(head -1 "${repo}/REVISION")"
fi
revision="${revision:-unknown}"
if ((sync)); then
    file_list="$(mktemp)"
    if git -C "${repo}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        {
            git -C "${repo}" ls-files --cached
            git -C "${repo}" ls-files --others --exclude-standard performance-measurement
        } | sort -u > "${file_list}"
    else
        (cd "${repo}" && find . -type f ! -path './build*' ! -path './performance-measurement/results/*' ! -path './performance-measurement/*/build*' ! -path './perf-run/*' |
            sed 's,^\./,,' | sort) > "${file_list}"
    fi
    for host in "${host_a}" "${host_b}"; do
        echo "##### sync and build on ${host} #####"
        if ((dry_run)); then
            echo "[${host}] rsync ${repo}/ -> ${remote_dir}/ ; pi-setup.sh $( ((install)) && echo "(installs packages)" || echo -n)"
            continue
        fi
        rsync -az --files-from="${file_list}" "${repo}/" "${host}:${remote_dir}/"
        if ((install)); then
            ssh -t "${host}" "cd $(printf %q "${remote_dir}") && \
performance-measurement/lg-someip/scripts/pi-setup.sh -r $(printf %q "${revision}")" || exit 1
            continue
        fi
        if ! ssh -o BatchMode=yes "${host}" \
            "cd $(printf %q "${remote_dir}") && performance-measurement/lg-someip/scripts/pi-setup.sh -n -r $(printf %q "${revision}")" \
            > "${TMPDIR:-/tmp}/pi-setup-${host}.log" 2>&1; then
            tail -n 5 "${TMPDIR:-/tmp}/pi-setup-${host}.log" >&2
            echo "build failed on ${host}. On a new board, install the packages once (sudo asks for a password):" >&2
            echo "  ssh -t ${host} ${remote_dir}/performance-measurement/lg-someip/scripts/pi-setup.sh" >&2
            exit 1
        fi
        tail -n 4 "${TMPDIR:-/tmp}/pi-setup-${host}.log"
    done
    rm -f "${file_list}"
fi

trap cleanup EXIT
cleanup
agent "${host_a}" clean
agent "${host_b}" clean
if ((!dry_run)); then
    mkdir -p "${results_dir}"
    {
        echo "revision=${revision}"
        echo "command=$0 $*"
        echo "host_a=${host_a} ip_a=${ip_a} host_b=${host_b} ip_b=${ip_b} transport=${transport}"
    } > "${results_dir}/run-info.txt"
    agent "${host_a}" sysinfo "${ip_a}" > "${results_dir}/A-sysinfo-before.txt"
    agent "${host_b}" sysinfo "${ip_b}" > "${results_dir}/B-sysinfo-before.txt"
    for side in A B; do
        info="${results_dir}/${side}-sysinfo-before.txt"
        grep -E "^(model|kernel|governor|temp_c|throttled|link_speed_mbps|link_rx_usecs|ptp4l_running)=" "${info}" |
            tr '\n' ' ' | sed "s/^/${side}: /"
        echo
        grep -q "^governor=performance" "${info}" ||
            echo "warning: ${side} CPU governor is not 'performance'; latency depends on the current frequency" >&2
        grep -q "^ptp4l_running=yes" "${info}" ||
            echo "warning: ptp4l is not running on ${side}; one-way event latency includes the clock offset" >&2
    done
    if [[ "$(sed -n 's/^kernel=//p' "${results_dir}/A-sysinfo-before.txt")" != \
          "$(sed -n 's/^kernel=//p' "${results_dir}/B-sysinfo-before.txt")" ]]; then
        echo "warning: A and B run different kernels; same-host results of the two are not directly comparable" >&2
    fi
    agent "${host_a}" build-size > "${results_dir}/A-build-size.txt"
    agent "${host_b}" build-size > "${results_dir}/B-build-size.txt"
fi

status=0
remote_started=0
daemon_usage_captured=0
for scenario in ${scenarios}; do
    echo "##### ${scenario} #####"
    case "${scenario}" in
        local)
            # The local suite starts its own daemon on each host, so it runs
            # before the two-host daemons are started.
            if ((remote_started)); then
                echo "run 'local' first in -s; skipping it" >&2
                continue
            fi
            if ((dry_run)); then
                agent "${host_a}" local-suite ${local_suite_args[@]+"${local_suite_args[@]}"}
                agent "${host_b}" local-suite ${local_suite_args[@]+"${local_suite_args[@]}"}
                continue
            fi
            agent "${host_a}" local-suite ${local_suite_args[@]+"${local_suite_args[@]}"} > "${results_dir}/A-local.log" 2>&1 &
            local_a=$!
            agent "${host_b}" local-suite ${local_suite_args[@]+"${local_suite_args[@]}"} > "${results_dir}/B-local.log" 2>&1 &
            local_b=$!
            wait "${local_a}" || { status=1; echo "local suite failed on A; see A-local.log" >&2; }
            wait "${local_b}" || { status=1; echo "local suite failed on B; see B-local.log" >&2; }
            ;;
        pingpong | rate | event | sd)
            if ((!remote_started)); then
                start_remote_stack
                remote_started=1
            fi
            # sd restarts both daemons during its crash phases, which would
            # reset their cumulative CPU time; snapshot "after" just before
            # that so it covers exactly pingpong/rate/event.
            if [[ "${scenario}" == sd && "${daemon_usage_captured}" -eq 0 ]]; then
                capture_daemon_usage after
                daemon_usage_captured=1
            fi
            "scenario_${scenario}"
            ;;
        *)
            echo "unknown scenario '${scenario}'" >&2
            status=1
            ;;
    esac
done

if ((remote_started && !daemon_usage_captured)); then
    capture_daemon_usage after
fi
cleanup
if ((!dry_run)); then
    agent "${host_a}" sysinfo "${ip_a}" > "${results_dir}/A-sysinfo-after.txt"
    agent "${host_b}" sysinfo "${ip_b}" > "${results_dir}/B-sysinfo-after.txt"
fi
collect
((dry_run)) || echo "results: ${results_dir}"
exit "${status}"
