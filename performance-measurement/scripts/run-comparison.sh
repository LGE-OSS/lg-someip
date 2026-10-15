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

# Run the two-host benchmarks of lg-someip, vsomeip, and opensomeip several
# times, interleaved, and merge the results into one comparison.
#
#   run-comparison.sh -A <ssh host A> -B <ssh host B> -a <A test IP> -b <B test IP>
#                     [-o <output dir>] [-R <repetitions>] [-s "<scenarios>"]
#                     [-S "<stacks>"] [-p "<payloads>"] [-r "<rates>"] [-N <count>]
#                     [-D <seconds per rate step>] [-c <daemon CPUs>,<app CPUs>]
#                     [-t udp|tcp] [-d <repo dir on hosts>] [-m standard|peers] [-O "<run-peers.sh options>"]
#                     [-y] [-q] [-f] [-n]
#
#   -R  repetitions of every stack (default 5). One repetition runs each stack
#       once; the stack order rotates between repetitions, so no stack always
#       runs first (warm hosts) or last (hot hosts).
#   -s  scenarios, default "pingpong rate event". Use the same value for all
#       stacks: the daemon CPU deltas cover every scenario of a run. opensomeip
#       has no Service Discovery in the harness, so "sd" runs for lgsomeip and
#       vsomeip only (it is skipped for opensomeip).
#   -S  stacks, default "lgsomeip vsomeip opensomeip". The extra stack
#       "lgsomeip-vsomeip-api" (standard mode only) runs the vsomeip
#       applications, unchanged, on lg-someip's vsomeip API layer and
#       someip-daemon; it needs an lg-someip install in ~/lgsomeip-install on
#       both hosts (cmake --install build-pi --prefix ~/lgsomeip-install).
#   -t  transport, default udp. With tcp, lg-someip and vsomeip run over TCP,
#       while opensomeip runs over UDP: its RpcServer and RpcClient are UDP-only
#       in v0.2.0 and split large messages with SOME/IP-TP instead.
#   -m  standard (default): the stacks' run-two-hosts.sh scenarios; peers: the
#       peer-count sweep of run-peers.sh (N requesters x M responders, M
#       publishers x N subscribers), whose scenarios are "rpc event"
#   -O  extra options for run-peers.sh in peers mode, e.g. -O '-R "1 4" -M "1 2"'
#   -y  copy the sources to the hosts and build them before the first repetition
#   -f  continue although the host preflight found differences
#   -q  quick smoke test with small counts (results are not meaningful)
#   -n  dry run: print the coordinator commands without running them
#
# Before measuring, equalize the two hosts: the same kernel on A and B, the
# same net.core.rmem_max, ptp4l running on both, and the performance governor.
# The preflight reports each difference; it stops unless -f is given.
#
# Output: <output dir>/<stack>/rep<k>/ per run, and <output dir>/merged/ with
# the unified CSV files and comparison.md (see merge-results.py).

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "${script_dir}/../.." && pwd)"

host_a=""
host_b=""
ip_a=""
ip_b=""
remote_dir="lg-someip"
out_dir="${repo}/performance-measurement/results/comparison-$(date +%Y%m%d-%H%M%S)"
repetitions=5
scenarios=""
mode="standard"
transport="udp"
peer_options=""
stacks="lgsomeip vsomeip opensomeip"
common_args=()
sync=0
force=0
dry_run=0
quick=0

while getopts "A:B:a:b:d:o:R:s:S:p:r:N:D:c:m:O:t:yqfnh" opt; do
    case "${opt}" in
        A) host_a="${OPTARG}" ;;
        B) host_b="${OPTARG}" ;;
        a) ip_a="${OPTARG}" ;;
        b) ip_b="${OPTARG}" ;;
        d) remote_dir="${OPTARG}" ;;
        o) out_dir="${OPTARG}" ;;
        R) repetitions="${OPTARG}" ;;
        s) scenarios="${OPTARG}" ;;
        S) stacks="${OPTARG}" ;;
        m) mode="${OPTARG}" ;;
        t) transport="${OPTARG}" ;;
        O) peer_options="${OPTARG}" ;;
        p | r | N | D | c) common_args+=("-${opt}" "${OPTARG}") ;;
        y) sync=1 ;;
        q) quick=1 ;;
        f) force=1 ;;
        n) dry_run=1 ;;
        *) sed -n '15,/^set -euo/p' "$0" | sed '$d'; exit 1 ;;
    esac
done
if [[ -z "${host_a}" || -z "${host_b}" || -z "${ip_a}" || -z "${ip_b}" ]]; then
    echo "run-comparison.sh: -A, -B, -a and -b are required" >&2
    sed -n '15,/^set -euo/p' "$0" | sed '$d'
    exit 1
fi
((quick)) && common_args+=(-q)
case "${mode}" in
    standard) scenarios="${scenarios:-pingpong rate event}" ;;
    peers) scenarios="${scenarios:-rpc event}" ;;
    *) echo "run-comparison.sh: -m must be standard or peers" >&2; exit 1 ;;
esac
if [[ "${mode}" == peers && " ${stacks} " == *" lgsomeip-vsomeip-api "* ]]; then
    echo "run-comparison.sh: lgsomeip-vsomeip-api runs in standard mode only" >&2
    exit 1
fi
# Options for run-peers.sh, split like a shell would (quotes group words).
peer_args=()
if [[ -n "${peer_options}" ]]; then
    eval "peer_args=(${peer_options})"
