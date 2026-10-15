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

# Print the host state that affects benchmark results as key=value lines:
# board, kernel, CPU governor and frequency, temperature, throttling (Raspberry
# Pi), clock synchronization, and the test interface.
#
#   sysinfo.sh [<test IP address>]
#
# With an IP address, the interface that carries it is reported as well.

set -uo pipefail

value() {
    # value <key> <command...>: print key=<first line of output>, or key= if it fails.
    local key="$1"; shift
    echo "${key}=$("$@" 2>/dev/null | head -1 | tr -d '\0')"
}

value host hostname
value date date -u +%Y-%m-%dT%H:%M:%SZ
value model cat /proc/device-tree/model
value os sh -c '. /etc/os-release && echo "${PRETTY_NAME}"'
value kernel uname -r
value arch uname -m
value cpus nproc
value mem_total_kb awk '/MemTotal/ { print $2 }' /proc/meminfo
value governor cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
for ((cpu = 0; cpu < $(nproc); cpu++)); do
    freq="/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_cur_freq"
    [[ -r "${freq}" ]] && echo "cpu${cpu}_mhz=$(($(cat "${freq}") / 1000))"
done
value temp_c awk '{ printf "%.1f", $1 / 1000 }' /sys/class/thermal/thermal_zone0/temp
# 0x0 means no under-voltage or throttling since boot; see the vcgencmd documentation.
value throttled sh -c 'vcgencmd get_throttled | cut -d= -f2'
value ptp4l_running sh -c 'pgrep -x ptp4l >/dev/null && echo yes || echo no'
value phc2sys_running sh -c 'pgrep -x phc2sys >/dev/null && echo yes || echo no'
value ntp_synchronized timedatectl show -p NTPSynchronized --value
repo_root="$(cd "$(dirname "$0")/../../.." && pwd)"
# Same order as run-two-hosts.sh and CMake: a git checkout first, REVISION
# (written by make-bundle.sh) only when there is no checkout.
if git -C "${repo_root}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    value revision git -C "${repo_root}" describe --always --dirty
else
    value revision head -1 "${repo_root}/REVISION"
fi
value rmem_default cat /proc/sys/net/core/rmem_default
value rmem_max cat /proc/sys/net/core/rmem_max
# Kernel UDP counters since boot; compare the before and after files. RcvbufErrors
# counts datagrams dropped because a socket's receive buffer was full.
udp_counters="$(awk '/^Udp:/ { if (!n) { split($0, k); n = 1 } else { for (i = 2; i <= NF; i++) printf "udp_%s=%s\n", k[i], $i } }' /proc/net/snmp 2>/dev/null)"
grep -E "^udp_(InDatagrams|NoPorts|InErrors|OutDatagrams|RcvbufErrors|SndbufErrors)=" <<< "${udp_counters}"

if [[ $# -ge 1 ]]; then
    iface="$(ip -o addr show 2>/dev/null | awk -v ip="$1" '{ split($4, a, "/"); if (a[1] == ip) { print $2; exit } }')"
    echo "test_ip=$1"
    echo "test_iface=${iface}"
    if [[ -n "${iface}" ]]; then
        value link_speed_mbps cat "/sys/class/net/${iface}/speed"
        value link_mtu cat "/sys/class/net/${iface}/mtu"
        value link_hw_timestamping sh -c "ethtool -T ${iface} | grep -q hardware-transmit && echo yes || echo no"
        # Interrupt coalescing delays packets on purpose and adds to every round trip.
        value link_rx_usecs sh -c "ethtool -c ${iface} | awk '/^rx-usecs:/ { print \$2 }'"
        value link_rx_frames sh -c "ethtool -c ${iface} | awk '/^rx-frames:/ { print \$2 }'"
        value link_rx_dropped cat "/sys/class/net/${iface}/statistics/rx_dropped"
    fi
fi
