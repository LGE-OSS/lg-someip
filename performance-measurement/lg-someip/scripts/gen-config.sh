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

# Generate an lg-someip configuration for the performance applications.
#
#   gen-config.sh -o <file> [-u <unicast>] [-t udp|tcp] [-r local|provider|consumer] [-l <log level>]
#                 [-I <initial delay ms>] [-R <repetition base delay ms>] [-M <repetitions>]
#                 [-C <cyclic offer delay ms>] [-i <instances>]
#
#   local     both provider and consumer run on this host (default)
#   provider  this host offers the benchmark services (remote setup)
#   consumer  this host only consumes them (remote setup)
#
#   -i  instances 1..N of the RPC (0x2001) and event (0x2002) services, for
#       peer measurements with several responders or publishers (default 1).
#       Instance 1 keeps the ports above; instance k >= 2 uses 31000+k (RPC)
#       and 31200+k (events) for UDP, 31100+k and 31300+k for TCP.
#
# The transport selects which endpoint the services declare. lg-someip prefers
# the reliable (TCP) endpoint when one is configured, so only one is emitted.

set -euo pipefail

output=""
unicast="127.0.0.1"
transport="udp"
role="local"
log_level="warning"
# Service Discovery timing. Only these four keys change lg-someip's behavior:
# the initial delay is always initial_delay_min (initial_delay_max is not
# used), and the offer TTL is fixed at 3 regardless of the "ttl" key.
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
    local|provider) is_provider="true" ;;
    consumer) is_provider="false" ;;
    *) echo "gen-config.sh: unknown role '${role}'" >&2; exit 1 ;;
esac

if [[ "${unicast}" == *:* ]]; then
    iptype="IPv6"
else
    iptype="IPv4"
fi

extra_services=""
for ((k = 2; k <= instances; k++)); do
    extra_services+="
        {
            \"service\" : \"0x2001\",
            \"instance\" : \"$(printf '0x%04x' "${k}")\",
            \"is-provider\" : \"${is_provider}\",
            $(endpoint $((31000 + k)) $((31100 + k)))
        },
        {
            \"service\" : \"0x2002\",
            \"instance\" : \"$(printf '0x%04x' "${k}")\",
            \"is-provider\" : \"${is_provider}\",
            $(endpoint $((31200 + k)) $((31300 + k))),
            \"events\" :
            [
                { \"event\" : \"0x8001\", \"is_field\" : \"false\", \"update-cycle\" : \"0\" },
                { \"event\" : \"0x8002\", \"is_field\" : \"false\", \"update-cycle\" : \"0\" }
            ],
            \"eventgroups\" :
            [
                { \"eventgroup\" : \"0x0001\", \"events\" : [ \"0x8001\", \"0x8002\" ] }
            ]
        },"
done

# Application IDs: 0x03xx RPC, 0x04xx events, 0x05xx discovery probes.
# Services: 0x2001 RPC (method 0x0001), 0x2002 events (group 0x0001 with data
# event 0x8001 and end marker 0x8002), 0x2003 SD probe (field 0x8001, heartbeat
# 0x8002, stopping 0x8003). update-cycle 0 sends on every notify().
# Several client names exist so multiple consumers can run at the same time.
cat > "${output}" <<EOF
{
    "unicast" : "${unicast}",
    "iptype" : "${iptype}",
    "logging" :
    {
        "level" : "${log_level}"
    },
    "applications" :
    [
        { "name" : "perf-rpc-server",   "id" : "0x0311" },
        { "name" : "perf-rpc-server-2", "id" : "0x0312" },
        { "name" : "perf-rpc-server-3", "id" : "0x0313" },
        { "name" : "perf-rpc-server-4", "id" : "0x0314" },
        { "name" : "perf-rpc-server-5", "id" : "0x0315" },
        { "name" : "perf-rpc-server-6", "id" : "0x0316" },
        { "name" : "perf-rpc-server-7", "id" : "0x0317" },
        { "name" : "perf-rpc-server-8", "id" : "0x0318" },
        { "name" : "perf-rpc-client",   "id" : "0x0321" },
        { "name" : "perf-rpc-client-2", "id" : "0x0322" },
        { "name" : "perf-rpc-client-3", "id" : "0x0323" },
        { "name" : "perf-rpc-client-4", "id" : "0x0324" },
        { "name" : "perf-rpc-client-5", "id" : "0x0325" },
        { "name" : "perf-rpc-client-6", "id" : "0x0326" },
        { "name" : "perf-rpc-client-7", "id" : "0x0327" },
        { "name" : "perf-rpc-client-8", "id" : "0x0328" },
        { "name" : "perf-event-pub",    "id" : "0x0411" },
        { "name" : "perf-event-pub-2",  "id" : "0x0412" },
        { "name" : "perf-event-pub-3",  "id" : "0x0413" },
        { "name" : "perf-event-pub-4",  "id" : "0x0414" },
        { "name" : "perf-event-pub-5",  "id" : "0x0415" },
        { "name" : "perf-event-pub-6",  "id" : "0x0416" },
        { "name" : "perf-event-pub-7",  "id" : "0x0417" },
        { "name" : "perf-event-pub-8",  "id" : "0x0418" },
        { "name" : "perf-event-sub-1",  "id" : "0x0421" },
        { "name" : "perf-event-sub-2",  "id" : "0x0422" },
        { "name" : "perf-event-sub-3",  "id" : "0x0423" },
        { "name" : "perf-event-sub-4",  "id" : "0x0424" },
        { "name" : "perf-event-sub-5",  "id" : "0x0425" },
        { "name" : "perf-event-sub-6",  "id" : "0x0426" },
        { "name" : "perf-event-sub-7",  "id" : "0x0427" },
        { "name" : "perf-event-sub-8",  "id" : "0x0428" },
        { "name" : "perf-sd-provider",  "id" : "0x0511" },
        { "name" : "perf-sd-probe",     "id" : "0x0521" }
    ],
    "services" :
    [${extra_services}
        {
            "service" : "0x2001",
            "instance" : "0x0001",
            "is-provider" : "${is_provider}",
            ${rpc_endpoint}
        },
        {
            "service" : "0x2002",
            "instance" : "0x0001",
            "is-provider" : "${is_provider}",
            ${event_endpoint},
            "events" :
            [
                { "event" : "0x8001", "is_field" : "false", "update-cycle" : "0" },
                { "event" : "0x8002", "is_field" : "false", "update-cycle" : "0" }
            ],
            "eventgroups" :
            [
                { "eventgroup" : "0x0001", "events" : [ "0x8001", "0x8002" ] }
            ]
        },
        {
            "service" : "0x2003",
            "instance" : "0x0001",
            "is-provider" : "${is_provider}",
            ${sd_endpoint},
            "events" :
            [
                { "event" : "0x8001", "is_field" : "true", "update-cycle" : "0" },
                { "event" : "0x8002", "is_field" : "false", "update-cycle" : "0" },
                { "event" : "0x8003", "is_field" : "false", "update-cycle" : "0" }
            ],
            "eventgroups" :
            [
                { "eventgroup" : "0x0001", "events" : [ "0x8001", "0x8002", "0x8003" ] }
            ]
        }
    ],
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
        "cyclic_offer_delay" : "${cyclic_offer}",
        "request_response_delay" : "1500"
    }
}
EOF

echo "gen-config.sh: wrote ${output} (role=${role}, transport=${transport}, unicast=${unicast}, instances=${instances})"
