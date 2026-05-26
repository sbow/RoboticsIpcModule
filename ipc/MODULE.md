# IPC module — consumption guide

Header-only C++20 **message fabric** for Linux robotics: transports
(UDP / UDS / SHM SPSC), a 32 B router frame, and routing primitives. This
file is the **public contract**. See [ADR 0004](../docs/adr/0004-robotics-module-boundaries.md)
for the module boundary decision and [DESIGN-PRINCIPLES.md](../robotics-ipc-module/DESIGN-PRINCIPLES.md)
for the layering rules every change is reviewed against.

## Platforms

| Target | Status | Notes |
|--------|--------|-------|
| **Linux x86_64** (glibc ≥ 2.31) | Supported (dev / HIL / sim) | UDP / UDS / SHM SPSC all green via `make test-router` |
| **NVIDIA Jetson** (aarch64, L4T) | Intended deployment | Same headers; SHM is the high-rate on-board transport (Phase C will harden idle wake + bounded send) |
| **macOS / Windows** | Not supported | Module uses Linux-specific SHM (`shm_open`), `eventfd`-class idle wake (Phase C), and UDS abstract namespace |

Cross-machine deployments use the **UDP profile** — SHM does not span hosts.

## Toolchain

- **Standard:** C++20 (concepts, `if constexpr`, `std::string_view`)
- **Compiler:** g++ ≥ 11 or clang++ ≥ 13 (tested on g++ 11+)
- **Link flags:**
  - Header-only for UDP / UDS consumers
  - `-lrt` *required* when including `ipc/shm_spsc.hpp` (or anything that transitively pulls it in — e.g. `router_protocol.hpp`, `ipc.hpp`)
  - `-pthread` for any multi-threaded consumer (every router demo uses it)
- **Standard library:** `libstdc++` or `libc++` both work; no external runtime dependencies

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

`RouterFrame` is 32 bytes — **frozen at version 1** (`kRouterFrameVersion = 1`
in `ipc/src/router/frame.hpp`):

| Offset | Size | Field |
|--------|------|-------|
| 0      | 1    | source peer id (stamped by router on forward) |
| 1..9   | 9    | monotonic timestamp ns, big-endian |
| 10..31 | 22   | payload, zero-padded |

The 22 B payload is for **control / metadata** — sensor scalars, command
acks, frame ids. Bulk data (camera frames, tensors, MAVLink wire bytes) goes
through **side channels** defined per peer (Phase B sideband ADR). Any
breaking change to layout must bump `kRouterFrameVersion` and ship a
migration ADR — see ADR 0004.

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
make test-ipc               # UDP + UDS echo benchmarks (SHM skipped, see below)
make test-router            # UDS + UDP + SHM router scenarios
make test-ipc-shm           # forces the SHM echo benchmark — fails until Phase C
make debug                  # rebuild with -g -O0
make clean
```

### Known limitations (deferred)

| Limitation | Today | Resolution |
|------------|-------|-----------|
| SHM echo benchmark hangs on `stop` | Client uses blocking `shm_push_slot`; on a full ring the stop flag is never observed | Phase C: `try_send` + bounded wait + drop metric |
| 22 B `RouterFrame` payload | Demo only | Phase B: sideband ADR for vision / tensor / MAVLink-bulk |
| Hardcoded `/tmp/cpp_tricks_*` paths in demos | `ipc/test/router_client_config.h` | Phase B: topology YAML; profile per environment (Jetson / x86 / HIL / sim) |
| No idle wake on SHM | `try_recv` + `yield` busy-loop (low priority of router thread) | Phase C: `eventfd` integration or ADR deferral |
| No router metrics (drops, queue depth) | Silent | Phase C |
| Bridges (Python / Node / MAVLink / vision) | Not present | Phase F: under `examples/bridges/` |

The `make test-ipc` target sets `IPC_SKIP_SHM=1` automatically so the
baseline gate stays green until Phase C lands the bounded send path.

## Related documents

- [DESIGN-PRINCIPLES.md](../robotics-ipc-module/DESIGN-PRINCIPLES.md) — layering, hot-path rules, identity & routing
- [CONTEXT.md](../robotics-ipc-module/CONTEXT.md) — code baseline summary
- [LESSONS-LEARNED.md](../robotics-ipc-module/LESSONS-LEARNED.md) — bugs and fixes to avoid repeating
- [SYSTEM-VISION.md](../robotics-ipc-module/SYSTEM-VISION.md) — deployment targets and peer catalog
- [docs/adr/0001-ipc-and-router.md](../docs/adr/0001-ipc-and-router.md) — original header-only IPC + router decision
- [docs/adr/0002-ipc-router-refactor.md](../docs/adr/0002-ipc-router-refactor.md) — layered split (transport / link / node / app)
- [docs/adr/0003-transport-agnostic-router.md](../docs/adr/0003-transport-agnostic-router.md) — peer-address adapters + factories
- [docs/adr/0004-robotics-module-boundaries.md](../docs/adr/0004-robotics-module-boundaries.md) — robotics module boundary, frame versioning, bridge exclusion
- [ipc/SHM_SPSC_TRANSPORT.md](SHM_SPSC_TRANSPORT.md) — single-producer / single-consumer SHM transport details
