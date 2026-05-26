# Execution status

> **Agents:** Update every session. Read [DESIGN-PRINCIPLES.md](DESIGN-PRINCIPLES.md) before coding.

## Configuration

| Key | Value |
|-----|-------|
| `IPC_ROOT` | `ipc` |
| `PLANS_ROOT` | `robotics-ipc-module` |
| Baseline tag / commit | `243ede0d905e0e5365073b83feff06ba3427db07` (initial commit, `main`, `github.com/sbow/RoboticsIpcModule`) |
| Target platforms | Jetson (embedded), x86+CUDA (dev), HIL/sim (UDP) |

## Current phase

**Active:** `D` — Validation & stress (D0–D2 complete; D3 next)

## Phase completion

| Phase | Name | Status | Notes |
|-------|------|--------|-------|
| A | Module packaging | `[x]` | Vendored ipc tree from sbow/cpp_tricks; flat layout `ipc/`; MODULE.md + ADR 0004 + lifecycle split + kRouterFrameVersion = 1 |
| B | Message & config | `[x]` | toml++ vendored; topology_loader + 4 profiles; ADR 0005 sideband; RouterLogFn; LastValueCache; unit tests 71/71 |
| C | Transport hardening | `[x]` | `ShmSpsc::try_send` + drop-on-full + `ShmRouterMetrics`; `idle_sleep_us` cuts idle CPU 100% → 1.63%; ADRs 0006 + 0007; SHM benchmark interruptible; `IPC_SKIP_SHM` default retired |
| D | Validation & stress | `[~]` | D0 (per-peer ring sizing, ADR 0009) + D1 (unit suites, self-routing rejection, SourceSeqTracker) + D2 (4 integration scenarios + D2a per-peer drop attribution) done; D3 next |
| E | Robotics integration | `[ ]` | Jetson + x86 layout |
| F | Interoperability bridges | `[ ]` | Python, Node, MAVLink, vision; also hosts eventfd idle-wake follow-up (ADR 0007 deferral) |

### Phase A deliverables

- [x] A1 `ipc/MODULE.md` (platforms, include graph, link flags, thread model, shutdown contract, public/example boundary, known limitations)
- [x] A2 Public vs example split (carved `router/lifecycle.hpp` from `router_app.h`; `node.hpp` no longer leaks `#include "router_app.h"`; grep gate documented)
- [x] A3 `kRouterFrameVersion = 1` added to `ipc/src/router/frame.hpp` with frozen-v1 layout comment
- [x] A4 `docs/adr/0004-robotics-module-boundaries.md` (in/out scope, module API promise, bridges deferred to Phase F)

### Phase B deliverables

- [x] B1 `docs/adr/0005-payload-policy-and-sideband.md` + `ipc/src/router/sideband.hpp` (control plane vs sideband; SidebandHeader 16 B v1 with magic `'RSB1'`; `/robot_<peer>_<class>` naming convention; class constants for vision_nv12 / vision_jpeg / ml_tensor_in / ml_tensor_out / mavlink_bulk)
- [x] B2 `ipc/src/router/topology_loader.hpp` (TOML 1.0 via vendored toml++ v3.4.0) + `config/profiles/{x86_dev,jetson_prod,hil,sim_cloud}.toml`; `--config <path>` CLI on `router_server`; address grammar `<uds|udp|shm>:<rest>`; LoadedTopology owns string storage in `std::deque<std::string>` (move-safe on Linux); validates duplicate ids, scheme, length, route → peer references; 45/45 loader assertions pass
- [x] B3 `RouterLogFn` typedef + `router_set_log_fn()` in `router_app.h`; demos register `demo_stderr_logger` with `[info]/[warn]/[err]` tags; library routing remains string-free on hot path
- [x] B4 `ipc/src/router/last_value_cache.hpp` (`LastValueCache<N=256>`, subscriber-side, thread-unsafe by design); 26/26 cache assertions pass

### Phase C deliverables

