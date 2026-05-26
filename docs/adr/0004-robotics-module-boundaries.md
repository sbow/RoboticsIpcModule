# ADR 0004: Robotics IPC module boundaries

- **Status:** Accepted
- **Date:** 2026-05-25
- **Builds on:** [ADR 0001](0001-ipc-and-router.md), [ADR 0002](0002-ipc-router-refactor.md), [ADR 0003](0003-transport-agnostic-router.md)
- **Scope:** Module boundary, public surface, wire-format versioning, and bridge/interop exclusion for the `ipc/` library that ships out of this repo.

> Path note: ADRs 0001–0003 were authored against the `cpp_tricks/ipc/...` source tree and reference paths in that layout. This module was vendored from `sbow/cpp_tricks` and re-rooted at `ipc/` (see [CONTEXT.md](../../robotics-ipc-module/CONTEXT.md) and [STATUS.md](../../robotics-ipc-module/STATUS.md)). The architectural decisions in 0001–0003 carry over unchanged; only directory paths are different. New ADRs (this one onward) use the new layout.

## Context

`sbow/cpp_tricks` evolved a header-only Linux IPC + router stack (ADRs 0001–0003). It now needs to become a **standalone C++ message fabric** suitable for vendoring into a robotics product repo (Jetson + x86 + HIL/sim) — not "an interesting C++ trick" but a module with a contract.

The plan pack under `robotics-ipc-module/` (vendored alongside the code) drives that evolution across phases A–F. Phase A is "packaging": before adding YAML loaders (B), bounded SHM send (C), unit tests (D), Jetson systemd (E), or Python / Node / MAVLink bridges (F), the module needs an unambiguous boundary that every later phase can lean on. Specifically:

1. **What is library, what is example.** The current `ipc/test/*.cpp` files double as integration tests *and* as wiring demos. `ipc/test/router_client_config.h` hardcodes peer IDs, UDS paths, and route rules used by the demo — apps embedded in a real robot must not pin themselves to those constants.
2. **Where can application-only concerns live?** Pre-refactor, `ipc/src/router/node.hpp` (a library header) `#include "router_app.h"` to reach `router_stop_flag()` and `router_idle_expired()`. That violates the layering rule "library headers depend downward only" from [DESIGN-PRINCIPLES.md](../../robotics-ipc-module/DESIGN-PRINCIPLES.md): `router_app.h` also defines `router_log` and `router_test_mode()`, which are unambiguously app-layer (stderr writer, env-var reader). The boundary was implicit and silently violated.
3. **How do consumers reason about wire compatibility?** `RouterFrame` is 32 bytes today, but nothing in the source declares "version 1, frozen". A neighbor process that wants to assert "this build speaks the same dialect as me" has no constant to compile-time check.
4. **Interop pressure.** Phase F adds Python (ML), Node (dashboards), GStreamer / V4L (camera), and serial / MAVLink (MCU) bridges. If we don't declare those out-of-core *before* Phase B / C land, principle drift is near-certain — somebody will be tempted to put a Python binding in `ipc/src/`.

## Decision

### 1. Module surface

The shipped module is the directory tree:

```text
ipc/
  src/
    ipc.hpp               # umbrella for transports
    ipc.h                 # compat shim → ipc.hpp
    ipc/*.hpp             # buffer, transport, datagram, shm_spsc, echo, endpoint, app_shutdown
    router_protocol.hpp   # umbrella for the router
    router_protocol.h     # compat shim → router_protocol.hpp
    router_app.h          # APP-only convenience header (signal handlers, logger)
    router/*.hpp          # frame, link, node, factory, peer-address adapters, lifecycle
  test/                   # examples and integration tests — NOT part of the API
  SHM_SPSC_TRANSPORT.md
  MODULE.md               # public consumption guide
```

Public entry points are the three umbrella headers (`ipc.hpp`, `router_protocol.hpp`, `router_app.h`) plus the slim sub-headers under `ipc/src/ipc/` and `ipc/src/router/`. Everything under `ipc/test/` is illustrative. The boundary is enforced by review and (in CI later, Phase D) by:

```bash
# Library code must not #include the app-only or demo-wiring headers.
# Exclude router_app.h itself (it may legitimately mention its own name in
# comments / pragma once context).
! grep -rn '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]\(router_app\.h\|router_client_config\.h\)[>"]' \
    ipc/src --include='*.hpp' --include='*.h' \
    | grep -v 'ipc/src/router_app.h:'
```

`router_client_config.h` lives **only** under `ipc/test/`; apps that include it have opted into the demo wiring and should expect breaking changes (it will be replaced by topology YAML in Phase B).

### 2. Layering enforcement: carve `router/lifecycle.hpp` from `router_app.h`

Library-side primitives needed by `RouterServer::run()` and `RouterClient::recv_message_until()` move into a new header:

