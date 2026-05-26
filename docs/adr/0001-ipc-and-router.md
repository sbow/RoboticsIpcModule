# ADR 0001: Header-only IPC library and message router

- **Status:** Accepted (updated 2026-05-25; registry/peers sections superseded by [ADR 0002](0002-ipc-router-refactor.md))
- **Date:** 2026-05-25
- **Scope:** `cpp_tricks/ipc/src/ipc.h`, `cpp_tricks/ipc/src/router_protocol.h`, `cpp_tricks/ipc/src/router_app.h`, router/echo tests

> **Note:** The refactor in [ADR 0002](0002-ipc-router-refactor.md) splits monolithic headers, replaces `EndpointRegistry` / `RouterPeers` with `RouterTopology` / `DatagramRouterLink`, and introduces `IpcEndpoint<Transport>` plus `ShmSpsc`. The narrative below describes the original design; use ADR 0002 for the current layout.

## Context

This repository explores C++ systems programming: building programs, comparing transports, and measuring IPC patterns. We need:

1. A small, reusable socket layer that works for **UDP** and **Unix-domain datagram (UDS)** without duplicating client/server code.
2. An **echo** benchmark to compare transport latency (in-process and multi-process).
3. A **router** scenario that better resembles real applications: multiple processes, a central fan-out/fan-in component, and a fixed binary message format.

Constraints:

- Keep the learning surface small (header-only, no external deps beyond POSIX).
- Preserve **`Client<Transport>`** and **`Server<Transport>`** as stable extension points for future work (e.g. shared-memory transport).
- Avoid transport-specific branching in application binaries where possible.
- Keep **application roles and routing rules** out of the reusable protocol header.

## Decision

Adopt a **layered** design:

| Layer | File(s) | Responsibility |
|-------|---------|------------------|
| Transport | `ipc.h` | Syscalls, RAII socket fd, `Udp` / `Uds` traits, generic `Client` / `Server`, echo helpers |
| Router protocol | `router_protocol.h` | `RouterFrame`, `EndpointRegistry`, `RouteRule`, `router_forward`, `RouterClient`, `RouterPeers` |
| App utilities | `router_app.h` | Signal handlers, `ROUTER_TEST` env flag (demo/test binaries) |
| Sample application | `router_client_config.h`, `router_client.cpp`, `router_server.cpp`, `router_test.cpp` | Sensor/controller/recorder topology, role logic, integration test |

Transports are modeled as **traits** (`Udp`, `Uds`) with static methods — not polymorphic base classes:

```text
bind / recv_from / send_to / connect_or_send / echo
         ↑
Client<Transport> / Server<Transport>  (own Socket, forward to trait)
         ↑
EchoClient / EchoServer / RouterClient / router_forward loop
```

### `ipc.h`

**Buffer** — non-owning view (`writable` / `read_only`); callers supply stack storage. No heap allocation on hot paths.

**Socket** — RAII wrapper around a single fd (copy/move deleted).

**Transport traits (`Udp`, `Uds`)** — static syscall wrappers:

- `bind`, `recv_from`, `send_to`, `connect_or_send`, `echo`
- Per-transport `BindParams`, `SendParams`, `RecvResult`
- `send_to` so callers (including the router) can address **any peer** without overloading client-only `connect_or_send` semantics

**Client / Server** — reusable endpoints:

- Own a `Socket` configured via `Transport::kCfg`
- Forward to trait static methods
- `Client` and `Server` expose `set_recv_timeout_ms` for poll/wait logic

**EchoClient / EchoServer** — thin protocol on top of Client/Server for the echo benchmark (`exchange`, `run`).

We **removed** an earlier CRTP `Endpoint` + `Role` layer. It only proved that a `role` constant existed at compile time and did not enforce runtime behavior. Keeping `Client` and `Server` as concrete templates is simpler and matches the intended reuse model.

### Router protocol (`router_protocol.h`)

Generic framing and routing — **no application role names or demo paths**.

**Fixed 32-byte `RouterFrame`:**

| Bytes | Field |
|-------|--------|
| 0 | Endpoint id (application-defined; 255 = server, 0 = invalid) |
| 1–9 | Nanoseconds since router started (9-byte big-endian unsigned) |
| 10–31 | Payload (22 bytes) |

**`EndpointRegistry`** — supplied by each application:

- `EndpointInfo[]` — `{ id, name, uds_path, udp_port }`
- Router address — `router_uds_path`, `router_udp_host`, `router_udp_port`
- Lookups: `endpoint_by_id`, `endpoint_by_name`, `endpoint_by_uds_path`, `endpoint_by_udp_port`

**`RouteRule` + `route_targets_for(rules, count, source)`** — routing table is application data, not protocol constants.

**Identity:** The router determines the sender from the **socket address** (`recvfrom` → `RouterPeers::endpoint_from(reg, recv)`), not from byte 0 of the payload. Byte 0 is **stamped by the router** on forward. Clients may set byte 0 before send, but the router overwrites it.

