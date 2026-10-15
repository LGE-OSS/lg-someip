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

# Peer-count sweep of one stack on two hosts: RPC with N requesters x M
# responders, events with M publishers x N subscribers, and RPC from one
# process with many threads. Responders and publishers run on host A,
# requesters and subscribers on host B.
#
#   run-peers.sh -S lgsomeip|vsomeip|opensomeip -A <ssh host A> -B <ssh host B>
#                -a <A test IP> -b <B test IP> [-o <results dir>] [-d <repo dir on hosts>]
#                [-s "rpc event threads"] [-m "pingpong rate"] [-R "<requester counts>"]
#                [-M "<responder counts>"] [-P "<publisher counts>"] [-U "<subscriber counts>"]
#                [-T "<thread counts>"] [-H "<requests/s per thread>"]
#                [-p "<payloads>"] [-r <rate per peer pair>] [-N <pingpong count>]
#                [-D <seconds per rate step>] [-c <daemon CPUs>,<app CPUs>] [-t udp|tcp]
#                [-w] [-y] [-q] [-n]
#
#   -R, -M  requester and responder counts (default "1 2 4 8" and "1 2 4", at
#           most 8). Every requester calls every responder: responders take
#           turns, so each requester offers -r x M requests per second in rate
#           mode, and each responder receives -r x N.
#   -P, -U  publisher and subscriber counts (default "1 2 4" and "1 2 4 8", at
#           most 8). Every subscriber subscribes to every publisher; each
#           publisher sends -r notifications per second.
#   -T, -H  scenario "threads": one requester process with -T worker threads
#           (default "1 8 32 128") calling -M responders (default "1 2 4").
#           Each worker sends a request, waits for the response, and repeats;
#           the response payload has the size of the request. -H is the request
#           rate of one worker (default "0 50"): 0 sends the next request as soon
#           as the response arrived (saturation), 50 is a fixed load, so the CPU
#           of different stacks can be compared at the same amount of work.
#           Every step runs 1 s of warm-up and -D seconds (default 3) measured.
#           Client CPU comes from the process itself; the CPU of the daemons and
#           responders on both hosts is read from /proc around the step, minus
#           their idle CPU measured just before (threads-host-cpu.csv). The
#           threads and the responders share the app CPUs of their host (-c).
#   -m      RPC modes: pingpong (closed loop, -N requests per requester) and
#           rate (open loop at -r per responder for -D seconds)
#   -r      default 1000 msg/s; -D default 3 s; -N default 2000; -p default 64
#   -w      also count the network traffic between the hosts for every step
#           (nftables counters, needs passwordless sudo on both hosts): SOME/IP
#           data and SD (UDP port 30490) in each direction, PTP excluded. Bytes
#           are IP packet lengths (IP and UDP headers included, Ethernet framing
#           not). Results: <results dir>/net-usage.csv, one row per step and host.
#   -y      copy the sources to the hosts and build them first
#   -q      quick smoke test: "1 2" peers, 1 thread rate, short steps; counts and
#           rates given with -R, -M, -P, -U, -T, or -H are kept
#   -n      dry run: print the host commands without running them
#
# All peers of one step start their measured phase together: each gets the
# same --start-at time, taken from the clock of the host they run on (A and B
# must be PTP-synchronized for the event step, whose publishers run on A).
# Every peer writes its own row and samples file; merge-results.py pools them.
# On a 4-core board all requesters or subscribers share the app CPUs (-c), so
# large peer counts measure CPU contention as well as the stack.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "${script_dir}/../.." && pwd)"

stack=""
host_a=""
host_b=""
ip_a=""
ip_b=""
remote_dir="lg-someip"
results_dir=""
scenarios="rpc event"
modes="pingpong rate"
requester_counts="1 2 4 8"
responder_counts="1 2 4"
publisher_counts="1 2 4"
subscriber_counts="1 2 4 8"
thread_counts="1 8 32 128"
thread_rates="0 50"
payloads="64"
rate=1000
count=2000
duration=3
cpus="1,2-3"
transport="udp"
sync=0
quick=0
given=""
dry_run=0
net_usage=0
status=0

