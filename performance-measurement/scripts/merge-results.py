#!/usr/bin/env python3
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

"""Merge the result directories of the lg-someip, vsomeip, and opensomeip
two-host runs into unified CSV files and one Markdown comparison.

    merge-results.py --out <dir> lgsomeip=<run>[,<run>...] vsomeip=<run>[,...] opensomeip=<run>[,...]

Each <run> is a results directory written by a run-two-hosts.sh coordinator
(or any directory tree that holds the benchmark CSV files). Several runs of one
stack are repetitions: every table cell shows the median and, in brackets, the
range over the repetitions. Rows are classified by their `scenario` column, so
the directory layout does not matter. Rows labelled `local*` are skipped unless
--include-local is given.

The tables are generated, never typed in: rerun this script after any new
measurement instead of editing numbers by hand.
"""

import argparse
import csv
import glob
import os
import re
import statistics
import sys

STACK_ORDER = ["lgsomeip", "vsomeip", "opensomeip"]
SCENARIOS = ("rpc-pingpong", "rpc-rate", "event-publish", "event-subscribe", "rpc-threads")


def to_number(text):
    try:
        return float(text)
    except (TypeError, ValueError):
        return None


def read_rows(run_dir, stack, run_index):
    """Yield every benchmark CSV row below run_dir as a dict."""
    for path in sorted(glob.glob(os.path.join(run_dir, "**", "*.csv"), recursive=True)):
        with open(path, newline="") as handle:
            for row in csv.DictReader(handle):
                if row.get("scenario") not in SCENARIOS:
                    continue
                row = dict(row)
                # The stack=dir label of the command line wins over the CSV's own
                # "stack" column: the same applications (e.g. the vsomeip ones)
                # can run on different backends.
                row["stack"] = stack
                row["run"] = str(run_index)
                row["source"] = os.path.relpath(path, run_dir)
                row["run_dir"] = run_dir
                yield row


def parse_key_values(path):
    values = {}
    if not os.path.isfile(path):
        return values
    with open(path) as handle:
        for line in handle:
            if "=" in line:
                key, _, value = line.strip().partition("=")
                values[key] = value
    return values


def parse_summary_block(path):
    """Read the 'name : value' lines an application prints at exit."""
    values = {}
    if not os.path.isfile(path):
        return values
    with open(path, errors="replace") as handle:
        for line in handle:
            match = re.match(r"^\s+(\w+)\s+:\s+(\S+)\s*$", line)
            if match:
                values[match.group(1)] = to_number(match.group(2))
    return values


def cell(values, digits=1):
    """Median of the repetitions, with the range in brackets when it varies."""
    values = [v for v in values if v is not None]
    if not values:
        return "n/a"
    median = statistics.median(values)
    text = f"{median:,.{digits}f}"
    if len(values) > 1:
        low, high = min(values), max(values)
        text += f" ({low:,.{digits}f}–{high:,.{digits}f}; n={len(values)})"
    return text


def usable(rows):
    """Rows of successful runs, and a note for the failed ones."""
    ok = [row for row in rows if not row.get("_failed")]
    failed = len(rows) - len(ok)
    return ok, (f" [{failed} failed run(s) excluded]" if failed else "")


def column(rows, name):
    return [to_number(row.get(name)) for row in rows]


def cpu_total(row):
    """cpu_total_ms, or user+system for files written before it existed."""
    total = to_number(row.get("cpu_total_ms"))
    if total is not None:
        return total
    user, system = to_number(row.get("cpu_user_ms")), to_number(row.get("cpu_sys_ms"))
    return None if user is None or system is None else user + system


def table(header, body):
    lines = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    lines += ["| " + " | ".join(row) + " |" for row in body]
    return "\n".join(lines) + "\n"


def group(rows, key):
    result = {}
    for row in rows:
        result.setdefault(key(row), []).append(row)
    return result


def sort_number(value):
    number = to_number(value)
    return (0, number) if number is not None else (1, str(value))


