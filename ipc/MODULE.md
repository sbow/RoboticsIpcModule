# IPC module — consumption guide

Header-only C++20 **message fabric** for Linux robotics: transports
(UDP / UDS / SHM SPSC), a 32 B router frame, routing primitives, a TOML
deployment-profile loader, and a subscriber-side last-value cache. This
file is the **public contract**. See [ADR 0004](../docs/adr/0004-robotics-module-boundaries.md)
for the module boundary decision, [ADR 0005](../docs/adr/0005-payload-policy-and-sideband.md)
for the payload / sideband policy, and [DESIGN-PRINCIPLES.md](../robotics-ipc-module/DESIGN-PRINCIPLES.md)
for the layering rules every change is reviewed against.

## Platforms

| Target | Status | Notes |
|--------|--------|-------|
| **Linux x86_64** (glibc ≥ 2.31) | Supported (dev / HIL / sim) | UDP / UDS / SHM SPSC all green via `make test-router` |
| **NVIDIA Jetson** (aarch64, L4T) | Intended deployment | Same headers; SHM is the high-rate on-board transport. Phase C: idle CPU < 5% one core (measured 1.6% on x86 dev host) + drop-on-full backpressure with metrics |
| **macOS / Windows** | Not supported | Module uses Linux-specific SHM (`shm_open`), `eventfd`-class idle wake (deferred to Phase F per [ADR 0007](../docs/adr/0007-router-idle-wake.md)), and UDS abstract namespace |

Cross-machine deployments use the **UDP profile** — SHM does not span hosts.

## Toolchain

- **Standard:** C++20 (concepts, `if constexpr`, `std::string_view`)
- **Compiler:** g++ ≥ 11 or clang++ ≥ 13 (tested on g++ 11+)
- **Link flags:**
  - Header-only for UDP / UDS consumers
  - `-lrt` *required* when including `ipc/shm_spsc.hpp` (or anything that transitively pulls it in — e.g. `router_protocol.hpp`, `ipc.hpp`)
  - `-pthread` for any multi-threaded consumer (every router demo uses it)
- **Standard library:** `libstdc++` or `libc++` both work; no external runtime dependencies
- **Vendored dependency:** `third_party/tomlplusplus/toml.hpp` (Phase B). Single-header MIT-licensed TOML 1.0 parser; only required when an app includes `router/topology_loader.hpp`. Add `-Ithird_party/tomlplusplus` to the include path; nothing to link.

## Public entry points

Apps include exactly these umbrella headers:

| Header | Pulls in | Use when |
|--------|----------|----------|
| `ipc.hpp` | `ipc/buffer.hpp`, `ipc/transport.hpp`, `ipc/datagram.hpp` (UDP+UDS), `ipc/shm_spsc.hpp`, `ipc/echo.hpp`, `ipc/endpoint.hpp` | Building a process that uses raw transports without the router (echo bench, custom datagram client) |
| `router_protocol.hpp` | All of `router/*.hpp` (frame, topology, factories, links, peer-address adapters) | Building a router server, router client, or anything that publishes / subscribes via the 32 B `RouterFrame` |
| `router_app.h` | `router/lifecycle.hpp` + signal-handler / `router_log` helpers | App-layer convenience header for demos and bridges. **Library code under `ipc/src/router/` must not include it.** |
| `ipc.h`, `router_protocol.h` | Thin shims that `#include` the `.hpp` versions | Backward compatibility only; new code should prefer the `.hpp` headers |

### Minimal include graph

```text
your_app.cpp
 ├── router_protocol.hpp          ← all router types you need
 └── router_app.h                 ← only in app/demo translation units (signals + logger)
                                    library files under ipc/src/router/ MUST NOT include
                                    router_app.h; use router/lifecycle.hpp instead
```

For a non-router app that just shuffles bytes over UDP:

```text
your_app.cpp
 └── ipc.hpp                      ← Udp / UdpEchoServer / UdpEchoClient + Buffer
```

### Slim include for router-only consumers