while getopts "S:A:B:a:b:o:d:s:m:R:M:P:U:T:H:p:r:N:D:c:t:wyqnh" opt; do
    case "${opt}" in
        S) stack="${OPTARG}" ;;
        A) host_a="${OPTARG}" ;;
        B) host_b="${OPTARG}" ;;
        a) ip_a="${OPTARG}" ;;
        b) ip_b="${OPTARG}" ;;
        o) results_dir="${OPTARG}" ;;
        d) remote_dir="${OPTARG}" ;;
        s) scenarios="${OPTARG}" ;;
        m) modes="${OPTARG}" ;;
        R) requester_counts="${OPTARG}"; given+="R" ;;
        M) responder_counts="${OPTARG}"; given+="M" ;;
        P) publisher_counts="${OPTARG}"; given+="P" ;;
        U) subscriber_counts="${OPTARG}"; given+="U" ;;
        T) thread_counts="${OPTARG}"; given+="T" ;;
        H) thread_rates="${OPTARG}"; given+="H" ;;
        p) payloads="${OPTARG}" ;;
        r) rate="${OPTARG}" ;;
        N) count="${OPTARG}" ;;
        D) duration="${OPTARG}" ;;
        c) cpus="${OPTARG}" ;;
        t) transport="${OPTARG}" ;;
        w) net_usage=1 ;;
        y) sync=1 ;;
        q) quick=1 ;;
        n) dry_run=1 ;;
        *) sed -n '15,/^set -euo/p' "$0" | sed '$d'; exit 1 ;;
    esac
done
if [[ -z "${stack}" || -z "${host_a}" || -z "${host_b}" || -z "${ip_a}" || -z "${ip_b}" ]]; then
    echo "run-peers.sh: -S, -A, -B, -a and -b are required" >&2
    sed -n '15,/^set -euo/p' "$0" | sed '$d'
    exit 1
fi
if ((quick)); then
    # Quick defaults; peer counts and rates given on the command line are kept.
    [[ "${given}" == *R* ]] || requester_counts="1 2"
    [[ "${given}" == *M* ]] || responder_counts="1 2"
    [[ "${given}" == *P* ]] || publisher_counts="1 2"
    [[ "${given}" == *U* ]] || subscriber_counts="1 2"
    [[ "${given}" == *T* ]] || thread_counts="1 8"
    [[ "${given}" == *H* ]] || thread_rates="0"
    count=300
    duration=1
fi
results_dir="${results_dir:-${repo}/performance-measurement/results/peers-${stack}-$(date +%Y%m%d-%H%M%S)}"

case "${stack}" in
    lgsomeip)
        coordinator="${repo}/performance-measurement/lg-someip/scripts/run-two-hosts.sh"
        agent_script="performance-measurement/lg-someip/scripts/remote-agent.sh"
        run_rel="perf-run"
        has_daemon=1
        daemon_comm="someip-daemon"
        ;;
    vsomeip)
        coordinator="${repo}/performance-measurement/vsomeip/scripts/run-two-hosts.sh"
        agent_script="performance-measurement/vsomeip/scripts/remote-agent.sh"
        run_rel="perf-run-vsomeip"
        has_daemon=1
        daemon_comm="routingmanagerd"
        ;;
    opensomeip)
        coordinator="${repo}/performance-measurement/opensomeip/scripts/run-two-hosts.sh"
        agent_script="performance-measurement/opensomeip/scripts/remote-agent.sh"
        run_rel="perf-run-opensomeip"
        has_daemon=0
        daemon_comm="no-such-daemon"
        ;;
    *) echo "run-peers.sh: unknown stack '${stack}'" >&2; exit 1 ;;
esac

# Ports of the opensomeip harness, which has no configuration file: responder
# k listens on 30500+k; subscriber k on 30700+k.
opensomeip_rpc_port=30501
opensomeip_sub_port=30701

max_of() {
    local max=0 value
    for value in "$@"; do
        ((value > max)) && max="${value}"
    done
    echo "${max}"
}
for value in ${requester_counts} ${responder_counts} ${publisher_counts} ${subscriber_counts}; do
    if ! [[ "${value}" =~ ^[1-8]$ ]]; then
        echo "run-peers.sh: peer counts must be 1..8, got '${value}'" >&2
        exit 1
    fi
