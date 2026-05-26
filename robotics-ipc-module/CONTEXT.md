# Codebase baseline (reference for agents)

Frozen summary of the IPC/router stack. Update STATUS baseline tag when the library changes; extend this file only for structural shifts.

**System context:** [SYSTEM-VISION.md](SYSTEM-VISION.md) — this module is the **router + framing** layer, not cameras, ML, or dashboards.

## Layout

| Path | Role |
|------|------|
| `ipc/src/ipc/` | Transports: UDP, UDS, SHM SPSC, echo, shutdown |
| `ipc/src/router/` | Frame, topology, links, facades, factories, sideband, topology loader, last-value cache, lifecycle, **metrics (Phase C)** |
| `ipc/test/` | Demos + unit tests — **not** the shipped module API |
| `ipc/MODULE.md` | Public consumption guide (Phase A) |
| `config/profiles/*.toml` | Deployment profiles (x86_dev / jetson_prod / hil / sim_cloud) — Phase B |
| `third_party/tomlplusplus/` | Vendored toml++ v3.4.0 single header (MIT) — Phase B |
| `docs/adr/0001–000N` | IPC/router architecture decisions (0005 = payload + sideband, 0006 = SHM backpressure + metrics, 0007 = router idle-wake, 0008 = RouterFrame v2) |
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

- 22 B payload demo; `router_client_config.h` is the compile-time fallback (Phase B `--config <toml>` is the runtime path)
- Phase C closed: SHM router drops on full + metrics (ADR 0006), idle CPU 1.6% / core via `idle_sleep_us=1 ms` (ADR 0007); `IPC_SKIP_SHM=1` default retired
- Client→router SHM publish still blocks on full ring (separate ADR, future)
- `dropped_full` is a single global counter; per-peer attribution is a Phase D candidate
- Datagram links (UDP/UDS) have no metrics yet — Phase D / E
- `eventfd`-based idle wake deferred to Phase F (sleep_for backoff meets the 5%-CPU bar today)
- No Python/Node/MAVLink until Phase F examples

## Verify baseline

```bash
make all && make test-ipc-unit && make test-ipc && make test-router
```