If you only need the frame + a single transport, you can include the
sub-headers directly to keep compile cost down — they are stable
re-exportable paths:

```cpp
#include "router/frame.hpp"            // RouterFrame, kRouterFrameVersion
#include "router/transport_kind.hpp"   // TransportKind
#include "router/factory.hpp"          // dispatch_transport_kind
#include "router/lifecycle.hpp"        // router_stop_flag(), router_idle_expired(...)
```

### Phase B + C headers (explicit opt-in)

These are library headers but **not** pulled in by `router_protocol.hpp` —
include them directly when you need them. Apps that want a tiny include
graph (e.g. embedded microcontroller bridges) can ignore them.

| Header | Purpose |
|--------|---------|
| `router/sideband.hpp` | `SidebandRegion` POD, `SidebandHeader` (16 B in-region header, magic `'RSB1'`), version + class constants per [ADR 0005](../docs/adr/0005-payload-policy-and-sideband.md). |
| `router/topology_loader.hpp` | TOML 1.0 loader (`load_topology_from_toml_string` / `load_topology_from_toml_file`) → owning `LoadedTopology` (returns a `RouterTopology` view + `RouteRule[]` + per-peer `SidebandRegion[]`). Requires the vendored toml++ header. |
| `router/last_value_cache.hpp` | `LastValueCache<N=256>` — subscriber-side latest-frame-per-source cache. Thread-unsafe by design; one cache per consumer thread. |
| `router/source_seq_tracker.hpp` | `SourceSeqTracker<N=256>` — subscriber-side per-source `RouterFrame::seq()` tracker (Phase D1). Classifies each observation as First / InOrder / Gap / Duplicate / OutOfOrder; counts gap totals using `uint32_t` modular arithmetic (handles 2³² wrap). Pairs with `LastValueCache` on the read path. Thread-unsafe by design. |
| `router/metrics.hpp` | `ShmRouterMetrics` (Phase C3) — atomic counters (`forwarded`, `dropped_full`, `recv_empty`, `recv_truncated`) sampled by ops/test code. Reached via `ShmRouterLink::metrics()`. See [ADR 0006](../docs/adr/0006-shm-backpressure-and-metrics.md). |

### SHM backpressure & metrics (Phase C1 / C3)

`ShmRouterLink::send_to_peer` no longer spins on a full destination ring.
A `try_send`-style publish returns `ShmSendResult::Full` and the router
drops that frame copy for that destination only — other destinations on
the same forward are unaffected. Counters are bumped accordingly.

```cpp
#include "router/metrics.hpp"
#include "router/shm_router_link.hpp"

ShmRouterServer server(topo);
server.bind_router({});
// ... run forwarding loop ...

const ShmRouterMetrics& m = server.link().metrics();
const uint64_t fwd  = m.forwarded.load(std::memory_order_relaxed);
const uint64_t drop = m.dropped_full.load(std::memory_order_relaxed);
```

Field semantics, ownership, and rejected alternatives are in
[ADR 0006](../docs/adr/0006-shm-backpressure-and-metrics.md). The
counter block is heap-allocated once per link and lives for the link's
lifetime; callers may cache the reference returned by `metrics()`.

Subscribers must be designed for "newest state wins" — a counter going up
means the *fabric* is healthy, even if a downstream peer is dropping. The
client→router direction is still blocking; see "Known limitations" below.

### Idle CPU and backoff (Phase C2)

`RouterRunOptions::idle_sleep_us` controls the wake granularity in the
empty branch of the forwarding loop. Default `1000` (1 ms) drops idle
CPU to ~1.6% of one core (measured against `jetson_prod.toml`, no
clients) — vs **100% one core** with the legacy `yield()` loop. Set to
`0` for latency-pinned benchmarks; tune lower (e.g. `100`) for tight
control loops. See [ADR 0007](../docs/adr/0007-router-idle-wake.md) for
the measurement and the deferred `eventfd` path.

### Logging callback (Phase B3)