done
# shellcheck disable=SC2086
instances="$(max_of ${responder_counts} ${publisher_counts})"

agent() {
    local host="$1"
    shift
    local command
    command="cd $(printf %q "${remote_dir}") && PERF_CPUS=$(printf %q "${cpus}") \
PERF_RUN_DIR=$(printf %q "${run_rel}") ${agent_script} $(printf '%q ' "$@")"
    if ((dry_run)); then
        echo "[${host}] ${agent_script##*/} $*"
        return 0
    fi
    ssh -o BatchMode=yes "${host}" "${command}"
}

pause() {
    ((dry_run)) || sleep "$(awk "BEGIN { print $1 / 1000 }")"
}

# Milliseconds since the epoch on <host>, for a start barrier of peers there.
host_now_ms() {
    if ((dry_run)); then
        echo 0
    else
        ssh -o BatchMode=yes "$1" 'date +%s%3N'
    fi
}

# The application name of peer <k> of a kind (perf-rpc-client, ...): the first
# keeps the plain name, as in the single-peer scenarios.
peer_name() {
    if (($2 == 1)); then echo "$1"; else echo "$1-$2"; fi
}

# wait_all <host> <seconds> <names...>
wait_all() {
    local host="$1" seconds="$2" name
    shift 2
    for name in "$@"; do
        agent "${host}" wait "${name}" "${seconds}" || status=1
    done
}

start_responders() {
    local m="$1" k
    for ((k = 1; k <= m; k++)); do
        if [[ "${stack}" == opensomeip ]]; then
            agent "${host_a}" start "rpc-server-${k}" perf-rpc-server --bind 0.0.0.0 \
                --port $((opensomeip_rpc_port + k - 1)) --transport "${transport}" --instance "${k}"
        else
            agent "${host_a}" start "rpc-server-${k}" perf-rpc-server --name "$(peer_name perf-rpc-server "${k}")" \
                --instance "${k}"
        fi
    done
    # Let the offers reach host B before the first requester looks for them.
    pause 2000
}

stop_responders() {
    local m="$1" k
    for ((k = 1; k <= m; k++)); do
        agent "${host_a}" stop "rpc-server-${k}"
    done
}

# Network accounting (-w). On each host an nftables table counts the UDP
# traffic exchanged with the other host: SD (port 30490) and SOME/IP data,
# per direction; PTP (319/320) is left out. SSH runs over TCP and is not counted.
net_setup() {
    ((net_usage)) || return 0
    local host="$1" peer="$2"
    if ((dry_run)); then
        echo "[${host}] nft: create table inet perfnet counting UDP with ${peer}"
        return 0
    fi
    ssh -o BatchMode=yes "${host}" "sudo -n /usr/sbin/nft -f -" <<EOF2
table inet perfnet
delete table inet perfnet
table inet perfnet {
    counter data_in {}
    counter data_out {}
    counter sd_in {}
    counter sd_out {}
    chain input {
        type filter hook input priority -10; policy accept;
        ip saddr ${peer} udp dport { 319, 320 } return
        ip saddr ${peer} udp dport 30490 counter name sd_in return
        ip saddr ${peer} meta l4proto udp counter name data_in
    }
    chain output {
        type filter hook output priority -10; policy accept;
        ip daddr ${peer} udp dport { 319, 320 } return
        ip daddr ${peer} udp dport 30490 counter name sd_out return
        ip daddr ${peer} meta l4proto udp counter name data_out
    }
}
EOF2
}

net_teardown() {
    ((net_usage && !dry_run)) || return 0
    ssh -o BatchMode=yes "$1" "sudo -n /usr/sbin/nft delete table inet perfnet" 2>/dev/null || true
}

net_reset() {
    ((net_usage && !dry_run)) || return 0
    local host
    for host in "${host_a}" "${host_b}"; do
        ssh -o BatchMode=yes "${host}" "sudo -n /usr/sbin/nft reset counters table inet perfnet >/dev/null"
    done
}

