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

# Generate a real-vsomeip configuration for the performance applications.
# Mirrors performance-measurement/lg-someip/scripts/gen-config.sh (same service/
# method IDs, same SD timing knobs, same -o/-u/-t/-r/-l/-I/-R/-M options) so
# the two stacks can be compared with matched settings. vsomeip's own JSON
# schema differs from lg-someip's: a "routing" key names the app that acts as
# (or connects to) the routing manager, and only the offering host needs the
# "services" block (the consumer finds it through Service Discovery).
#
# Two-host prerequisite: real vsomeip only starts Service Discovery once the
# kernel reports an explicit multicast route, which a plain static-IP link
# usually doesn't have. Without it, offers/finds never leave the host and
# nothing times out loudly -- run this once per boot on both hosts first:
#   sudo ip route add 224.0.0.0/4 dev <interface>
#
# Another two-host gotcha: real vsomeip batches small messages sent over a
# REMOTE (network) endpoint into "trains" by default (debounce 2ms / max
# retention 5ms per direction -- ~10ms added to a request/response round
# trip), but does NOT do this for local (same-host, UDS) traffic. This script
# disables that batching via "npdu-default-timings" so single small RPC
# messages are sent immediately, matching lg-someip's behavior.
#
#   gen-config.sh -o <file> [-u <unicast>] [-t udp|tcp] [-r local|provider|consumer] [-l <log level>]
#                 [-I <initial delay ms>] [-R <repetition base delay ms>] [-M <repetitions>]
#                 [-C <cyclic offer delay ms>] [-i <instances>]
#
#   -i  instances 1..N (1..8) of the RPC (0x2001) and event (0x2002) services,
#       for peer measurements; same ports as the lg-someip generator
#       (instance k >= 2: 31000+k RPC and 31200+k events for UDP, 31100+k and
#       31300+k for TCP).

set -euo pipefail

output=""
unicast="127.0.0.1"
transport="udp"
role="local"
log_level="warning"
initial_delay=10
repetition_base=200
repetitions=3
cyclic_offer=2000
instances=1

while getopts "o:u:t:r:l:I:R:M:C:i:h" opt; do
    case "${opt}" in
        o) output="${OPTARG}" ;;
        u) unicast="${OPTARG}" ;;
        t) transport="${OPTARG}" ;;
        r) role="${OPTARG}" ;;
        l) log_level="${OPTARG}" ;;
        I) initial_delay="${OPTARG}" ;;
        R) repetition_base="${OPTARG}" ;;
        M) repetitions="${OPTARG}" ;;
        C) cyclic_offer="${OPTARG}" ;;
        i) instances="${OPTARG}" ;;
        *) sed -n '15,/^set -euo/p' "$0" | sed '$d'; exit 1 ;;
    esac
done

if [[ -z "${output}" ]]; then
    echo "gen-config.sh: -o <file> is required" >&2
    exit 1
fi

if ! [[ "${instances}" =~ ^[1-8]$ ]]; then
    echo "gen-config.sh: -i must be 1..8" >&2
    exit 1
fi

# endpoint <udp port> <tcp port>: the endpoint declaration for the transport.
endpoint() {
    case "${transport}" in
        udp) echo "\"unreliable\" : \"$1\"" ;;
        tcp) echo "\"reliable\" : { \"port\" : \"$2\", \"enable-magic-cookies\" : \"false\" }" ;;
        *) echo "gen-config.sh: unknown transport '${transport}'" >&2; exit 1 ;;
    esac
}
endpoint 0 0 >/dev/null
rpc_endpoint="$(endpoint 30601 30602)"
event_endpoint="$(endpoint 30611 30612)"
sd_endpoint="$(endpoint 30621 30622)"

case "${role}" in
    local|provider) declare_services=1 ;;
    consumer) declare_services=0 ;;
    *) echo "gen-config.sh: unknown role '${role}'" >&2; exit 1 ;;
esac

# Events are sent over TCP only when the provider declares them reliable.
events_block() {
    [[ "${transport}" == tcp ]] || return 0
    local event events=""
    for event in "$@"; do
        events+="${events:+, }{ \"event\" : \"${event}\", \"is_reliable\" : \"true\" }"
    done
    echo ",
            \"events\" : [ ${events} ]"
}

extra_services=""
for ((k = 2; k <= instances; k++)); do
    extra_services+="        {
            \"service\" : \"0x2001\",
            \"instance\" : \"$(printf '0x%04x' "${k}")\",
            $(endpoint $((31000 + k)) $((31100 + k)))
        },
        {
            \"service\" : \"0x2002\",
            \"instance\" : \"$(printf '0x%04x' "${k}")\",
            $(endpoint $((31200 + k)) $((31300 + k)))$(events_block 0x8001 0x8002)
        },
