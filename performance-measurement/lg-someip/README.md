# Performance Measurement Applications

Benchmark applications for LG SOME/IP. They use only the public
`lgsomeip::api`, so the results show what a real application sees: the library
send and receive paths, the local IPC hops through `someip-daemon`, and the
network when provider and consumer run on different hosts.

The measurement kit in `common/` has no dependency on any SOME/IP stack. The
vsomeip and opensomeip applications in [`vsomeip`](../vsomeip) and
[`opensomeip`](../opensomeip) compile the same headers, so all stacks are
measured with identical code.

| Scenario | Applications | Status |
|---|---|---|
| RPC round-trip latency (closed loop) | `perf-rpc-server`, `perf-rpc-client --mode pingpong` | available |
| RPC throughput and latency under load (open loop) | `perf-rpc-server`, `perf-rpc-client --mode rate` | available |
| RPC from one process with many request threads (CPU, throughput) | `perf-rpc-server`, `perf-rpc-threads` | available |
| Event one-way latency, throughput, loss, reordering, fan-out | `perf-event-publisher`, `perf-event-subscriber` | available |
| Service Discovery timing: start-up, offer, subscribe, stop, crash detection | `perf-sd-provider`, `perf-sd-probe` | available |
| Sweeps and plots | `scripts/run-matrix.sh`, `scripts/plot.py` | planned |

## Build

The applications are built with the other examples (`-DENABLE_EXAMPLES=ON`,
Linux only). Use a release build for meaningful numbers:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Binaries are written to `build/performance-measurement/lg-someip/`.

## Quick start: same-host RPC ping-pong

```bash
performance-measurement/lg-someip/scripts/run-rpc-pingpong.sh -b build -o performance-measurement/results -c 1,2,3
```

The script generates a configuration, starts the daemon and the server, runs
the client once per payload size, and stops everything afterwards. Results:

- `performance-measurement/results/rpc-pingpong.csv`: one row per run, appended across runs.
- `performance-measurement/results/rpc-pingpong-<transport>-<payload>.samples`: every round-trip
  time in microseconds, in send order, for histograms and CDF plots.
- `performance-measurement/results/daemon.log`, `server.log`: process output, including the server
  CPU and context-switch summary.

Script options: `-p "<sizes>"`, `-n <count>`, `-w <warmup>`, `-t udp|tcp`,
`-s` (busy-wait in the client), and `-c <daemon>,<server>,<client>` CPU lists.

## Quick start: same-host RPC throughput

```bash
performance-measurement/lg-someip/scripts/run-rpc-rate.sh -b build -o performance-measurement/results -c 1,2,3-4
```

For each payload size (`-p`, default `64 1024 16384`), the script steps the
offered rate up (`-r`, default `1000 5000 10000 20000 50000 0`). Each step runs
for `-d` seconds (default 5) after a warm-up of one tenth of the step. Rate `0`
sends as fast as the in-flight window (`-W`, default 1000) allows and runs
`-n` requests; it gives the saturation throughput. Results go to
`performance-measurement/results/rpc-rate.csv`, and one samples file is written per step.

To find the saturation point, look for the step where `throughput_msg_s` stops
following the offered rate, or where `p99_us` and `max_send_lag_us` jump.

## Quick start: same-host events

```bash
performance-measurement/lg-someip/scripts/run-event.sh -b build -o performance-measurement/results -c 1,2,3-10
```

For each subscriber count (`-S`, default `1 4`, at most 8), payload size
(`-p`), and offered rate (`-r`, default `1000 10000 0`), the script works as
follows:

1. It starts N subscribers.
2. It starts one publisher, which waits until all N have subscribed.
3. The publisher sends a warm-up of one tenth of the step, then `rate × -d`
   notifications (rate `0` sends `-n` notifications as fast as possible).
4. The publisher repeats an end marker that carries the counts.

Each subscriber writes one row to `performance-measurement/results/event-subscribe.csv`, and the
publisher writes one row to `performance-measurement/results/event-publish.csv`. The `-c` option
takes the daemon, publisher, and subscriber CPU lists; all subscribers share
the third list.

## Quick start: Service Discovery timing

```bash
performance-measurement/lg-someip/scripts/run-sd.sh -b build -o performance-measurement/results -k 20 -c 1,2,3-4
```

The script runs three phases with `-k` iterations each (`-P` selects phases):