# net_record <scenario> <label> <payload> <messages sent>: append one row per host.
net_record() {
    ((net_usage && !dry_run)) || return 0
    local scenario="$1" label="$2" payload="$3" messages="$4" side host values
    local file="${results_dir}/net-usage.csv"
    [[ -s "${file}" ]] || echo "stack,scenario,label,payload_bytes,messages,host,data_in_packets,data_in_bytes,data_out_packets,data_out_bytes,sd_in_packets,sd_in_bytes,sd_out_packets,sd_out_bytes" > "${file}"
    for side in A B; do
        [[ "${side}" == A ]] && host="${host_a}" || host="${host_b}"
        values="$(ssh -o BatchMode=yes "${host}" "sudo -n /usr/sbin/nft list counters table inet perfnet" |
            awk '/counter /{name=$2} /packets/{for(i=1;i<=NF;i++){if($i=="packets")p=$(i+1); if($i=="bytes")b=$(i+1)}; v[name]=p","b}
                 END{printf "%s,%s,%s,%s", v["data_in"], v["data_out"], v["sd_in"], v["sd_out"]}')"
        echo "${stack},${scenario},${label},${payload},${messages},${side},${values}" >> "${file}"
    done
}

# One RPC step: N requesters, M responders, one mode and payload.
rpc_step() {
    local n="$1" m="$2" mode="$3" payload="$4" c start_at step_count label names=() csvs=()
    label="peers-r${n}-s${m}"
    if [[ "${mode}" == rate ]]; then
        step_count=$((rate * m * duration))
    else
        step_count="${count}"
    fi
    echo "=== ${stack} rpc ${mode} requesters=${n} responders=${m} payload=${payload} ==="
    # Every requester discovers M responders before the barrier; give each a moment.
    start_at=$(($(host_now_ms "${host_b}") + 4000 + 300 * n))
    net_reset
    for ((c = 1; c <= n; c++)); do
        local peer_args=()
        if [[ "${stack}" == opensomeip ]]; then
            peer_args=(--server "${ip_a}" --port "${opensomeip_rpc_port}" --transport "${transport}"
                --client-id $((0x0321 + c - 1)))
        else
            peer_args=(--name "$(peer_name perf-rpc-client "${c}")")
            [[ "${stack}" == vsomeip ]] && peer_args+=(--transport "${transport}")
        fi
        agent "${host_b}" start "rpc-client-${c}" perf-rpc-client "${peer_args[@]}" --responders "${m}" \
            --mode "${mode}" --rate "${rate}" --payload "${payload}" --count "${step_count}" \
            --warmup $((step_count / 10)) --start-at "${start_at}" --label "${label}" \
            --csv "${run_rel}/.peer-rpc-${c}.csv" \
            --samples-out "${run_rel}/peer-rpc-${mode}-p${payload}-r${n}-s${m}-c${c}.samples"
        names+=("rpc-client-${c}")
        csvs+=("${run_rel}/.peer-rpc-${c}.csv")
    done
    local budget=$((15 + step_count / (rate > 0 ? rate : 1000) + n))
    wait_all "${host_b}" "${budget}" "${names[@]}"
    # Requests sent by all requesters, warm-up included.
    net_record "rpc-${mode}" "${label}" "${payload}" $((n * (step_count + step_count / 10)))
    agent "${host_b}" merge-csv "${run_rel}/peers-rpc-${mode}.csv" "${csvs[@]}"
}

scenario_rpc() {
    local m n mode payload
    for m in ${responder_counts}; do
        start_responders "${m}"
        for n in ${requester_counts}; do
            for mode in ${modes}; do
                for payload in ${payloads}; do
                    rpc_step "${n}" "${m}" "${mode}" "${payload}"
                done
            done
        done
        stop_responders "${m}"
    done
}