class Comparison:
    def __init__(self, runs, include_local):
        self.runs = runs  # stack -> [dir, ...]
        self.rows = []
        # Rows of the peer sweeps (run-peers.sh): labelled peers-r<N>-s<M> (RPC)
        # or peers-p<M>-s<N> (events), reported in their own tables.
        self.peer_rows = []
        # Rows of the multi-threaded requester (labelled threads-t<T>-s<M>-h<rate>).
        self.thread_rows = []
        for stack, dirs in runs.items():
            for index, run_dir in enumerate(dirs, start=1):
                for row in read_rows(run_dir, stack, index):
                    label = row.get("label", "")
                    if not include_local and label.startswith("local"):
                        continue
                    # A run that received nothing (all requests lost, or it stalled)
                    # has no latency; keep it out of the statistics, but report it.
                    row["_failed"] = row["scenario"].startswith("rpc") and (to_number(row.get("samples")) or 0) == 0
                    if label.startswith("peers-"):
                        self.peer_rows.append(row)
                    elif row["scenario"] == "rpc-threads":
                        self.thread_rows.append(row)
                    else:
                        self.rows.append(row)
        self.stacks = [s for s in STACK_ORDER if s in runs] + [s for s in runs if s not in STACK_ORDER]

    def select(self, scenario):
        return [row for row in self.rows if row["scenario"] == scenario]

    def by_stack(self, rows):
        grouped = group(rows, lambda row: row["stack"])
        return {stack: grouped.get(stack, []) for stack in self.stacks}

    # ----------------------------------------------------------------- context
    def context(self):
        header = ["Stack", "Run", "Revision", "Transport", "Library", "Kernel A / B", "PTP A / B", "rmem_max A / B"]
        body = []
        for stack in self.stacks:
            for index, run_dir in enumerate(self.runs[stack], start=1):
                info = parse_key_values(os.path.join(run_dir, "run-info.txt"))
                versions = {}
                for side in "AB":
                    versions.update(parse_key_values(os.path.join(run_dir, f"{side}-versions.txt")))
                info_path = os.path.join(run_dir, "run-info.txt")
                text = open(info_path).read() if os.path.isfile(info_path) else ""
                match = re.search(r"transport=(\w+)", text)
                transport = match.group(1) if match else "n/a"
                a = parse_key_values(os.path.join(run_dir, "A-sysinfo-before.txt"))
                b = parse_key_values(os.path.join(run_dir, "B-sysinfo-before.txt"))
                body.append([
                    stack, str(index), info.get("revision", "n/a"), transport,
                    ", ".join(f"{k}={v}" for k, v in sorted(versions.items())) or "n/a",
                    f"{a.get('kernel', 'n/a')} / {b.get('kernel', 'n/a')}",
                    f"{a.get('ptp4l_running', 'n/a')} / {b.get('ptp4l_running', 'n/a')}",
                    f"{a.get('rmem_max', 'n/a')} / {b.get('rmem_max', 'n/a')}",
                ])
        return table(header, body)

    def warnings(self):
        notes = []
        kernels, rmem = set(), set()
        for stack in self.stacks:
            for run_dir in self.runs[stack]:
                for side in "AB":
                    info = parse_key_values(os.path.join(run_dir, f"{side}-sysinfo-before.txt"))
                    if info:
                        kernels.add((side, info.get("kernel")))
                        rmem.add((side, info.get("rmem_max")))
        for name, values in (("kernel", kernels), ("rmem_max", rmem)):
            a = {v for side, v in values if side == "A"}
            b = {v for side, v in values if side == "B"}
            if len(a) > 1 or len(b) > 1:
                notes.append(f"{name} differs between runs of the same host ({sorted(a | b)}); stacks were not "
                             "measured under identical host conditions.")
            elif a and b and a != b:
                notes.append(f"host A and B differ in {name} (A={sorted(a)}, B={sorted(b)}); both roles are the "
                             "same for every stack, so relative results are less affected, but absolute ones are.")
        coverage = {}
        for stack in self.stacks:
            for run_dir in self.runs[stack]:
                keys = {(r["scenario"], r.get("payload_bytes"), r.get("target_rate_msg_s")) for r in self.rows
                        if r["run_dir"] == run_dir and r["stack"] == stack}
                coverage.setdefault(frozenset(keys), []).append(f"{stack}:{os.sep.join(run_dir.rstrip('/').split('/')[-2:])}")
        if len(coverage) > 1:
            notes.append("the runs did not cover the same scenarios, payloads and rates. Daemon CPU deltas span "
                         "every scenario of a run, so compare them only between runs with identical coverage. "
                         "Groups with identical coverage: " + "; ".join(", ".join(v) for v in coverage.values()) + ".")
        counts = {len(self.runs[s]) for s in self.stacks}
        if counts == {1}:
            notes.append("every stack has a single run; differences of a few percent are within run-to-run noise "
                         "until repetitions confirm them.")
        elif len(counts) > 1:
            notes.append("stacks have different numbers of repetitions: " +
                         ", ".join(f"{s}={len(self.runs[s])}" for s in self.stacks) + ".")
        return notes

    # --------------------------------------------------------------------- rpc
    def rpc_pingpong(self):
        rows = self.select("rpc-pingpong")
        payloads = sorted({r["payload_bytes"] for r in rows}, key=sort_number)
        header = ["Payload (B)"] + [f"{s} p50 / p99 (µs), lost" for s in self.stacks]
        body = []
        for payload in payloads:
            line = [payload]
            for stack in self.stacks:
                cur = [r for r in rows if r["stack"] == stack and r["payload_bytes"] == payload]
                if not cur:
                    line.append("n/a")
                    continue
                ok, note = usable(cur)
                lost = sum(v or 0 for v in column(ok, "lost"))
                line.append(f"{cell(column(ok, 'p50_us'))} / {cell(column(ok, 'p99_us'))}, {lost:.0f}{note}")
            body.append(line)
        return table(header, body)

    def rpc_rate(self, saturation):
        rows = [r for r in self.select("rpc-rate") if (to_number(r.get("target_rate_msg_s")) == 0) == saturation]
        keys = sorted({(r["payload_bytes"], r["target_rate_msg_s"]) for r in rows},
                      key=lambda k: (sort_number(k[0]), sort_number(k[1])))
        if saturation:
            header = ["Payload (B)"] + [f"{s} throughput (msg/s), loss %" for s in self.stacks]
        else:
            header = ["Payload (B)", "Rate"] + [f"{s} p50 / p99 (µs), loss %" for s in self.stacks]
        body = []
        for payload, rate in keys:
            line = [payload] if saturation else [payload, f"{to_number(rate):,.0f}"]
            for stack in self.stacks:
                cur = [r for r in rows if r["stack"] == stack and r["payload_bytes"] == payload and
                       r["target_rate_msg_s"] == rate]
                if not cur:
                    line.append("n/a")
                    continue
                ok, note = usable(cur)
                if saturation:
                    line.append(f"{cell(column(ok, 'throughput_msg_s'), 0)}, {cell(column(ok, 'loss_pct'), 2)}{note}")
                else:
                    line.append(f"{cell(column(ok, 'p50_us'))} / {cell(column(ok, 'p99_us'))}, "
                                f"{cell(column(ok, 'loss_pct'), 2)}{note}")
            body.append(line)
        return table(header, body)

    # --------------------------------------------------------------- resources
    def daemon_delta(self, run_dir, side):
        before = parse_key_values(os.path.join(run_dir, f"{side}-daemon-usage-before.txt"))
        after = parse_key_values(os.path.join(run_dir, f"{side}-daemon-usage-after.txt"))
        cpu = None
        if before.get("daemon_cpu_ms") and after.get("daemon_cpu_ms"):
            cpu = float(after["daemon_cpu_ms"]) - float(before["daemon_cpu_ms"])
        source = after.get("daemon_cpu_source", "ticks (10 ms steps)")
        peak = to_number(after.get("daemon_peak_rss_kb"))
        return cpu, peak, source

    def client_resources(self, payload, rate):
        """Client CPU and memory of the paced RPC run of one payload and rate."""
        rows = [r for r in self.select("rpc-rate") if r["payload_bytes"] == payload and
                to_number(r.get("target_rate_msg_s")) == rate]
        header = ["Stack", "Client cpu_net ms", "Client cpu_pacing ms", "Client peak RSS KB"]
        body = []
        for stack in self.stacks:
            cur, _ = usable([r for r in rows if r["stack"] == stack])
            if not cur:
                continue
            net = [to_number(r.get("cpu_net_ms")) if r.get("cpu_net_ms") not in (None, "") else None for r in cur]
            body.append([stack, cell(net, 1), cell(column(cur, "cpu_pacing_ms"), 1), cell(column(cur, "max_rss_kb"), 0)])
        return table(header, body)

    def run_resources(self):
        """Server and daemon cost over everything a run did (all payloads and rates)."""
        header = ["Stack", "Server CPU per request (µs)", "Server lifetime CPU ms", "Server peak RSS KB",
                  "Daemon CPU ms A / B", "Daemon peak RSS KB A / B"]
        body = []
        for stack in self.stacks:
            per_request, lifetime, server_rss = [], [], []
            dcpu_a, dcpu_b, drss_a, drss_b = [], [], [], []
            for run_dir in self.runs[stack]:
                for log in glob.glob(os.path.join(run_dir, "**", "*rpc-server*.log"), recursive=True):
                    summary = parse_summary_block(log)
                    served = summary.get("requests_served")
                    if not served:
                        continue
                    total = summary.get("cpu_total_ms")
                    if total is None and summary.get("cpu_sys_ms") is not None:
                        total = (summary.get("cpu_user_ms") or 0) + summary["cpu_sys_ms"]
                    if total is not None and "lifetime_cpu_ms" in summary:
                        per_request.append(total * 1000.0 / served)
                    lifetime.append(summary.get("lifetime_cpu_ms", total))
                    server_rss.append(summary.get("max_rss_kb"))
                for side, cpu_list, rss_list in (("A", dcpu_a, drss_a), ("B", dcpu_b, drss_b)):
                    cpu, peak, _ = self.daemon_delta(run_dir, side)
                    if cpu is not None:
                        cpu_list.append(cpu)
                        rss_list.append(peak)
            has_daemon = bool(dcpu_a or dcpu_b)
            body.append([
                stack, cell(per_request, 1), cell(lifetime, 0), cell(server_rss, 0),
                f"{cell(dcpu_a, 0)} / {cell(dcpu_b, 0)}" if has_daemon else "no daemon",
                f"{cell(drss_a, 0)} / {cell(drss_b, 0)}" if has_daemon else "no daemon",
            ])
        return table(header, body)

    # ------------------------------------------------------------------ events
    @staticmethod
    def clocks_synchronized(run_dir):
        """Both hosts ran ptp4l (a necessary, not sufficient, condition)."""
        flags = [parse_key_values(os.path.join(run_dir, f"{side}-sysinfo-before.txt")).get("ptp4l_running")
                 for side in "AB"]
        return all(flag == "yes" for flag in flags)

    @staticmethod
    def subscriber_count(row):
        """Subscriber count of an event row, from its label (...-<n>sub)."""
        match = re.search(r"(\d+)sub", row.get("label", ""))
        return match.group(1) if match else "1"

    def events(self):
        publish = self.select("event-publish")
        subscribe = self.select("event-subscribe")
        keys = sorted({(self.subscriber_count(r), r["payload_bytes"], r["target_rate_msg_s"]) for r in subscribe} |
                      {(self.subscriber_count(r), r["payload_bytes"], r["target_rate_msg_s"]) for r in publish},
                      key=lambda k: (sort_number(k[0]), sort_number(k[1]), sort_number(k[2])))
        header = ["Subscribers", "Payload (B)", "Rate"]
        for stack in self.stacks:
            header += [f"{stack} received / lost (per subscriber)", f"{stack} notify p50 / p99 (µs)",
                       f"{stack} one-way p50"]
        body = []
        for subscribers, payload, rate in keys:
            line = [subscribers, payload, f"{to_number(rate):,.0f}"]
            for stack in self.stacks:
                def match(r):
                    return (r["stack"] == stack and r["payload_bytes"] == payload and
                            r["target_rate_msg_s"] == rate and self.subscriber_count(r) == subscribers)
                subs = [r for r in subscribe if match(r)]
                pubs = [r for r in publish if match(r)]
                if not subs and not pubs:
                    line += ["n/a", "n/a", "n/a"]
                    continue
                pcell = "n/a"
                for p50, p99 in (("notify_p50_us", "notify_p99_us"), ("publish_p50_us", "publish_p99_us")):
                    if pubs and pubs[0].get(p50):
                        pcell = f"{cell(column(pubs, p50), 1)} / {cell(column(pubs, p99), 1)}"
                if not subs:
                    # The publisher ran but no subscriber wrote a row: it received no notification at all.
                    line += ["no data (subscriber received nothing)", pcell, "n/a"]
                    continue
                expected = cell(column(subs, "expected"), 0)
                lost = cell(column(subs, "lost"), 0)
                negative = sum(v or 0 for v in column(subs, "negative_latency"))
                p50_us = column(subs, "p50_us")
                clock = subs[0].get("clock", "")
                synced = all(self.clocks_synchronized(r["run_dir"]) for r in subs)
                if clock != "realtime":
                    one_way = f"{cell(p50_us)} µs (monotonic: same host only)"
                elif negative > 0 or not synced:
                    one_way = "invalid: hosts not PTP-synchronized"
                else:
                    one_way = f"{cell(p50_us)} µs"
                line += [f"{expected} / {lost}", pcell, one_way]
            body.append(line)
        return table(header, body)


