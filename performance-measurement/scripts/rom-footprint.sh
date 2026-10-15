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

# Storage (ROM) footprint of one deployment role, from stripped copies.
#
#   rom-footprint.sh [-i <regex>] <label>=<file>[,<file>...] ...
#
#   <label>   a name for the role, for example lgsomeip-consumer
#   <file>    an executable or shared library that role deploys. Name every
#             file of ONE ECU role: for a consumer that is the routing daemon,
#             the stack's shared library, and the client application, not the
#             server application as well.
#   -i <re>   also add every shared library that ldd resolves for the listed
#             files and whose path matches <re> (extended regular expression),
#             for example 'boost|vsomeip|lgsomeip', so a stack's third-party
#             dependencies are counted. Libraries outside the match are listed
#             as "not counted" so the omission is visible.
#
# A role that links the stack statically (opensomeip) names only its
# application; a role that uses a daemon names the daemon and the shared
# library once, however many applications run on that ECU. Sizes are KB
# (bytes / 1024) of copies stripped with `strip --strip-unneeded`; the
# originals are not touched.

set -euo pipefail

include_re=""
while getopts "i:h" opt; do
    case "${opt}" in
        i) include_re="${OPTARG}" ;;
        *) sed -n '15,/^set -euo/p' "$0" | sed '$d'; exit 1 ;;
    esac
done
shift $((OPTIND - 1))
if (($# == 0)); then
    sed -n '15,/^set -euo/p' "$0" | sed '$d'
    exit 1
fi

workdir="$(mktemp -d)"
trap 'rm -rf "${workdir:?}"' EXIT

stripped_size() {
    # stripped_size <file>: size in bytes of a stripped copy
    local copy="${workdir}/copy"
    cp -L "$1" "${copy}"
    strip --strip-unneeded "${copy}" 2>/dev/null || true
    stat -c%s "${copy}"
    rm -f "${copy}"
}

for spec in "$@"; do
    label="${spec%%=*}"
    IFS=, read -r -a files <<< "${spec#*=}"
    declare -A seen=()
    total=0
    echo "== ${label}"
    for file in "${files[@]}"; do
        [[ -f "${file}" ]] || { echo "  missing: ${file}" >&2; exit 1; }
        candidates=("${file}")
        if [[ -n "${include_re}" ]]; then
            while read -r library; do
                candidates+=("${library}")
            done < <(ldd "${file}" 2>/dev/null | awk '/=> \// { print $3 }' | sort -u)
        fi
        for candidate in "${candidates[@]}"; do
            real="$(readlink -f "${candidate}")"
            # A role deploys one copy of each library, even when two of its
            # binaries resolve it from different directories (build tree and
            # install prefix).
            name="$(basename "${real}")"
            [[ -z "${seen[${name}]:-}" ]] || continue
            seen[${name}]=1
            if [[ "${candidate}" != "${file}" && ! "${real}" =~ ${include_re} ]]; then
                printf '  %-60s not counted (system library)\n' "$(basename "${real}")"
                continue
            fi
            bytes="$(stripped_size "${real}")"
            total=$((total + bytes))
            printf '  %-60s %10.1f KB\n' "$(basename "${real}")" "$(awk "BEGIN { print ${bytes} / 1024 }")"
        done
    done
    printf '  %-60s %10.1f KB\n' "total (stripped)" "$(awk "BEGIN { print ${total} / 1024 }")"
    unset seen
done
