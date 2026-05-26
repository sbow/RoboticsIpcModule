# ADR 0002: IPC and router refactor (transport concept, topology, links)

- **Status:** Accepted
- **Date:** 2026-05-25
- **Supersedes:** Parts of [ADR 0001](0001-ipc-and-router.md) (`EndpointRegistry`, `RouterPeers`, monolithic headers)
- **Scope:** `cpp_tricks/ipc/src/ipc/`, `cpp_tricks/ipc/src/router/`, test binaries

## Context

ADR 0001 introduced a header-only IPC stack and router demo. Growth exposed:

- `Client`/`Server` assume a socket `fd` — blocks shared-memory transport.
- `EndpointRegistry` embeds UDS paths and UDP ports — not extensible to DDS or SHM without struct churn.
- `RouterPeers<Uds>` and `RouterPeers<Udp>` duplicate the same logic.
- `router_protocol.h` mixes framing, routing, transport adapters, and logging.

Goals: **DRY**, **SRP**, **extensibility** (SHM now, DDS later), **simple app integration** (`RouterServer` / `RouterClient`).

## Decision

### Layered headers

| Layer | Path | Responsibility |
|-------|------|------------------|
| IPC core | `ipc/buffer.hpp`, `ipc/transport.hpp` | `Buffer`, C++20 `Transport` concept |
| IPC modes | `ipc/datagram.hpp`, `ipc/shm_spsc.hpp` | `Udp`, `Uds`, `ShmSpsc` |
| IPC endpoint | `ipc/endpoint.hpp`, `ipc/echo.hpp` | `IpcEndpoint<Transport>`, echo helpers |
| Router | `router/frame.hpp`, `routing.hpp`, `peer_table.hpp`, `link.hpp`, `node.hpp` | Frame, rules, topology, datagram link, facades |
| Sample app | `test/router_client_config.h`, `router_*.cpp` | Sensor/controller/recorder only |

Umbrella includes: `ipc.hpp`, `router_protocol.hpp` (backward-compatible entry points).

### Transport concept

Each mode implements:

- `using Handle` — `Socket` (datagram) or `std::unique_ptr<ShmRegion>` (SHM)
- `open()`, `bind`, `recv`, `send`, optional `set_recv_timeout`
- Datagram-only: `connect_or_send`, `echo`, `send_to` helpers

### Router topology (replaces `EndpointRegistry`)

```cpp
struct PeerAddress { PeerAddressKind kind; union { uds_path; udp{host,port}; shm_name; } u; };
struct PeerEntry { uint8_t id; const char* name; PeerAddress local; };
struct RouterTopology { const PeerEntry* peers; size_t peer_count; PeerAddress router_listen; };
```

DDS: reserve `PeerAddressKind::DdsTopic` in ADR only — no code until needed.

### DatagramRouterLink (replaces `RouterPeers`)

Single template `DatagramRouterLink<Transport>` for server and client roles; `router_forward` delegates to the link.

### Application facades

- `RouterServer` — bind router, run forward loop
- `RouterClient` — bind peer, send/recv framed messages

## Consequences

### Positive

- New datagram topology = config + existing link; new SHM = `ShmSpsc` + future `ShmRouterLink`.
- Apps do not include `RouterPeers` or transport templates in role code.
- Headers stay small and testable in isolation.

### Negative

- Breaking change: `EndpointRegistry`, `RouterPeers`, `Client`/`Server` renamed/aliased.
- More files to navigate (mitigated by umbrella includes).

## References

- [ADR 0001](0001-ipc-and-router.md)
- [SHM_SPSC_TRANSPORT.md](../../cpp_tricks/ipc/SHM_SPSC_TRANSPORT.md)