PEER_RPC_LABEL = re.compile(r"^peers-r(\d+)-s(\d+)$")
PEER_EVENT_LABEL = re.compile(r"^peers-p(\d+)-s(\d+)$")


def read_samples(paths):
    """Every latency sample (µs) of the given samples files, pooled."""
    values = []
    for path in paths:
        with open(path) as handle:
            values.extend(float(line) for line in handle if line.strip())
    return values


def pooled_percentile(values, pct):
    """Nearest-rank percentile, as in LatencyStats.h."""
    if not values:
        return None
    ordered = sorted(values)
    rank = max(1, int(-(-pct * len(ordered) // 100)))
    return ordered[min(rank, len(ordered)) - 1]


class PeerSweep:
    """Tables of the peer sweeps: one step is N requesters x M responders (RPC)
    or M publishers x N subscribers (events). Within one run, the rows of all
    peers of a step are combined: throughput and loss are summed, latency
    percentiles come from the pooled samples of every peer. Across runs (the
    repetitions) each cell shows the median and the range, as elsewhere."""

    def __init__(self, comparison):
        self.c = comparison

    def steps(self, rows, pattern):
        keys = set()
        for row in rows:
            match = pattern.match(row.get("label", ""))
            if match:
                keys.add((int(match.group(1)), int(match.group(2)), row.get("payload_bytes")))
        return sorted(keys, key=lambda k: (sort_number(k[2]), k[0], k[1]))

    @staticmethod
    def samples_dir(row):
        return os.path.join(row["run_dir"], os.path.dirname(row["source"]))

    def rpc(self, scenario):
        rows = [r for r in self.c.peer_rows if r["scenario"] == scenario]
        if not rows:
            return ""
        mode = "pingpong" if scenario == "rpc-pingpong" else "rate"
        rate_column = "rate_msg_s" if mode == "pingpong" else "throughput_msg_s"
        header = ["Requesters", "Responders", "Payload (B)"]
        for stack in self.c.stacks:
            header += [f"{stack} p50 / p99 (µs)", f"{stack} total msg/s", f"{stack} loss %"]
        body = []
        for n, m, payload in self.steps(rows, PEER_RPC_LABEL):
            label = f"peers-r{n}-s{m}"
            line = [str(n), str(m), str(payload)]
            for stack in self.c.stacks:
                p50, p99, total, loss, notes = [], [], [], [], set()
                for run_dir in self.c.runs.get(stack, []):
                    cur = [r for r in rows if r["stack"] == stack and r["run_dir"] == run_dir and
                           r["label"] == label and r.get("payload_bytes") == payload]
                    if not cur:
                        continue
                    if len(cur) < n:
                        notes.add(f"{len(cur)}/{n} requesters reported")
                    if any((to_number(r.get("start_late_ms")) or 0) > 50 for r in cur):
                        notes.add("late start")
                    files = glob.glob(os.path.join(self.samples_dir(cur[0]),
                                                   f"peer-rpc-{mode}-p{payload}-r{n}-s{m}-c*.samples"))
                    pooled = read_samples(files)
                    p50.append(pooled_percentile(pooled, 50))
                    p99.append(pooled_percentile(pooled, 99))
                    total.append(sum(to_number(r.get(rate_column)) or 0 for r in cur))
                    sent = sum(to_number(r.get("sent")) or 0 for r in cur)
                    lost = sum(to_number(r.get("lost")) or 0 for r in cur)
                    loss.append(100.0 * lost / sent if sent else None)
                note = f" [{'; '.join(sorted(notes))}]" if notes else ""
                line += [f"{cell(p50)} / {cell(p99)}", cell(total, 0), cell(loss, 2) + note]
            body.append(line)
        return table(header, body)

    def events(self):
        subscribe = [r for r in self.c.peer_rows if r["scenario"] == "event-subscribe"]
        publish = [r for r in self.c.peer_rows if r["scenario"] == "event-publish"]
        if not subscribe and not publish:
            return ""
        header = ["Publishers", "Subscribers", "Payload (B)"]
        for stack in self.c.stacks:
            header += [f"{stack} one-way p50 / p99 (µs)", f"{stack} received", f"{stack} loss %",
                       f"{stack} notify p99 (µs)"]
        body = []
        for m, n, payload in self.steps(subscribe + publish, PEER_EVENT_LABEL):
            label = f"peers-p{m}-s{n}"
            line = [str(m), str(n), str(payload)]
            for stack in self.c.stacks:
                p50, p99, received, loss, notify, notes = [], [], [], [], [], set()
                for run_dir in self.c.runs.get(stack, []):
                    subs = [r for r in subscribe if r["stack"] == stack and r["run_dir"] == run_dir and
                            r["label"] == label and r.get("payload_bytes") == payload]
                    pubs = [r for r in publish if r["stack"] == stack and r["run_dir"] == run_dir and
                            r["label"] == label and r.get("payload_bytes") == payload]
                    if not subs and not pubs:
                        continue
                    if pubs and any((to_number(r.get("start_late_ms")) or 0) > 50 for r in pubs):
                        notes.add("late start")
                    if pubs:
                        call = [to_number(r.get("notify_p99_us") or r.get("publish_p99_us")) for r in pubs]
                        call = [v for v in call if v is not None]
                        notify.append(max(call) if call else None)
                    if len(subs) < n:
                        notes.add(f"{len(subs)}/{n} subscribers reported (the others received nothing)")
                    if any((to_number(r.get("publishers")) or 0) < m for r in subs):
                        notes.add("some subscribers missed a publisher entirely")
                    if not subs:
                        continue
                    synced = Comparison.clocks_synchronized(run_dir)
                    negative = sum(to_number(r.get("negative_latency")) or 0 for r in subs)
                    if synced and negative == 0:
                        files = glob.glob(os.path.join(self.samples_dir(subs[0]),
                                                       f"peer-event-p{payload}-p{m}-s{n}-sub*.samples"))
                        pooled = read_samples(files)
                        p50.append(pooled_percentile(pooled, 50))
                        p99.append(pooled_percentile(pooled, 99))
                    else:
                        notes.add("one-way latency invalid: hosts not PTP-synchronized")
                    expected = sum(to_number(r.get("expected")) or 0 for r in subs)
                    lost = sum(to_number(r.get("lost")) or 0 for r in subs)
                    received.append(expected - lost)
                    loss.append(100.0 * lost / expected if expected else None)
                note = f" [{'; '.join(sorted(notes))}]" if notes else ""
                line += [f"{cell(p50)} / {cell(p99)}", cell(received, 0), cell(loss, 2) + note, cell(notify, 1)]
            body.append(line)
        return table(header, body)


class NetUsage:
    """Traffic on the link between the hosts per message, from the nftables
    counters that run-peers.sh -w records (net-usage.csv in each run). Bytes are
    IP packet lengths. Cells show the median over the repetitions, and the
    factor against the same stack with one client."""

    def __init__(self, comparison):
        self.c = comparison
        self.rows = []
        for stack, dirs in comparison.runs.items():
            for run_dir in dirs:
                path = os.path.join(run_dir, "net-usage.csv")
                if os.path.isfile(path):
                    with open(path, newline="") as handle:
                        for row in csv.DictReader(handle):
                            row["stack"] = stack
                            self.rows.append(row)

    def per_message(self, stack, scenario, label, payload, fields_by_host):
        """Median over runs of sum(field)/messages; fields_by_host = [(host, field), ...]."""
        values = []
        # Rows of one step come in host pairs (A, B) written one after the other.
        pairs = [r for r in self.rows if (r["stack"], r["scenario"], r["label"], r["payload_bytes"]) ==
                 (stack, scenario, label, payload)]
        for i in range(0, len(pairs) - 1, 2):
            by_host = {pairs[i]["host"]: pairs[i], pairs[i + 1]["host"]: pairs[i + 1]}
            messages = to_number(pairs[i].get("messages")) or 0
            if not messages or set(by_host) != {"A", "B"}:
                continue
            if fields_by_host == "per-packet":
                # Bytes per packet in each direction, summed: one request plus one response
                # on the wire. Unlike dividing by the planned count, loss does not distort it.
                b = by_host["B"]
                out_p, in_p = to_number(b.get("data_out_packets")) or 0, to_number(b.get("data_in_packets")) or 0
                if out_p and in_p:
                    values.append((to_number(b.get("data_out_bytes")) or 0) / out_p +
                                  (to_number(b.get("data_in_bytes")) or 0) / in_p)
                continue
            total = sum(to_number(by_host[h].get(f)) or 0 for h, f in fields_by_host)
            values.append(total / messages)
        return statistics.median(values) if values else None

    def table(self, scenario, pattern, count_index, fields, unit_fn):
        rows = [r for r in self.rows if r["scenario"] == scenario]
        keys = sorted({(int(pattern.match(r["label"]).group(count_index)), r["payload_bytes"]) for r in rows
                       if pattern.match(r["label"])}, key=lambda k: (sort_number(k[1]), k[0]))
        if not keys:
            return ""
        header = ["Clients", "Payload (B)"] + [f"{s}" for s in self.c.stacks]
        body = []
        for count, payload in keys:
            line = [str(count), str(payload)]
            for stack in self.c.stacks:
                labels = {r["label"] for r in rows if r["stack"] == stack and pattern.match(r["label"]) and
                          int(pattern.match(r["label"]).group(count_index)) == count and r["payload_bytes"] == payload}
                if not labels:
                    line.append("n/a")
                    continue
                label = labels.pop()
                value = self.per_message(stack, scenario, label, payload, fields)
                base_labels = {r["label"] for r in rows if r["stack"] == stack and pattern.match(r["label"]) and
                               int(pattern.match(r["label"]).group(count_index)) == 1 and r["payload_bytes"] == payload}
                base = self.per_message(stack, scenario, base_labels.pop(), payload, fields) if base_labels else None
                if value is None:
                    line.append("n/a")
                else:
                    factor = f" ({value / base:.1f}×)" if base else ""
                    line.append(unit_fn(value) + factor)
            body.append(line)
        return table(header, body)

    def event_table(self):
        # Host B receives the notifications: data bytes and packets in, per published notification.
        return self.table("event", PEER_EVENT_LABEL, 2, [("B", "data_in_bytes")],
                          lambda v: f"{v:,.0f} B")

    def event_packets_table(self):
        return self.table("event", PEER_EVENT_LABEL, 2, [("B", "data_in_packets")],
                          lambda v: f"{v:.2f} pkt")

    def rpc_table(self):
        # Both directions on the link per request: requests B to A plus responses A to B.
        return self.table("rpc-rate", PEER_RPC_LABEL, 1, "per-packet", lambda v: f"{v:,.0f} B")

    def sd_table(self):
        rows = [r for r in self.rows if r["scenario"] == "event"]
        if not rows:
            return ""
        header = ["Subscribers"] + self.c.stacks
        body = []
        for count in sorted({int(PEER_EVENT_LABEL.match(r["label"]).group(2)) for r in rows
                             if PEER_EVENT_LABEL.match(r["label"])}):
            line = [str(count)]
            for stack in self.c.stacks:
                pairs = [r for r in rows if r["stack"] == stack and PEER_EVENT_LABEL.match(r["label"]) and
                         int(PEER_EVENT_LABEL.match(r["label"]).group(2)) == count]
                values = [sum(to_number(r.get(f)) or 0 for f in ("sd_in_packets", "sd_out_packets"))
                          for r in pairs if r["host"] == "B"]
                line.append(f"{statistics.median(values):.0f}" if values else "n/a")
            body.append(line)
        return table(header, body)


class ThreadStress:
    """Tables of the multi-threaded requester (run-peers.sh -s threads): one
    process with T worker threads, each sending a request and waiting for the
    response, which has the size of the request. Client CPU is the process's own
    CPU over the measured window. Daemon and responder CPU come from /proc around
    the whole step (threads-host-cpu.csv), minus their idle CPU, divided by the
    calls of the step; the total adds them up over both hosts."""

    def __init__(self, comparison):
        self.c = comparison
        self.host_cpu = {}
        for stack, dirs in comparison.runs.items():
            for run_dir in dirs:
                path = os.path.join(run_dir, "threads-host-cpu.csv")
                if os.path.isfile(path):
                    with open(path, newline="") as handle:
                        for row in csv.DictReader(handle):
                            self.host_cpu[(stack, run_dir, row["label"], to_number(row["payload_bytes"]))] = row

    def derived(self, row):
        """The metrics of one client row, with the host CPU joined in."""
        host = self.host_cpu.get((row["stack"], row["run_dir"], row["label"], to_number(row["payload_bytes"])))
        calls = to_number(host.get("calls_total")) if host else None

        def per_call(name):
            value = to_number(host.get(name)) if host else None
            return value * 1000 / calls if value is not None and calls else None

        result = {
            "throughput": to_number(row.get("throughput_msg_s")),
            "p50": to_number(row.get("p50_us")),
            "p99": to_number(row.get("p99_us")),
            "loss": to_number(row.get("loss_pct")),
            "client_pct": to_number(row.get("cpu_pct")),
            "client_us": to_number(row.get("cpu_per_req_us")),
            "b_daemon_us": per_call("b_daemon_cpu_ms"),
            "a_daemon_us": per_call("a_daemon_cpu_ms"),
            "a_server_us": per_call("a_server_cpu_ms"),
        }
        parts = [result[k] for k in ("client_us", "b_daemon_us", "a_daemon_us", "a_server_us")]
        result["total_us"] = sum(parts) if all(p is not None for p in parts) else None
        return result

    def configs(self, rows):
        keys = {(row.get("payload_bytes"), row.get("thread_rate"), row.get("responders"), row.get("threads"))
                for row in rows}
        return sorted(keys, key=lambda k: tuple(sort_number(v) for v in k))

    @staticmethod
    def rate_text(value):
        number = to_number(value)
        return "max" if not number else f"{number:g}/s"

    def select(self, stack, key):
        return [r for r in self.c.thread_rows
                if r["stack"] == stack and not r.get("_failed") and
                (r.get("payload_bytes"), r.get("thread_rate"), r.get("responders"), r.get("threads")) == key]

    def table(self, columns):
        """columns: [(header suffix, function(derived) -> cell text)]. One group of columns per stack."""
        header = ["Payload (B)", "Rate per thread", "Responders", "Threads"]
        for stack in self.c.stacks:
            header += [f"{stack} {name}" for name, _ in columns]
        body = []
        for key in self.configs(self.c.thread_rows):
            line = [f"{to_number(key[0]):g}", self.rate_text(key[1]), f"{to_number(key[2]):g}",
                    f"{to_number(key[3]):g}"]
            for stack in self.c.stacks:
                metrics = [self.derived(r) for r in self.select(stack, key)]
                line += [fn(metrics) for _, fn in columns]
            body.append(line)
        return table(header, body)

    @staticmethod
    def values(metrics, name):
        return [m[name] for m in metrics]

    def throughput_table(self):
        return self.table([
            ("msg/s", lambda m: cell(self.values(m, "throughput"), 0)),
            ("p50 / p99 (µs)", lambda m: f"{cell(self.values(m, 'p50'))} / {cell(self.values(m, 'p99'))}"),
            ("loss %", lambda m: cell(self.values(m, "loss"), 2)),
        ])

    def cpu_table(self):
        return self.table([
            ("client CPU (% of one core)", lambda m: cell(self.values(m, "client_pct"))),
            ("client µs/request", lambda m: cell(self.values(m, "client_us"))),
            ("all processes µs/request", lambda m: cell(self.values(m, "total_us"))),
        ])

    def breakdown_table(self):
        header = ["Payload (B)", "Rate per thread", "Responders", "Threads", "Stack", "client", "daemon B",
                  "daemon A", "responders A", "total"]
        body = []
        for key in self.configs(self.c.thread_rows):
            for stack in self.c.stacks:
                metrics = [self.derived(r) for r in self.select(stack, key)]
                if not metrics:
                    continue
                body.append([f"{to_number(key[0]):g}", self.rate_text(key[1]), f"{to_number(key[2]):g}",
                             f"{to_number(key[3]):g}", stack] +
                            [cell(self.values(metrics, name)) for name in
                             ("client_us", "b_daemon_us", "a_daemon_us", "a_server_us", "total_us")])
        return table(header, body)

    def write_csv(self, out_dir):
        columns = ["stack", "run", "payload_bytes", "threads", "thread_rate", "responders", "throughput_msg_s",
                   "p50_us", "p99_us", "loss_pct", "client_cpu_pct", "client_us", "b_daemon_us", "a_daemon_us",
                   "a_server_us", "total_us", "client_rss_kb"]
        with open(os.path.join(out_dir, "threads.csv"), "w", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(columns)
            for row in self.c.thread_rows:
                if row.get("_failed"):
                    continue
                m = self.derived(row)
                writer.writerow([row["stack"], row["run"], row.get("payload_bytes"), row.get("threads"),
                                 row.get("thread_rate"), row.get("responders"), m["throughput"], m["p50"],
                                 m["p99"], m["loss"], m["client_pct"], m["client_us"], m["b_daemon_us"],
                                 m["a_daemon_us"], m["a_server_us"], m["total_us"], row.get("max_rss_kb")])


def write_unified_csvs(rows, out_dir, prefix=""):
    for scenario in SCENARIOS:
        selected = [r for r in rows if r["scenario"] == scenario]
        if not selected:
            continue
        columns = ["stack", "run", "source"]
        for row in selected:
            for name in row:
                if name not in columns and name != "run_dir":
                    columns.append(name)
        with open(os.path.join(out_dir, f"{prefix}{scenario}.csv"), "w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=columns, extrasaction="ignore")
            writer.writeheader()
            writer.writerows(selected)


def parse_runs(specs):
    runs = {}
    for spec in specs:
        stack, sep, dirs = spec.partition("=")
        if not sep or not dirs:
            raise SystemExit(f"bad run specification '{spec}'; use <stack>=<dir>[,<dir>...]")
        listed = [d for d in dirs.split(",") if d]
        for directory in listed:
            if not os.path.isdir(directory):
                raise SystemExit(f"{directory}: not a directory")
        runs[stack] = listed
    return runs


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("runs", nargs="+", metavar="stack=dir[,dir...]")
    parser.add_argument("--out", required=True, help="output directory")
    parser.add_argument("--include-local", action="store_true", help="also report rows labelled local*")
    parser.add_argument("--payload", default="64", help="payload size of the resource table (default 64)")
    parser.add_argument("--rate", type=float, default=1000, help="paced rate of the resource table (default 1000)")
    args = parser.parse_args()

    comparison = Comparison(parse_runs(args.runs), args.include_local)
    if not comparison.rows and not comparison.peer_rows and not comparison.thread_rows:
        raise SystemExit("no benchmark rows found")
    os.makedirs(args.out, exist_ok=True)
    write_unified_csvs(comparison.rows, args.out)
    write_unified_csvs(comparison.peer_rows, args.out, prefix="peers-")
    stress = ThreadStress(comparison)
    if comparison.thread_rows:
        stress.write_csv(args.out)

    sections = ["# SOME/IP stack comparison\n", "_Generated by merge-results.py; do not edit by hand._\n"]
    notes = comparison.warnings() if comparison.rows else []
    if notes:
        sections.append("## Read this first\n\n" + "\n".join(f"- {note}" for note in notes) + "\n")
    sections.append("## Runs\n\n" + comparison.context())
    if comparison.rows:
        sections.append("## RPC ping-pong (closed loop)\n\nCells: median µs (range over repetitions). "
                        "opensomeip has no daemon and no Service Discovery in this harness.\n\n" +
                        comparison.rpc_pingpong())
        sections.append("## RPC at fixed offered rates (open loop)\n\n" + comparison.rpc_rate(False))
        sections.append("## RPC at maximum rate (stress, not a latency comparison)\n\n"
                        "Throughput and loss depend on the in-flight window and on how each stack applies "
                        "back-pressure; latency is left out on purpose.\n\n" + comparison.rpc_rate(True))
        sections.append(f"## Client resources ({args.payload} B, {args.rate:g} msg/s)\n\n"
                        "`cpu_net` is client CPU without the pacing busy-wait (`cpu_pacing`), measured over the "
                        "measured phase of that one run.\n\n" +
                        comparison.client_resources(args.payload, args.rate))
        sections.append("## Server and daemon resources (whole run)\n\n"
                        "These cover everything a run did: every payload and rate of the RPC scenarios, and for "
                        "the daemon also the event scenarios. They are comparable only between runs with identical "
                        "coverage, which the stacks have when this report shows no coverage warning. Server CPU "
                        "per request is the server's CPU between its first and last request divided by the "
                        "requests served. Daemon CPU includes the daemon's idle Service Discovery timers.\n\n" +
                        comparison.run_resources())
        sections.append("## Events\n\n"
                        "Rate-limited throughput equals the offered rate and is not reported. `notify` is the "
                        "duration of the publisher API call, not network latency. One-way latency is only "
                        "shown when both hosts ran ptp4l; check the offset in the ptp4l log before trusting it. "
                        "At the maximum rate it is dominated by queueing, not by the network.\n\n" +
                        comparison.events())
    if comparison.peer_rows:
        sweep = PeerSweep(comparison)
        intro = ("Each step runs all peers at once (common start time). Latency percentiles pool the samples "
                 "of every peer of the step; throughput and loss are summed over the peers. Requesters, "
                 "subscribers, and the consumer daemon share host B's app CPUs, so large peer counts include "
                 "CPU contention. opensomeip has no daemon and no Service Discovery in this harness.\n\n")
        for scenario, title in (("rpc-pingpong", "RPC peers, ping-pong (closed loop per requester)"),
                                ("rpc-rate", "RPC peers, fixed rate per requester-responder pair")):
            text = sweep.rpc(scenario)
            if text:
                sections.append(f"## {title}\n\n" + intro +
                                ("In ping-pong each requester keeps one request outstanding and visits its "
                                 "responders in turn; total msg/s is the sum of completed round trips.\n\n"
                                 if scenario == "rpc-pingpong" else
                                 "Each requester offers the per-pair rate times the responder count; total msg/s "
                                 "is the sum of the requesters' response throughput.\n\n") + text)
        text = sweep.events()
        if text:
            sections.append("## Event peers\n\n" + intro +
                            "Every subscriber subscribes to every publisher; received counts notifications over all "
                            "subscribers, and notify p99 is the highest of the publishers.\n\n" + text)

    if comparison.thread_rows:
        intro = ("One requester process with T worker threads; each worker sends a request and waits for the "
                 "response, and the response has the size of the request. Rate per thread `max` sends the next "
                 "request as soon as the response arrived (saturation); a number is a fixed load, so CPU can be "
                 "compared at the same amount of work. The worker threads, the responders, and the consumer "
                 "daemon share their host's app CPUs. opensomeip has no daemon and no Service Discovery in this "
                 "harness.\n\n")
        sections.append("## Many request threads in one process: throughput and latency\n\n" + intro +
                        "Latency runs from just before the stack's send call to the return of the waiting "
                        "worker, so it includes the thread wake-up.\n\n" + stress.throughput_table())
        sections.append("## Many request threads in one process: CPU\n\n"
                        "Client CPU is the process's CPU over the measured window (100 = one core busy). "
                        "`all processes` adds the daemons and responders of both hosts, minus their idle CPU, per "
                        "request.\n\n" + stress.cpu_table())
        sections.append("## Many request threads in one process: CPU per request by process (µs)\n\n" +
                        stress.breakdown_table())

    net = NetUsage(comparison)
    if net.rows:
        sections.append("## Network usage: one server, N clients on the other host\n\n"
                        "Counted with nftables on the link between the hosts (SOME/IP data over UDP; SD and PTP "
                        "excluded). Bytes are IP packet lengths, so a 64-byte payload is 108 bytes on the wire "
                        "(20 IP + 8 UDP + 16 SOME/IP header). The factor compares with the same stack and one "
                        "client.\n\n"
                        "### Events: data received by the subscriber host per published notification\n\n" +
                        net.event_table() +
                        "\n### Events: packets received by the subscriber host per published notification\n\n" +
                        net.event_packets_table() +
                        "\n### RPC: bytes on the wire per request plus its response\n\n"
                        "Every request and response is a separate message, so total RPC traffic grows with the number "
                        "of requesters in every stack; this table shows that each exchange costs the same.\n\n" + net.rpc_table() +
                        "\n### SD packets per event step on the subscriber host (both directions)\n\n"
                        "opensomeip used fixed endpoints in this harness, so it sends no SD.\n\n" + net.sd_table())

    with open(os.path.join(args.out, "comparison.md"), "w") as handle:
        handle.write("\n".join(sections))
    print(f"wrote {os.path.join(args.out, 'comparison.md')} and the unified CSV files "
          f"({len(comparison.rows)} rows, {len(comparison.peer_rows)} peer rows, "
          f"{len(comparison.thread_rows)} thread rows)")


if __name__ == "__main__":
    sys.exit(main())