# One event step: M publishers, N subscribers, one payload.
event_step() {
    local m="$1" n="$2" payload="$3" k start_at step_count label subs=() pubs=() sub_csvs=() pub_csvs=()
    label="peers-p${m}-s${n}"
    step_count=$((rate * duration))
    echo "=== ${stack} event publishers=${m} subscribers=${n} payload=${payload} ==="
    net_reset
    for ((k = 1; k <= n; k++)); do
        local peer_args=()
        if [[ "${stack}" == opensomeip ]]; then
            peer_args=(--bind 0.0.0.0 --port $((opensomeip_sub_port + k - 1)))
        else
            peer_args=(--name "perf-event-sub-${k}")
        fi
        agent "${host_b}" start "event-sub-${k}" perf-event-subscriber "${peer_args[@]}" --publishers "${m}" \
            --clock realtime --label "${label}" --csv "${run_rel}/.peer-sub-${k}.csv" \
            --samples-out "${run_rel}/peer-event-p${payload}-p${m}-s${n}-sub${k}.samples"
        subs+=("event-sub-${k}")
        sub_csvs+=("${run_rel}/.peer-sub-${k}.csv")
    done
    pause 1500
    # Publishers wait for their subscriptions first; the barrier leaves time for that.
    start_at=$(($(host_now_ms "${host_a}") + 5000 + 300 * m))
    for ((k = 1; k <= m; k++)); do
        local peer_args=()
        if [[ "${stack}" == opensomeip ]]; then
            peer_args=(--subscriber "${ip_b}" --port "${opensomeip_sub_port}" --subscribers "${n}")
        else
            # Host B's daemon subscribes once per publisher for all its local
            # subscribers; settle-ms gives the others time to attach.
            peer_args=(--name "$(peer_name perf-event-pub "${k}")" --subscribers 1)
        fi
        agent "${host_a}" start "event-pub-${k}" perf-event-publisher "${peer_args[@]}" --instance "${k}" \
            --settle-ms 1000 --start-at "${start_at}" --clock realtime --rate "${rate}" --payload "${payload}" \
            --count "${step_count}" --warmup $((step_count / 10)) --label "${label}" \
            --csv "${run_rel}/.peer-pub-${k}.csv"
        pubs+=("event-pub-${k}")
        pub_csvs+=("${run_rel}/.peer-pub-${k}.csv")
    done
    wait_all "${host_a}" $((30 + duration + m)) "${pubs[@]}"
    wait_all "${host_b}" 30 "${subs[@]}"
    # Notifications published by all publishers: warm-up, data, and 5 end markers each.
    net_record event "${label}" "${payload}" $((m * (step_count + step_count / 10 + 5)))
    agent "${host_a}" merge-csv "${run_rel}/peers-event-publish.csv" "${pub_csvs[@]}"
    agent "${host_b}" merge-csv "${run_rel}/peers-event-subscribe.csv" "${sub_csvs[@]}"
}

scenario_event() {
    if [[ "${stack}" == opensomeip && "${transport}" != udp ]]; then
        echo "event peers: the opensomeip direct-event harness is UDP only; skipping" >&2
        return 0
    fi
    local m n payload
    for m in ${publisher_counts}; do
        for n in ${subscriber_counts}; do
            for payload in ${payloads}; do
                event_step "${m}" "${n}" "${payload}"
            done
        done
    done
}

# host_sample <host> <process name>...: cumulative CPU time in ms (user + system
# of every thread, from /proc) of the processes with each name, in the order
# given, followed by the host clock in ms. Names are scheduler names, at most
# 15 characters.
host_sample() {
    local host="$1"
    shift
    if ((dry_run)); then
        printf '0 %.0s' "$@"
        echo 0
        return 0
    fi
    ssh -o BatchMode=yes "${host}" bash -s -- "$@" <<'EOF'
hz=$(getconf CLK_TCK)
snapshot=$(cat /proc/[0-9]*/stat 2>/dev/null)
for name in "$@"; do
    printf '%s ' "$(awk -v name="(${name})" -v hz="${hz}" '$2 == name { t += $14 + $15 } END { printf "%d", t * 1000 / hz }' <<< "${snapshot}")"
done
date +%s%3N
EOF
}