- [x] C1 SHM `try_send` / bounded wait — `ShmSendResult { Ok, Full }` + `shm_try_push_slot` + `ShmSpsc::try_send`; `ShmRouterLink::send_to_peer` drops on Full instead of spinning; client→router blocking publish documented as a known limitation in MODULE.md (separate ADR future). Echo benchmark client now uses interruptible try_send+try_recv path; `IPC_SKIP_SHM` default removed from `make test-ipc`.
- [x] C2 Idle wake — `RouterRunOptions::idle_sleep_us` (default 1 ms) replaces unconditional `yield()`. Measured idle CPU dropped from 100% → 1.63% of one core over 60 s on `jetson_prod.toml`. `eventfd` path documented and deferred in [ADR 0007](../docs/adr/0007-router-idle-wake.md).
- [x] C3 Router metrics — `ipc/src/router/metrics.hpp` with `ShmRouterMetrics { forwarded, dropped_full, recv_empty, recv_truncated }`; `ShmRouterLink::metrics()` returns a stable reference (heap-allocated; link stays movable). See [ADR 0006](../docs/adr/0006-shm-backpressure-and-metrics.md).
- [ ] C4 Datagram seq — optional, deferred to Phase D / F (no in-tree consumer yet that needs newest-only sequencing on UDP).

### Phase D deliverables

- [x] D0 Per-peer SHM ring sizing — ADR 0009; `[[peers]] shm_slot_count` / `shm_max_payload` optional fields parsed and validated (≥ `kRouterFrameSize`, ≤ 256 MiB, ≤ 2²⁰ slots, only on `shm:` peers); `PeerEntry` carries sentinel-zero `uint32_t` overrides (aggregate-init compatible); new `bind_shm_endpoint(IpcEndpoint<ShmSpsc>&, const PeerEntry&, bool)` overload wires overrides through `ShmRouterLink`; `jetson_prod.toml` shipped with the recommended `256 × 64` per peer (~15× memory reduction vs. legacy 256 × 1024); `topology_loader_test` extended (+26 assertions). No source change in any compile-time consumer; `make test-router` green on all three transports.
- [x] D1 Unit tests — `datagram_seq_test`, `routing_test`, `resolver_test`, `cli_args_test` + topology-loader self-routing rejection; new library header `router/source_seq_tracker.hpp` + test-only `router_cli_args.hpp` helper; **8 unit binaries / 642 assertions** aggregate
- [x] D2 Integration — `slow_recorder_test` (D2a per-peer drop attribution gate), `burst_sensor_test` (closes deferred C4 via `SourceSeqTracker`), `profile_switch_test` (jetson_prod SHM ↔ hil UDP round-trip), `router_restart_test` (SIGKILL + re-bind); **4 binaries / 64 assertions**. D2a: `ShmRouterMetrics::dropped_full_per_peer[256]` (ADR 0006 closed-loop), aggregate counter preserved
- [ ] D3 Stress/soak (soak_router.sh, shm_leak_check.sh, idle_cpu_check.sh)
- [ ] D4 Fault injection
- [ ] CI workflow (PR gate)

### Phase E deliverables

- [ ] E1 Reference layout (vision, ML, MAVLink, dashboard peers)
- [ ] E2 systemd (Jetson-oriented)
- [ ] E3 Bridge pointers to Phase F
- [ ] E4 Monotonic/PTP timestamp ADR

### Phase F deliverables

- [ ] F1 Profile templates + `deployment-profiles.md`
- [ ] F2 Python bridge example
- [ ] F3 Node gateway example
- [ ] F4 MAVLink gateway sketch
- [ ] F5 Vision metadata peer sketch

## Blockers

_None._

## Session log

