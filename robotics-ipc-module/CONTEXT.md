# Codebase baseline (reference for agents)

Frozen summary of the IPC/router stack. Update STATUS baseline tag when the library changes; extend this file only for structural shifts.

**System context:** [SYSTEM-VISION.md](SYSTEM-VISION.md) — this module is the **router + framing** layer, not cameras, ML, or dashboards.

## Layout

| Path | Role |
|------|------|
| `ipc/src/ipc/` | Transports: UDP, UDS, SHM SPSC, echo, shutdown |
| `ipc/src/router/` | Frame, topology, links, facades, factories, sideband, topology loader, last-value cache, lifecycle, **metrics (Phase C — SHM; Phase D4 — Datagram)**, **source-seq tracker (Phase D1)** |
| `ipc/test/` | Demos + unit tests — **not** the shipped module API |
| `ipc/MODULE.md` | Public consumption guide (Phase A) |
| `robotics-ipc-module/scripts/` | Phase D3 stress/soak wrappers (soak / leak-check / idle-CPU / latency-histogram) |
| `.github/workflows/ci.yml` | Phase D PR gate — build + unit + integration + router + leak-check on every push/PR |
| `config/profiles/*.toml` | Deployment profiles (x86_dev / jetson_prod / hil / sim_cloud) — Phase B |
| `third_party/tomlplusplus/` | Vendored toml++ v3.4.0 single header (MIT) — Phase B |
| `docs/adr/0001–000N` | IPC/router architecture decisions (0005 = payload + sideband, 0006 = SHM backpressure + metrics, 0007 = router idle-wake, 0008 = RouterFrame v2, 0009 = per-peer SHM ring sizing) |
| `robotics-ipc-module/` | Plans, principles, lessons (portable) |

> Code was vendored from `sbow/cpp_tricks` (`cpp_tricks/ipc/...`) as the starting point and is now evolved here. Original paths are preserved inside `ipc/src/` so includes like `"router/foo.hpp"` and `"ipc/foo.hpp"` continue to resolve unchanged.

## Transports & when to use (robotics)

| Transport | Use on | Identity |
|-----------|--------|----------|
| **SHM** | Jetson co-located peers | Per-peer ring |
| **UDS** | x86 dev, local services, bridges | Socket path |
| **UDP** | HIL, sim, cloud, cross-container | IP:port |

## Wire format (today)

- `RouterFrame`: **64 B v2** — typed (`topic_id`), sequenced (`seq`), with in-frame sideband descriptor (`sideband_idx` / `sideband_seq` / `sideband_len`); 32 B inline payload (ADR 0008)
- Bulk data (vision, tensors): sideband regions per ADR 0005; v2 frame carries the cross-reference

## Design constraints

See [DESIGN-PRINCIPLES.md](DESIGN-PRINCIPLES.md). Summary: no virtual hot path, adapters for transport, interruptible poll loops, bridges outside `src/`.

## Known limitations

- 32 B inline payload (v2); `router_client_config.h` is the compile-time fallback (Phase B `--config <toml>` is the runtime path)
- Phase C closed: SHM router drops on full + metrics (ADR 0006), idle CPU 1.6% / core via `idle_sleep_us=1 ms` (ADR 0007); `IPC_SKIP_SHM=1` default retired
- Phase D1 closed: subscriber-side `SourceSeqTracker` (uint32 seq, 2³² wrap-aware) ships as a library header; topology loader rejects self-routing; 8 unit-test binaries / 652 assertions
- Phase D2 closed: per-peer drop attribution (`ShmRouterMetrics::dropped_full_per_peer[256]`, additive on ADR 0006); 4 integration binaries (slow recorder / burst sensor / profile switch / router restart) / 64 assertions
- Phase D3 closed: stress/soak shell scripts under `robotics-ipc-module/scripts/` + `make test-soak / test-leak-check / test-idle-cpu / test-latency-histogram`; leak check globs `/dev/shm/cpp_tricks_*` + `/tmp/cpp_tricks_*.sock` around the full unit+integration+router pass; idle-CPU gate at ≤ 5 % (ADR 0007 regression)
- Phase D4 closed: `DatagramRouterMetrics { forwarded, recv_truncated, recv_unknown_source, recv_empty }` heap-owned by `DatagramRouterLink<T>` (mirrors `ShmRouterMetrics` pattern, ADR 0006 update); new `fault_injection_test` (34 assertions, 6 scenarios — truncated UDP, unknown-source UDP, wrong UDS path, UDS rebind, TOML reject, SIGKILL mid-traffic). `test-ipc-integration` now 98/98 across 5 binaries.
- Phase D closed: CI PR gate (`.github/workflows/ci.yml`) builds with ccache, runs `make test-ipc-unit` / `test-ipc-integration` / `test-router` / `shm_leak_check.sh` on every push to `main` and PR; `make ci` is the local mirror (serialized sub-makes so `-jN` still parallelises the build but tests don't race on shared `/cpp_tricks_router_*` SHM paths). End-to-end ~32 s on the dev box.
- Client→router SHM publish still blocks on full ring (separate ADR, future)
- `eventfd`-based idle wake deferred to Phase F (sleep_for backoff meets the 5%-CPU bar today)
- No Python/Node/MAVLink until Phase F examples

## Verify baseline

```bash
make ci   # mirrors .github/workflows/ci.yml — build + unit + integration + router + leak-check
```

Older one-liner (handy when you only want a subset):

```bash
make all && make test-ipc-unit && make test-ipc && make test-router
```