- **cold**: the provider is already offering; a new probe process starts each
  time.
- **toggle**: the provider offers for `-T <on ms>`, then withdraws for
  `<off ms>`.
- **crash**: the provider is started, then killed with `kill -9`.

Each episode becomes one row of `performance-measurement/results/sd-probe.csv`, and the script
prints n/min/p50/max per phase. Options after `--` are passed to
`gen-config.sh`, so the SD timing can be varied, for example
`run-sd.sh -- -I 50 -R 100 -M 2 -C 1000`.

On one host, the daemon passes local offers, stops, and subscriptions straight
to the applications without the SD timers. A local run therefore measures the
local path (below 1 ms), not Service Discovery. The SD timers and TTL only
apply between hosts; run the same applications on two hosts as described
below.

## Remote measurement (two hosts)

Generate one configuration per host. Use the same transport on both hosts:

```bash
# provider host (192.168.0.10)
scripts/gen-config.sh -o perf.json -r provider -u 192.168.0.10 -t udp
someip-daemon perf.json &
perf-rpc-server --config perf.json

# consumer host (192.168.0.20)
scripts/gen-config.sh -o perf.json -r consumer -u 192.168.0.20 -t udp
someip-daemon perf.json &
perf-rpc-client --config perf.json --payload 1024 --label remote-udp --csv rpc-pingpong.csv
```

For events, run `perf-event-publisher` on the provider host and
`perf-event-subscriber` on the consumer host. Use `--clock realtime` on both
for one-way latency, and synchronize the two hosts' `CLOCK_REALTIME` with PTP
(`ptp4l` + `phc2sys`). The remaining clock offset is part of the result, so
record it (`pmc` or `phc2sys` logs) together with the numbers. A latency that
would come out negative is stored as 0 and counted in the `negative_latency`
column of the subscriber row; a non-zero count means the clocks disagree.

For Service Discovery, run `perf-sd-provider` on the provider host and
`perf-sd-probe` on the consumer host:

```bash
# provider host: offer forever (cold start, crash) or toggle (graceful stop)
perf-sd-provider --config perf.json --cycles 20 --on-ms 5000 --off-ms 3000

# consumer host
perf-sd-probe --config perf.json --episodes 20 --wait-down --phase toggle --label remote-udp --csv sd-probe.csv
```

For a crash test, kill the provider host's `perf-sd-provider` with `kill -9`,
or unplug its link, while the probe runs with `--wait-down`.
`offer_to_available_ms` needs PTP-synchronized `CLOCK_REALTIME` on both hosts;
the other metrics use only the consumer's clock.

lg-someip uses TCP when a service has a `reliable` endpoint, so
`gen-config.sh -t` writes only the endpoint for the chosen transport. Payloads
larger than `SOMEIP_UDP_MAX_PAYLOAD_SIZE` (1400 bytes by default) need TCP or a
build with `ENABLE_SOMEIP_TP=ON` for remote UDP.

## Raspberry Pi 4/5: all measurements on two boards

This setup uses two Raspberry Pis connected by one Ethernet cable for the
benchmark traffic, and a PC that reaches both over SSH (for example over
Wi-Fi) to drive the runs. Pi A is the provider and Pi B the consumer.

| Script | Runs on | Purpose |
|---|---|---|
| `scripts/pi-setup.sh` | each Pi | Install packages, build into `build-pi/`, warn about the CPU governor and throttling. |
| `scripts/run-local-suite.sh` | one Pi | All same-host benchmarks with 4-core defaults (`-c 1,2,3`). |
| `scripts/run-two-hosts.sh` | the PC | Copy and build the working tree, run the local suite on both Pis, then the two-host benchmarks, and collect all results. |
| `scripts/remote-agent.sh` | each Pi | Called by `run-two-hosts.sh` over SSH; not used directly. |
| `scripts/sysinfo.sh` | each Pi | Board, kernel, governor, frequencies, temperature, throttling, clock sync, and link state. |

### 1. Prepare each Pi (once)

- Use a 64-bit OS (Raspberry Pi OS 64-bit or Ubuntu arm64), with
  cooling and the official power supply. Throttling changes results without
  any other sign; `sysinfo.sh` reports `throttled` before and after each run.
