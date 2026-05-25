# Lessons learned — From cpp_tricks IPC/router development

Concrete failures and fixes from this codebase. **Agents:** grep for these anti-patterns before marking a phase complete.

---

## Architecture & refactor

| Lesson | Detail | Action |
|--------|--------|--------|
| Monolithic headers do not scale | `router_protocol.h` mixed framing, routing, UDS branches | Keep layers in ADR 0002 layout; umbrella includes only |
| Registry with embedded ports/paths | Old `EndpointRegistry` blocked SHM/DDS | Use `RouterTopology` + `PeerAddress` union |
| Duplicate `RouterPeers<Uds/Udp>` | Copy-paste forwarding | Single `DatagramRouterLink<Transport>` + adapters |
| Templates at app boundary | Role code repeated `template<typename Transport>` | `dispatch_transport_kind` + factories only |

---

## SHM router

| Lesson | Detail | Action |
|--------|--------|--------|
| One ring per peer | Shared single ring broke identity | Router creates N rings; client joins one; source = channel id |
| `router_listen` SHM name unused | Listen address in topology is validation only | Document in `shm_peer_address_io.hpp`; do not `shm_open` listen name unless ADR changes model |
| `if constexpr` needs `else` | `router_client` instantiated datagram path for `ShmSpsc` | Always `else` branch so wrong template is not compiled |
| SHM idle-exit vs datagram | `RouterServer::run` only idle-exited on **exceptions**; SHM `forward()` returns `{}` | Idle check on empty `forward()` + `yield` (see `node.hpp`) |
| SHM recv tight loop | `recv_message_until` spun at 100% CPU | `yield` each iteration; recorder loop too |
| Full ring = infinite spin | `shm_push_slot` blocks forever | Phase C: `try_send` + drop/metric |

---

## Testing & demo apps

| Lesson | Detail | Action |
|--------|--------|--------|
| Wrong `argv` for log paths | Controller used `argv[4]` (port) instead of `argv[5]` (log) | Document arity table in `router_client` or shared `cli_args.hpp`; unit-test log_path_for_role |
| Recorder log on socket path | `argc >= 3` took `argv[2]` for UDS recorder | UDS/UDP recorder log at `argv[4]` when `argc >= 5`; SHM at `argv[3]` when `argc >= 4` |
| Log file not cleared between scenarios | SHM test counted 10 lines (2×) | `unlink` recorder/controller logs at **every** `run_scenario` start |
| Integration test thresholds only | `router_test` counts CSV lines, no latency | Phase D: add unit tests + optional timing |
| `ROUTER_TEST=1` behavior | Idle-exit changes router/recorder lifetime | Never enable in production systemd units |

---

## Robustness & ops

| Lesson | Detail | Action |
|--------|--------|--------|
| Blocking `recv` blocks SIGTERM | Long-running servers need poll | 200 ms `SO_RCVTIMEO` or `try_recv` + stop flag |
| `SIGKILL` skips destructors | Stale UDS/SHM until cleanup | Test `cleanup_*`; document in MODULE.md |
| Stop order matters less for SHM | Router creator unlinks on exit; stop router after clients in tests | Keep test `stop_all(recorder, controller, router)` |
| Silent `send_to_peer` miss | SHM dropped forwards to unknown dest | Throw or increment `dropped` metric |

---

## Robotics / multi-environment (forward-looking)

| Lesson | Detail | Action |
|--------|--------|--------|
| 22 B payload is demo-only | Real stack needs metadata + sideband | Phase B payload ADR; cameras/ML not in frame body |
| Same peer IDs across environments | HIL should not recompile routes | Topology file per profile; stable `kEndpoint*` ids |
| Python/Node in core = bloat | Bridges must be separate processes | Phase F examples only |
| Jetson vs x86 differs by transport | Jetson: SHM; sim/cloud: UDP loopback | Profile YAML maps `local:` per peer |

---

## `if constexpr` / factory pattern

```cpp
// WRONG — both branches may be instantiated
if constexpr (std::is_same_v<T, ShmSpsc>) { return shm(); }
return datagram<T>();

// RIGHT
if constexpr (std::is_same_v<T, ShmSpsc>) { return shm(); }
else { return datagram<T>(); }
```

---

## CLI arity reference (demo client)

| Role | Transport | argc | Log path argv |
|------|-----------|------|----------------|
| controller | uds/udp | 6 | `argv[5]` |
| controller | shm | 4 | `argv[3]` |
| recorder | uds/udp | 5 | `argv[4]` |
| recorder | shm | 4 | `argv[3]` |

Add tests when touching argument parsing.