- `ipc/src/router/lifecycle.hpp` — owns `router_stop_flag()` and `router_idle_expired(...)`; only depends on `ipc/app_shutdown.hpp` + `<chrono>`.
- `ipc/src/router_app.h` — keeps app-only conveniences (`install_router_stop_handlers`, `router_stop_requested`, `router_test_mode`, `router_log`) and `#include`s `router/lifecycle.hpp` so existing demo bins (`router_server.cpp`, `router_client.cpp`, `router_test.cpp`) compile unchanged.
- `ipc/src/router/node.hpp` no longer `#include`s `router_app.h`.

Result: library code depends only on library headers; apps may include either tier; the design-principles rule is now grep-checkable.

### 3. Wire-format versioning

A `kRouterFrameVersion` constant in `ipc/src/router/frame.hpp` declares the
current wire layout. Phase A's frozen-v1 contract has been **superseded by
RouterFrame v2** ([ADR 0008](0008-router-frame-v2.md), 2026-05-25):
`kRouterFrameVersion = 2`, `kRouterFrameSize = 64`. The current layout
lives in the `frame.hpp` header comment; the historical v1 block is
preserved for archeology in ADR 0008's "Context" section.

Any change to frame size, field offsets, or interpretation MUST:

1. Bump `kRouterFrameVersion`.
2. Ship a migration ADR describing how old and new senders / receivers
   interoperate (or whether they're incompatible — v2 chose
   "incompatible single-deploy migration" per ADR 0008).

No `frame_version` byte is on the wire today — the byte before the
payload is reserved as part of the `flags` field in v2, and on-wire
version negotiation is explicitly out of scope (deployments pin a
version; bridges rebuild). If multi-version coexistence ever becomes a
requirement (e.g. heterogeneous Phase F bridges), it gets its own ADR.

#### v1 → v2 history (do not regress)

v1 (deprecated, retained here for diffability):

```
offset  size  field
 0      1     source peer id
 1      9     monotonic timestamp ns, big-endian
10     22     payload, zero-padded
```

v2 (active — see [ADR 0008](0008-router-frame-v2.md) for full table):

```
offset  size  field
 0      1     source
 1      1     flags (has_sideband / keyframe / is_ack / eos / priority)
 2      2     topic_id
 4      4     seq (per-source monotonic, wraps)
 8      8     timestamp_ns (host little-endian)
16      2     sideband_idx (0xFFFF = none)
18      6     sideband_len (uint48 LE, cap 256 TB)
24      8     sideband_seq
32     32     payload
```

### 4. In scope for this module

- **Linux SBC + x86 dev**: Jetson (aarch64, L4T) on-robot; x86_64 for dev / HIL / sim / cloud.
- **Multi-process**: One thread per `RouterServer`/`RouterClient`; scale by spawning processes.
- **Profile-swappable transports**: SHM on Jetson, UDS for x86 dev / local bridges, UDP for HIL / sim / cloud / cross-container. Same peer IDs, same `RouteRule[]`, only `PeerAddress` and `TransportKind` change per profile (Phase B).
- **Non-safety-critical**: Robotics R&D and engineering use. No SIL claims.
- **Header-only C++20**: `-pthread`; `-lrt` only when SHM is used.

### 5. Out of scope for this module (without a new ADR)

- **Safety certification** (SIL, DO-178C, ISO 26262, redundant routers, formal verification).
- **In-core DDS / ROS 2 / gRPC** — would re-introduce the heavyweight messaging stacks this module exists to avoid.
- **In-core Python / Node / CUDA / GStreamer / V4L** — all bridges go under `examples/bridges/` (Phase F), as separate processes. No `pybind11`, no V8, no CUDA kernels in `ipc/src/`.
- **Raw MAVLink wire bytes inside `RouterFrame` payload** — the 32 B v2 payload window carries control / metadata only; `mavlink_gateway` parses and republishes status frames (see SYSTEM-VISION).
- **Raw camera pixels inside `RouterFrame` payload** — vision bridges publish metadata frames (topic_id, seq, timestamp, sideband_idx/seq/len); pixel buffers go via a sideband SHM region or DMA buffer (ADR 0005; v2 frame carries the descriptor explicitly per ADR 0008).
- **Cross-machine SHM** — SHM is host-local; cross-host deployments use the UDP profile.
- **TLS on localhost UDS** — trusted-host assumption for lab / robot use; remote UDP may grow an opt-in TLS ADR later.
- **Auth / crypto on peer identity** — lab / trusted LAN assumption; signed identity is a separate ADR if needed.
- **Bare-metal RTOS port** — not in this module.

### 6. Module API promise

Until further notice (Phase B may add new public headers, Phase C may add bounded-send overloads, etc.):

- The **three umbrella headers** (`ipc.hpp`, `router_protocol.hpp`, `router_app.h`) and their re-exported sub-headers under `ipc/src/ipc/` and `ipc/src/router/` are stable include paths. Removing or relocating one of them requires an ADR.
- `kRouterFrameVersion = 2` is the wire contract (see ADR 0008).
- The shutdown contract documented in [MODULE.md](../../ipc/MODULE.md) (`install_router_stop_handlers` / `router_stop_flag` / 200 ms recv timeout / `try_recv` + `yield` for SHM) is the supported lifecycle pattern. Apps that block signal handling on an infinite spin are unsupported.