`router_app.h` exposes a pluggable logger. The library never logs from
the hot path (routing is `std::string`-free); the callback is for app /
bridge code that wants to emit human-readable lines.

```cpp
using RouterLogFn = void (*)(int level, const char* msg, std::size_t len);
enum RouterLogLevel { ROUTER_LOG_INFO, ROUTER_LOG_WARN, ROUTER_LOG_ERR };

void router_set_log_fn(RouterLogFn fn);   // call once at startup
void router_log(int level, std::string_view line);   // leveled
void router_log(std::string_view line);              // INFO shorthand
```

Default behaviour (no callback registered): write `<msg>\n` to
`STDERR_FILENO` — matches the pre-Phase-B behaviour for tests and demos.

## Examples vs library

Everything under `ipc/test/` is a **demo / integration test**, not part of the
module API:

| Path | Treat as |
|------|----------|
| `ipc/src/ipc/*.hpp`, `ipc/src/router/*.hpp`, `ipc/src/ipc.{h,hpp}`, `ipc/src/router_protocol.{h,hpp}`, `ipc/src/router_app.h` | **Public library** (header-only, stable) |
| `ipc/test/echo_*.cpp`, `ipc/test/router_*.cpp` | **Examples**; copy patterns, don't link against |
| `ipc/test/router_client_config.h` | **Demo wiring only** (hard-codes `/tmp/cpp_tricks_*` paths, demo peer IDs, demo route rules). **Apps MUST NOT include this header.** Phase B will replace it with topology YAML. |

The library/example boundary is enforced by inspection (review checklist
in [plans/A-module-packaging.md](../robotics-ipc-module/plans/A-module-packaging.md)).
A failing grep gates Phase A acceptance:

```bash
# Library headers must not #include the app-only or demo-wiring headers.
# (We exclude router_app.h's self-mention in its own header comment.)
! grep -rn '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]\(router_app\.h\|router_client_config\.h\)[>"]' \
    ipc/src --include='*.hpp' --include='*.h' \
  | grep -v '^ipc/src/router_app\.h:'
```

## Wire format

`RouterFrame` is **64 bytes — version 2** (`kRouterFrameVersion = 2` in
`ipc/src/router/frame.hpp`; see [ADR 0008](../docs/adr/0008-router-frame-v2.md)).
The whole frame fits in one cache line on every supported architecture.

| Offset | Size | Field          | Notes |
|--------|------|----------------|-------|
| 0      | 1    | `source`       | router-stamped on forward |
| 1      | 1    | `flags`        | bit 0 has_sideband, bit 1 keyframe, bit 2 is_ack, bit 3 eos, bits 4–6 priority, bit 7 reserved |
| 2      | 2    | `topic_id`     | publisher-set; subscribers dispatch on this |
| 4      | 4    | `seq`          | per-source monotonic; wraps; subscriber uses modular arithmetic for gap detection |
| 8      | 8    | `timestamp_ns` | monotonic ns, host (little-endian) byte order |
| 16     | 2    | `sideband_idx` | index into source peer's `[[peers.sideband]]` table; `kSidebandIdxNone = 0xFFFF` |
| 18     | 6    | `sideband_len` | uint48 LE; cap 256 TB |
| 24     | 8    | `sideband_seq` | slot index / sequence within the sideband region |
| 32     | 32   | `payload`      | inline scratchpad; zero-padded |

All multi-byte fields are host (little-endian) byte order. A
`static_assert` in `frame.hpp` fails compilation on big-endian targets.

The 32 B inline payload is for **control / metadata** — sensor scalars,
command acks, twists, IMU samples. Bulk data (camera frames, tensors,
MAVLink wire bytes) goes through **sideband regions** ([ADR 0005](../docs/adr/0005-payload-policy-and-sideband.md));
v2 carries the `(idx, seq, len)` descriptor in-frame so subscribers find
the bulk bytes without consulting the topology at runtime.

