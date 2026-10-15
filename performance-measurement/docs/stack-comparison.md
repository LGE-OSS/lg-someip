# SOME/IP stack comparison: lg-someip vs vsomeip vs opensomeip

The stack comparison benchmarks three independent SOME/IP implementations
against each other with the same measurement code as
[`performance-measurement/lg-someip`](../lg-someip): a
16-byte header (magic, sequence, send timestamp) in every payload, open-loop
rate scheduling to avoid coordinated omission, and exact (not bucketed)
percentiles. The [lg-someip README](../lg-someip/README.md) explains the
measurement rationale, and the [measurement guide](../README.md) walks through
a full measurement; this document covers what is specific to comparing stacks.

| Scenario | lg-someip | vsomeip | opensomeip |
|---|---|---|---|
| RPC ping-pong and rate | yes | yes | yes, direct endpoint (no daemon, no SD) |
| Events | yes, daemon + SD | yes, routing manager + SD | yes, direct event harness (no SD, one subscriber) |
| Service Discovery timing | yes | yes (graceful toggle only) | not available in the harness |
| Peer-count sweeps (N×M RPC, M×N events) | yes | yes | yes (RPC direct, events direct with one endpoint per subscriber) |

## Fairness contract

- **One measurement kit.** `common/` of the lg-someip suite
  (`EventTally.h`, `LatencyStats.h`, `Options.h`, `Pacing.h`, `PerfPayload.h`,
  `Report.h`, `ThreadStress.h`) is compiled into all three stacks' applications; the vsomeip and opensomeip
  CMake files point at it. It has no stack dependency (its payload type is the
  same `std::vector<uint8_t>` as `lgsomeip::api::Payload`), so percentiles,
  pacing, CPU accounting, and the wire layout cannot drift between stacks.
- **Identical IDs.** The same service, instance, method, and event IDs
  (`0x2001`, `0x2002`, `0x2003`, ...) appear on the wire for every stack.
- **What a timed call covers.** Ping-pong round-trip time runs from just before
  the stack's send call to the start of the response callback. Where a stack's
  API needs extra work per message (vsomeip creates a payload object for every
  request or notification), that work is done before the timestamp in ping-pong
  and in `notify_*_us`, so these columns cover the send path only. Rate mode
  measures from the scheduled send time and therefore includes it.
