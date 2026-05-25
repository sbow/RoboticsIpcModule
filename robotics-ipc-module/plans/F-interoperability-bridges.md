# Phase F — Interoperability & bridges

**Skill:** `@ipc-robotics-phase-f` (install after adding skill file)  
**Depends on:** B1 (payload/sideband ADR), E1 (reference layout)

## Objective

Prove the module fits a **multi-language, multi-environment** stack per [SYSTEM-VISION.md](../SYSTEM-VISION.md)—without polluting C++ core.

## Principles

- Bridges are **separate processes** under `examples/bridges/`
- Same `RouterFrame` / peer IDs as C++ peers
- HIL/sim use **UDP profile**; Jetson uses **SHM profile**

## Deliverables

### F1 — Deployment profile templates

Under `config/profiles/`:

- `jetson_prod.yaml` — SHM for sensor/controller/vision/ml; UDS for logger/dashboard_feed
- `x86_dev.yaml` — UDS under `/run/robot/`
- `hil.yaml` / `sim_cloud.yaml` — UDP loopback ports

Document mapping in `docs/deployment-profiles.md`.

### F2 — Python bridge example

`examples/bridges/python_peer/`:

- Minimal subscriber/publisher matching frame layout (ctypes or small C extension)
- README: run against `router_server uds` on laptop
- Optional: recv JSON metadata for ML tooling (not in hot path)

### F3 — Node dashboard gateway example

`examples/bridges/node_gateway/`:

- UDS client → WebSocket broadcast (TypeScript or JS)
- README only; no npm dependency in main Makefile unless isolated target

### F4 — MAVLink gateway sketch

`examples/bridges/mavlink_gateway/README.md`:

- Serial open, parse MAVLink, emit compact `RouterFrame` status to router
- Commands: controller route → gateway → MCU
- State: not implemented = document interface + ADR stub

### F5 — Vision metadata peer sketch

`examples/bridges/vision_peer/README.md`:

- CSI vs V4L: separate capture binary; router frames carry `frame_id`, `timestamp`, `drops`
- Sideband SHM name convention documented (ties to B1 ADR)

## Review checklist

- [ ] No `Python.h`, `node.h`, or `mavlink.h` in `cpp_tricks/ipc/src/`
- [ ] Profiles validate with topology loader (Phase B)
- [ ] One manual demo: router + python_peer OR node_gateway on x86

## Acceptance

```bash
make all && make test-router
test -d config/profiles
test -f docs/deployment-profiles.md
# Bridge smoke (document command in README):
# ./build/.../router_server uds & ./examples/bridges/python_peer/...
```

## Do not

- Merge bridges into `RouterServer` core
- Put JPEG/NV12 in 32 B frame

## Session prompt

```
Execute Phase F from robotics-ipc-module/plans/F-interoperability-bridges.md.
Read SYSTEM-VISION.md and DESIGN-PRINCIPLES.md. Update STATUS.md.
```
