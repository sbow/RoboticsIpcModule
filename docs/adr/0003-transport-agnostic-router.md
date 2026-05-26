# ADR 0003: Transport-agnostic router adapters and app factories

- **Status:** Accepted
- **Date:** 2026-05-25
- **Builds on:** [ADR 0002](0002-ipc-router-refactor.md)
- **Scope:** `cpp_tricks/ipc/src/router/`, sample apps under `cpp_tricks/ipc/test/`

## Context

ADR 0002 split headers and introduced `RouterTopology`, but `DatagramRouterLink` still branched on `Uds`/`Udp`, sample apps repeated `template<typename Transport>`, and `BindParams` duplicated addresses already in topology.

Goals:

- **OCP / DIP:** Router forwarding speaks `PeerAddress`; transport specifics live in adapters.
- **Ease of use:** Apps pick `TransportKind` once; templates stay in factories only.
- **No type-erased runtime:** Keep zero-overhead `DatagramRouterLink<Transport>` (user preference).

## Decision

### Adapter headers

| Header | Role |
|--------|------|
| [`peer_address_io.hpp`](../../cpp_tricks/ipc/src/router/peer_address_io.hpp) | `bind_datagram_endpoint`, `send_datagram_to_address`, `router_listen_bind_params` |
| [`datagram_peer_resolver.hpp`](../../cpp_tricks/ipc/src/router/datagram_peer_resolver.hpp) | `peer_id_from_recv<Transport>(topo, recv)` |
| [`link_concept.hpp`](../../cpp_tricks/ipc/src/router/link_concept.hpp) | `RouterLink` concept documenting link surface |
| [`transport_kind.hpp`](../../cpp_tricks/ipc/src/router/transport_kind.hpp) | `TransportKind` enum + CLI parse |
| [`factory.hpp`](../../cpp_tricks/ipc/src/router/factory.hpp) | `dispatch_transport_kind`, `make_router_server/client`, `bind_router_listen` |

`DatagramRouterLink` uses adapters only; no `Uds`/`Udp` names in [`link.hpp`](../../cpp_tricks/ipc/src/router/link.hpp).

### Application integration

```cpp
const TransportKind kind = transport_kind_from_cli(argv[1]);
const RouterTopology& topo = demo_topology(kind);

dispatch_transport_kind(argv[1], [&](auto) -> int {
    return with_transport(kind, topo, ...);  // templates inside factory/lambdas only
});
```

`RouterServer::run` accepts `RouterRunOptions` (`poll_timeout_ms`, optional `idle_exit_ms` for `ROUTER_TEST`).

### IPC

- **`InterruptibleTransport`** refinement in [`transport.hpp`](../../cpp_tricks/ipc/src/ipc/transport.hpp) (`try_recv` or `set_recv_timeout`).
- **`EchoServer::run_until`** requires `InterruptibleTransport`; `run()` remains blocking (benchmarks). No `app_shutdown` include in [`echo.hpp`](../../cpp_tricks/ipc/src/ipc/echo.hpp).

## Non-goals

- Type-erased `RouterSession` / virtual `RouterLink`
- DDS (hooks unchanged in `PeerAddressKind`)

### SHM router (added after ADR 0003)

[`shm_peer_address_io.hpp`](../../cpp_tricks/ipc/src/router/shm_peer_address_io.hpp) and [`shm_router_link.hpp`](../../cpp_tricks/ipc/src/router/shm_router_link.hpp) mirror the datagram adapter pattern: one SPSC ring per peer, router creates rings, clients join. `RouterServer` / `RouterClient` facades are unchanged; `dispatch_transport_kind` includes `shm`.

## Consequences

- New datagram behavior: edit `peer_address_io` / resolver, not `link.hpp` forwarding.
- Custom CLI paths that differ from `demo_topology()` still require topology/config alignment (known limitation).

## References

- [ADR 0002](0002-ipc-router-refactor.md)
- [README](../../README.md) — App integration