### 7. Bridges and external integrations

All neighbors of the IPC module ship under `examples/bridges/` (Phase F) as **separate processes** that peer in over UDS / SHM / UDP:

| Neighbor | Pattern | Lives in |
|----------|---------|----------|
| Python ML / training | UDS bridge process; tensors via sideband SHM (or ZMQ — TBD by Phase F ADR) | `examples/bridges/python_peer/` |
| Node / dashboard | UDS subscriber → WebSocket → browser | `examples/bridges/node_gateway/` |
| MAVLink / MCU | Serial gateway parses MAVLink, republishes compact status frames | `examples/bridges/mavlink_gateway/` |
| Camera (CSI / V4L) | GStreamer or vendor pipeline process emits metadata frames + sideband SHM for pixels | `examples/bridges/vision_metadata/` |
| CUDA / ML inference | GPU process peers in; tensor sideband | `examples/bridges/ml_inference/` |

The peer catalog in [SYSTEM-VISION.md](../../robotics-ipc-module/SYSTEM-VISION.md) is **illustrative**, not a Phase A or B implementation list — it describes the deployment surface the module is designed to support, not what the next PR must build.

## Consequences

### Positive

- The library/example boundary is **grep-checkable**, not just documented; Phase D can wire it into CI.
- Layering violation in `node.hpp` is removed; future hot-path changes review against a clean dependency direction.
- `kRouterFrameVersion` gives downstream consumers a compile-time hook for version assertions and forces future wire changes to be deliberate (ADR + bump).
- Phase B (config / topology YAML) and Phase C (bounded SHM send) start from a clean module surface — they extend `ipc/src/`, they don't reshape it.
- Phase F bridges have a designated home (`examples/bridges/`) so the temptation to inline a Python binding into `ipc/src/` is removed up front.

### Negative

- ADRs 0001–0003 reference the old `cpp_tricks/ipc/...` paths. Readers must mentally translate `cpp_tricks/ipc/src/router/foo.hpp` → `ipc/src/router/foo.hpp`. The historical record stays intact; a follow-up cosmetic ADR could rewrite the path references but offers little value.
- `router_client_config.h` and `kRouterUdsPath` / `kSensorShmName` constants are now explicitly demo-only. Anyone who was depending on them implicitly will have to wait for Phase B's topology loader or pin to a tag.
- The `make test-ipc` baseline still skips the SHM portion of the in-process echo benchmark (`IPC_SKIP_SHM=1`) because of the blocking `shm_push_slot` issue (LESSONS-LEARNED: "Full ring = infinite spin"). This is **expected** for Phase A — Phase C will land `try_send` + bounded wait and remove the skip default. `make test-router` exercises all three transports (UDS / UDP / SHM) and passes today.

### Neutral

- `router_app.h` still exists. It became a *thinner* app-only header that re-exports lifecycle primitives, so existing test bins compile unchanged. Apps that prefer the library-only entry point can `#include "router/lifecycle.hpp"` directly.

## Alternatives considered

1. **Delete `router_app.h` entirely; inline its remaining helpers into each demo.** Rejected: would force every test bin (and every future app) to recreate `install_router_stop_handlers` / `router_log` boilerplate. The header is small, app-only, and unambiguously labeled.
2. **Add a `frame_version` byte at offset 0 today.** Rejected: breaks the 22 B payload window in a single-version world; introduces churn for no benefit. Constant + ADR captures the version commitment without touching the wire.
3. **Move ADRs 0001–0003 from `docs/adr/` to `robotics-ipc-module/`.** Rejected: ADRs are architectural artefacts of the *code*, not the plan pack. They stay co-located with the code they govern.
4. **Reorganize to `module/ipc/` instead of `ipc/` at root.** Rejected during Phase A intake (see STATUS.md baseline log): the flat layout is cleaner for a standalone module repo; only meaningful gain of the nested option is preserving plan-doc path references verbatim, which is a small price to pay (the doc paths are now updated).
5. **Use `git subtree` to import history from `sbow/cpp_tricks`.** Rejected during Phase A intake: a plain copy reads as "starting point" rather than "fork" and avoids a tangled merge base if `cpp_tricks` keeps evolving separately. The relationship is captured in CONTEXT.md.

## Verification

```bash
make all && make test-ipc && make test-router                  # acceptance gate
test -f ipc/MODULE.md                                          # A1
# A2: no #include of the app/demo headers from anywhere under ipc/src,
# excluding the self-reference inside router_app.h.
! grep -rn '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]\(router_app\.h\|router_client_config\.h\)[>"]' \
    ipc/src --include='*.hpp' --include='*.h' \
  | grep -v '^ipc/src/router_app\.h:'
grep -q 'kRouterFrameVersion' ipc/src/router/frame.hpp         # A3
test -f docs/adr/0004-robotics-module-boundaries.md            # A4
```

All five checks pass on `main` as of the Phase A completion commit (see STATUS.md baseline log).
