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
| Full ring = infinite spin | `shm_push_slot` blocked forever in router send path | **Fixed Phase C1:** `shm_try_push_slot` + `ShmSpsc::try_send`; `ShmRouterLink::send_to_peer` drops + `dropped_full` metric (ADR 0006). Client→router publish is still blocking — separate ADR. |
| `yield()` is not "sleep" | Baseline router idle CPU was **100% of one core** sustained — `std::this_thread::yield()` only reorders ready threads, doesn't deschedule on an idle box | **Fixed Phase C2:** `RouterRunOptions::idle_sleep_us` (default 1 ms) — idle CPU drops to ~1.6% one core. `eventfd` deferred to Phase F (ADR 0007). Measure CPU *before* claiming an idle path works. |

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
| Silent `send_to_peer` miss | SHM dropped forwards to unknown dest | Throw on unknown dest (still); on **known dest with full ring**, drop + `dropped_full++` (Phase C, ADR 0006) — never silent |
| `std::atomic` member breaks `= default` move | Adding a `std::atomic<uint64_t>` to a class deletes its move ctor (atomics are non-movable). Pattern affected `ShmRouterMetrics` integration into `ShmRouterLink`. | Use `std::unique_ptr<MetricsBlock>` member. Metrics block stays heap-allocated; link is movable; `metrics()` returns `const&` that remains valid for link lifetime (ADR 0006). |
| 9-byte timestamp field in v1 frame | v1 used 9 B for `timestamp_ns` despite `uint64_t` covering 584 years in 8 B; the extra byte was wasted and forced a manual `for` loop in `set_timestamp_ns`/`timestamp_ns` (no native 9 B type). | **Fixed in v2:** 8 B `uint64_t` timestamp, host byte order, accessed via `std::memcpy`. Pick natural integer widths (1/2/4/8) — anything else costs you accessor complexity and screams "bug" to readers. ADR 0008. |
| Cache-line alignment is structural | v1 was 32 B (half a 64-B line); going to v2 64 B is "twice as big" by header but identical in cache lines touched per forward. Meanwhile the SHM slot stride was 1024 B regardless of frame size — the dominant cache effect was the slot, not the frame. | When sizing a header, think in **cache lines on the platforms of intent**, not bytes. 64 B is the universal x86_64 / aarch64 line. Pre-measure ring-slot stride before optimizing frame size. ADR 0008. |
| Big-endian "for forward compatibility" pays now, helps never | v1's big-endian timestamp was paid every forward by a manual shift-and-OR loop, for a hypothetical big-endian deployment that never came (and never will under ADR 0004's scope). | v2 uses host (little-endian) byte order and a `static_assert(std::endian::native == std::endian::little)`. Pay the byte-swap cost only when a real big-endian port motivates it. |
| Wire-format break? Just rip the band-aid | Maintaining v1 and v2 in parallel would have required version tag bytes, branch logic in every accessor, and a forever-shim. The project has zero external production consumers today. | **Pin the version per deployment, rebuild all peers atomically, preserve historical layout in the migration ADR.** Wait-for-real-pressure on multi-version coexistence; don't pre-pay it. ADR 0008. |

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
