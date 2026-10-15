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

# Package the source for a machine that has no git checkout, for example a PC
# that reaches the benchmark hosts when the development server cannot.
#
#   make-bundle.sh [-o <output directory>]
#
# The archive holds the files git tracks plus untracked files under
# performance-measurement/ (except results/), and a REVISION file with the git
# revision, which the build records in every result row.
# Default output: <repo>/performance-measurement/results/bundle/ (ignored by git).

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "${script_dir}/../../.." && pwd)"
output_dir="${repo}/performance-measurement/results/bundle"

while getopts "o:h" opt; do
    case "${opt}" in
        o) output_dir="${OPTARG}" ;;
        *) sed -n '15,/^set -euo/p' "$0" | sed '$d'; exit 1 ;;
    esac
done

revision="$(git -C "${repo}" describe --always --dirty)"
mkdir -p "${output_dir}"
archive="${output_dir}/lg-someip-perf-${revision}.tar.gz"
staging="$(mktemp -d)"
trap 'rm -rf "${staging}"' EXIT

{
    git -C "${repo}" ls-files --cached
    git -C "${repo}" ls-files --others --exclude-standard performance-measurement
} | sort -u > "${staging}/files"
echo "${revision}" > "${staging}/REVISION"

tar -C "${repo}" -czf "${archive}" --transform 's,^,lg-someip/,' -T "${staging}/files" \
    -C "${staging}" --transform 's,^REVISION$,lg-someip/REVISION,' REVISION
echo "make-bundle.sh: wrote ${archive} ($(wc -l < "${staging}/files") files, revision ${revision})"