- Allow key-based SSH login from the PC.
- Give the cable interface a static address. On Raspberry Pi OS
  (NetworkManager), for example:

  ```bash
  # Pi A (use 192.168.100.2/24 on Pi B)
  sudo nmcli con add type ethernet ifname eth0 con-name perf-link \
      ipv4.method manual ipv4.addresses 192.168.100.1/24 ipv6.method disabled
  sudo nmcli con up perf-link
  ```

  The SD multicast group is sent through the interface that holds the
  configured unicast address, so no multicast route is needed.
- Install the build packages once: add `-I` to the first
  `run-two-hosts.sh -y` (step 3). It asks for each Pi's sudo password in your
  terminal and installs `build-essential cmake git rapidjson-dev linuxptp
  ethtool rsync nftables`. Later `-y` runs rebuild without sudo (`pi-setup.sh -n`).
  Without `-I`, a `-y` run on a new Pi stops and prints the install command.

### 2. Before each measurement session

```bash
# on both Pis: fixed CPU frequency
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# clock sync over the cable, needed for one-way event latency and offer_to_available;
# ptp4l runs in the background and logs to ~/ptp4l.log
sudo timedatectl set-ntp false            # stop NTP from adjusting the clock as well
sudo nohup ptp4l -i eth0 -S -m > ~/ptp4l.log 2>&1 < /dev/null &      # Pi A (time source)
sudo nohup ptp4l -i eth0 -S -s -m > ~/ptp4l.log 2>&1 < /dev/null &   # Pi B (follows Pi A)
```