"
done

services=""
if ((declare_services)); then
    services=",
    \"services\" :
    [
        {
            \"service\" : \"0x2001\",
            \"instance\" : \"0x0001\",
            ${rpc_endpoint}
        },
        {
            \"service\" : \"0x2002\",
            \"instance\" : \"0x0001\",
            ${event_endpoint}$(events_block 0x8001 0x8002)
        },
${extra_services}
        {
            \"service\" : \"0x2003\",
            \"instance\" : \"0x0001\",
            ${sd_endpoint}$(events_block 0x8001 0x8002 0x8003)
        }
    ]"
fi

# Application IDs and service/method IDs match
# performance-measurement/lg-someip/scripts/gen-config.sh so the two stacks
# exchange identical SOME/IP messages.
cat > "${output}" <<EOF
{
    "unicast" : "${unicast}",
    "logging" :
    {
        "level" : "${log_level}",
        "console" : "true"
    },
    "applications" :
    [
        { "name" : "routingmanagerd", "id" : "0x1000" },
        { "name" : "perf-rpc-server", "id" : "0x0311" },
        { "name" : "perf-rpc-server-2", "id" : "0x0312" },
        { "name" : "perf-rpc-server-3", "id" : "0x0313" },
        { "name" : "perf-rpc-server-4", "id" : "0x0314" },
        { "name" : "perf-rpc-server-5", "id" : "0x0315" },
        { "name" : "perf-rpc-server-6", "id" : "0x0316" },
        { "name" : "perf-rpc-server-7", "id" : "0x0317" },
        { "name" : "perf-rpc-server-8", "id" : "0x0318" },
        { "name" : "perf-rpc-client", "id" : "0x0321" },
        { "name" : "perf-rpc-client-2", "id" : "0x0322" },
        { "name" : "perf-rpc-client-3", "id" : "0x0323" },
        { "name" : "perf-rpc-client-4", "id" : "0x0324" },
        { "name" : "perf-rpc-client-5", "id" : "0x0325" },
        { "name" : "perf-rpc-client-6", "id" : "0x0326" },
        { "name" : "perf-rpc-client-7", "id" : "0x0327" },
        { "name" : "perf-rpc-client-8", "id" : "0x0328" },
        { "name" : "perf-event-pub", "id" : "0x0411" },
        { "name" : "perf-event-pub-2", "id" : "0x0412" },
        { "name" : "perf-event-pub-3", "id" : "0x0413" },
        { "name" : "perf-event-pub-4", "id" : "0x0414" },
        { "name" : "perf-event-pub-5", "id" : "0x0415" },
        { "name" : "perf-event-pub-6", "id" : "0x0416" },
        { "name" : "perf-event-pub-7", "id" : "0x0417" },
        { "name" : "perf-event-pub-8", "id" : "0x0418" },
        { "name" : "perf-event-sub-1", "id" : "0x0421" },
        { "name" : "perf-event-sub-2", "id" : "0x0422" },
        { "name" : "perf-event-sub-3", "id" : "0x0423" },
        { "name" : "perf-event-sub-4", "id" : "0x0424" },
        { "name" : "perf-event-sub-5", "id" : "0x0425" },
        { "name" : "perf-event-sub-6", "id" : "0x0426" },
        { "name" : "perf-event-sub-7", "id" : "0x0427" },
        { "name" : "perf-event-sub-8", "id" : "0x0428" },
        { "name" : "perf-sd-provider", "id" : "0x0511" },
        { "name" : "perf-sd-probe", "id" : "0x0521" }
    ]${services},
    "routing" : "routingmanagerd",
    "npdu-default-timings" :
    {
        "debounce-time-request" : "0",
        "debounce-time-response" : "0",
        "max-retention-time-request" : "0",
        "max-retention-time-response" : "0"
    },
    "service-discovery" :
    {
        "enable" : "true",
        "multicast" : "224.244.224.245",
        "port" : "30490",
        "protocol" : "udp",
        "initial_delay_min" : "${initial_delay}",
        "initial_delay_max" : "${initial_delay}",
        "repetitions_base_delay" : "${repetition_base}",
        "repetitions_max" : "${repetitions}",
        "ttl" : "3",
        "cyclic_offer_delay" : "${cyclic_offer}"
    }
}
EOF

echo "gen-config.sh: wrote ${output} (role=${role}, transport=${transport}, unicast=${unicast}, instances=${instances})"
