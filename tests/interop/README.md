# Interoperability Tests

This directory contains small applications that check LG SOME/IP against itself
over a real network: serialization, methods, and events. They need no test
framework, so they build and run on Linux and QNX.

| Application | Needs daemon | What it checks |
|---|---|---|
| `interop-serialization` | No | Encodes every test vector, compares it with the golden bytes, and decodes it again. |
| `interop-server` | Yes | Provides service `0x2001`. Answers method requests and sends events twice a second. |
| `interop-client` | Yes | Method tests: every test vector, fire-and-forget, return codes, request/session correlation. |
| `interop-subscriber` | Yes | Event tests on the unicast and the multicast event group. |

Every application prints `PASS` or `FAIL` per check. The exit code is `0` when
everything passes and `1` on a failure.

## Build

Enable the tests with `ENABLE_INTEROP_TESTS`:

```sh
mkdir build
cd build
cmake .. -DENABLE_INTEROP_TESTS=ON
make
```

The applications are written to `build/tests/interop/`.

## Offline Check

`interop-serialization` needs no daemon and no network. Run it first on each
target:

```sh
build/tests/interop/interop-serialization
```

It is also registered with CTest under the `interop` label:

```sh
ctest --test-dir build --label-regex '^interop$'
```

## Network Test Setup

The network tests use two hosts:

- **Host A** runs `interop-server`.
- **Host B** runs `interop-client` and `interop-subscriber`.

Both hosts use [`config/interop_config.json`](config/interop_config.json). Copy it
to each host and change two things:

| Key | Host A (server) | Host B (client) |
|---|---|---|
| `unicast` | IP address of host A | IP address of host B |
| `is-provider` (both services) | `"true"` | `"false"` |

The service is offered as two instances, one per transport:

| Instance | Transport | Port | Event groups |
|---|---|---|---|
| `0x0001` | TCP | `30609` | `0x0100` (unicast) |
| `0x0002` | UDP | `30610` | `0x0100` (unicast), `0x0200` (multicast `224.225.226.235:32345`) |

`interop-client` and `interop-subscriber` choose the instance with `--tcp`
(the default) or `--udp`, so each run tests exactly one transport.

## Run

Start the daemon on **both** hosts with the edited configuration:

```sh
build/daemon/someip-daemon <path>/interop_config.json
```

On **host A**, start the server and leave it running:

```sh
export LGSOMEIP_CONFIGURATION=<path>/interop_config.json
build/tests/interop/interop-server
```

On **host B**, run the tests:

```sh
export LGSOMEIP_CONFIGURATION=<path>/interop_config.json

# Methods
build/tests/interop/interop-client --tcp --large
build/tests/interop/interop-client --udp

# Events
build/tests/interop/interop-subscriber --tcp --count 5
build/tests/interop/interop-subscriber --udp --count 5
```

A passing subscriber run looks like this:

```
PASS  event 0x8001 scalars_min  good=5 bad=0  first after 464 ms
PASS  event 0x8002 array_of_struct  good=5 bad=0  first after 464 ms
PASS  event 0x8003 utf8_korean (field)  good=1 bad=0  first after 3 ms
PASS  event 0x8011 scalars_max  good=5 bad=0  first after 464 ms
PASS  event 0x8012 struct_len32  good=5 bad=0  first after 464 ms
```

### Multicast

The multicast group `0x0200` has a threshold of two subscribers: one subscriber
is served by unicast, two or more by multicast. To test multicast delivery, run
two UDP subscribers at the same time on host B and watch the wire:

```sh
build/tests/interop/interop-subscriber --udp --app interop-sub-1 --count 5 &
build/tests/interop/interop-subscriber --udp --app interop-sub-2 --count 5
tcpdump -ni <interface> 'udp and host 224.225.226.235'
```

## Options

`interop-client`:

| Option | Meaning |
|---|---|
| `--tcp` / `--udp` | Transport to test (default `--tcp`). |
| `--app <name>` | Application name from the configuration (default `interop-client`). |
| `--large` | Also send 4000 and 30000 byte payloads. TCP only. |
| `--timeout <s>` | Wait per request (default 5). |
| `--wait <s>` | Wait for the service to become available (default 30). |

`interop-subscriber`:

| Option | Meaning |
|---|---|
| `--tcp` / `--udp` | Transport to test (default `--tcp`). |
| `--app <name>` | Application name from the configuration (default `interop-sub-1`). |
| `--group <g>` | `unicast`, `multicast` (needs `--udp`), or `all` (default). |
| `--count <n>` | Notifications required per event (default 5). A field needs one. |
| `--timeout <s>` | Total wait for the notifications (default 30). |

`interop-server` takes an optional application name (default `interop-server`).

Exit codes: `0` pass, `1` failure, `2` usage error. `interop-client` returns `3`
when the service never becomes available.

## Files

- [`common/InteropIds.h`](common/InteropIds.h): service, instance, method, and
  event group IDs. They must match the configuration.
- [`common/InteropVectors.cpp`](common/InteropVectors.cpp): test vectors with
  their golden bytes, and the event list.
- [`config/interop_config.json`](config/interop_config.json): configuration for
  both hosts.