fi

coordinator() {
    if [[ "${mode}" == peers ]]; then
        echo "${script_dir}/run-peers.sh"
        return 0
    fi
    case "$1" in
        lgsomeip) echo "${repo}/performance-measurement/lg-someip/scripts/run-two-hosts.sh" ;;
        vsomeip | lgsomeip-vsomeip-api) echo "${repo}/performance-measurement/vsomeip/scripts/run-two-hosts.sh" ;;
        opensomeip) echo "${repo}/performance-measurement/opensomeip/scripts/run-two-hosts.sh" ;;
        *) echo "run-comparison.sh: unknown stack '$1'" >&2; exit 1 ;;
    esac
}

# Print "kernel rmem_max ptp governor" of a host.
host_state() {
    ssh -o BatchMode=yes "$1" 'echo "$(uname -r) $(cat /proc/sys/net/core/rmem_max) \
$(pgrep -x ptp4l >/dev/null && echo ptp4l || echo no-ptp4l) \
$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"'
}

preflight() {
    ((dry_run)) && return 0
    local state_a state_b problems=0
    state_a="$(host_state "${host_a}")"
    state_b="$(host_state "${host_b}")"
    echo "preflight A: ${state_a}"
    echo "preflight B: ${state_b}"
    read -r kernel_a rmem_a ptp_a gov_a <<< "${state_a}"
    read -r kernel_b rmem_b ptp_b gov_b <<< "${state_b}"
    if [[ "${kernel_a}" != "${kernel_b}" ]]; then
        echo "  difference: kernel ${kernel_a} vs ${kernel_b}" >&2
        problems=1
    fi
    if [[ "${rmem_a}" != "${rmem_b}" ]]; then
        echo "  difference: rmem_max ${rmem_a} vs ${rmem_b} (set both with: sudo sysctl -w net.core.rmem_max=<bytes>)" >&2
        problems=1
    fi
    if [[ "${ptp_a}" != ptp4l || "${ptp_b}" != ptp4l ]]; then
        echo "  difference: ptp4l is not running on both hosts (needed for one-way event latency)" >&2
        problems=1
    fi
    if [[ "${gov_a}" != performance || "${gov_b}" != performance ]]; then
        echo "  difference: CPU governor is ${gov_a} / ${gov_b}, not performance" >&2
        problems=1
    fi
    if ((problems && !force)); then
        echo "run-comparison.sh: hosts are not equalized; fix the differences above or pass -f" >&2
        exit 1
    fi
}

# The stack order of repetition <k> (1-based), rotated by k - 1.
order_for() {
    local k="$1" list=(${stacks}) count shift_by i
    count="${#list[@]}"
    shift_by=$(((k - 1) % count))
    for ((i = 0; i < count; i++)); do
        echo "${list[$(((i + shift_by) % count))]}"
    done
}

preflight
declare -A run_dirs=()
failures=0
for ((rep = 1; rep <= repetitions; rep++)); do
    for stack in $(order_for "${rep}"); do
        stack_scenarios="${scenarios}"
        if [[ "${stack}" == opensomeip && "${mode}" == standard ]]; then
            stack_scenarios="${scenarios//sd/}"
            [[ -n "${stack_scenarios// /}" ]] || continue
        fi
        run_dir="${out_dir}/${stack}/rep${rep}"
        sync_arg=()
        ((sync && rep == 1)) && sync_arg=(-y)
        dry_arg=()
        ((dry_run)) && dry_arg=(-n)
        echo "##### repetition ${rep}/${repetitions}: ${stack} -> ${run_dir} #####"
        stack_transport="${transport}"
        [[ "${stack}" == opensomeip ]] && stack_transport=udp
        stack_arg=()
        [[ "${mode}" == peers ]] && stack_arg=(-S "${stack}" ${peer_args[@]+"${peer_args[@]}"})
        backend=vsomeip
        [[ "${stack}" == lgsomeip-vsomeip-api ]] && backend=lgsomeip
        if ! PERF_VSOMEIP_BACKEND="${backend}" "$(coordinator "${stack}")" ${stack_arg[@]+"${stack_arg[@]}"} -A "${host_a}" -B "${host_b}" -a "${ip_a}" -b "${ip_b}" -d "${remote_dir}" \
            -s "${stack_scenarios}" -o "${run_dir}" -t "${stack_transport}" ${common_args[@]+"${common_args[@]}"} \
            ${sync_arg[@]+"${sync_arg[@]}"} ${dry_arg[@]+"${dry_arg[@]}"}; then
            echo "run-comparison.sh: ${stack} repetition ${rep} reported failures; continuing" >&2
            failures=$((failures + 1))
        fi
        # The standard coordinators append "-quick" to the directory of a quick
        # run; run-peers.sh writes to the given directory.
        ((quick)) && [[ "${mode}" == standard ]] && run_dir="${run_dir}-quick"
        run_dirs[${stack}]+="${run_dir},"
    done
done

if ((dry_run)); then
    exit 0
fi

merge_args=()
for stack in ${stacks}; do
    [[ -n "${run_dirs[${stack}]:-}" ]] && merge_args+=("${stack}=${run_dirs[${stack}]%,}")
done
python3 "${script_dir}/merge-results.py" --out "${out_dir}/merged" "${merge_args[@]}"
echo "comparison: ${out_dir}/merged/comparison.md"
exit $((failures > 0))