# Idle CPU of the daemons and responders in ms per second, measured over a few
# seconds with the responders running and no requester. A step subtracts it so
# the figures are the CPU the traffic caused, not the Service Discovery timers.
thread_idle_a_daemon=0
thread_idle_a_server=0
thread_idle_b_daemon=0
measure_idle_cpu() {
    local ad0 as0 at0 bd0 bt0 ad1 as1 at1 bd1 bt1
    read -r ad0 as0 at0 <<< "$(host_sample "${host_a}" "${daemon_comm}" perf-rpc-server)"
    read -r bd0 bt0 <<< "$(host_sample "${host_b}" "${daemon_comm}")"
    pause 4000
    read -r ad1 as1 at1 <<< "$(host_sample "${host_a}" "${daemon_comm}" perf-rpc-server)"
    read -r bd1 bt1 <<< "$(host_sample "${host_b}" "${daemon_comm}")"
    thread_idle_a_daemon="$(awk -v d="$((ad1 - ad0))" -v t="$((at1 - at0))" 'BEGIN { print (t > 0 ? d * 1000 / t : 0) }')"
    thread_idle_a_server="$(awk -v d="$((as1 - as0))" -v t="$((at1 - at0))" 'BEGIN { print (t > 0 ? d * 1000 / t : 0) }')"
    thread_idle_b_daemon="$(awk -v d="$((bd1 - bd0))" -v t="$((bt1 - bt0))" 'BEGIN { print (t > 0 ? d * 1000 / t : 0) }')"
    echo "  idle CPU (ms/s): daemon A ${thread_idle_a_daemon}, responders A ${thread_idle_a_server}, daemon B ${thread_idle_b_daemon}"
}

# One thread step: one requester process with T worker threads and M responders.
threads_step() {
    local m="$1" t="$2" thread_rate="$3" payload="$4" label rc=0 out calls
    local peer_args=() ad0 as0 at0 bd0 bt0 ad1 as1 at1 bd1 bt1
    label="threads-t${t}-s${m}-h${thread_rate}"
    echo "=== ${stack} rpc threads=${t} responders=${m} rate/thread=${thread_rate} payload=${payload} ==="
    if [[ "${stack}" == opensomeip ]]; then
        peer_args=(--server "${ip_a}" --port "${opensomeip_rpc_port}" --transport "${transport}" --client-id 801)
    else
        peer_args=(--name perf-rpc-client)
        [[ "${stack}" == vsomeip ]] && peer_args+=(--transport "${transport}")
    fi
    read -r ad0 as0 at0 <<< "$(host_sample "${host_a}" "${daemon_comm}" perf-rpc-server)"
    read -r bd0 bt0 <<< "$(host_sample "${host_b}" "${daemon_comm}")"
    out="$(agent "${host_b}" run perf-rpc-threads "${peer_args[@]}" --responders "${m}" --threads "${t}" \
        --thread-rate "${thread_rate}" --payload "${payload}" --warmup-s 1 --duration-s "${duration}" \
        --label "${label}" --csv "${run_rel}/peers-threads.csv" 2>&1)" || rc=$?
    read -r ad1 as1 at1 <<< "$(host_sample "${host_a}" "${daemon_comm}" perf-rpc-server)"
    read -r bd1 bt1 <<< "$(host_sample "${host_b}" "${daemon_comm}")"
    if ((dry_run)); then
        echo "${out}"
        return 0
    fi
    echo "${out}" | grep -E '^  (throughput_msg_s|loss_pct|p50_us|p99_us|cpu_pct|cpu_per_req_us|bad_size) ' || true
    # A lost request is a measured result (exit 3); any other failure is not.
    if ((rc != 0 && rc != 3)); then
        echo "  threads step failed (exit ${rc})" >&2
        echo "${out}" | tail -n 5 >&2
        status=1
        return 0
    fi
    calls="$(echo "${out}" | awk '/^  calls_total /{ print int($NF) }')"
    local file="${results_dir}/threads-host-cpu.csv"
    [[ -s "${file}" ]] ||
        echo "stack,label,threads,thread_rate,responders,payload_bytes,calls_total,a_daemon_cpu_ms,a_server_cpu_ms,b_daemon_cpu_ms,a_wall_ms,b_wall_ms" > "${file}"
    awk -v stack="${stack}" -v label="${label}" -v t="${t}" -v rate="${thread_rate}" -v m="${m}" -v p="${payload}" \
        -v calls="${calls:-0}" -v ad="$((ad1 - ad0))" -v as="$((as1 - as0))" -v bd="$((bd1 - bd0))" \
        -v aw="$((at1 - at0))" -v bw="$((bt1 - bt0))" \
        -v iad="${thread_idle_a_daemon}" -v ias="${thread_idle_a_server}" -v ibd="${thread_idle_b_daemon}" '
        function net(total, idle, wall) { v = total - idle * wall / 1000; return v < 0 ? 0 : v }
        BEGIN {
            printf "%s,%s,%d,%s,%d,%d,%d,%.0f,%.0f,%.0f,%d,%d\n", stack, label, t, rate, m, p, calls,
                net(ad, iad, aw), net(as, ias, aw), net(bd, ibd, bw), aw, bw
        }' >> "${file}"
    pause 1000
}

