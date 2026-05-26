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

**Next:** `C` — Transport hardening (or `D1` unit-test expansion in parallel)

## Phase completion

| Phase | Name | Status | Notes |
|-------|------|--------|-------|
| A | Module packaging | `[x]` | Vendored ipc tree from sbow/cpp_tricks; flat layout `ipc/`; MODULE.md + ADR 0004 + lifecycle split + kRouterFrameVersion = 1 |
| B | Message & config | `[x]` | toml++ vendored; topology_loader + 4 profiles; ADR 0005 sideband; RouterLogFn; LastValueCache; unit tests 71/71 |
| C | Transport hardening | `[ ]` | Jetson SHM; lift `IPC_SKIP_SHM` default once `shm_push_slot` → `try_send` lands |
| D | Validation & stress | `[ ]` | Wire the A2 grep check + B unit tests into CI; D1 partially seeded |
| E | Robotics integration | `[ ]` | Jetson + x86 layout |
| F | Interoperability bridges | `[ ]` | Python, Node, MAVLink, vision |

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

- [ ] C1 SHM `try_send` / bounded wait
- [ ] C2 Idle wake (eventfd or ADR deferral)
- [ ] C3 Router metrics
- [ ] C4 Datagram seq (optional)

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