Any breaking change to layout must bump `kRouterFrameVersion` and ship a
migration ADR — see [ADR 0004](../docs/adr/0004-robotics-module-boundaries.md)
and [ADR 0008](../docs/adr/0008-router-frame-v2.md) (v1 → v2 migration).
The v1 32 B layout is preserved for archeology in ADR 0008.

## Thread model

- **One thread per `RouterServer<Link>` instance.** The poll loop owns the
  link; sharing a server across threads is not supported.
- **One thread per `RouterClient<Link>` instance.** Same constraint.
- **One thread per `UdpEchoServer` / `UdsEchoServer` / `ShmEchoServer`.**
- Scale by spawning **processes**, not by sharing mutable endpoints between
  threads. The IPC profile (SHM vs UDS vs UDP) is the right knob — see
  SYSTEM-VISION.

## Shutdown contract

Long-running router / echo loops are interruptible:

| Helper | Header | Use |
|--------|--------|-----|
| `install_app_stop_handlers()` | `ipc/app_shutdown.hpp` (re-exported via `router_app.h`) | Install SIGTERM + SIGINT handlers once at `main()` entry |
| `app_stop_requested()` | `ipc/app_shutdown.hpp` | Poll in long loops; identical to dereferencing `app_stop_flag()` |
| `router_stop_flag()` | `router/lifecycle.hpp` | Library-side handle used as default param by `RouterServer::run()` and `RouterClient::recv_message_until()` |
| `install_router_stop_handlers()` / `router_stop_requested()` | `router_app.h` | App-only thin wrappers; convenience for demos and bridges |

Contract summary:

1. **Apps** call `install_router_stop_handlers()` (or `install_app_stop_handlers()`
   directly) once at startup.
2. **Long-running loops** check `app_stop_requested()` / pass `router_stop_flag()`
   to `RouterServer::run()`.
3. **Recv calls** use 200 ms `SO_RCVTIMEO` (datagram) or `try_recv` + `yield`
   (SHM) so the stop flag is observed within one poll tick — never block
   signal handling on an infinite spin.
4. **SIGKILL** is unsupported for clean shutdown: it skips destructors, leaving
   stale UDS sockets at `/tmp/cpp_tricks_router_*.sock` and (currently) SHM
   regions at `/dev/shm/cpp_tricks_*`. Tests `unlink` and `shm_unlink` on every
   scenario start to recover.

## Resource cleanup

| Resource | Owner | Cleanup |
|----------|-------|---------|
| UDS socket path | Process that calls `bind()` | `unlink` before bind; tests `unlink` at start and exit |
| SHM ring (`shm_open`) | Process passing `create=true` | Destructor calls `shm_unlink` on the creator side; clients with `create=false` only `shm_open` then `mmap` |
| Recorder / controller log files | Demo apps in `ipc/test/router_client.cpp` | Tests `unlink` at every scenario start (see LESSONS-LEARNED) |

## Build & verify

```bash
make all                    # build every binary under build/ipc/test/
make test-ipc               # full UDP + UDS + SHM echo benchmark (Phase C: SHM interruptible)
make test-router            # UDS + UDP + SHM router scenarios
make test-ipc-shm           # alias for test-ipc (kept for older docs)
make test-ipc-unit          # all unit tests (frame v2, topology, LV cache, shm backpressure, D1 suites)
make test-frame             # RouterFrame v2 layout / accessor test
make test-topology-loader   # topology loader only
make test-last-value-cache  # last value cache only
make test-shm-backpressure  # Phase C drop-on-full + metrics unit test
make test-datagram-seq      # Phase D1 — SourceSeqTracker (gap detection, 2^32 wrap)
make test-routing           # Phase D1 — route_targets_for edge cases
make test-resolver          # Phase D1 — peer_id_from_recv<Uds/Udp>
make test-cli-args          # Phase D1 — log_path_for_role arity regression
make debug                  # rebuild with -g -O0
make clean
```

### Deployment profiles (Phase B)

`config/profiles/*.toml` ships four reference profiles. Run the router
server against any of them:

