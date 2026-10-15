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

"""Write the lg-someip performance report (Markdown and figures) from the runs
that run-comparison.sh produced.

    make-report.py --standard <run-comparison output dir of the standard run> \
                   --threads <run-comparison output dir of the threads run> \
                   --rom <rom-footprint.sh output file> --out <report dir>

Every number and figure is computed from the result files; nothing is typed in.
The sentences around them state what the numbers show, so read the generated
report before publishing it: if a rerun changes a ranking, the text must change.
"""

import argparse
import glob
import importlib.util
import os
import re
import statistics

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
STACKS = ["lgsomeip", "vsomeip", "opensomeip"]
NAME = {"lgsomeip": "lg-someip", "vsomeip": "vsomeip", "opensomeip": "opensomeip"}
COLOR = {"lgsomeip": "#1f6feb", "vsomeip": "#8b949e", "opensomeip": "#d29922"}
PAYLOADS = [64, 1024, 1400]


def load_merge_module():
    spec = importlib.util.spec_from_file_location("merge_results", os.path.join(HERE, "merge-results.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


mr = load_merge_module()


def med(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def runs_of(root):
    runs = {}
    for stack in STACKS:
        # rep<k>, or rep<k>-quick for a run with -q
        dirs = sorted(glob.glob(os.path.join(root, stack, "rep*")),
                      key=lambda p: int(re.match(r"rep(\d+)", os.path.basename(p)).group(1)))
        if dirs:
            runs[stack] = dirs
    return runs


def fmt(value, digits=0):
    return "n/a" if value is None else f"{value:,.{digits}f}"


def lower_pct(ours, other):
    """How much lower `ours` is than `other`, in percent (positive = ours is lower)."""
    if ours is None or other in (None, 0):
        return None
    return (other - ours) / other * 100.0


def ratio(ours, other):
    if ours is None or other in (None, 0):
        return None
    return ours / other


def span(values, digits=0, unit="%"):
    values = [v for v in values if v is not None]
    if not values:
        return "n/a"
    low, high = min(values), max(values)
    if round(low, digits) == round(high, digits):
        return f"{low:.{digits}f}{unit}"
    return f"{low:.{digits}f}–{high:.{digits}f}{unit}"


def summary_blocks(path):
    """The 'name : value' blocks of an application log that holds several summaries."""
    blocks, current = [], None
    with open(path, errors="replace") as handle:
        for line in handle:
            if "summary:" in line:
                current = {}
                blocks.append(current)
                continue
            match = re.match(r"^\s+(\w+)\s+:\s+(\S+)\s*$", line)
            if match and current is not None:
                current[match.group(1)] = mr.to_number(match.group(2))
    return blocks


def md_table(header, rows):
    lines = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    lines += ["| " + " | ".join(str(c) for c in row) + " |" for row in rows]
    return "\n".join(lines) + "\n"


def best(cells, lower=True):
    """Mark the best of numeric cells (dict stack -> number) in bold."""
    numbers = {s: v for s, v in cells.items() if v is not None}
    if not numbers:
        return None
    return (min if lower else max)(numbers, key=numbers.get)


class Data:
    def __init__(self, standard_dir, threads_dir, rom_file, events_dir=None, rpcnet_dir=None):
        self.standard = mr.Comparison(runs_of(standard_dir), False)
        self.threads_dir = threads_dir
        self.thread_runs = runs_of(threads_dir)
        self.rom = self.parse_rom(rom_file)
        self.run_info = self.read_run_info(standard_dir)
        self.thread_rows = self.read_threads(os.path.join(threads_dir, "merged", "threads.csv"))
        self.load_standard()
        self.load_events(events_dir)
        self.load_rpcnet(rpcnet_dir)

    # --------------------------------------------------------- RPC on the wire
    def load_rpcnet(self, rpcnet_dir):
        """Packets and bytes on the link per RPC request and its response (rate 1,000/s, one requester)."""
        import csv

        self.rpcnet = {}
        if not rpcnet_dir:
            return
        for stack, dirs in runs_of(rpcnet_dir).items():
            samples = {}
            for run_dir in dirs:
                path = os.path.join(run_dir, "net-usage.csv")
                if not os.path.isfile(path):
                    continue
                by_payload = {}
                with open(path, newline="") as handle:
                    for row in csv.DictReader(handle):
                        if row["scenario"] == "rpc-rate":
                            by_payload.setdefault(int(row["payload_bytes"]), {})[row["host"]] = row
                for payload, hosts in by_payload.items():
                    host = hosts.get("A")
                    messages = mr.to_number(host["messages"]) if host else None
                    if not messages:
                        continue
                    packets = (mr.to_number(host["data_in_packets"]) + mr.to_number(host["data_out_packets"])) / messages
                    size = (mr.to_number(host["data_in_bytes"]) + mr.to_number(host["data_out_bytes"])) / messages
                    samples.setdefault(payload, []).append((packets, size))
            for payload, values in samples.items():
                self.rpcnet[(stack, payload)] = (med([v[0] for v in values]), med([v[1] for v in values]))

    def rpcnet_section(self):
        if not self.rpcnet:
            return ""
        rows = []
        for payload in PAYLOADS:
            row = [f"{payload} B"]
            for stack in STACKS:
                packets, size = self.rpcnet.get((stack, payload), (None, None))
                row.append("n/a" if packets is None else f"{packets:.2f} pkt · {size:,.0f} B")
            rows.append(row)
        text = ("**On the wire.** Packets and bytes on the link per request and response (both directions, "
                "IP packet lengths, counted with nftables at 1,000 requests/s):\n\n")
        text += md_table(["Payload"] + [NAME[s] for s in STACKS], rows)
        os_pk = self.rpcnet.get(("opensomeip", 1400), (None,))[0]
        lg_pk = self.rpcnet.get(("lgsomeip", 1400), (None,))[0]
        if os_pk and lg_pk and os_pk > lg_pk * 1.5:
            text += (f"\nAt 1400 B opensomeip sends every message as two packets (the harness documents that its RPC "
                     f"classes split large messages with SOME/IP-TP): {os_pk:.0f} packets per request against "
                     f"{lg_pk:.0f} for lg-someip and vsomeip, which carry 1400 B in one datagram. This is "
                     f"consistent with the jump in opensomeip's CPU per request and the drop in its throughput at "
                     f"that size, which we did not isolate further.\n")
        return text

    # ------------------------------------------------------------- event fan-out
    SUBSCRIBERS = (1, 2, 4, 8)

    def load_events(self, events_dir):
        """Bytes and packets that reach the subscriber host per published notification."""
        self.fan = {}
        if not events_dir:
            return
        comp = mr.Comparison(runs_of(events_dir), False)
        net = mr.NetUsage(comp)
        for stack in STACKS:
            for payload in PAYLOADS:
                for count in self.SUBSCRIBERS:
                    label = f"peers-p1-s{count}"
                    size = net.per_message(stack, "event", label, str(payload), [("B", "data_in_bytes")])
                    packets = net.per_message(stack, "event", label, str(payload), [("B", "data_in_packets")])
                    self.fan[(stack, payload, count)] = (size, packets)

    def fan_text(self):
        have = {k: v for k, v in self.fan.items() if v[0] is not None}
        lg_bytes = have.get(("lgsomeip", 1024, 8), (None,))[0]
        os_bytes = have.get(("opensomeip", 1024, 8), (None,))[0]
        if lg_bytes is None or os_bytes is None:
            return "(measured in the findings document; not repeated in this run)."
        return (f"with 8 subscribers of a 1024 B event the subscriber host receives {lg_bytes:,.0f} B per "
                f"notification with lg-someip and {os_bytes:,.0f} B with opensomeip ({os_bytes / lg_bytes:.1f}×).")

    def fan_section(self):
        have = {k: v for k, v in self.fan.items() if v[0] is not None}
        if not have:
            return ("**Event fan-out** (quoted from the findings document, not measured here): with one publisher "
                    "and 8 subscribers of a 1024 B event on another host, opensomeip sends every notification "
                    "once per subscriber, about 68 Mbit/s into the subscriber host at 1,000/s; the daemon stacks "
                    "send one copy, about 8.5 Mbit/s.\n")
        header = ["Payload", "Subscribers on host B"] + [NAME[s] for s in STACKS]
        rows = []
        for payload in PAYLOADS:
            for count in self.SUBSCRIBERS:
                row = [f"{payload} B", count]
                for stack in STACKS:
                    size, packets = self.fan.get((stack, payload, count), (None, None))
                    row.append("n/a" if size is None else f"{size:,.0f} B · {packets:.2f} pkt")
                rows.append(row)
        text = ("**Event fan-out, measured.** One publisher on host A, 1 to 8 subscribers on host B, 1,000 "
                "notifications/s. The table counts what reaches host B per published notification (IP packet "
                "bytes and packets, from nftables counters on the link; a few lost notifications lower a value "
                "slightly).\n\n")
        text += md_table(header, rows)
        lg_b, os_b = self.fan.get(("lgsomeip", 1024, 8), (None,))[0], self.fan.get(("opensomeip", 1024, 8), (None,))[0]
        if lg_b and os_b:
            text += (f"\nIn lg-someip, host B's daemon subscribes once and delivers locally, so one copy crosses "
                     f"the link however many subscribers run there: {lg_b:,.0f} B per notification with 8 "
                     f"subscribers of a 1024 B event, about {lg_b * 8 * 1000 / 1e6:.1f} Mbit/s at 1,000/s. "
                     f"opensomeip's publisher sends one copy per subscriber: {os_b:,.0f} B, about "
                     f"{os_b * 8 * 1000 / 1e6:.0f} Mbit/s ({os_b / lg_b:.1f}× the traffic). The saving grows "
                     f"with every subscriber and with the payload size, on a link that is shared with all other "
                     f"traffic.\n")
        return text

    @staticmethod
    def parse_rom(path):
        result, label = {}, None
        with open(path) as handle:
            for line in handle:
                head = re.match(r"^== (\S+)", line)
                if head:
                    label = head.group(1)
                    result[label] = {"files": [], "total": None}
                    continue
                total = re.match(r"^\s+total \(stripped\)\s+([\d.]+) KB", line)
                if total and label:
                    result[label]["total"] = float(total.group(1))
                    continue
                item = re.match(r"^\s+(\S+)\s+([\d.]+) KB", line)
                if item and label:
                    result[label]["files"].append((item.group(1), float(item.group(2))))
        return {label.split("-")[0]: value for label, value in result.items()}

    def read_run_info(self, root):
        info = {}
        first = self.standard.runs[STACKS[0]][0]
        info["revision"] = mr.parse_key_values(os.path.join(first, "run-info.txt")).get("revision", "unknown")
        for stack in STACKS:
            versions = {}
            for side in ("A",):
                versions.update(mr.parse_key_values(os.path.join(self.standard.runs[stack][0], f"{side}-versions.txt")))
            info[stack] = versions
        sysinfo = mr.parse_key_values(os.path.join(first, "A-sysinfo-before.txt"))
        info["model"] = sysinfo.get("model", "Raspberry Pi")
        info["kernel"] = sysinfo.get("kernel", "")
        info["governor"] = sysinfo.get("governor", "")
        info["repetitions"] = {s: len(self.standard.runs[s]) for s in STACKS}
        return info

    @staticmethod
    def read_threads(path):
        import csv

        rows = []
        with open(path, newline="") as handle:
            for row in csv.DictReader(handle):
                rows.append({k: (mr.to_number(v) if k not in ("stack",) else v) for k, v in row.items()})
        return rows

    # ------------------------------------------------------------ standard run
    def load_standard(self):
        comp = self.standard
        pingpong = [r for r in comp.select("rpc-pingpong") if not r.get("_failed")]
        rate = [r for r in comp.select("rpc-rate") if not r.get("_failed")]
        self.lat, self.fixed, self.sat = {}, {}, {}
        for stack in STACKS:
            for payload in PAYLOADS:
                pp = [r for r in pingpong if r["stack"] == stack and mr.to_number(r["payload_bytes"]) == payload]
                self.lat[(stack, payload)] = {
                    "p50": med([mr.to_number(r["p50_us"]) for r in pp]),
                    "p99": med([mr.to_number(r["p99_us"]) for r in pp]),
                    "mean": med([mr.to_number(r["mean_us"]) for r in pp]),
                    "lost": sum(mr.to_number(r["lost"]) or 0 for r in pp),
                    "n": len(pp),
                }
                fx = [r for r in rate if r["stack"] == stack and mr.to_number(r["payload_bytes"]) == payload and
                      mr.to_number(r["target_rate_msg_s"]) == 1000]
                self.fixed[(stack, payload)] = {
                    "p50": med([mr.to_number(r["p50_us"]) for r in fx]),
                    "p99": med([mr.to_number(r["p99_us"]) for r in fx]),
                    "cpu_us": med([mr.to_number(r["cpu_net_ms"]) * 1000 / mr.to_number(r["sent"]) for r in fx]),
                    "rss": med([mr.to_number(r["max_rss_kb"]) for r in fx]),
                    "loss": med([mr.to_number(r["loss_pct"]) for r in fx]),
                }
                sa = [r for r in rate if r["stack"] == stack and mr.to_number(r["payload_bytes"]) == payload and
                      mr.to_number(r["target_rate_msg_s"]) == 0]
                self.sat[(stack, payload)] = {
                    "throughput": med([mr.to_number(r["throughput_msg_s"]) for r in sa]),
                    "loss": med([mr.to_number(r["loss_pct"]) for r in sa]),
                    "cpu_us": med([mr.to_number(r["cpu_net_ms"]) * 1000 / mr.to_number(r["sent"]) for r in sa]),
                    "n": len(sa),
                }
        # Server and daemon memory over the whole run.
        self.mem = {}
        for stack in STACKS:
            server_rss, daemon_rss = [], []
            for run_dir in comp.runs[stack]:
                for log in glob.glob(os.path.join(run_dir, "**", "*rpc-server*.log"), recursive=True):
                    # The log is appended on every restart; the last block is an idle restarted server.
                    for summary in summary_blocks(log):
                        if summary.get("requests_served"):
                            server_rss.append(summary.get("max_rss_kb"))
                for side in "AB":
                    _, peak, _ = comp.daemon_delta(run_dir, side)
                    if peak is not None:
                        daemon_rss.append(peak)
            self.mem[stack] = {"server": med(server_rss), "daemon": med(daemon_rss)}

    # ------------------------------------------------------------ thread stress
    def t(self, stack, payload, rate, responders, threads, field):
        values = [r[field] for r in self.thread_rows if r["stack"] == stack and r["payload_bytes"] == payload and
                  r["thread_rate"] == rate and r["responders"] == responders and r["threads"] == threads]
        return med(values)


def grouped_bars(path, title, ylabel, categories, series, fmt_value="{:,.0f}", ylim=None):
    figure, axis = plt.subplots(figsize=(7.2, 3.6), dpi=140)
    width = 0.26
    for index, stack in enumerate(STACKS):
        values = [series[stack][c] or 0 for c in categories]
        positions = [i + (index - 1) * width for i in range(len(categories))]
        bars = axis.bar(positions, values, width, label=NAME[stack], color=COLOR[stack])
        for bar, value in zip(bars, values):
            axis.annotate(fmt_value.format(value), (bar.get_x() + bar.get_width() / 2, bar.get_height()),
                          ha="center", va="bottom", fontsize=6.5, xytext=(0, 1.5), textcoords="offset points")
    axis.set_xticks(range(len(categories)))
    axis.set_xticklabels([f"{c} B" for c in categories])
    axis.set_ylabel(ylabel)
    axis.set_title(title, fontsize=10, loc="left")
    if ylim:
        axis.set_ylim(*ylim)
    axis.spines[["top", "right"]].set_visible(False)
    axis.legend(frameon=False, fontsize=8)
    figure.tight_layout()
    figure.savefig(path)
    plt.close(figure)


def make_figures(data, out_dir):
    figures = os.path.join(out_dir, "figures")
    os.makedirs(figures, exist_ok=True)

    grouped_bars(os.path.join(figures, "latency.png"), "RPC round-trip latency, p50 (lower is better)", "µs",
                 PAYLOADS, {s: {p: data.lat[(s, p)]["p50"] for p in PAYLOADS} for s in STACKS}, "{:.0f}")
    grouped_bars(os.path.join(figures, "saturation.png"),
                 "Maximum RPC throughput, one requester (higher is better)", "responses per second", PAYLOADS,
                 {s: {p: data.sat[(s, p)]["throughput"] for p in PAYLOADS} for s in STACKS})

    # Total CPU per request by process, at the fixed load with 128 threads and 4 responders.
    figure, axes = plt.subplots(1, 3, figsize=(9.2, 3.6), dpi=140, sharey=True)
    parts = [("client_us", "client", "#1f6feb"), ("b_daemon_us", "daemon, requester host", "#58a6ff"),
             ("a_daemon_us", "daemon, responder host", "#9ecbff"), ("a_server_us", "responders", "#d29922")]
    for axis, payload in zip(axes, PAYLOADS):
        bottoms = [0.0] * len(STACKS)
        for field, label, color in parts:
            values = [data.t(s, payload, 50.0, 4, 128, field) or 0 for s in STACKS]
            axis.bar([NAME[s] for s in STACKS], values, bottom=bottoms, label=label, color=color)
            bottoms = [b + v for b, v in zip(bottoms, values)]
        for index, total in enumerate(bottoms):
            axis.annotate(f"{total:.0f}", (index, total), ha="center", va="bottom", fontsize=8,
                          xytext=(0, 2), textcoords="offset points")
        axis.set_title(f"{payload} B", fontsize=9)
        axis.tick_params(axis="x", labelsize=7.5)
        axis.spines[["top", "right"]].set_visible(False)
    axes[0].set_ylabel("CPU µs per request, both hosts")
    handles, labels = axes[0].get_legend_handles_labels()
    figure.legend(handles, labels, frameon=False, fontsize=8, loc="lower center", ncol=4)
    figure.suptitle("CPU per request, 128 threads at 6,400 requests/s (lower is better)", fontsize=10, x=0.01,
                    ha="left")
    figure.tight_layout(rect=(0, 0.07, 1, 1))
    figure.savefig(os.path.join(figures, "cpu-per-request.png"))
    plt.close(figure)

    # Throughput against thread count.
    figure, axes = plt.subplots(1, 3, figsize=(9.2, 3.4), dpi=140, sharey=True)
    for axis, payload in zip(axes, PAYLOADS):
        for stack in STACKS:
            xs = [1, 8, 32, 128]
            ys = [data.t(stack, payload, 0.0, 4, t, "throughput_msg_s") for t in xs]
            axis.plot(range(len(xs)), [y or 0 for y in ys], marker="o", label=NAME[stack], color=COLOR[stack])
        axis.set_xticks(range(4))
        axis.set_xticklabels(["1", "8", "32", "128"])
        axis.set_xlabel("request threads")
        axis.set_title(f"{payload} B", fontsize=9)
        axis.spines[["top", "right"]].set_visible(False)
    axes[0].set_ylabel("responses per second (4 responders)")
    axes[0].legend(frameon=False, fontsize=8)
    figure.suptitle("Throughput with many request threads in one process (higher is better)", fontsize=10, x=0.01,
                    ha="left")
    figure.tight_layout()
    figure.savefig(os.path.join(figures, "threads-throughput.png"))
    plt.close(figure)

    # Memory and storage.
    figure, axes = plt.subplots(1, 2, figsize=(9.0, 3.4), dpi=140)
    roles = [("client", [data.fixed[(s, 64)]["rss"] for s in STACKS]),
             ("responder", [data.mem[s]["server"] for s in STACKS]),
             ("daemon", [data.mem[s]["daemon"] or 0 for s in STACKS])]
    width = 0.26
    for index, (role, values) in enumerate(roles):
        axes[0].bar([i + (index - 1) * width for i in range(3)], [(v or 0) / 1024 for v in values], width,
                    label=role)
    axes[0].set_xticks(range(3))
    axes[0].set_xticklabels([NAME[s] for s in STACKS], fontsize=8)
    axes[0].set_ylabel("peak RSS, MB")
    axes[0].set_title("Memory per process", fontsize=10, loc="left")
    axes[0].legend(frameon=False, fontsize=8)
    totals = [data.rom[s]["total"] for s in STACKS]
    bars = axes[1].bar([NAME[s] for s in STACKS], totals, color=[COLOR[s] for s in STACKS])
    for bar, value in zip(bars, totals):
        axes[1].annotate(f"{value:,.0f}", (bar.get_x() + bar.get_width() / 2, value), ha="center", va="bottom",
                         fontsize=8, xytext=(0, 2), textcoords="offset points")
    axes[1].set_ylabel("KB, stripped")
    axes[1].set_title("Storage of a consumer ECU", fontsize=10, loc="left")
    for axis in axes:
        axis.spines[["top", "right"]].set_visible(False)
    figure.tight_layout()
    figure.savefig(os.path.join(figures, "memory-storage.png"))
    plt.close(figure)


def build_report(data, args):
    L = data.lat
    S = data.sat
    lg, vs, os_ = "lgsomeip", "vsomeip", "opensomeip"
    info = data.run_info
    reps = info["repetitions"]
    out = []
    add = out.append

    # ---------------------------------------------------------------- numbers
    lat_vs = [lower_pct(L[(lg, p)]["p50"], L[(vs, p)]["p50"]) for p in PAYLOADS]
    lat99_vs = [lower_pct(L[(lg, p)]["p99"], L[(vs, p)]["p99"]) for p in PAYLOADS]
    sat_vs = [ratio(S[(lg, p)]["throughput"], S[(vs, p)]["throughput"]) for p in PAYLOADS]
    sat_os = [ratio(S[(lg, p)]["throughput"], S[(os_, p)]["throughput"]) for p in PAYLOADS]
    cpu_vs = [lower_pct(data.t(lg, p, 50.0, 4, 128, "total_us"), data.t(vs, p, 50.0, 4, 128, "total_us"))
              for p in PAYLOADS]
    client_vs = [lower_pct(data.t(lg, p, 50.0, 4, 128, "client_us"), data.t(vs, p, 50.0, 4, 128, "client_us"))
                 for p in PAYLOADS]
    thr_vs = [ratio(data.t(lg, p, 0.0, 4, 128, "throughput_msg_s"), data.t(vs, p, 0.0, 4, 128, "throughput_msg_s"))
              for p in PAYLOADS]
    thr_os = [ratio(data.t(lg, p, 0.0, 4, 128, "throughput_msg_s"), data.t(os_, p, 0.0, 4, 128, "throughput_msg_s"))
              for p in PAYLOADS]
    rss_vs = {
        "client": lower_pct(data.fixed[(lg, 64)]["rss"], data.fixed[(vs, 64)]["rss"]),
        "responder": lower_pct(data.mem[lg]["server"], data.mem[vs]["server"]),
        "daemon": lower_pct(data.mem[lg]["daemon"], data.mem[vs]["daemon"]),
    }
    rom_vs = lower_pct(data.rom[lg]["total"], data.rom[vs]["total"])
    rom_os = ratio(data.rom[lg]["total"], data.rom[os_]["total"])
    flat = [data.t(lg, p, 0.0, 4, 128, "client_us") / data.t(lg, p, 0.0, 4, 8, "client_us") for p in PAYLOADS]
    scale = [data.t(lg, p, 0.0, 4, 128, "throughput_msg_s") / data.t(lg, p, 0.0, 4, 8, "throughput_msg_s")
             for p in PAYLOADS]
    all_steps = [(p, m, t) for p in PAYLOADS for m in (1, 4) for t in (1, 8, 32, 128)]
    lg_loss_max = max(data.t(lg, p, 0.0, m, t, "loss_pct") for p, m, t in all_steps)
    os_loss_max = max(data.t(os_, p, 0.0, m, t, "loss_pct") for p, m, t in all_steps)
    vs_p99_better = sum(1 for p, m, t in all_steps if data.t(lg, p, 0.0, m, t, "p99_us") <
                        data.t(vs, p, 0.0, m, t, "p99_us"))
    fan_text = data.fan_text()

    def cpu_tot(stack, payload):
        return data.t(stack, payload, 50.0, 4, 128, "total_us")

    platform = (info["model"] or "Raspberry Pi").split(" Rev")[0]
    add(f"# lg-someip performance report — {platform}\n")
    add(f"_SOME/IP stack evaluation · generated by `performance-measurement/scripts/make-report.py` from measured data · "
        f"lg-someip `{info['revision']}`, vsomeip {info[vs].get('vsomeip_version', '3.7.6')}, "
        f"opensomeip {info[os_].get('opensomeip_version', 'v0.2.0')}_\n")

    # ---------------------------------------------------------------- summary
    add("## 1. Summary\n")
    add("lg-someip is a **routing-daemon stack**, like vsomeip: applications talk to a per-host daemon that owns the "
        "ports, Service Discovery (SD), and the network. opensomeip has no daemon; each application owns its "
        "sockets. The two kinds of stack therefore answer different questions, and this report keeps them apart:\n")
    add(f"- **Against vsomeip, the like-for-like comparison, lg-someip is ahead on every axis we measured.** "
        f"Round-trip latency is {span(lat_vs, 1)} lower at p50 and {span(lat99_vs, 1)} lower at p99. "
        f"Processing one request costs {span(cpu_vs)} less CPU in total (all processes on both hosts), and "
        f"{span(client_vs)} less in the requesting process. Memory is {rss_vs['client']:.0f}% (client), "
        f"{rss_vs['responder']:.0f}% (responder), and {rss_vs['daemon']:.0f}% (daemon) lower, storage is "
        f"{rom_vs:.0f}% smaller, and the maximum throughput is {span(sat_vs, 2, '×')} that of vsomeip "
        f"({span(thr_vs, 2, '×')} with 128 request threads).")
    add(f"- **Against opensomeip, lg-someip trades some raw speed for what a daemon provides.** opensomeip is "
        f"faster on small messages because a round trip crosses two processes instead of four "
        f"(p50 {L[(os_, 64)]['p50']:.0f} µs against {L[(lg, 64)]['p50']:.0f} µs at 64 B). The gap shrinks to "
        f"{L[(lg, 1400)]['p50'] - L[(os_, 1400)]['p50']:.0f} µs at 1400 B, and there lg-someip is the faster stack "
        f"under load: {thr_os[2]:.1f}× the throughput of opensomeip with 128 request threads, and "
        f"{sat_os[2]:.1f}× at maximum rate.")
    add(f"- **lg-someip scales with the number of threads at a steady cost.** From 8 to 128 request threads its CPU "
        f"per request grows by only {span([(f - 1) * 100 for f in flat], 0)}, its throughput rises "
        f"{span(scale, 1, '×')}, and it loses at most {lg_loss_max:.2f}% of requests in any step. It never hung.")
    add(f"- **The daemon is a feature, not only a cost.** It gives a host one SD endpoint for all its applications "
        f"and one copy of an event on the link for any number of local subscribers: {fan_text} "
        f"Section 8 has the detail.\n")

    add("### Scorecard\n")
    rows = []

    def score(label, values, digits=0, lower=True, unit=""):
        winner = best(values, lower)
        cells = []
        for stack in STACKS:
            text = f"{fmt(values[stack], digits)}{unit}"
            cells.append(f"**{text}**" if stack == winner else text)
        rows.append([label] + cells)

    score("p50 latency, 64 B (µs)", {s: L[(s, 64)]["p50"] for s in STACKS}, 0)
    score("p50 latency, 1400 B (µs)", {s: L[(s, 1400)]["p50"] for s in STACKS}, 0)
    score("Max throughput, 64 B (msg/s)", {s: S[(s, 64)]["throughput"] for s in STACKS}, 0, False)
    score("Max throughput, 1400 B (msg/s)", {s: S[(s, 1400)]["throughput"] for s in STACKS}, 0, False)
    score("128 threads, 1400 B (msg/s)", {s: data.t(s, 1400, 0.0, 4, 128, "throughput_msg_s") for s in STACKS}, 0,
          False)
    score("CPU per request, all processes, 128 threads at 6,400/s, 64 B (µs)",
          {s: data.t(s, 64, 50.0, 4, 128, "total_us") for s in STACKS}, 0)
    score("CPU per request, all processes, same load, 1400 B (µs)",
          {s: data.t(s, 1400, 50.0, 4, 128, "total_us") for s in STACKS}, 0)
    score("Client memory, peak RSS (MB)", {s: (data.fixed[(s, 64)]["rss"] or 0) / 1024 for s in STACKS}, 1)
    score("Storage, consumer ECU (KB)", {s: data.rom[s]["total"] for s in STACKS}, 0)
    add(md_table(["Metric (lower is better unless it is throughput)"] + [NAME[s] for s in STACKS], rows))
    add("Bold marks the best value of a row. opensomeip often wins the rows it skips a daemon for; "
        "read it together with section 8.\n")

    # ---------------------------------------------------------------- setup
    add("## 2. What was measured, and how\n")
    add(f"- **Hardware.** Two {info['model'] or 'Raspberry Pi'} boards on a 1 Gb/s Ethernet link, both on kernel "
        f"{info['kernel']}, `net.core.rmem_max` 4 MB, CPU governor `{info['governor']}`, PTP running on both. "
        f"The responders ran on host A and the requesters on host B. Application processes were pinned to CPUs "
        f"2–3 and the daemons to CPU 1.")
    add("- **Same measurement code in every stack.** One shared kit (`performance-measurement/lg-someip/common`) "
        "provides the payload layout, pacing, exact percentiles, and CPU accounting. The same service, method, "
        "instance, and payload sizes appear on the wire for every stack. Every response has the size of its "
        "request.")
    rep_text = (f"{min(reps.values())}" if min(reps.values()) == max(reps.values())
                else f"{min(reps.values())}–{max(reps.values())}")
    add(f"- **Repetitions.** The latency, throughput, and memory figures are medians of {rep_text} repetitions that "
        f"rotate the stack order, so no stack always runs first on cold hardware. The many-thread and event "
        f"figures are medians of 3 repetitions. Ranges are in the generated tables of each run "
        f"(`merged/comparison.md`).")
    add("- **Payloads.** 64, 1024, and 1400 B (1400 B is the largest payload that fits one UDP datagram "
        "without SOME/IP-TP).")
    add("- **Scenarios.** Closed-loop round trip (latency); fixed load of 1,000 requests/s (latency under load and "
        "client CPU); maximum rate with a 1,000-request window (throughput); and one process with 1, 8, 32, or "
        "128 request threads calling 1 or 4 responders, both at maximum rate and at a fixed 50 requests/s per "
        "thread (CPU).")
    add("- **CPU** is the scheduler's own accounting. The requesting process reports its own CPU over the measured "
        "window. The daemons and responders are read from `/proc` around each many-thread step, minus their "
        "idle CPU, divided by the calls of the step. The scheduler counts in 10 ms ticks, so single figures at "
        "8 threads carry a few microseconds of rounding; the 32- and 128-thread rows are accurate to a few "
        "percent.")
    add("- **opensomeip** has no daemon and no SD in this harness; the benchmark passes the provider address and "
        "port explicitly. Its numbers exclude daemon IPC and discovery by design.\n")

    # ---------------------------------------------------------------- latency
    add("## 3. Latency\n")
    add("Round trip of one request with one outstanding (closed loop), from just before the send call to the "
        "start of the response callback. Medians of the repetitions.\n")
    rows = []
    for payload in PAYLOADS:
        cells = {s: L[(s, payload)]["p50"] for s in STACKS}
        winner = best(cells)
        row = [f"{payload} B"]
        for stack in STACKS:
            text = f"{L[(stack, payload)]['p50']:.1f} / {L[(stack, payload)]['p99']:.1f}"
            row.append(f"**{text}**" if stack == winner else text)
        row.append(f"{lower_pct(L[(lg, payload)]['p50'], L[(vs, payload)]['p50']):.1f}%")
        row.append(f"{(L[(lg, payload)]['p50'] - L[(os_, payload)]['p50']):+.0f} µs")
        rows.append(row)
    add(md_table(["Payload", "lg-someip p50 / p99 (µs)", "vsomeip p50 / p99 (µs)", "opensomeip p50 / p99 (µs)",
                  "lg-someip lower than vsomeip (p50)", "lg-someip minus opensomeip (p50)"], rows))
    add("![Latency](figures/latency.png)\n")
    lost = sum(L[(s, p)]["lost"] for s in STACKS for p in PAYLOADS)
    add(f"- lg-someip's round trip is {span(lat_vs, 1)} shorter than vsomeip's at p50 and {span(lat99_vs, 1)} "
        f"shorter at p99. Both use the same four-hop path (application → daemon → network → daemon → "
        f"application), so the difference is the cost of each hop. lg-someip's p99 is within "
        f"{max(L[(lg, p)]['p99'] - L[(lg, p)]['p50'] for p in PAYLOADS):.0f} µs of its p50 at every size: "
        f"the path has no heavy tail.")
    add(f"- opensomeip skips the daemon hops and is faster at small sizes. Its round trip grows by "
        f"{L[(os_, 1400)]['p50'] - L[(os_, 64)]['p50']:.0f} µs from 64 to 1400 B, lg-someip's by "
        f"{L[(lg, 1400)]['p50'] - L[(lg, 64)]['p50']:.0f} µs, so opensomeip's lead shrinks from "
        f"{L[(lg, 64)]['p50'] - L[(os_, 64)]['p50']:.0f} µs at 64 B to "
        f"{L[(lg, 1400)]['p50'] - L[(os_, 1400)]['p50']:.0f} µs at 1400 B.")
    add(f"- Lost requests in the closed-loop runs: {lost:.0f}.\n")
    rows = []
    for payload in PAYLOADS:
        row = [f"{payload} B"]
        for stack in STACKS:
            f = data.fixed[(stack, payload)]
            row.append(f"{f['p50']:.1f} / {f['p99']:.1f}")
        rows.append(row)
    add("At a fixed load of 1,000 requests/s the picture is the same:\n")
    add(md_table(["Payload", "lg-someip p50 / p99 (µs)", "vsomeip p50 / p99 (µs)", "opensomeip p50 / p99 (µs)"], rows))

    # ---------------------------------------------------------------- throughput
    add("## 4. Throughput\n")
    add("One requester sends as fast as it can with at most 1,000 requests in flight (maximum rate, open loop).\n")
    rows = []
    for payload in PAYLOADS:
        cells = {s: S[(s, payload)]["throughput"] for s in STACKS}
        winner = best(cells, False)
        row = [f"{payload} B"]
        for stack in STACKS:
            text = f"{S[(stack, payload)]['throughput']:,.0f} ({S[(stack, payload)]['loss']:.2f}% lost)"
            row.append(f"**{text}**" if stack == winner else text)
        row.append(f"{ratio(S[(lg, payload)]['throughput'], S[(vs, payload)]['throughput']):.2f}×")
        rows.append(row)
    add(md_table(["Payload", "lg-someip (msg/s)", "vsomeip (msg/s)", "opensomeip (msg/s)",
                  "lg-someip ÷ vsomeip"], rows))
    add("![Maximum throughput](figures/saturation.png)\n")
    add(f"- lg-someip delivers {span(sat_vs, 2, '×')} the throughput of vsomeip at every size.")
    add(f"- Loss at the maximum rate depends on back-pressure. vsomeip's client blocks when its queue is full, so "
        f"it loses nothing but is slower. lg-someip ({span([S[(lg, p)]['loss'] for p in PAYLOADS], 1)}) and "
        f"opensomeip ({span([S[(os_, p)]['loss'] for p in PAYLOADS], 1)}) drop a small share of requests when "
        f"they are offered more than the path carries; this is an overload test, not a latency comparison. "
        f"Where the application waits for each response (section 7), lg-someip loses at most "
        f"{lg_loss_max:.2f}%.")
    add(f"- opensomeip leads at 64 and 1024 B, where the missing daemon matters most. At 1400 B its throughput "
        f"falls to {S[(os_, 1400)]['throughput']:,.0f} msg/s while lg-someip holds "
        f"{S[(lg, 1400)]['throughput']:,.0f}.\n")
    add(data.rpcnet_section())

    # ---------------------------------------------------------------- cpu
    add("## 5. CPU\n")
    add("CPU time per request is the cost that matters on a small ECU: it decides how many requests a core can "
        "serve. The table adds up **all processes on both hosts** (the requester, the daemon on each host, and "
        "the responders) for one process with 128 request threads calling 4 responders at a fixed 6,400 "
        "requests/s, the same offered work for every stack.\n")
    rows = []
    for payload in PAYLOADS:
        for stack in STACKS:
            row = [f"{payload} B", NAME[stack]]
            for field in ("client_us", "b_daemon_us", "a_daemon_us", "a_server_us", "total_us"):
                value = data.t(stack, payload, 50.0, 4, 128, field)
                row.append(f"{value:.1f}" if value is not None else "n/a")
            row.append(f"{data.t(stack, payload, 50.0, 4, 128, 'client_cpu_pct'):.1f}")
            rows.append(row)
    add(md_table(["Payload", "Stack", "Requester µs", "Daemon (requester host) µs", "Daemon (responder host) µs",
                  "Responders µs", "**Total µs / request**", "Requester CPU (% of one core)"], rows))
    add("![CPU per request](figures/cpu-per-request.png)\n")
    add(f"- **vs vsomeip:** lg-someip needs {span(cpu_vs)} less CPU per request in total, and each part is "
        f"cheaper: the requester ({span(client_vs)} less), both daemons, and the responders. The daemon on the "
        f"requester host costs lg-someip "
        f"{data.t(lg, 1024, 50.0, 4, 128, 'b_daemon_us'):.0f} µs per request at 1024 B against "
        f"{data.t(vs, 1024, 50.0, 4, 128, 'b_daemon_us'):.0f} µs for vsomeip's routing manager.")
    add(f"- **vs opensomeip:** the daemons are the price of the architecture: "
        f"{(data.t(lg, 64, 50.0, 4, 128, 'b_daemon_us') + data.t(lg, 64, 50.0, 4, 128, 'a_daemon_us')):.0f} µs "
        f"per request at 64 B, about {data.t(lg, 64, 50.0, 4, 128, 'total_us') / data.t(os_, 64, 50.0, 4, 128, 'total_us'):.1f}× "
        f"opensomeip's total. At 1400 B the gap narrows to "
        f"{data.t(lg, 1400, 50.0, 4, 128, 'total_us') / data.t(os_, 1400, 50.0, 4, 128, 'total_us'):.1f}×, "
        f"because opensomeip's own CPU per request rises "
        f"({data.t(os_, 1024, 50.0, 4, 128, 'total_us'):.0f} µs at 1024 B to "
        f"{data.t(os_, 1400, 50.0, 4, 128, 'total_us'):.0f} µs at 1400 B) while lg-someip's barely does "
        f"({data.t(lg, 1024, 50.0, 4, 128, 'total_us'):.0f} to {data.t(lg, 1400, 50.0, 4, 128, 'total_us'):.0f} µs).")
    add(f"- **Single requester at 1,000 requests/s** (requesting process only, pacing excluded): "
        f"lg-someip {data.fixed[(lg, 64)]['cpu_us']:.1f} µs, vsomeip {data.fixed[(vs, 64)]['cpu_us']:.1f} µs, "
        f"opensomeip {data.fixed[(os_, 64)]['cpu_us']:.1f} µs per request at 64 B.\n")

    # ---------------------------------------------------------------- memory & rom
    add("## 6. Memory and storage\n")
    rows = []
    for stack in STACKS:
        daemon = data.mem[stack]["daemon"]
        rss128 = data.t(stack, 1400, 0.0, 4, 128, "client_rss_kb")
        rows.append([NAME[stack], f"{data.fixed[(stack, 64)]['rss'] / 1024:.1f}",
                     f"{rss128 / 1024:.1f}" if rss128 else "n/a",
                     f"{data.mem[stack]['server'] / 1024:.1f}",
                     f"{daemon / 1024:.1f}" if daemon else "no daemon"])
    add("**RAM**, peak resident set size (MB):\n")
    add(md_table(["Stack", "Requester (1 thread)", "Requester (128 threads)", "Responder", "Daemon"], rows))
    add("**ROM**, stripped files of one consumer ECU (the daemon, the stack library, and the benchmark's client "
        "application; third-party shared libraries would count, the C++ runtime and libc do not):\n")
    rows = []
    for stack in STACKS:
        parts = ", ".join(f"{name} {size:,.0f} KB" for name, size in data.rom[stack]["files"])
        rows.append([NAME[stack], parts, f"{data.rom[stack]['total']:,.0f} KB"])
    add(md_table(["Stack", "Files", "Total"], rows))
    add("![Memory and storage](figures/memory-storage.png)\n")
    add(f"- lg-someip's memory is {rss_vs['client']:.0f}% (requester), {rss_vs['responder']:.0f}% (responder), and "
        f"{rss_vs['daemon']:.0f}% (daemon) below vsomeip's. A consumer ECU that runs a daemon and two applications "
        f"saves about "
        f"{((data.mem[vs]['daemon'] - data.mem[lg]['daemon']) + 2 * (data.fixed[(vs, 64)]['rss'] - data.fixed[(lg, 64)]['rss'])) / 1024:.0f} MB.")
    add(f"- lg-someip needs {rom_vs:.0f}% less storage than vsomeip, "
        f"{data.rom[vs]['total'] - data.rom[lg]['total']:,.0f} KB less: vsomeip's library "
        f"alone is {dict(data.rom[vs]['files']).get('libvsomeip3.so.3.7.6', 0):,.0f} KB. "
        f"opensomeip is the smallest ({data.rom[os_]['total']:,.0f} KB, {rom_os:.1f}× smaller than lg-someip) "
        f"because the library is linked into the application and there is no daemon. The daemon and the "
        f"library are shared by every application on an ECU, so lg-someip's fixed cost is paid once while "
        f"each opensomeip application carries its own copy of the stack.\n")

    # ---------------------------------------------------------------- threads
    add("## 7. Many request threads in one process\n")
    add("A service with many request threads is the hard case for a SOME/IP library: every thread calls into the "
        "same stack at once. One process runs 1, 8, 32, or 128 worker threads; each sends a request, waits for "
        "the response, and repeats, calling 1 or 4 responders in turn. The response has the size of the "
        "request. All of it shares the board's two application CPUs.\n")
    add("**Maximum rate** (each thread sends again as soon as its response arrives), 4 responders:\n")
    rows = []
    for payload in PAYLOADS:
        for threads in (1, 8, 32, 128):
            cells = {s: data.t(s, payload, 0.0, 4, threads, "throughput_msg_s") for s in STACKS}
            winner = best(cells, False)
            row = [f"{payload} B", threads]
            for stack in STACKS:
                text = (f"{cells[stack]:,.0f} · p99 {data.t(stack, payload, 0.0, 4, threads, 'p99_us'):,.0f} µs"
                        f" · {data.t(stack, payload, 0.0, 4, threads, 'loss_pct'):.2f}% lost")
                row.append(f"**{text}**" if stack == winner else text)
            rows.append(row)
    add(md_table(["Payload", "Threads", "lg-someip", "vsomeip", "opensomeip"], rows))
    add("![Throughput with many threads](figures/threads-throughput.png)\n")
    add(f"- **lg-someip scales at a steady cost.** From 8 to 128 threads its requester CPU per request grows by "
        f"{span([(f - 1) * 100 for f in flat], 0)} while its throughput rises {span(scale, 1, '×')}. "
        f"Latency percentiles rise with the thread count in every stack: the threads queue for two CPUs. "
        f"lg-someip loses at most {lg_loss_max:.2f}% of requests in any step.")
    add(f"- **vs vsomeip**, the same design, lg-someip delivers {span(thr_vs, 2, '×')} the throughput at 128 threads "
        f"(4 responders) and a lower p99 in {vs_p99_better} of {len(all_steps)} maximum-rate steps.")
    add(f"- **vs opensomeip**, lg-someip is {thr_os[0]:.2f}× at 64 B, {thr_os[1]:.2f}× at 1024 B, and "
        f"{thr_os[2]:.1f}× at 1400 B with 128 threads: opensomeip leads on small messages. At 1400 B it loses "
        f"up to {os_loss_max:.2f}% of its requests, and a lost request holds its thread until the 1 s timeout, so a "
        f"few losses per second idle several threads and closed-loop throughput falls. In the same step "
        f"lg-someip loses {data.t(lg, 1400, 0.0, 4, 128, 'loss_pct'):.2f}%.")
    add("- **opensomeip needed a workaround to run at all.** Its `RpcClient` never releases sessions; after "
        "65,535 calls on one client the next call spins forever holding a lock, and every thread hangs. The "
        "benchmark replaces each opensomeip client every 50,000 calls to avoid it. lg-someip and vsomeip "
        "needed no such measure. See the opensomeip notes in `performance-measurement/docs/stack-comparison.md`.\n")
    add("**Fixed load** (50 requests/s per thread, 6,400/s with 128 threads): every stack carries the same offered "
        "work, which makes this the fair basis for the CPU comparison of section 5. Latency stays close to the idle "
        "round trip of section 3, with the rare outliers noted below.\n")
    rows = []
    outliers = []
    for payload in PAYLOADS:
        row = [f"{payload} B"]
        for stack in STACKS:
            p50 = data.t(stack, payload, 50.0, 4, 128, "p50_us")
            p99 = data.t(stack, payload, 50.0, 4, 128, "p99_us")
            row.append(f"{p50:.0f} / {p99:.0f}")
            if p99 > 3 * p50:
                outliers.append(f"{NAME[stack]} at {payload} B (p99 {p99:,.0f} µs against p50 {p50:.0f} µs)")
        rows.append(row)
    add(md_table(["Payload (128 threads, 4 responders)", "lg-someip p50 / p99 (µs)", "vsomeip p50 / p99 (µs)",
                  "opensomeip p50 / p99 (µs)"], rows))
    if outliers:
        add("Tail outliers (p99 above 3× the p50): " + "; ".join(outliers) + ".\n")

    # ---------------------------------------------------------------- merits
    add("## 8. Why lg-someip: advantages and merits\n")
    add("### 8.1 Against vsomeip\n")
    add("vsomeip is the established open-source stack and has the same architecture, so this is the comparison "
        "that isolates implementation quality. lg-someip wins it everywhere:\n")
    rows = [
        ["Round-trip latency (p50)", f"{span(lat_vs, 1)} lower", "section 3"],
        ["Round-trip latency (p99)", f"{span(lat99_vs, 1)} lower", "section 3"],
        ["Maximum throughput", f"{span(sat_vs, 2, '×')}", "section 4"],
        ["Throughput, 128 threads", f"{span(thr_vs, 2, '×')}", "section 7"],
        ["CPU per request, all processes", f"{span(cpu_vs)} lower", "section 5"],
        ["CPU per request, requesting process", f"{span(client_vs)} lower", "section 5"],
        ["RAM: requester / responder / daemon",
         f"{rss_vs['client']:.0f}% / {rss_vs['responder']:.0f}% / {rss_vs['daemon']:.0f}% lower", "section 6"],
        ["Storage (consumer ECU)", f"{rom_vs:.0f}% smaller", "section 6"],
    ]
    add(md_table(["Axis", "lg-someip against vsomeip", "Where"], rows))
    add("What this means in practice:\n")
    add("- **More headroom on the same silicon.** Fewer CPU microseconds per request means the same ECU serves more "
        "requests, or the same load leaves more of the core to the application. The saving is in every process "
        "on the path, so it does not depend on where the work runs.")
    add("- **A smaller footprint on a constrained ECU.** "
        f"{rom_vs:.0f}% less flash, {span([rss_vs['client'], rss_vs['responder']], 0)} less RAM in applications and "
        f"{rss_vs['daemon']:.0f}% less in the daemon than vsomeip.")
    add("- **Steadier behaviour under many threads.** lg-someip's cost per request grows slowly with the thread "
        "count, and its throughput stays ahead of vsomeip's at every thread count.\n")
    add("### 8.2 Against opensomeip\n")
    add("opensomeip's data path is the fastest we measured because it leaves the daemon out. That is a design "
        "trade, not a free win: the jobs the daemon does for a whole host fall to every application instead.\n")
    add(data.fan_section())
    add("The existing findings document (`opensomeip-v0.2.0-findings.md`) tested more of what the missing daemon "
        "costs. Those results are quoted from that document and were **not re-measured** here:\n")
    rows = [
        ["Several applications on one host", "SD replies reach only one of two applications (1 of 2 in every trial); "
         "different services cannot share a port", "One daemon owns the ports and SD for the whole host"],
        ["Event and SD glue", "The application connects SD to RPC and events itself; SD defaults are slow",
         "SD, RPC, and events go through the daemon and are configured from one file"],
    ]
    add(md_table(["Topic", "opensomeip v0.2.0 (findings document)", "lg-someip"], rows))
    add("And in this report's own measurements:\n")
    add(f"- **At 1400 B lg-someip is the faster stack under load** ({thr_os[2]:.1f}× opensomeip's throughput at "
        f"128 threads). From 1024 to 1400 B its CPU per request rises "
        f"{(cpu_tot(lg, 1400) / cpu_tot(lg, 1024) - 1) * 100:.0f}%, opensomeip's "
        f"{(cpu_tot(os_, 1400) / cpu_tot(os_, 1024) - 1) * 100:.0f}%. "
        f"lg-someip carries a 1400 B payload in one UDP datagram; opensomeip sends two packets per message "
        f"(section 4).")
    add(f"- **lg-someip is the more predictable one.** It lost at most {lg_loss_max:.2f}% in any many-thread step. "
        f"opensomeip needed a client-replacement workaround to avoid a hang and lost up to {os_loss_max:.2f}%.")
    add(f"- **The cost is bounded.** The extra round-trip time is {L[(lg, 64)]['p50'] - L[(os_, 64)]['p50']:.0f} µs "
        f"at 64 B and {L[(lg, 1400)]['p50'] - L[(os_, 1400)]['p50']:.0f} µs at 1400 B, and the shared daemon is "
        f"paid once per ECU rather than once per application.\n")
    add("### 8.3 When each stack fits\n")
    add(md_table(["Situation", "Choose"], [
        ["One application per ECU, small messages, lowest possible latency, SD handled elsewhere", "opensomeip"],
        ["Several applications per ECU, SD and events needed, configuration-driven deployment", "lg-someip"],
        ["Same architecture as vsomeip with less CPU, memory, and storage", "lg-someip"],
        ["Many request threads or messages near 1400 B under load", "lg-someip"],
    ]))

    # ---------------------------------------------------------------- caveats
    add("## 9. Limits of this report\n")
    add("- Two boards on one Ethernet link: the absolute numbers belong to this hardware. The ranking and the "
        "percentages are the transferable result.")
    add("- **Service Discovery timing was not measured** in this round, and events were measured for traffic on the "
        "link only (section 8.2), not for latency. Everything else is RPC over UDP.")
    add("- The three stacks use different thread-pool defaults; their configuration formats do not expose "
        "equivalent knobs, so none was tuned.")
    add("- CPU of the daemons and responders comes from 10 ms scheduler ticks; the 32- and 128-thread rows are "
        "reliable to a few percent, the 8-thread rows less so.")
    add("- 128 threads on two cores partly measure scheduling behaviour, the same for every stack.")
    add("- opensomeip numbers use direct endpoints and no SD. A run with SD would add cost; the numbers here are "
        "its best case.")
    add("- The maximum-rate loss figures depend on each stack's back-pressure design and are not a latency "
        "comparison. In that open-loop test lg-someip drops about as many requests as opensomeip; only vsomeip, "
        "whose client blocks, loses none.\n")

    add("## 10. Reproduce\n")
    add("```bash")
    add("# standard run: latency, throughput, memory (about 30 minutes)")
    add("performance-measurement/scripts/run-comparison.sh -A <ssh A> -B <ssh B> -a <ip A> -b <ip B> -y -R 5 \\")
    add("    -s \"pingpong rate\" -p \"64 1024 1400\" -r \"1000 0\" -N 6000 -D 3")
    add("# many threads: CPU and throughput (about 75 minutes)")
    add("performance-measurement/scripts/run-comparison.sh -A <ssh A> -B <ssh B> -a <ip A> -b <ip B> -m peers -s threads -R 3 \\")
    add("    -p \"64 1024 1400\" -D 3 -O '-T \"1 8 32 128\" -M \"1 4\"'")
    add("# storage, event fan-out on the link, then this report")
    add("performance-measurement/scripts/run-comparison.sh -A <ssh A> -B <ssh B> -a <ip A> -b <ip B> -m peers -s event -R 3 \\")
    add("    -p \"64 1024 1400\" -D 3 -O '-P 1 -U \"1 2 4 8\" -w'")
    add("performance-measurement/scripts/run-comparison.sh -A <ssh A> -B <ssh B> -a <ip A> -b <ip B> -m peers -s rpc -R 1 \\")
    add("    -p \"64 1024 1400\" -D 3 -O '-R 1 -M 1 -m rate -w'    # packets per request on the link")
    add("performance-measurement/scripts/rom-footprint.sh ... > rom-footprint.txt")
    add("performance-measurement/scripts/make-report.py --standard <dir> --threads <dir> --events <dir> --rpcnet <dir> \\")
    add("    --rom rom-footprint.txt --out <dir>")
    add("```\n")
    dirs = [args.standard, args.threads] + [d for d in (args.events, args.rpcnet) if d]
    add("Data: " + ", ".join(f"`{os.path.relpath(d)}`" for d in dirs) + " (`merged/comparison.md`, "
        "`merged/*.csv`; ROM in `rom-footprint.txt`).\n")
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--standard", required=True)
    parser.add_argument("--threads", required=True)
    parser.add_argument("--rom", required=True)
    parser.add_argument("--events", help="run-comparison output dir of the event fan-out run (optional)")
    parser.add_argument("--rpcnet", help="run-comparison output dir of the RPC wire-traffic run (optional)")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    data = Data(args.standard, args.threads, args.rom, args.events, args.rpcnet)
    make_figures(data, args.out)
    report = build_report(data, args)
    path = os.path.join(args.out, "lg-someip-performance-report.md")
    with open(path, "w") as handle:
        handle.write(report)
    print(f"wrote {path}")


if __name__ == "__main__":
    main()
