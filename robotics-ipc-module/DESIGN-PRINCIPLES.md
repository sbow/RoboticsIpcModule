# Design principles — Robotics IPC module

**Audience:** Humans and AI agents implementing any phase.  
**Authority:** Deviations require an ADR. These principles survived the IPC/router refactor (vendored from `sbow/cpp_tricks` at baseline) and the SHM router work that followed.

## North star

This module is the **C++ message fabric** inside a larger Linux robotics stack. It is not the whole robot. It must:

- Run on **Jetson** (embedded) and **x86 + CUDA** (dev/lab) with the same topology schema
- Support **swappable IPC backends** per environment (SHM on-robot, UDP for sim/cloud/HIL)
- **Bridge** to Python (ML), Node (dashboards), and serial/MAVLink (MCU) without pulling those into core headers

See [SYSTEM-VISION.md](SYSTEM-VISION.md) for the full system map.

---

## Layering (SRP / DIP)

| Layer | Responsibility | Must not |
|-------|----------------|----------|
| **Transport** (`ipc/`) | Bytes in/out: UDP, UDS, SHM SPSC | Know peer names, routes, or JSON |
| **Adapters** (`peer_address_io`, `shm_peer_address_io`, resolvers) | `PeerAddress` ↔ bind/send/identity | Branch on `Uds`/`Udp` inside `link.hpp` forwarding |
| **Link** (`DatagramRouterLink`, `ShmRouterLink`) | Recv, stamp, fan-out one frame | Parse CLI or load YAML |
| **Node** (`RouterServer`, `RouterClient`) | Poll loop, SIGTERM-aware run | Choose transport at runtime via virtuals |
| **Factory** (`dispatch_transport_kind`) | Compile-time transport dispatch | Live in application role logic |
| **App / bridge** (your processes) | Topology, routes, logging, ML, UI | Include test-only `router_client_config.h` |

**Dependency rule:** Core library headers depend downward only (app → node → link → adapters → transport). No `router_app.h` in `src/router/` or `src/ipc/`.

---

## Performance & embedded fit

1. **Zero type-erasure on hot path** — `RouterLink` is a concept; use `RouterServer<Link>` templates, not virtual `RouterLink*`.
2. **Fixed frames first** — Prefer small fixed headers (`RouterFrame`) for control/metadata; bulk data (images, tensors, MAVLink streams) uses **side channels** (separate SHM region, socket, or file) defined in ADR, not stuffed into 22 B.
3. **Non-owning buffers** — `Buffer` views on stack storage; no per-message `std::string` in forward path.
4. **Interruptible I/O** — Datagram: recv timeout + poll. SHM: `try_recv` + cooperative `yield` (or eventfd idle). Never block signal handling on infinite spin.
5. **One thread per endpoint instance** — Document in MODULE.md; scale via processes, not shared mutable endpoints.

---

## Identity & routing

1. **Router stamps source on forward** — Datagram: identity from **socket address**, not unchecked frame byte 0. SHM: identity from **which ring** received.
2. **Routes are app data** — `RouteRule[]` is supplied by the process; library does not hardcode sensor/controller names.
3. **Topology is data** — `RouterTopology` + file loader (Phase B); same schema for Jetson, laptop, HIL, sim.

---

## Configuration & environment swapping

1. **TransportKind at the boundary** — Choose `uds` / `udp` / `shm` once at startup (or per deployment profile).
2. **Profiles, not `#ifdef`** — Use `config/topology.jetson.yaml`, `topology.hil.yaml`, `topology.sim.yaml` mapping peers to SHM vs UDP vs loopback.
3. **HIL / sim / cloud** — Same peer IDs and route table; change `PeerAddress` bindings only so processes can be replaced by mocks without recompiling routing logic.

---

## Shutdown & resources

1. **SIGTERM/SIGINT** — `app_stop_flag()`; all long-running loops check it.
2. **Idle exit for tests only** — `ROUTER_TEST` / `RouterRunOptions.idle_exit_ms`; not default in production.
3. **Creator owns SHM unlink** — Router creates rings; clients join; destructor or test cleanup calls `shm_unlink`.
4. **UDS stale paths** — `unlink` before bind; `SO_REUSEADDR` on datagram binds.
5. **Log paths** — Derive from `argc` with explicit arity rules (see [LESSONS-LEARNED.md](LESSONS-LEARNED.md)).

---

## Interoperability (outside core)

| Neighbor | Integration pattern |
|----------|---------------------|
| **Python / ML** | Separate process; bridge peer on UDS/SHM; tensor/video via shared memory or ZMQ—**not** inside `router/frame.hpp` |
| **Node / dashboard** | WebSocket/HTTP gateway process subscribing via UDS or reading logs; no V8 in C++ core |
| **MAVLink / MCU** | Serial gateway process (peer `mavlink_bridge`); frames carry **commands/status**, not raw MAVLink bytes in 32 B frame |
| **Cameras (V4L / CSI)** | GStreamer or vendor pipeline process; router carries **metadata** (frame id, ts, drop count) |
| **CUDA / ML inference** | GPU process as peer; large payloads sideband |

Bridges live under `examples/bridges/` (Phase F). Core stays C++20 header-only.

---

## What we explicitly do not do (without ADR)

- Safety certification, SIL, redundant routers
- In-core DDS, ROS 2, gRPC, WebSocket server
- TLS on localhost UDS (optional later for remote UDP only)
- Bare-metal RTOS port in this module
- Auth/crypto on peer identity (lab/trusted LAN assumption)

---

## ADR alignment

| ADR | Principle embodied |
|-----|-------------------|
| 0001 | Layered IPC + router; fixed frames |
| 0002 | Transport concept, topology, links, facades |
| 0003 | Peer-address adapters; factories; `InterruptibleTransport` |

New work: **0004** module boundaries, **0005+** payload/sideband, deployment profiles, timestamps.

---

## Agent checklist (every PR)

- [ ] Read active phase plan + this file
- [ ] Hot path free of new heap, exceptions, or virtuals?
- [ ] Transport-specific code only in adapters?
- [ ] Topology/transport swappable via config, not hardcoded demo paths?
- [ ] Shutdown and resource cleanup documented if behavior changed?
- [ ] Bridge code only under `examples/`, not `src/`?
