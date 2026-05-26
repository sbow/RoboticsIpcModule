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

**Next:** `D` — Validation & stress (or `E1` reference layout in parallel)

## Phase completion

| Phase | Name | Status | Notes |
|-------|------|--------|-------|
| A | Module packaging | `[x]` | Vendored ipc tree from sbow/cpp_tricks; flat layout `ipc/`; MODULE.md + ADR 0004 + lifecycle split + kRouterFrameVersion = 1 |
| B | Message & config | `[x]` | toml++ vendored; topology_loader + 4 profiles; ADR 0005 sideband; RouterLogFn; LastValueCache; unit tests 71/71 |
| C | Transport hardening | `[x]` | `ShmSpsc::try_send` + drop-on-full + `ShmRouterMetrics`; `idle_sleep_us` cuts idle CPU 100% → 1.63%; ADRs 0006 + 0007; SHM benchmark interruptible; `IPC_SKIP_SHM` default retired |
| D | Validation & stress | `[ ]` | Wire the A2 grep check + B/C unit tests into CI; D1 partially seeded |
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

- [ ] D1 Unit tests (+ cli_args regression)
- [ ] D2 Integration (restart, profile smoke)
- [ ] D3 Stress/soak
- [ ] D4 Fault injection

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