- **CPU.** Compare `cpu_net_ms` (client CPU without the pacing busy-wait), not
  `cpu_user_ms` or `cpu_sys_ms`. Server rows report `lifetime_cpu_ms` (whole
  process) and `cpu_total_ms` (first to last request). Daemon CPU comes from
  scheduler time summed over all threads. See the
  [lg-someip README](../lg-someip/README.md#what-is-measured) for details.
- **Same hosts, equalized.** All stacks run on the same pair of boards, one
  stack at a time. Before measuring, make host A and B identical in kernel,
  `net.core.rmem_max`, CPU governor (`performance`), and run `ptp4l` on both.
  `scripts/run-comparison.sh` checks all four and stops unless told otherwise.
- **Repetitions, interleaved.** A single run proves little. `run-comparison.sh`
  repeats every stack (default 5 times) and rotates the stack order between
  repetitions, so no stack always runs first on cold hardware or last on hot
  hardware. `merge-results.py` reports the median and the range.
- **Architecture differences are reported, not hidden.** lg-someip and vsomeip
  route every message through a per-host daemon (`someip-daemon`,
  `routingmanagerd`) and use Service Discovery. opensomeip has no daemon: its
  RPC and event classes own their UDP/TCP sockets, and the benchmark passes the
  provider address and port explicitly. Its numbers therefore exclude daemon IPC
  and discovery by design; never present them as a blind "faster" result.

## Layout

```
performance-measurement/
  README.md              measurement guide (start here)
  lg-someip/             lg-someip applications, the shared measurement kit (common/), and their scripts
  scripts/
    run-comparison.sh    interleaved repetitions of all stacks + merge
    run-peers.sh         peer-count sweep of one stack (requesters x responders, publishers x subscribers, many request threads)
    merge-results.py     unified CSV files and the generated comparison.md
    make-report.py       the lg-someip performance report (Markdown + figures) from the merged runs
    build-stacks.sh      build and install vsomeip 3.7.6 and opensomeip v0.2.0 on a board
    rom-footprint.sh     stripped storage footprint of one deployment role
  vsomeip/               apps written against the public vsomeip API
    scripts/             gen-config.sh, remote-agent.sh, run-two-hosts.sh
  opensomeip/            apps written against opensomeip's native API
    scripts/             remote-agent.sh, run-two-hosts.sh
  docs/                  this document
  reports/
    pi5/                 the generated report for the Raspberry Pi 5 pair (rasp3, rasp4)
    pi4/                 the same report for the Raspberry Pi 4 pair (rasp1, rasp2)
    lg-someip-vs-vsomeip/            comparison document for S-CORE
    opensomeip-v0.2.0-findings.md    opensomeip v0.2.0 analysis
  results/               raw measurement output (ignored by git)
```

The lg-someip applications and their scripts live in
[`performance-measurement/lg-someip`](../lg-someip); the
coordinator there (`scripts/run-two-hosts.sh`) is the lg-someip counterpart of
the two coordinators here and takes the same options.

## Preparing the hosts

1. **Hosts.** Two boards on one Ethernet link, key-based SSH from the driving
   PC, as described in the
   [measurement guide](../README.md#2-prepare-the-environment). Use the same
   OS image and kernel on both, and set the same `net.core.rmem_max` on both
   (`sudo sysctl -w net.core.rmem_max=4194304`).
2. **Clocks.** Run `ptp4l` on both (see the measurement guide) when you want
   one-way event latency. Without it, `merge-results.py` marks one-way
   latency "invalid" instead of showing the clock offset as a latency.
3. **vsomeip.** Real vsomeip needs Boost (`filesystem`, 1.75 or newer) and a C++20
   compiler. Build and install it, then the apps:

   ```bash
   git clone --depth 1 --branch 3.7.6 https://github.com/COVESA/vsomeip.git vsomeip-src
   cmake -S vsomeip-src -B vsomeip-src/build -DCMAKE_BUILD_TYPE=Release \
       -DCMAKE_INSTALL_PREFIX=$HOME/vsomeip-install -DENABLE_SIGNAL_HANDLING=1
   cmake --build vsomeip-src/build -j"$(nproc)"
   cmake --install vsomeip-src/build

   cd performance-measurement/vsomeip
   cmake -S . -B build -DUSE_REAL_VSOMEIP=ON -DCMAKE_PREFIX_PATH=$HOME/vsomeip-install -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   ```

   `routingmanagerd` is built with vsomeip's examples and is not installed; the
   agent runs it from `~/vsomeip-src/build/examples/routingmanagerd/`. Real
   vsomeip also needs a multicast route on both hosts (see "vsomeip notes").
   `scripts/build-stacks.sh` builds and installs both vsomeip and opensomeip on
   a board in one step.
4. **opensomeip.** It needs only a C++17 compiler and CMake 3.20+:

   ```bash
   cd performance-measurement/opensomeip
   cmake -S . -B build -DCMAKE_PREFIX_PATH=$HOME/opensomeip-install -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   ```

   with opensomeip 0.2.0 installed to `~/opensomeip-install` (or set
   `OPENSOMEIP_PREFIX`).
5. **lg-someip.** `scripts/pi-setup.sh` in the suite builds it into `build-pi/`.

`run-two-hosts.sh -y` of each stack copies the sources and builds them on both
hosts; `run-comparison.sh -y` does that once before the first repetition.

## Running a comparison

```bash
performance-measurement/scripts/run-comparison.sh -A <ssh host A> -B <ssh host B> \
    -a 192.168.1.103 -b 192.168.1.104 -y -R 5 -p "64 1024 1400" -r "1000 0" -N 2000 -D 2
```

Use `-q` first for a smoke test and `-n` for a dry run that prints every host
command. Results go to `performance-measurement/results/comparison-<date>/<stack>/rep<k>/` and the
merged tables to `.../merged/comparison.md` together with unified CSV files
(`rpc-pingpong.csv`, `rpc-rate.csv`, `event-publish.csv`,
`event-subscribe.csv`, with `stack` and `run` columns). To merge existing
result directories by hand:

```bash
performance-measurement/scripts/merge-results.py --out merged \
    lgsomeip=run1,run2 vsomeip=run1,run2 opensomeip=run1,run2
```

Run the same scenarios, payloads, and rates for every stack: the daemon CPU
delta covers all scenarios of a run, and `merge-results.py` warns when the
coverage differs. Each stack's coordinator can also be run alone; they take the
same options (`-A -B -a -b -s -p -r -N -D -c -y -q -n`).

Service Discovery runs for lg-someip and vsomeip only (`-s sd`). The two
coordinators use the same offer and heartbeat timing (10 ms heartbeat, 2 s on
and 2 s off per cycle); only vsomeip is limited to the graceful toggle phase.

### The same vsomeip applications on lg-someip

The stack `lgsomeip-vsomeip-api` runs the vsomeip applications in `vsomeip/`,
unchanged, on lg-someip's vsomeip API layer (`third_party/vsomeip_interface`)
and `someip-daemon`, with the lg-someip configuration. Compared with the
`vsomeip` stack, only the library and the daemon differ, so it shows what an
existing vsomeip application gains by relinking. Install lg-someip on both
hosts first, then add the stack (standard mode only):

```bash
# on each host, after pi-setup.sh has built build-pi
cmake --build build-pi && cmake --install build-pi --prefix ~/lgsomeip-install

performance-measurement/scripts/run-comparison.sh -A <ssh host A> -B <ssh host B> \
    -a <ip A> -b <ip B> -y -R 5 -S "lgsomeip lgsomeip-vsomeip-api vsomeip" \
    -s "pingpong rate" -p "64 1024 1400" -r "1000 0" -N 6000 -D 3
```

A single coordinator selects the backend with `PERF_VSOMEIP_BACKEND=lgsomeip`
(default `vsomeip`); it builds the applications in `vsomeip/build-lgsomeip`.

### Peer-count sweeps

`scripts/run-peers.sh` measures how each stack scales with the number of
peers, with responders and publishers on host A and requesters and
subscribers on host B:

- **RPC, N requesters × M responders.** Each responder offers its own
  instance of the RPC service. Every requester calls every responder in turn:
  in rate mode it offers `-r` requests per second to each responder (so a
  responder receives `-r × N`), in ping-pong mode it keeps one request
  outstanding and moves to the next responder after each answer. Defaults:
  N in `1 2 4 8`, M in `1 2 4`, both modes.
- **Events, M publishers × N subscribers.** Each publisher offers its own
  instance of the event service and sends `-r` notifications per second;
  every subscriber subscribes to every publisher and keeps loss accounting per
  publisher. Defaults: M in `1 2 4`, N in `1 2 4 8`.

All peers of one step start their measured phase at the same moment
(`--start-at`, taken from the clock of the host they run on; rows report
`start_late_ms` and the report flags a late start). Each peer writes its own
CSV row and samples file; `merge-results.py` combines a step by summing
throughput and loss over the peers and computing latency percentiles from the
pooled samples of all of them, then shows the median and range over the
repetitions as usual.

```bash
performance-measurement/scripts/run-comparison.sh -m peers -A <ssh host A> -B <ssh host B> \
    -a 192.168.1.103 -b 192.168.1.104 -y -R 5 -p 64 -r 1000 -N 2000 -D 3
# a smaller matrix: -O '-R "1 4" -M "1 2" -P "1 2" -U "1 4"'
```

Add `-w` (for example `-O '-m rate -M 1 -P 1 -w'`) to also count the traffic
between the hosts in every step. nftables counters on both boards (passwordless
sudo needed) count SOME/IP data and SD (port 30490) per direction and leave PTP
out; bytes are IP packet lengths. Each run gets a `net-usage.csv`, and
`merge-results.py` adds tables of bytes and packets per notification and per
request. With one publisher and N subscribers on the other host, opensomeip
sends N copies of every notification across the link; the daemon stacks send
one.

How the stacks map the peers:

- lg-someip and vsomeip: instance k of `0x2001`/`0x2002` (k = 1..8) is
  declared by `gen-config.sh -i <max>` with its own port (instance 1 keeps the
  usual ports; k >= 2 uses 31000+k for RPC and 31200+k for events over UDP).
  Peers use the application names `perf-rpc-server[-k]`, `perf-rpc-client[-k]`,
  `perf-event-pub[-k]`, and `perf-event-sub-k`. Host B's daemon subscribes once
  per publisher for all of its local subscribers.
- opensomeip: responder k listens on port 30500+k and a requester opens one
  `RpcClient` per responder. Subscriber k listens on port 30700+k; each
  publisher registers every subscriber endpoint with `handle_subscription()`
  and sends each notification to all of them. Subscribers tell publishers apart
  by the sender port, since SOME/IP has no instance field in its header.

Read the results with the hosts in mind: requesters, subscribers, and host
B's daemon share B's app CPUs (`-c`, default `1,2-3`), so at 8 peers part of
what is measured is CPU contention on a 4-core board, the same for every
stack.

### Many request threads in one process

The peer sweeps use one process per requester. `perf-rpc-threads` is the
opposite case: one process whose `-T` worker threads all call the responders,
as a service with many request threads would. Each worker sends a request,
blocks until its response arrives, and repeats; it takes the responders in turn.
**The response has the size of the request** (the responders echo the payload,
and the client counts a response of another size as `bad_size`).

```bash
performance-measurement/scripts/run-comparison.sh -m peers -s threads -A <ssh host A> -B <ssh host B> \
    -a 192.168.1.103 -b 192.168.1.104 -y -R 3 -p "64 1024 1400" -D 3 \
    -O '-T "1 8 32 128" -M "1 4"'
# -H "0 50" (the default): requests per second of one worker; 0 = saturation
```

- **Two loads.** Rate `0` sends the next request as soon as the response
  arrives: it shows how far a stack scales and where it loses messages. Rate
  `50` per thread is a fixed load, identical for every stack, which is the
  fair basis for comparing CPU.
- **Same waiting code.** The shared header `common/ThreadStress.h` holds the
  worker loop, the sequence numbering (the worker index travels in the
  sequence), the one-slot-per-worker condition variable, and the result
  columns. A stack adapter only sends the payload. lg-someip and vsomeip use
  `send()` and the response callback. opensomeip uses `call_method_async` with
  the same slots, not `call_method_sync`: that call polls with a 1 ms sleep,
  which would add latency and wake-ups unrelated to the network path.
- **Shared client.** vsomeip and lg-someip have one application per process;
  opensomeip has one `RpcClient` per responder, shared by all workers. Giving
  every worker its own `RpcClient` (its own socket) is a different design and
  is not measured here.
- **What is timed.** Latency runs from just before the send call to the return
  of the waiting worker, so it includes the thread wake-up (unlike ping-pong,
  which stops at the start of the response callback). A request that gets no
  response within `--timeout-ms` (1 s) counts as lost and holds its worker for
  that time, so loss also lowers the throughput of the saturation step.
- **CPU.** The client row has the process's own CPU over the measured window
  (`cpu_pct`: 100 = one core; `cpu_per_req_us`). Everything else comes from
  `/proc` around each step: the daemons of both hosts and the responders, minus
  their idle CPU measured for 4 s before the steps (`threads-host-cpu.csv`),
  divided by the calls of the step. `merge-results.py` adds them up as "all
  processes µs/request" and also prints the split. The scheduler counts CPU in
  10 ms ticks; the figures are per step of several seconds, so the error is
  small but not zero.
- **Contention.** The workers, the responders, and the consumer daemon share
  their host's app CPUs (`-c`). 128 threads on two cores measure scheduling
  and lock behavior as much as the stacks' send paths, the same for every
  stack.

### The report

`scripts/make-report.py` writes `lg-someip-performance-report.md` and its
figures from the merged runs: the standard run (latency, throughput, memory),
the thread run (`-m peers -s threads`), optionally the event run
(`-m peers -s event -O '-P 1 -U "1 2 4 8" -w'`) and an RPC wire-traffic run
(`-m peers -s rpc -R 1 -O '-R 1 -M 1 -m rate -w'`), and the output of
`rom-footprint.sh`. Every number and figure is computed from the result files.
The sentences around them state what the numbers show, so read the generated
report after a rerun: if a ranking changes, the text must change with it.

```bash
performance-measurement/scripts/make-report.py --standard <dir> --threads <dir> \
    [--events <dir>] [--rpcnet <dir>] --rom rom-footprint.txt --out performance-measurement/reports/<board>
```

### Storage footprint

Compare one deployment role at a time, from stripped copies, and count the
files that role really deploys. A consumer ECU needs the daemon, the stack
library, and the client application, not the server application as well; a
statically linked stack (opensomeip) is just the application. Third-party
libraries count (`-i` adds the matching shared libraries `ldd` finds):

```bash
performance-measurement/scripts/rom-footprint.sh -i 'boost|vsomeip|lgsomeip' \
    "lgsomeip-consumer=build-pi/daemon/someip-daemon,build-pi/performance-measurement/lg-someip/perf-rpc-client" \
    "vsomeip-consumer=$HOME/vsomeip-src/build/examples/routingmanagerd/routingmanagerd,performance-measurement/vsomeip/build/perf-rpc-client" \
    "opensomeip-consumer=performance-measurement/opensomeip/build/perf-rpc-client"
```

Unstripped vsomeip builds carry debug information and are not a ROM number.

## How to read the results

- **RPC ping-pong** is the latency floor of each stack's path. Between lg-someip
  and vsomeip (same architecture) the comparison is direct; opensomeip skips the
  daemon hops, so a lower figure there is expected.
- **RPC at fixed rates** shows latency under a steady load. Compare rows with
  the same payload and rate.
- **Maximum rate (rate 0)** is a stress test, not a latency comparison. With a
  fixed in-flight window, latency is roughly window / throughput for a stack
  whose send call never blocks (vsomeip), but not for one that blocks when its
  sockets are full (lg-someip). Throughput and loss depend on that
  back-pressure behavior, so the table leaves latency out.
- **Events.** At a fixed rate every stack delivers the offered rate, so
  throughput does not separate them; compare loss at high rates and, with
  synchronized clocks, one-way latency. `notify_*_us` is the duration of the
  publisher's API call, not latency: lg-someip and vsomeip hand the
  notification to a daemon, opensomeip sends a UDP datagram. Do not rank stacks
  by it. A clock offset between unsynchronized hosts appears as a constant
  latency (the subscriber's `negative_latency` column counts samples that came
  out negative and were stored as 0).
- **Resources.** Client `cpu_net_ms`, server CPU, daemon CPU, and peak RSS are
  cumulative process figures for the whole run, not per-message costs. A fixed
  idle cost (SD timers) is part of the daemon figure, so read it with the
  length of the run in mind.

## Known limitations

- opensomeip's event harness is a one-subscriber, fixed-endpoint UDP setup; it
  is not a stock `EventSubscriber` plus Service Discovery flow (see
  "opensomeip notes").
- TCP is measured for RPC only; the direct-event harness is UDP.
- vsomeip's crash phases (application, node) are not implemented.
- Thread-pool sizes (vsomeip's dispatcher threads, lg-someip's worker pool) are
  not matched; the configuration formats do not expose equivalent knobs.
- `offer_to_available_ms` and one-way latency depend on the quality of the
  PTP synchronization; record the `ptp4l` offset next to the numbers.
- In the event peer sweeps, the daemon-based stacks (lg-someip and vsomeip)
  intermittently lose a contiguous block of notifications (0.2 to 1.7 s) at
  1000 msg/s: in about one step in ten, in a different step each repetition,
  while opensomeip loses none. A single long stream (20 s, same hosts) shows no
  loss, and the loss recurs without the RPC sweep before it, so it is tied to
  how a sweep step starts (new publisher processes re-offering instances that
  were just withdrawn) together with Service Discovery. The cause is not yet
  established. The tables show the median, which is 0; the range shows the
  affected repetitions.

## vsomeip notes

**API differences.** lg-someip bundles an older vsomeip API snapshot and
installs a CMake package named `vsomeip`; upstream's package and target are
`vsomeip3`. `USE_REAL_VSOMEIP` selects between them without changing the app
source. `PERF_VSOMEIP_LEGACY_API` (set when `USE_REAL_VSOMEIP=OFF`) covers two
differences: `application::send()` lost its `_flush` parameter and
`register_message_handler()` lost `_is_provider`. Upstream vsomeip puts
everything in `vsomeip_v3`; the `vsomeip` alias keeps the source unchanged. The
shim reads lg-someip's own JSON format; real vsomeip uses its own schema, which
`vsomeip/scripts/gen-config.sh` writes.

**`offer_service()` deadlock.** `application::offer_service()` blocks until
`start()` has set an internal "enabled" flag. Calling it from the state handler
can run on the thread that is part-way through `start()` and deadlock. The apps
follow the vsomeip samples: the handler only signals a condition variable and
the main thread offers.

**Multicast route.** Real vsomeip starts Service Discovery only after the kernel
reports a multicast route (it waits for a netlink notification). On a plain
static-IP link there is none, so offers and finds never leave the host and
nothing logs an error. Run once per boot on both hosts:

```bash
sudo ip route add 224.0.0.0/4 dev eth0
```

The coordinator's `route-check` stops with a message when the route is missing.

**nPDU batching.** By default real vsomeip batches small messages sent on a
remote endpoint (debounce 2 ms, maximum retention 5 ms per direction), which
adds about 10 ms to a round trip and hides in a flat latency with almost no
spread. `gen-config.sh` disables it with `npdu-default-timings` set to 0. Local
(Unix socket) traffic is unaffected, which is why a same-host control run does
not show it.

**Re-offer grace window.** Real vsomeip rejects a re-offer that arrives soon
after a StopOffer, so the SD toggle uses a 2 s gap between cycles.

**`pkill -f` self-matching.** `ssh host 'pkill -f routingmanagerd'` can kill the
remote shell, because the pattern is in its own command line. Write the pattern
as `rout[i]ngmanagerd`.

## opensomeip notes

opensomeip 0.2.0 provides RPC (`RpcClient`, `RpcServer`), events
(`EventPublisher`, `EventSubscriber`), and Service Discovery (`SdClient`,
`SdServer`) as separate layers, without a routing daemon or a configuration
file. Using them together for a benchmark needs application glue:

1. `EventPublisher::on_message_received()` is an empty stub, so applications
   must call `handle_subscription()` themselves.
2. `SdClient` sends `SUBSCRIBE_EVENTGROUP` from its own UDP transport and
   ephemeral port, and `SdServer` reports the accepted endpoint through
   `set_subscription_accepted_callback()`, without the subscriber's client ID
   that `handle_subscription()` needs.
3. `EventSubscriber` owns a different internal UDP transport and exposes no bind
   endpoint, so a stock `SdClient` plus stock `EventSubscriber` cannot share the
   endpoint advertised in the subscription.

The event benchmark therefore registers a fixed subscriber endpoint with
`handle_subscription()` (a stable client ID) and receives the notifications
with the low-level `UdpTransport`. Its rows carry
`architecture=direct-event-no-sd` and must not be read as equivalent to the
daemon and SD event rows. A stock `EventSubscriber` plus SD flow, and a fair
multi-subscriber benchmark, need more glue or an upstream API extension; that
is a limitation of the harness, not a statement that opensomeip lacks events or
Service Discovery.

**RPC sessions are never released.** `RpcClient` creates a SOME/IP session for
every call (`SessionManager::create_session`) and nothing removes it:
`remove_session()` and `cleanup_expired_sessions()` are not called by the client.
After 65,535 calls on one `RpcClient` the session table holds every id, and
`SessionManager::get_next_session_id()` loops forever while it holds the table's
mutex. Every thread that calls the client then hangs, one of them at 100% CPU
(found with gdb: the spinning thread sits in `get_next_session_id()` and the
others wait in `pthread_mutex_lock` in `create_session()`). The table also grows
with every call until then. Any long-running requester is affected, not only a
benchmark; this is what stopped the 100,000-request saturation step of the first
runs. The single-request harnesses stay below the limit (at most 30,000 calls
per run). `perf-rpc-threads` runs longer, so it replaces each `RpcClient` after
50,000 calls (`client_renewals` in its row; the call that triggers the renewal
includes the new socket's setup). Report the defect upstream before relying on
long-lived opensomeip clients.