```bash
./build/ipc/test/router_server --config config/profiles/x86_dev.toml      # UDS
./build/ipc/test/router_server --config config/profiles/jetson_prod.toml  # SHM
./build/ipc/test/router_server --config config/profiles/hil.toml          # UDP (127.0.0.1)
./build/ipc/test/router_server --config config/profiles/sim_cloud.toml    # UDP (cloud)
```

Same peer IDs (1=sensor, 2=controller, 3=recorder) and route rules across
profiles — only addresses and transport kind change. See
[SYSTEM-VISION.md](../robotics-ipc-module/SYSTEM-VISION.md) for the
deployment matrix.

#### Per-peer SHM ring sizing (Phase D0 / [ADR 0009](../docs/adr/0009-per-peer-ring-sizing.md))

`[[peers]]` entries with `local = "shm:..."` may opt into right-sized
SHM rings:

```toml
[[peers]]
id              = 1
name            = "sensor"
local           = "shm:/cpp_tricks_router_sensor"
shm_slot_count  = 256       # optional; default = ShmSpsc::BindParams (256)
shm_max_payload = 64        # optional; default = ShmSpsc::BindParams (1024)
                            # must be >= kRouterFrameSize (64), <= 256 MiB
```

`shm_max_payload = 64` matches the RouterFrame v2 size, dropping each
ring's footprint from `2 × 256 × 1028 ≈ 526 KiB` (legacy default) to
`2 × 256 × 68 ≈ 35 KiB` — a ~15× reduction that fits the router-frame
ring inside L1/L2 cache. Sideband regions (under
`[[peers.sideband]]`) have independent sizing and continue to carry
bulk data. See `jetson_prod.toml` for the recommended SHM profile.

### Known limitations (deferred)

| Limitation | Today | Resolution |
|------------|-------|-----------|
| Client→router SHM publish blocks | `ShmRouterLink::send_to_router` still calls blocking `shm_push_slot`; if the router crashes or stops with a full client req ring, the client hangs until killed | Future ADR — symmetric `try_send_to_router` returning `ShmSendResult` (requires API change to client publish path) |
| 32 B v2 `RouterFrame` payload | Inline scratchpad sized for control plane only | Use sideband for bulk: v2 frame carries `(sideband_idx, sideband_seq, sideband_len)` directly (ADR 0008); ADR 0005 describes naming + lifecycle |
| `[[peers.sideband]]` has no `memory_class` field | Topology loader parses name + `max_payload_bytes` only; subscribers infer CPU/CUDA/NvBufSurface out-of-band | Forward-declared in ADR 0008, parsed in Phase F vision/ML bridge work |
| Hardcoded `/tmp/cpp_tricks_*` paths in demos | `ipc/test/router_client_config.h` | Phase B done — TOML profiles in `config/profiles/*.toml`; demos still link the legacy header for the route table |
| No `eventfd`-based wake on SHM | `try_recv` + 1 ms `sleep_for` (Phase C2) — idle CPU ~1.6% of one core | Phase F: `eventfd` per peer ring (see [ADR 0007](../docs/adr/0007-router-idle-wake.md)) |
| `dropped_full` does not identify the slow peer | Single global counter on the link | Phase D2a — extend `ShmRouterMetrics` with `dropped_full_per_peer`; required to validate the "Slow recorder" integration scenario |
| Default `shm_max_payload = 1024` is ~15× larger than RouterFrame v2 (64 B) | **Resolved (D0 / ADR 0009)** — `[[peers]] shm_slot_count` / `shm_max_payload` are now optional TOML overrides on SHM peers; `jetson_prod.toml` ships with `256 × 64` per peer; compile-time topologies still use ShmSpsc defaults (sentinel zero in `PeerEntry`) so the demo path is unchanged | — |
| Datagram link metrics | `DatagramRouterLink<Udp/Uds>` has no metrics; kernel-side drops via `SO_RXQ_OVFL` only | Phase D / E: unified metrics surface across transports |
| Bridges (Python / Node / MAVLink / vision) | Not present | Phase F: under `examples/bridges/` |

