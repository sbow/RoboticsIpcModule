# Codebase baseline (reference for agents)

Frozen summary of the IPC/router stack. Update STATUS baseline tag when the library changes; extend this file only for structural shifts.

**System context:** [SYSTEM-VISION.md](SYSTEM-VISION.md) — this module is the **router + framing** layer, not cameras, ML, or dashboards.

## Layout

| Path | Role |
|------|------|
| `ipc/src/ipc/` | Transports: UDP, UDS, SHM SPSC, echo, shutdown |
| `ipc/src/router/` | Frame, topology, links, facades, factories |
| `ipc/test/` | Demos — **not** the shipped module API |
| `ipc/MODULE.md` | Public consumption guide (Phase A) |
| `docs/adr/0001–000N` | IPC/router architecture decisions |
| `robotics-ipc-module/` | Plans, principles, lessons (portable) |

> Code was vendored from `sbow/cpp_tricks` (`cpp_tricks/ipc/...`) as the starting point and is now evolved here. Original paths are preserved inside `ipc/src/` so includes like `"router/foo.hpp"` and `"ipc/foo.hpp"` continue to resolve unchanged.

## Transports & when to use (robotics)

| Transport | Use on | Identity |
|-----------|--------|----------|
| **SHM** | Jetson co-located peers | Per-peer ring |
| **UDS** | x86 dev, local services, bridges | Socket path |
| **UDP** | HIL, sim, cloud, cross-container | IP:port |

## Wire format (today)

- `RouterFrame`: 32 B — control/metadata only in current demo
- Bulk data (vision, tensors): **not implemented** — Phase B sideband ADR

## Design constraints

See [DESIGN-PRINCIPLES.md](DESIGN-PRINCIPLES.md). Summary: no virtual hot path, adapters for transport, interruptible poll loops, bridges outside `src/`.

## Known limitations

- 22 B payload demo; static `router_client_config.h`
- SHM spin on full ring until Phase C
- No profile YAML until Phase B
- No Python/Node/MAVLink until Phase F examples

## Verify baseline

```bash
make all && make test-ipc && make test-router
```
