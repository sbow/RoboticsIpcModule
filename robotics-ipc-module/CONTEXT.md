# Codebase baseline (reference for agents)

Frozen summary of the IPC/router stack. Update STATUS baseline tag when the library changes; extend this file only for structural shifts.

**System context:** [SYSTEM-VISION.md](SYSTEM-VISION.md) — this module is the **router + framing** layer, not cameras, ML, or dashboards.

## Layout

| Path | Role |
|------|------|
| `cpp_tricks/ipc/src/ipc/` | Transports: UDP, UDS, SHM SPSC, echo, shutdown |
| `cpp_tricks/ipc/src/router/` | Frame, topology, links, facades, factories |
| `cpp_tricks/ipc/test/` | Demos — **not** the shipped module API |
| `docs/adr/0001–0003` | IPC/router architecture |
| `robotics-ipc-module/` | Plans, principles, lessons (portable) |

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
make test-ipc && make test-router
```
