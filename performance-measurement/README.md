# SOME/IP Performance Measurement Guide

This folder contains everything used to measure the performance of lg-someip
and compare it with [vsomeip](https://github.com/COVESA/vsomeip) 3.7.6 and
[opensomeip](https://github.com/vtz/opensomeip) v0.2.0: the benchmark
applications, the scripts that run them on two hosts, the tools that merge the
results and generate the report, and the published reports.

It is maintained on the `performance_comparison` branch and kept apart from the
lg-someip sources, so it can be updated without touching the library.

This guide walks through a full measurement cycle:

1. [Prepare the environment](#2-prepare-the-environment)
2. [Run the measurements](#3-run-the-measurements)
3. [Check the results](#4-check-the-results)
4. [Create the report](#5-create-the-report)
5. [Publish the report](#6-publish-the-report)

For the design of each measurement and the full option lists, see the
[reference documents](#8-reference-documents).

## 1. Folder layout

```
performance-measurement/
  README.md                this guide
  lg-someip/               lg-someip benchmark applications (public lgsomeip::api)
    common/                stack-independent measurement kit, shared by all three stacks
    scripts/               same-host runs, two-host coordinator, Pi setup, bundling
    README.md              application reference and lg-someip-only measurements
  vsomeip/                 the same applications written against the vsomeip API
    scripts/               vsomeip configuration, agent, and two-host coordinator
  opensomeip/              the same applications written against the opensomeip API
    scripts/               opensomeip agent and two-host coordinator
  scripts/                 stack comparison: run, merge, report, storage footprint
  docs/
    stack-comparison.md    comparison method, fairness rules, and stack-specific notes
  reports/
    pi4/                   generated report, Raspberry Pi 4 Model B pair
    pi5/                   generated report, Raspberry Pi 5 Model B pair
    lg-someip-vs-vsomeip/  comparison document based on the Pi 4 and Pi 5 results
    opensomeip-v0.2.0-findings.md
  results/                 raw measurement output (ignored by git)
```

The repository root contains two small hooks for this folder: the root
`CMakeLists.txt` builds `lg-someip/` with the other examples
(`-DENABLE_EXAMPLES=ON`, Linux only), and the CI formatting and clang-tidy
checks include `performance-measurement/`. Binaries are written to
`<build dir>/performance-measurement/lg-someip/`.

### What is measured

| Scenario | lg-someip | vsomeip | opensomeip |
|---|---|---|---|
| RPC round-trip latency (ping-pong) | yes | yes | yes, direct endpoint (no daemon, no SD) |
| RPC throughput and latency under load (fixed rates, maximum rate) | yes | yes | yes, direct endpoint |
| RPC from one process with many request threads (CPU, throughput) | yes | yes | yes |
| Events: one-way latency, loss, fan-out, traffic on the link | yes | yes | yes, direct event harness |
| Service Discovery timing | yes | graceful stop only | not available |
| Daemon CPU, peak RSS, storage footprint | yes | yes | yes (no daemon) |

## 2. Prepare the environment

### 2.1 Hardware and network

- **Two boards of the same model** (for example two Raspberry Pi 4 Model B) on
  one 1 Gb/s Ethernet link. A direct cable that carries only benchmark traffic
  gives the most stable numbers. Host A runs the providers (servers,
  publishers), host B the consumers (clients, subscribers).
- **A driving machine** (the development server or a PC with Linux, macOS, or
  WSL) that reaches both boards over SSH. SSH may use the benchmark link or
  another network; another network keeps SSH traffic off the measurement. The
  driving machine needs `bash`, `ssh`, `rsync`, `tar`, and Python 3 with
  `matplotlib` for the report.

On each board:

1. Install a 64-bit OS (Raspberry Pi OS 64-bit or Ubuntu arm64) with the same
   image and kernel on both boards. The published Pi 4 results used Raspberry
   Pi OS based on Debian 13 (trixie) with kernel 6.12. Use proper cooling and
   the official power supply; throttling changes results without any other
   sign.
2. Allow key-based SSH login from the driving machine:

   ```bash
   ssh-copy-id <ssh host>
   ssh -o BatchMode=yes <ssh host> true    # must succeed without a prompt
   ```

3. Give the benchmark interface a static address. With NetworkManager:

   ```bash
   # host A (use 192.168.100.2/24 on host B)
   sudo nmcli con add type ethernet ifname eth0 con-name perf-link \
       ipv4.method manual ipv4.addresses 192.168.100.1/24 ipv6.method disabled
   sudo nmcli con up perf-link
   ```

4. Allow `sudo` without a password for the benchmark user, or at least for
   `nft`. The traffic counters (`-w`, section 3.3) need it; everything else
   works with a password prompt.

### 2.2 Build lg-someip on the boards

Run this from the repository root on the driving machine. It copies the source
tree to `~/lg-someip` on both boards, installs the build packages (`-I` asks
for each board's sudo password in your terminal), builds lg-someip into
`~/lg-someip/build-pi`, and runs a short smoke test of all lg-someip scenarios
(about 10 minutes on a Raspberry Pi 4). Do the steps of section 2.4 first, so
that the smoke test runs on prepared boards.

```bash
performance-measurement/lg-someip/scripts/run-two-hosts.sh -y -I -q \
    -A <ssh host A> -B <ssh host B> -a 192.168.100.1 -b 192.168.100.2
```

`-I` is needed only once per board. Later `-y` runs copy and rebuild without
sudo. `-d <dir>` changes the directory on the boards (default `lg-someip`).

### 2.3 Build vsomeip and opensomeip on the boards

Skip this step if you only measure lg-someip. The step above copied
`performance-measurement/` to the boards, so run the build script on each board
(about 15 minutes on a Raspberry Pi 4):

```bash
ssh -t <ssh host> 'sudo apt-get install -y libboost-dev libboost-system-dev \
    libboost-thread-dev libboost-filesystem-dev'
ssh <ssh host> lg-someip/performance-measurement/scripts/build-stacks.sh
```

It installs vsomeip to `~/vsomeip-install` and opensomeip to
`~/opensomeip-install`, and leaves vsomeip's `routingmanagerd` in
`~/vsomeip-src/build/examples/routingmanagerd/`, where the scripts expect it.
The benchmark applications of both stacks are built later by `-y`.

Real vsomeip also needs a multicast route on both boards (section 2.4).

To also measure the vsomeip applications on lg-someip's vsomeip API layer
(stack `lgsomeip-vsomeip-api`), install lg-someip on each board:

```bash
ssh <ssh host> 'cd lg-someip && cmake --build build-pi -j"$(nproc)" &&
    cmake --install build-pi --prefix ~/lgsomeip-install'
```

The full build is needed because `pi-setup.sh` builds only the daemon and the
benchmark applications, not the vsomeip API library.

### 2.4 Before each measurement session

Except for the NTP setting, none of these settings survive a reboot. Do this on
both boards after every boot (replace `eth0` with the benchmark interface):

```bash
# fixed CPU frequency
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# the same socket receive buffer limit on both boards
sudo sysctl -w net.core.rmem_max=4194304

# multicast route, needed by real vsomeip to start Service Discovery
sudo ip route replace 224.0.0.0/4 dev eth0

# clock synchronization, needed for one-way event latency and Service
# Discovery offer_to_available; round-trip results do not need it.
# ptp4l runs in the background and logs to ~/ptp4l.log.
sudo timedatectl set-ntp false     # stop NTP from adjusting the clock as well
sudo nohup ptp4l -i eth0 -S -m > ~/ptp4l.log 2>&1 < /dev/null &       # host A (time source)
sudo nohup ptp4l -i eth0 -S -s -m > ~/ptp4l.log 2>&1 < /dev/null &    # host B (follows host A)
```

On host B, watch `tail -f ~/ptp4l.log` until the `master offset` values
settle, and note their typical size: it is the error bound of every one-way
latency. If other PTP devices share the network, give the pair its own domain
by adding `--domainNumber <n>` (the same value on both boards) to both
commands. Unlike the other settings, `set-ntp false` survives a reboot; after
the session, run `sudo timedatectl set-ntp true` on both boards, because a
Raspberry Pi has no battery-backed clock.

`run-comparison.sh` checks both boards before it starts. It stops if their
kernel or `rmem_max` differ, if `ptp4l` is not running on both, or if a
governor is not `performance`; `-f` continues anyway. Each run also records the
board state before and after the run (`sysinfo.sh`: governor, frequencies,
temperature, throttling, clock sync, and link state).

## 3. Run the measurements

All commands run on the driving machine from the repository root. The
`run-*.sh` scripts print their options with `-h`. Use `-n` for a dry run that
prints each host command without running it, and `-q` for a quick smoke test
with small counts (the numbers are not meaningful).

### 3.1 Same-host check (optional)

To check the lg-someip applications on one machine first:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_EXAMPLES=ON
cmake --build build -j
performance-measurement/lg-someip/scripts/run-local-suite.sh -b build
```

Results go to `performance-measurement/results/local/`. On one host, the daemon
forwards local offers and subscriptions directly, so a local run does not
measure Service Discovery timing.

### 3.2 lg-someip on two hosts

```bash
performance-measurement/lg-someip/scripts/run-two-hosts.sh -y \
    -A <ssh host A> -B <ssh host B> -a 192.168.100.1 -b 192.168.100.2
```

This runs the same-host suite on both boards, then RPC ping-pong, RPC rate,
events, and Service Discovery between the boards (about 30 minutes with UDP).
Add `-t tcp` for a TCP run. See
[lg-someip/README.md](lg-someip/README.md#3-run-everything-from-the-pc) for
the scenarios and options.

### 3.3 Stack comparison

`scripts/run-comparison.sh` runs the coordinator of every stack several times,
rotating the order between repetitions so that no stack always runs first or
last, and merges the results at the end. Start with a smoke test of one
repetition (about 7 minutes on a Raspberry Pi 4, including the build):

```bash
performance-measurement/scripts/run-comparison.sh -A <ssh host A> -B <ssh host B> \
    -a 192.168.100.1 -b 192.168.100.2 -y -q -R 1
```

`-y` copies and builds the applications of every stack on both boards before
the first repetition. It is needed again after the source changes.

The published reports are based on four runs. Name each output directory with
`-o` so that the runs can be found later (the examples use
`<board>-<date>-<run>`), and keep the console output next to it with `tee`:

```bash
cmp=performance-measurement/scripts/run-comparison.sh
hosts=(-A <ssh host A> -B <ssh host B> -a 192.168.100.1 -b 192.168.100.2)
out=performance-measurement/results/pi4-$(date +%Y%m%d-%H%M)
mkdir -p performance-measurement/results

# 1. standard: latency, throughput, memory (about 30 minutes)
"${cmp}" "${hosts[@]}" -y -R 5 -o "${out}-standard" \
    -s "pingpong rate" -p "64 1024 1400" -r "1000 0" -N 6000 -D 3 \
    2>&1 | tee "${out}-standard.log"

# 2. many request threads in one process: CPU and throughput (about 75 minutes)
"${cmp}" "${hosts[@]}" -m peers -s threads -R 3 -o "${out}-threads" \
    -p "64 1024 1400" -D 3 -O '-T "1 8 32 128" -M "1 4"' \
    2>&1 | tee "${out}-threads.log"

# 3. event fan-out and traffic on the link
"${cmp}" "${hosts[@]}" -m peers -s event -R 3 -o "${out}-events" \
    -p "64 1024 1400" -D 3 -O '-P 1 -U "1 2 4 8" -w' \
    2>&1 | tee "${out}-events.log"

# 4. packets per request on the link
"${cmp}" "${hosts[@]}" -m peers -s rpc -R 1 -o "${out}-rpcnet" \
    -p "64 1024 1400" -D 3 -O '-R 1 -M 1 -m rate -w' \
    2>&1 | tee "${out}-rpcnet.log"
```

Notes:

- Use the same scenarios, payloads, and rates for every stack. The daemon CPU
  figures cover all scenarios of a run, and `merge-results.py` warns when the
  stacks' coverage differs.
- `-w` counts the traffic between the boards with nftables, which needs
  passwordless `sudo` on both boards.
- `-S` selects the stacks (default `lgsomeip vsomeip opensomeip`). Add
  `lgsomeip-vsomeip-api` in standard mode to include the vsomeip applications
  on lg-someip (see section 2.3).
- Peer-count sweeps (`-m peers -s "rpc event"`) and the other options are
  described in [stack-comparison.md](docs/stack-comparison.md#running-a-comparison).

### 3.4 Driving the run from a PC without a git checkout

If the development server cannot reach the boards, create an archive there and
run the same commands from a PC that can reach them:

```bash
# on the development server
performance-measurement/lg-someip/scripts/make-bundle.sh
# -> performance-measurement/results/bundle/lg-someip-perf-<revision>.tar.gz

# on the PC
tar -xzf lg-someip-perf-<revision>.tar.gz && cd lg-someip
```

The archive contains the files git tracks, the untracked files under
`performance-measurement/` (except `results/`), and a `REVISION` file. The
revision from that file is recorded in every result row.

## 4. Check the results

By default, results are written under `performance-measurement/results/`,
which git ignores. A comparison run has this layout:

```
results/<run>/
  <stack>/rep<k>/          one repetition of one stack (rep<k>-quick with -q)
    A-provider/            CSV files, samples, and logs from host A
    B-consumer/            CSV files, samples, and logs from host B
    *-sysinfo-before.txt   board state before and after the run
    *-sysinfo-after.txt
    *-daemon-usage-*.txt   daemon CPU time and peak RSS (lg-someip and vsomeip)
    *-versions.txt         library versions (vsomeip and opensomeip)
    *-build-size.txt       binary sizes
    ping-B-to-A.txt        round-trip time of the link without a stack
    run-info.txt           revision and command line of the run
  merged/
    comparison.md          tables: median and range over the repetitions
    *.csv                  unified CSV files with "stack" and "run" columns
<run>.log                  console output, if saved with tee as shown above
```

Before you use the numbers:

1. Read `merged/comparison.md`. If it starts with a "Read this first"
   section, read each note. A difference in kernel, `rmem_max`, or scenario
   coverage between the runs means the stacks were not measured under the same
   conditions; fix the cause and repeat the run. The note about a single run is
   expected for a run with `-R 1`.
2. Check `*-sysinfo-after.txt` of both boards: `throttled` must be `0x0`, the
   governor must still be `performance`, and `ptp4l_running` must be `yes`.
3. Compare the one-way latencies with the PTP offset you noted in section 2.4.
   A non-zero `negative_latency` count in the subscriber rows means that the
   clocks disagree.
4. Look at the spread over the repetitions. A wide range usually means
   background load or thermal throttling; repeat the run.

To merge existing result directories by hand, for example after a repeated run:

```bash
performance-measurement/scripts/merge-results.py --out <dir>/merged \
    lgsomeip=<run1>,<run2> vsomeip=<run1>,<run2> opensomeip=<run1>,<run2>
```

The meaning of each column and how to compare the stacks fairly are described
in [stack-comparison.md](docs/stack-comparison.md#how-to-read-the-results) and
[lg-someip/README.md](lg-someip/README.md#what-is-measured).

## 5. Create the report

### 5.1 Measure the storage footprint

The report compares the storage footprint of one deployment role from stripped
binaries. Run `rom-footprint.sh` on one board, where the binaries were built, and
save its output with the results:

```bash
ssh <ssh host B> 'cd lg-someip && performance-measurement/scripts/rom-footprint.sh \
    -i "boost|vsomeip|lgsomeip" \
    "lgsomeip-consumer=build-pi/daemon/someip-daemon,build-pi/performance-measurement/lg-someip/perf-rpc-client" \
    "vsomeip-consumer=$HOME/vsomeip-src/build/examples/routingmanagerd/routingmanagerd,performance-measurement/vsomeip/build/perf-rpc-client" \
    "opensomeip-consumer=performance-measurement/opensomeip/build/perf-rpc-client"' \
    > "${out}-rom-footprint.txt"
```

See [stack-comparison.md](docs/stack-comparison.md#storage-footprint) for how
to choose the files of a role.

### 5.2 Generate the report

`make-report.py` writes `lg-someip-performance-report.md` and the PNG figures
in `figures/` from the merged results. `--events` and `--rpcnet` are optional.
It needs every step of the four runs in section 3.3 (for example 128 threads
at 50 requests/s), so a run with other options cannot be turned into a report.
With `-q`, keep the `-O` options of section 3.3 and add `-H "0 50"` to the
threads run; quick mode would otherwise measure rate 0 only.
Write the report to a working directory first:

```bash
sudo apt-get install python3-matplotlib    # once, if it is missing

performance-measurement/scripts/make-report.py \
    --standard "${out}-standard" --threads "${out}-threads" \
    --events "${out}-events" --rpcnet "${out}-rpcnet" \
    --rom "${out}-rom-footprint.txt" --out "${out}-report"
```

Every number and figure is computed from the result files. The sentences
around them are fixed text that describes the published results. After a new
run, read the generated report carefully: if a ranking or trend has changed,
update the text in `make-report.py` and generate the report again.

## 6. Publish the report

1. Copy the reviewed report to the folder of the board:

   ```bash
   rm -rf performance-measurement/reports/pi4
   cp -r "${out}-report" performance-measurement/reports/pi4
   cp "${out}-rom-footprint.txt" performance-measurement/reports/pi4/rom-footprint.txt
   ```

   Use a new folder under `reports/` for a new board model.

2. Update the documents that cite the numbers, if they changed:
   [lg-someip-vs-vsomeip.md](reports/lg-someip-vs-vsomeip/lg-someip-vs-vsomeip.md)
   and its figures, and
   [opensomeip-v0.2.0-findings.md](reports/opensomeip-v0.2.0-findings.md).

3. Commit to the `performance_comparison` branch. The raw data in `results/`
   is not committed; keep the result directories that a published report cites
   (the "Data:" line at the end of the report) in a separate archive.

   ```bash
   git switch performance_comparison
   git add performance-measurement
   git commit -m "Update the Raspberry Pi 4 performance report"
   ```

The report records the lg-someip revision that was measured. To measure a newer
lg-someip, merge `main` into `performance_comparison` first, and then run the
measurements again.

## 7. Troubleshooting

| Symptom | What to check |
|---|---|
| `ssh ... BatchMode` errors | Key-based login (`ssh-copy-id`), or an SSH config entry that resolves the host. |
| `-y` build fails on a new board | Run once with `-I`, or install the packages printed by the script. |
| Preflight stops before the run | Equalize kernel, `rmem_max`, `ptp4l`, and governor on both boards, or use `-f`. |
| One-way latency is "invalid" or `negative_latency` > 0 | `ptp4l` is not running on both boards, or the offset has not settled (`~/ptp4l.log` on host B). |
| vsomeip `route-check` fails, or events and SD do not arrive | Multicast route on both boards (section 2.4); see also "vsomeip notes" in [stack-comparison.md](docs/stack-comparison.md#vsomeip-notes). |
| A phase fails without a clear message | Rerun the stack's `run-two-hosts.sh` with `-L debug -q` and read the daemon and application logs in the result directory. |
| Results vary a lot between repetitions | `throttled` in the sysinfo files, background load, and a direct cable link. |

## 8. Reference documents

- [lg-someip/README.md](lg-someip/README.md): the lg-someip applications, their
  options, what each metric means, and how to get stable numbers.
- [docs/stack-comparison.md](docs/stack-comparison.md): the fairness rules of
  the comparison, peer-count and thread sweeps, how to read the results, known
  limitations, and notes for vsomeip and opensomeip.
- [reports/pi4](reports/pi4/lg-someip-performance-report.md) and
  [reports/pi5](reports/pi5/lg-someip-performance-report.md): the generated
  performance reports.
- [reports/lg-someip-vs-vsomeip](reports/lg-someip-vs-vsomeip/lg-someip-vs-vsomeip.md):
  comparison document for S-CORE.
- [reports/opensomeip-v0.2.0-findings.md](reports/opensomeip-v0.2.0-findings.md):
  analysis of opensomeip v0.2.0.