Phase A's `IPC_SKIP_SHM=1` workaround is retired: `make test-ipc` now
runs the full UDP/UDS/SHM benchmark end-to-end. The environment variable
is still honored if a CI host disallows `/dev/shm`.

### Testing framework

This module **does not depend on GoogleTest, Catch2, or any external test
runner** — that would violate the header-only / no-extra-deps principle.
Every unit test under `ipc/test/*_test.cpp` uses a lightweight in-repo
pattern, one `#define` block per file:

```cpp
int assertions_run = 0;
int assertions_failed = 0;

#define EXPECT(cond) do {                                                  \
    ++assertions_run;                                                      \
    if (!(cond)) {                                                         \
        ++assertions_failed;                                               \
        std::cerr << "EXPECT failed @ " << __FILE__ << ':' << __LINE__     \
                  << " : " #cond "\n";                                     \
    }                                                                      \
} while (0)

#define EXPECT_EQ(a, b) /* same shape, with both values printed on fail */
```

Each test's `main` calls the scenario functions in sequence and prints
`<test>: N/N assertions passed`; non-zero exit on any failure. This
pattern carries the **eight** current suites (frame, topology loader,
last-value cache, SHM backpressure, datagram seq tracker, routing,
resolver, CLI args) — 642 assertions aggregate — and is the supported
style for any new test added in Phase D and beyond. If a future
fixture-heavy suite makes this painful, an ADR is the bar to introduce
a runner.

## Related documents

- [DESIGN-PRINCIPLES.md](../robotics-ipc-module/DESIGN-PRINCIPLES.md) — layering, hot-path rules, identity & routing
- [CONTEXT.md](../robotics-ipc-module/CONTEXT.md) — code baseline summary
- [LESSONS-LEARNED.md](../robotics-ipc-module/LESSONS-LEARNED.md) — bugs and fixes to avoid repeating
- [SYSTEM-VISION.md](../robotics-ipc-module/SYSTEM-VISION.md) — deployment targets and peer catalog
- [docs/adr/0001-ipc-and-router.md](../docs/adr/0001-ipc-and-router.md) — original header-only IPC + router decision
- [docs/adr/0002-ipc-router-refactor.md](../docs/adr/0002-ipc-router-refactor.md) — layered split (transport / link / node / app)
- [docs/adr/0003-transport-agnostic-router.md](../docs/adr/0003-transport-agnostic-router.md) — peer-address adapters + factories
- [docs/adr/0004-robotics-module-boundaries.md](../docs/adr/0004-robotics-module-boundaries.md) — robotics module boundary, frame versioning, bridge exclusion
- [docs/adr/0005-payload-policy-and-sideband.md](../docs/adr/0005-payload-policy-and-sideband.md) — control plane vs sideband; SidebandHeader v1
- [docs/adr/0006-shm-backpressure-and-metrics.md](../docs/adr/0006-shm-backpressure-and-metrics.md) — drop-on-full policy + `ShmRouterMetrics`
- [docs/adr/0007-router-idle-wake.md](../docs/adr/0007-router-idle-wake.md) — `idle_sleep_us` backoff, `eventfd` deferred to Phase F
- [docs/adr/0008-router-frame-v2.md](../docs/adr/0008-router-frame-v2.md) — RouterFrame v2: 64 B, typed, sequenced, in-frame sideband descriptor
- [docs/adr/0009-per-peer-ring-sizing.md](../docs/adr/0009-per-peer-ring-sizing.md) — `[[peers]] shm_slot_count` / `shm_max_payload`; right-sizes router-frame rings to one cache line
- [ipc/SHM_SPSC_TRANSPORT.md](SHM_SPSC_TRANSPORT.md) — single-producer / single-consumer SHM transport details
- [config/profiles/*.toml](../config/profiles/) — deployment profiles (x86_dev / jetson_prod / hil / sim_cloud)
- [third_party/tomlplusplus/LICENSE](../third_party/tomlplusplus/LICENSE) — MIT license for vendored toml++ v3.4.0 single-header parser