**`RouterPeers<Transport>`** — parameterized by `EndpointRegistry`:

- `bind_local(reg, fd, endpoint_id)`
- `send_to_router(reg, fd, buf)`
- `send(reg, fd, dest_id, buf)`
- `endpoint_from(reg, recv)`

**`router_forward(server, reg, rules, rule_count, frame, ts_ns)`** — recv, validate, stamp, fan-out; returns `ForwardResult`.

**`RouterClient<Transport>(reg, endpoint_id)`** — binds from registry; `send_message`, `recv_message`, `recv_message_blocking_until`.

### Sample application (`router_client_config.h` + test binaries)

The **sensor / controller / recorder** demo is sample code, not part of the protocol:

| Id | Role | Demo routing |
|----|------|--------------|
| 1 | sensor | → controller and recorder |
| 2 | controller | → recorder |
| 3 | recorder | receive-only |

- **`router_client_config.h`** — `kDemoEndpoints`, `kDemoRouteRules`, `demo_registry()`, paths/ports/logs
- **`router_client.cpp`** — role behavior (`run_sensor`, `run_controller`, `run_recorder`), CLI
- **`router_server.cpp`** — thin loop calling `router_forward` with `demo_registry()` and `kDemoRouteRules`
- **`router_test.cpp`** — fork/exec four processes; verifies CSV logs; sets `ROUTER_TEST=1` for idle self-exit

**Controller behavior (demo):** blocks until a sensor packet arrives before each control message.

### Test programs

| Binary | Role |
|--------|------|
| `echo_tests` | In-process threaded UDP + UDS echo benchmark |
| `echo_server` / `echo_client` | Multi-process echo (simple + benchmark variants) |
| `router_server` | Central router (sample app) |
| `router_client` | Sample roles (sensor / controller / recorder) |
| `router_test` | Fork/exec integration test; UDS then UDP |

**Build:** `make test-ipc`, `make test-ipc-mp`, `make test-router`.

## Alternatives considered

| Alternative | Why not chosen |
|-------------|----------------|
| Separate `.cpp` for `ipc` | Library is entirely templates + inline static methods; header-only keeps `make all` simple and tests include one header |
| Virtual `Transport` interface | Virtual calls on hot path; static traits match “zero overhead abstraction” goal |
| CRTP `Endpoint` + `Role` | No runtime enforcement; removed in favor of plain `Client`/`Server` |
| Identity from frame byte 0 | Spoofable; address-based identity matches datagram best practice |
| Single mega-binary for router test | Poor fit for “real multi-process” story; fork + dedicated binaries kept |
| `connect_or_send` only (no `send_to`) | Router must send to arbitrary peers; `send_to` is the minimal generalization |
| Put router logic entirely in `ipc.h` | Would couple generic IPC to application routing; split at `router_protocol.h` |
| Hard-code sensor/controller/recorder in `router_protocol.h` | Prevents reuse with other topologies; moved to `router_client_config.h` |
| `EndpointInfo` with `log_path` in protocol | Logging paths are application concern; demo logs live in sample config only |

## Consequences

### Positive

- **Same client/server code** for UDP and UDS; transport chosen at compile time.
- **Protocol reusable:** new app = new `EndpointRegistry` + `RouteRule[]`; no protocol header edits.
- **Clear extension path:** new transport = `RouterPeers` specialization + `Transport::kCfg`.
- **Router server loop** is a thin `router_forward` wrapper.
- **Echo and router** share `ipc.h`; router adds protocol without polluting echo APIs.

### Negative / limitations

- **Header-only** → compile time grows as templates instantiate in many TUs.
- **Sample app config shared** via `router_client_config.h` across server/client/test (same demo topology).
- **Datagram limits:** no guaranteed delivery, ordering, or fragmentation handling; frames must fit one datagram (32 bytes today).
- **No authentication or encryption** — lab/test code only.
- **UDS client bind-once** — datagram clients bind a local path; echo tests still special-case empty `client_path` after first send.
- **Integration test** silences child stdout/stderr; use manual four-process run for live logging.

### Planned follow-ups

- ~~**`ShmSpsc` transport**~~ — implemented; see [ADR 0002](0002-ipc-router-refactor.md) and `cpp_tricks/ipc/src/ipc/shm_spsc.hpp`.

## References

- `cpp_tricks/ipc/src/ipc.hpp` (umbrella; `ipc.h` includes it)
- `cpp_tricks/ipc/src/router_protocol.hpp` (umbrella; `router_protocol.h` includes it)
- [ADR 0002: IPC and router refactor](0002-ipc-router-refactor.md)
- `cpp_tricks/ipc/src/router_app.h`
- `cpp_tricks/ipc/test/router_client_config.h`
- `cpp_tricks/ipc/SHM_SPSC_TRANSPORT.md`
- `cpp_tricks/ipc/test/router_server.cpp`, `router_client.cpp`, `router_test.cpp`
- Linux `sendto` / `recvfrom`, `AF_UNIX` + `SOCK_DGRAM`, `SOCK_DGRAM` + `AF_INET`