scenario_threads() {
    local m t thread_rate payload
    for m in ${responder_counts}; do
        start_responders "${m}"
        measure_idle_cpu
        for payload in ${payloads}; do
            for thread_rate in ${thread_rates}; do
                for t in ${thread_counts}; do
                    threads_step "${m}" "${t}" "${thread_rate}" "${payload}"
                done
            done
        done
        stop_responders "${m}"
    done
}

capture_daemon_usage() {
    ((has_daemon)) || return 0
    if ((dry_run)); then
        agent "${host_a}" daemon-usage
        return 0
    fi
    agent "${host_a}" daemon-usage > "${results_dir}/A-daemon-usage-$1.txt"
    agent "${host_b}" daemon-usage > "${results_dir}/B-daemon-usage-$1.txt"
}

cleanup() {
    agent "${host_a}" stop-all >/dev/null 2>&1 || true
    agent "${host_b}" stop-all >/dev/null 2>&1 || true
}

collect() {
    ((dry_run)) && return 0
    mkdir -p "${results_dir}/A-provider" "${results_dir}/B-consumer"
    rsync -az "${host_a}:${remote_dir}/${run_rel}/" "${results_dir}/A-provider/" || true
    rsync -az "${host_b}:${remote_dir}/${run_rel}/" "${results_dir}/B-consumer/" || true
}

# Sync, build, and record the host state with the stack's own coordinator and
# no scenario: run-info, versions, sysinfo, and build sizes land in results_dir.
setup_args=(-A "${host_a}" -B "${host_b}" -a "${ip_a}" -b "${ip_b}" -d "${remote_dir}" -c "${cpus}"
    -t "${transport}" -s " " -o "${results_dir}")
((sync)) && setup_args+=(-y)
((dry_run)) && setup_args+=(-n)
"${coordinator}" "${setup_args[@]}" >/dev/null || { echo "run-peers.sh: ${stack} setup failed" >&2; exit 1; }
((dry_run)) || echo "peers=requesters:${requester_counts// /,} responders:${responder_counts// /,} \
publishers:${publisher_counts// /,} subscribers:${subscriber_counts// /,} rate=${rate} modes=${modes// /,}" \
    >> "${results_dir}/run-info.txt"

trap 'cleanup; net_teardown "${host_a}"; net_teardown "${host_b}"' EXIT
net_setup "${host_a}" "${ip_b}"
net_setup "${host_b}" "${ip_a}"
if ((has_daemon)); then
    agent "${host_a}" config -r provider -u "${ip_a}" -t "${transport}" -i "${instances}"
    agent "${host_b}" config -r consumer -u "${ip_b}" -t "${transport}" -i "${instances}"
    if [[ "${stack}" == vsomeip ]]; then
        agent "${host_a}" route-check
        agent "${host_b}" route-check
    fi
    agent "${host_a}" daemon-start
    agent "${host_b}" daemon-start
fi
capture_daemon_usage before

for scenario in ${scenarios}; do
    echo "##### peers ${scenario} (${stack}) #####"
    case "${scenario}" in
        rpc | event | threads) "scenario_${scenario}" ;;
        *) echo "unknown scenario '${scenario}'" >&2; status=1 ;;
    esac
done

capture_daemon_usage after
cleanup
collect
((dry_run)) || echo "results: ${results_dir}"
exit "${status}"