| Date | Phase | Summary | Agent/human |
|------|-------|---------|-------------|
| | | Plan pack created | initial |
| | | Added DESIGN-PRINCIPLES, LESSONS-LEARNED, SYSTEM-VISION, Phase F | plan update |
| 2026-05-25 | — | Repo init + push to `github.com/sbow/RoboticsIpcModule`; baseline `243ede0` recorded | orchestrator |
| 2026-05-25 | A | Vendored `ipc/` + `docs/adr/0001-0003` from `sbow/cpp_tricks` (plain copy); flat IPC_ROOT=`ipc`; fresh top-level `Makefile` (test-ipc skips SHM until Phase C, test-router runs all 3 transports) | phase-a |
| 2026-05-25 | A | A1 MODULE.md; A2 lifecycle split (`router/lifecycle.hpp`) removes `node.hpp` → `router_app.h` leak; A3 `kRouterFrameVersion = 1`; A4 ADR 0004 — `make all && make test-ipc && make test-router` all green | phase-a |
| 2026-05-25 | B | Vendored toml++ v3.4.0; B1 ADR 0005 + `router/sideband.hpp`; B2 `router/topology_loader.hpp` + 4 profiles + `router_server --config <toml>` (auto-derives transport from listen kind); B3 `RouterLogFn` plug-in + leveled `router_log`; B4 `router/last_value_cache.hpp`; `make test-ipc-unit` adds topology + cache tests (71/71 assertions); `make test-ipc` + `make test-router` regression green | phase-b |
| 2026-05-25 | C | **Baseline measurement (yield-only):** idle CPU = 100% one core sustained over 60 s (`pidstat -p $PID 10 6`, jetson_prod profile, no clients). C1 `ShmSendResult` + `shm_try_push_slot` + `ShmSpsc::try_send`; `ShmRouterLink::send_to_peer` drops on Full and bumps `dropped_full` (no infinite spin). C3 `router/metrics.hpp` with `ShmRouterMetrics` (atomics, heap-owned via `unique_ptr`, link stays movable); `metrics()` accessor stable for link lifetime. C2 `RouterRunOptions::idle_sleep_us` (default 1 ms) replaces unconditional `yield()`. **Post-fix measurement:** idle CPU = 1.68% one core averaged over 60 s (same host, same profile) — comfortably under the 5% acceptance bar. ADR 0006 (drop-on-full + metrics) and ADR 0007 (idle-wake sleep_for now, eventfd deferred to Phase F). Echo client SHM path made interruptible via try_send + try_recv + stop checks; `IPC_SKIP_SHM=1` default removed from `make test-ipc`. New unit test `ipc/test/shm_backpressure_test.cpp` (3 scenarios, 27/27 assertions) verifies forwarded / recv_empty / drop policy under ring saturation (published=1024 → forwarded=256, dropped_full=768; 2s deadline guard catches regressions). Aggregate: `make test-ipc-unit` 98/98 assertions, `make test-ipc` UDP 828k + UDS 890k + SHM 7.9M trips/5s, `make test-router` all 3 transports green. | phase-c |
| 2026-05-25 | C→D bridge | **ADR 0008 — RouterFrame v2.** Co-authored decision to retire v1 (32 B, 9 B big-endian timestamp, 22 B payload, no topic / seq / sideband descriptor) in favor of a 64 B cache-line-sized header: `source`, `flags` (has_sideband/keyframe/is_ack/eos/priority), `topic_id`, `seq`, `timestamp_ns` (host LE), `sideband_idx`, `sideband_len` (uint48, 256 TB cap), `sideband_seq`, 32 B inline payload. `kRouterFrameVersion` bumped 1→2. Single-deploy migration (no on-wire negotiation); existing in-tree callers (`router_client.cpp`, `router_server.cpp`, `router_test.cpp`, all unit tests) compiled unchanged thanks to API-compatible accessors. New unit test `ipc/test/frame_test.cpp` covers every field + offset + uint48 truncation + neighbor-non-clobber (163/163 assertions). ADR 0004 updated to point at v2 as active contract (v1 layout preserved for archeology); ADR 0005 refined with the in-frame sideband descriptor and a forward declaration of `[[peers.sideband]] memory_class` for Phase F (CUDA/NvBufSurface). Aggregate post-v2: `make test-ipc-unit` 261/261 assertions, `make test-ipc` UDP 734k + UDS 825k + SHM 8.6M trips/5s, `make test-router` all 3 transports green. | phase-c→d |
| 2026-05-25 | D0 | **ADR 0009 — Per-peer SHM ring sizing.** `[[peers]]` schema gains optional `shm_slot_count` (default 256) and `shm_max_payload` (default 1024, must be ≥ `kRouterFrameSize` when set), validated at load time. `PeerEntry` grows two `uint32_t` trailing fields with in-class default `= 0` (sentinel for "use `ShmSpsc::BindParams` defaults"); all existing aggregate initializers (`kDemoPeers` in `router_client_config.h`, `kPeers` in `shm_backpressure_test.cpp`) compile unchanged. New `bind_shm_endpoint(IpcEndpoint<ShmSpsc>&, const PeerEntry&, bool)` overload threads the overrides through `ShmRouterLink::bind_router` and `bind_peer`. Loader rejects: `shm_max_payload < 64`, `shm_max_payload > 256 MiB`, `shm_slot_count > 2²⁰`, or any `shm_*` field set on a non-SHM peer — each with frame-aware error text. `jetson_prod.toml` updated to demonstrate the recommended sizing on all three peers (256 × 64 = ~35 KiB per ring direction; ~15× reduction vs. the legacy 256 × 1024 ≈ 514 KiB). `topology_loader_test` extended with 5 new scenarios (defaults preserved, overrides parsed, cache-footprint ratio asserted, validation matrix, jetson_prod profile invariant). Aggregate post-D0: `make test-ipc-unit` 287/287 assertions (163 + 71 + 26 + 27); `make test-router` all 3 transports green; `make test-ipc` UDP 745k + UDS 866k + SHM 8.2M trips/5s (no regression). | phase-d0 |
| 2026-05-25 | D2 | **Integration scenarios + per-peer drop attribution (D2a).** D2a extends `ShmRouterMetrics` with `std::array<std::atomic<uint64_t>, 256> dropped_full_per_peer` (indexed by destination peer id; ~2 KiB per metrics block, heap-owned via the existing `unique_ptr` so `ShmRouterLink` stays movable). `ShmRouterLink::send_to_peer` now bumps both the aggregate `dropped_full` (Phase C compatibility) and the per-destination slot on Full; verified by `shm_backpressure_test::test_per_peer_attribution_isolates_slow_destination` (sensor → {B drained, C ignored} fanout: drop_to_B=0, drop_to_C=768, per-peer counters sum to aggregate). ADR 0006 "Negative" → "Closed" for per-peer attribution. Four new D2 integration binaries (each follows the lightweight EXPECT/EXPECT_EQ pattern; no test framework deps): `slow_recorder_test` (in-process: 2 kHz sensor / 200 Hz recorder / full-rate controller — controller=2000/2000, recorder=261/2000, drop[ctrl]=0, drop[rec]=1523, attribution gate green); `burst_sensor_test` (closes the deferred C4 deliverable: 5000-frame burst with retry-on-full publish, subscriber drains via real `ShmRouterLink` + `SourceSeqTracker`, asserts `samples + gaps == seq_range` invariant + out_of_order=0 + duplicates=0 + ≥99% delivery; measured 5000/5000 with zero gaps under right-sized rings); `profile_switch_test` (loads `jetson_prod.toml` SHM → `hil.toml` UDP → `jetson_prod.toml` again, one round-trip per profile, validates ADR 0009 per-peer sizing carries through the loader); `router_restart_test` (subprocess: spawns `router_server --config jetson_prod.toml`, SIGKILLs it, asserts SHM regions linger then a fresh spawn binds cleanly via `shm_open`+`ftruncate`+`memset`; also covers overlapping spawns). Aggregate post-D2: `make test-ipc-unit` **652/652 assertions** across 8 binaries (added 10 D2a assertions to shm_backpressure); `make test-ipc-integration` **64/64 assertions** across 4 new binaries (11 + 9 + 27 + 17). `make test-router` UDS / UDP / SHM all green; `make test-ipc` UDP 724k + UDS 802k + SHM 7.7M trips/5s (no regression). | phase-d2 |
| 2026-05-25 | D1 | **Unit suites + self-routing rejection.** Four new test binaries: `datagram_seq_test` (296 assertions covering `SourceSeqTracker` first-observation, in-order streams, gap accounting, duplicate vs. out-of-order classification, 2³² wrap with and without pre-wrap gap, independent-source isolation, reset/clear, LVC integration under 10% packet loss, template `N` bounds), `routing_test` (28 assertions for `route_targets_for` empty-rules / unknown-source / unicast / broadcast / first-match-wins / dest1==0 sentinel / `kEndpointInvalid` source / `ForwardResult` truthiness), `resolver_test` (16 assertions for `peer_id_from_recv<Uds>` strcmp match + unknown + empty + prefix non-match, `peer_id_from_recv<Udp>` port match + unknown + host-byte-order ignored, mixed-transport topology branch isolation), `cli_args_test` (13 assertions locking `log_path_for_role` arity — controller `argv[5]`/`argv[3]`/fallback, recorder `argv[4]`/`argv[3]`/fallback, recorder does NOT consume `argv[5]`, unknown role short-circuits to `""`, null-fallback safety). New library header `ipc/src/router/source_seq_tracker.hpp` (per-source uint32 seq tracker, modular-arithmetic wrap, 2³¹-wide forward/backward classification windows; pairs with `LastValueCache` on subscriber read path). New test-only helper `ipc/test/router_cli_args.hpp` (callback-based fallback so the same function is used by both `router_client.cpp` and the regression test). Topology loader now rejects self-routing (`route.source` ∈ `{dest0, dest1}`) with frame-aware error text — `topology_loader_test` grows +2 assertions (73 total). Aggregate post-D1: `make test-ipc-unit` **642/642 assertions across 8 binaries** (163 frame + 73 loader + 26 LVC + 27 SHM backpressure + 296 datagram_seq + 28 routing + 16 resolver + 13 cli_args); `make test-ipc` UDP 720k + UDS 795k + SHM 8.0M trips/5s; `make test-router` all 3 transports green (no demo CLI regression). | phase-d1 |