The settings do not survive a reboot. The stack comparison also needs the same
`net.core.rmem_max` on both Pis and, for vsomeip, a multicast route; see
section 2.4 of the [measurement guide](../README.md#24-before-each-measurement-session).

`-S` uses software timestamps and steers `CLOCK_REALTIME` directly, which
works on both models. If `ethtool -T eth0` lists `hardware-transmit` (shown as
`link_hw_timestamping=yes` by `sysinfo.sh`), you can use hardware
timestamping instead: run `ptp4l` without `-S` and `phc2sys -s eth0 -w` to
copy the NIC clock to the system clock. Wait until Pi B's `master offset`
values (`tail -f ~/ptp4l.log`) settle, and note their typical size: it is the error bound of every
one-way and `offer_to_available` number. Round-trip results do not depend on
clock sync.

### Driving the run from a PC without the git checkout

When the source lives on a development server that cannot reach the Pis,
make an archive there and run from the PC that can reach them (Linux,
macOS, or WSL on Windows; it needs `bash`, `ssh`, `rsync`, and `tar`):

```bash
# on the development server
performance-measurement/lg-someip/scripts/make-bundle.sh
# -> performance-measurement/results/bundle/lg-someip-perf-<revision>.tar.gz

# on the PC, after copying the archive there
tar -xzf lg-someip-perf-<revision>.tar.gz && cd lg-someip
```

The archive holds the files git tracks plus untracked files under
`performance-measurement/` (except `results/`), and a `REVISION` file. Run the commands
of step 3 from the unpacked `lg-someip` directory; `-y` then copies every file
of the unpacked tree, and the revision from `REVISION` is recorded in the
results.

### 3. Run everything from the PC

```bash
performance-measurement/lg-someip/scripts/run-two-hosts.sh -y \
    -A <ssh host of Pi A> -B <ssh host of Pi B> -a 192.168.100.1 -b 192.168.100.2
```

This copies the source to `~/lg-someip` on both Pis (`-d` changes the path):
the files git tracks plus untracked files under `performance-measurement/`
(except `results/`), so uncommitted work there is included and nothing else in the tree is copied.
It then builds, records the PC's git revision in every result row, and runs
these scenarios in order (`-s` selects them):

1. `local`: `run-local-suite.sh` on both Pis at the same time, before any
   two-host daemon starts (the suites use their own daemons).
2. `pingpong`, `rate`: `perf-rpc-server` on A, `perf-rpc-client` on B.
3. `event`: `perf-event-publisher` on A, 1 and 4 subscribers on B
   (`-S`).
4. `sd`: four phases with `-k` iterations each:
   - `toggle`: graceful StopOffer.
   - `app-crash`: the provider app is killed while A's daemon keeps running.
   - `node-crash`: A's daemon and app are killed, so B detects the loss by
     TTL. Each restart is also a provider cold start.
   - `consumer-cold`: B's daemon and probe are restarted.

Add `-q` for a smoke test of a few minutes with small counts before the
first full run. Before the two-host tests, the script pings A from B and
saves the result as `ping-B-to-A.txt`, which is the round-trip floor of the
link without lg-someip. `-L debug` raises the log level of the two-host
daemons and apps to diagnose a failing phase; logs are appended across
restarts, so every phase keeps its output. UDP runs use payloads of 64, 1024 and 1400 bytes by default; 1400 bytes is
the largest SOME/IP payload the specification allows in one UDP datagram
without SOME/IP-TP. Add `-t tcp` for a TCP run with 4096, 16384 and 65536
bytes. Use `-n` to print every host command without running anything.

Results are copied to `performance-measurement/results/two-hosts-<date>/` on the PC:
`A-provider/` and `B-consumer/` hold the CSVs, samples, and logs of each Pi
(`local/` holds its same-host suite), and `*-sysinfo-before.txt` and
`*-sysinfo-after.txt` record the board state. `*-build-size.txt` holds the
daemon and app binary sizes (storage footprint), and `*-daemon-usage-before.txt`
/ `*-daemon-usage-after.txt` hold each daemon's cumulative CPU time and peak
RSS (CPU is the scheduler run time summed over all threads, in milliseconds,
not the 10 ms ticks of `/proc/<pid>/stat`; `daemon_cpu_source` says which was
available; time of threads that already exited is missing); their difference
covers the `pingpong`/`rate`/`event` scenarios (`sd`
restarts the daemons, so it falls outside that window). By estimate, a full
UDP run with the defaults takes about half an hour; the event sweep is the
longest part.

To run the same-host benchmarks on a single Pi only:

```bash
performance-measurement/lg-someip/scripts/run-local-suite.sh -b build-pi -o performance-measurement/results/local
```

### Notes for two-host results

- In two-host runs, every message also crosses the daemon on each Pi, so a
  round trip makes four local IPC hops plus two link traversals.
- Host B's daemon subscribes once for all of its local subscribers, so the
  publisher sees one remote subscription however many subscribers run on B.
- Over the link, UDP has no backpressure: `rate` and `event` steps above the
  link or CPU capacity show loss instead of the blocking seen on one host.
- CPU 0 is left free by default (`-c 1,2-3`) for network interrupts.

## perf-rpc-client options

| Option | Default | Meaning |
|---|---|---|
| `--mode` | `pingpong` | `pingpong` (closed loop) or `rate` (open loop). |
| `--config` | library default | Configuration file path. |
| `--name` | `perf-rpc-client` | Application name; `perf-rpc-client-2` to `-4` exist for parallel clients. |
| `--payload` | `64` | Request size in bytes (minimum 16, the measurement header). |
| `--count` | `10000` | Measured requests. |
| `--warmup` | `1000` | Requests sent first and not recorded. |
| `--interval-us` | `0` | pingpong: pause between requests; `0` sends back to back. |
| `--rate` | `1000` | rate: offered requests per second; `0` sends as fast as the window allows. |
| `--window` | `1000` | rate: maximum requests in flight. |
| `--timeout-ms` | `1000` | pingpong: wait per response. rate: final drain time, and how long a full window may last before the run stops as stalled. |
| `--discovery-timeout-ms` | `10000` | Time to wait for the service to become available. |
| `--spin` | off | pingpong: busy-wait for the response instead of a condition variable. |
| `--label` | empty | Free text written to the CSV, such as `local-udp` or `remote-tcp-e2e`. |
| `--csv` | none | Append the result row to this CSV file. |
| `--samples-out` | none | Write every round-trip time in microseconds. |
| `--responders` | `1` | Call service instances 1..N in turn; `--rate` is then per responder (the requester sends N × rate). |
| `--start-at` | `0` | Start the run at this wall-clock time (milliseconds since the epoch), so several requesters begin together; `start_late_ms` reports a late start. |

In rate mode, `--warmup + --count` must fit a 32-bit sequence number.

The exit code is `0` when every request was answered, `2` when the service was
not found, and `3` when requests were lost, the run stalled, or it was
interrupted.

`perf-rpc-server` takes `--config`, `--name`, `--instance` (the service
instance it offers, default 1), and `--response-size` (`0`, the default,
echoes the request payload). Several responders need several instances in the
configuration: `scripts/gen-config.sh -i <count>` declares instances 1..count
of the RPC and event services. `performance-measurement/scripts/run-peers.sh` runs
whole requester/responder and publisher/subscriber matrices.

## perf-rpc-threads options

One process, `--threads` worker threads. Each worker sends a request, waits for
its response, and repeats, taking the `--responders` in turn. The response
payload has the size of the request (the server echoes it); a response of
another size is counted as `bad_size`. Shared code: `common/ThreadStress.h`.

| Option | Default | Meaning |
|---|---|---|
| `--threads` | `8` | Worker threads (at most 1024). |
| `--payload` | `64` | Request and response size in bytes (minimum 16). |
| `--responders` | `1` | Call service instances 1..N in turn. |
| `--thread-rate` | `0` | Requests per second of one worker; `0` sends the next request as soon as the response arrived. A paced worker sleeps between requests, so pacing costs no CPU. |
| `--warmup-s`, `--duration-s` | `1`, `5` | Unmeasured and measured seconds. |
| `--timeout-ms` | `1000` | Wait per response; a request without one counts as lost and holds its worker for that time. |
| `--config`, `--name`, `--discovery-timeout-ms`, `--label`, `--csv`, `--samples-out`, `--start-at` | | As for `perf-rpc-client`. |

The row reports throughput, loss, latency percentiles (from before `send()` to
the worker's wake-up), and the process CPU over the measured window:
`cpu_pct` (100 = one core) and `cpu_per_req_us`. `calls_total` counts every
call including warm-up, for dividing host-level CPU figures. The exit code is
`3` when requests were lost.

## perf-event-publisher and perf-event-subscriber options

| Publisher option | Default | Meaning |
|---|---|---|
| `--payload`, `--count`, `--warmup` | `64`, `10000`, `1000` | As for the RPC client. |
| `--rate` | `1000` | Notifications per second; `0` publishes as fast as `notify()` returns. |
| `--subscribers` | `1` | Wait for this many subscriptions before publishing. |
| `--subscribe-timeout-ms` | `10000` | Give up (exit code 2) if they do not arrive. |
| `--settle-ms` | `500` | Pause after the last subscription so its acknowledgement completes. |
| `--end-repeat` | `5` | How often the end marker is sent, 100 ms apart. |
| `--clock` | `monotonic` | Timestamp clock: `monotonic` (same host) or `realtime` (PTP-synchronized hosts). |
| `--instance` | `1` | Service instance to offer; one publisher per instance. |
| `--start-at` | `0` | Start publishing at this wall-clock time (ms since the epoch), as for the RPC client. |
| `--name`, `--config`, `--label`, `--csv` | | As for the RPC client. |

| Subscriber option | Default | Meaning |
|---|---|---|
| `--name` | `perf-event-sub-1` | `perf-event-sub-1` to `-8` exist in the generated configuration. |
| `--clock` | `monotonic` | Must match the publisher. |
| `--publishers` | `1` | Subscribe to instances 1..N; the run ends when every publisher's end marker arrived. Loss is counted per publisher; `publishers` and `publishers_expected` show how many were heard. |
| `--start-timeout-ms` | `30000` | Give up if no notification arrives (exit code 2). |
| `--idle-timeout-ms` | `5000` | Stop after this long without traffic if the end marker never comes. |
| `--drain-ms` | `200` | Keep receiving after the end marker; it can overtake queued data. |
| `--config`, `--label`, `--csv`, `--samples-out` | | As for the RPC client. |

The subscriber exits with `0` when nothing was lost, and with `3` on loss,
an idle timeout, or interruption.

## perf-sd-provider and perf-sd-probe options

| Provider option | Default | Meaning |
|---|---|---|
| `--cycles` | `0` | Offer/withdraw cycles; `0` offers until SIGINT. |
| `--on-ms`, `--off-ms` | `2000`, `2000` | Time offered and time withdrawn per cycle. |
| `--heartbeat-ms` | `10` | Heartbeat period while offered; it bounds the crash-detection error. |

| Probe option | Default | Meaning |
|---|---|---|
| `--episodes` | `1` | Availability periods to record before exiting. |
| `--wait-down` | off | Also wait for each period to end (toggle and crash tests). |
| `--timeout-ms` | `30000` | Maximum wait for availability or unavailability (exit code 3). |
| `--event-timeout-ms` | `3000` | Maximum wait for the initial field event after subscribing. |
| `--phase` | empty | Free text for the `phase` column. |

Both also take `--name`, `--config`, and the probe takes `--label` and `--csv`.

## What is measured

Each request carries a 16-byte header (magic, sequence number, send timestamp,
big-endian) followed by filler bytes. The server echoes it back.

**pingpong (closed loop).** One request is outstanding at a time. The
round-trip time runs from just before `send()` to the start of the response
callback. It includes both library paths, both daemon hops, and the provider
callback, but not the wakeup of the client's main thread. This gives the
latency floor.

**rate (open loop).** Requests are sent on a fixed schedule, whether or not
responses have arrived. The header carries the *scheduled* send time, and the
latency is measured from that time. If the sender falls behind (because
`send()` blocks or the window is full), the delay therefore counts toward the
latency rather than being hidden. This avoids the "coordinated omission" error
of closed-loop load tests. Responses are matched by sequence number, so loss
and duplicates are counted exactly.

Common result columns:

- `discovery_ms`: time from `start()` to the availability callback. This is
  only a rough indicator; use `perf-sd-probe` for discovery timing.
- `min_us` to `max_us`: exact nearest-rank percentiles over every sample.
- `cpu_user_ms`, `cpu_sys_ms`, `cpu_total_ms`, `*_ctx_switches`: process usage
  during the measured phase. The scheduler may charge CPU in whole ticks
  (4 to 10 ms), so the user/system split can be one-sided; compare
  `cpu_total_ms`. `max_rss_kb` is the process peak.
- `cpu_pacing_ms`, `cpu_net_ms`: the sender waits for each scheduled send by
  sleeping and then yielding for the last 200 µs. The CPU time of that wait is
  benchmark overhead, so it is reported as `cpu_pacing_ms` and removed from
  `cpu_net_ms` (`cpu_total_ms` minus `cpu_pacing_ms`). Compare `cpu_net_ms`
  across stacks and rates. It still includes the library's own threads and,
  with `--spin` or a full in-flight window, the busy-waiting for responses.
- `revision`: the git revision the binary was built from.

pingpong columns:

- `sent`, `lost`, `late`, `invalid`: requests sent; requests with no response
  within the timeout; responses that arrived after their timeout; responses
  without a valid header.
- `rate_msg_s`: completed round trips per second. This is limited by latency,
  not by the library's maximum throughput.

rate columns:

- `target_rate_msg_s`, `send_rate_msg_s`: offered rate, and the rate actually
  achieved by the sender.
- `throughput_msg_s`, `throughput_mbit_s`: measured responses per second, from
  the first measured send to the last response. Megabits count the payload in
  one direction.
- `lost`, `loss_pct`, `duplicates`: responses that never arrived (including
  after the drain), and responses seen twice.
- `max_in_flight`, `window_waits`, `stalled`: highest number of outstanding
  requests, how often the window was full, and whether the run stopped
  because the window stayed full for `--timeout-ms`.
- `max_send_lag_us`: worst delay between a scheduled send time and the
  actual send.

`perf-rpc-server` prints a summary when it stops. `lifetime_cpu_ms` is its whole
process CPU time, including start-up and idle time. The `cpu_*` columns cover
the traffic window only, from the first to the last request served (warm-up
included), so idle time before and after the benchmark stays out of the figure.

**Events (one way).** Each notification carries its sequence number and its
scheduled send time. The subscriber records the time at the start of its
callback minus that send time. The end marker tells each subscriber the total
and warm-up counts, so loss is exact.

event-subscribe columns:

- `end_marker`: `1` if the end marker arrived. Without it, the expected count
  falls back to the highest sequence seen, so loss among the final
  notifications is not visible.
- `expected`, `lost`, `loss_pct`, `duplicates`: measured notifications
  (warm-up excluded), and how many of them were missing or seen twice.
- `gap_runs`, `first_missing`, `last_missing`: how many separate runs of
  missing sequence numbers there were, and the first and last missing one.
  This separates a loss at start-up or at the end from drops during the run.
  With several publishers, sequence numbers are per publisher, so
  `first_missing` and `last_missing` are 0 and only `gap_runs` is summed.
  When several subscribers on one host miss exactly the same count, the
  notifications were lost before that host's daemon, not in local delivery.
- `reordered`: notifications whose callback ran after one with a higher
  sequence number. The library hands notifications to a pool of callback
  threads, so even a loss-free local run can deliver some out of order.
- `negative_latency`: notifications whose receive time was before their send
  time. Their latency is stored as 0. A non-zero count means the two clocks
  disagree (no PTP, or a wrong `--clock`), and the latency columns are invalid.
- `throughput_msg_s`, `throughput_mbit_s`: every notification received
  (warm-up included) over the time from the first to the last arrival.
- `min_us` to `max_us`: one-way latency.

event-publish columns:

- `send_rate_msg_s`: notifications actually published per second. For local
  subscribers, `notify()` blocks when the local sockets are full, so at rate
  `0` this shows how fast the stack accepts notifications for N subscribers.
- `notify_*_us`: time spent inside the `notify()` call, not including the
  payload stamping. It is the cost of handing the notification to the stack,
  not a delivery latency.
- `max_send_lag_us`: worst delay between a scheduled send time and the
  actual send.

**Service Discovery.** The provider writes its wall-clock offer time into a
field, which reaches every new subscriber as its initial event. While offered,
it sends a heartbeat every `--heartbeat-ms`, and it sends a "stopping"
notification just before `stop_offer_service()`. The probe subscribes as soon
as the service becomes available, and records one row per availability period
(episode):

- `offer_minus_start`: provider offer time minus probe start time, in
  milliseconds (wall clocks of both hosts). It decides whether an episode counts
  as a cold start or as an offer after start; within the clock offset of zero
  the decision can be wrong (the probe prints a warning below 200 ms).
- `start_to_available_ms`: probe `start()` to availability. Only reported
  when the service was already offered at start-up (cold start).
- `offer_to_available_ms`: provider `offer_service()` to availability. Only
  reported for offers made after the probe started; it needs synchronized
  clocks between hosts. Every provider notification carries the offer time,
  so this works even when the initial field event is missing.
- `subscribe_to_field_ms`, `subscribe_to_first_event_ms`: `subscribe()` to
  the initial field event, and to the first notification of any kind.
- `stop_to_unavailable_ms`: "stopping" notification to unavailability, that
  is, how fast a graceful StopOffer reaches the consumer. It is negative, and
  `stopping_after_unavailable` is 1, when the StopOffer overtook the
  notification that the provider sent before it.
- `last_event_to_unavailable_ms`: last notification to unavailability. For a
  crashed provider, this is the detection time, with an error of up to one
  heartbeat period.

### What to expect between hosts

The values below come from reading the current code, not from a two-host
measurement. Use them to check results.

- The daemon runs SD on a 100 ms timer, so offers and finds go out in 100 ms
  steps.
- The first offer waits `initial_delay_min`. `initial_delay_max` is parsed but
  not used, so the random initial delay of the SOME/IP SD specification is not
  applied.
- Repetitions are sent `repetitions_base_delay × 2^(n+1)` apart, up to
  `repetitions_max`. After that, offers follow `cyclic_offer_delay`.
  FindService has no cyclic phase.
- The offer TTL is fixed at 3, whatever the `ttl` key says. TTLs are counted
  down about every 1.1 s, so a crashed remote provider should be detected
  about 3.3 to 4.4 s after its last offer, and a crash should never be
  detected sooner than that.
- `request_response_delay` is parsed but not used.
- A graceful StopOffer is forwarded to applications immediately.

## Getting stable numbers

- Build with `CMAKE_BUILD_TYPE=Release` and keep logging at `warning` (the
  generated configuration does this).
- Pin the daemon, the server, and the client with `-c`. On a many-core host,
  pinning alone can cut the median latency by more than half, because it
  avoids thread migration between cores.
- `taskset` pins every thread of a process. With `--spin`, give the client at
  least two CPUs (for example `-c 1,2,3-4`). Otherwise the spinning thread
  starves the library's receive thread, and the result shows scheduler
  timeslices (about 1 ms) instead of latency.
- Set the CPU frequency governor to `performance`. Avoid running other loads
  at the same time.
- Repeat each run and compare medians and high percentiles rather than one
  mean value.
- At low rates, latency can be *higher* than at moderate rates, because idle
  threads sleep and CPUs enter idle states between messages. Compare runs at
  the same rate, and report the rate together with the latency.
