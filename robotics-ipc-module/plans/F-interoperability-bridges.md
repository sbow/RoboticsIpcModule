# Phase F — Interoperability & bridges

**Skill:** `@ipc-robotics-phase-f` (install after adding skill file)
**Depends on:** B1 (payload/sideband ADR 0005), E1 (reference layout), ADR 0008 (RouterFrame v2 — bridges read v2 fields), ADR 0009 (per-peer ring sizing — vision/ML peers want large `shm_max_payload`)

## Objective

Prove the module fits a **multi-language, multi-environment** stack per [SYSTEM-VISION.md](../SYSTEM-VISION.md)—without polluting C++ core.

## Principles

- Bridges are **separate processes** under `examples/bridges/`
- Same `RouterFrame` v2 wire / peer IDs as C++ peers (see ADR 0008 for the field layout external bridges must mirror)
- HIL/sim use **UDP profile**; Jetson uses **SHM profile**

## Deliverables

### F1 — Deployment profile templates

Under `config/profiles/`:

- `jetson_prod.toml` — SHM for sensor/controller/vision/ml; UDS for logger/dashboard_feed; per-peer `shm_max_payload` (small for control-plane peers, large for sideband-bridge peers per ADR 0009)
- `x86_dev.toml` — UDS under `/run/robot/`
- `hil.toml` / `sim_cloud.toml` — UDP loopback ports

Document mapping in `docs/deployment-profiles.md`.

### F2 — Python bridge example

`examples/bridges/python_peer/`:

- Minimal subscriber/publisher matching the v2 frame layout (ctypes `struct` or small C extension); fields: `source`, `flags`, `topic_id`, `seq`, `timestamp_ns`, `sideband_idx`, `sideband_len` (uint48), `sideband_seq`, 32 B payload — all host little-endian (ADR 0008)
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

- CSI vs V4L: separate capture binary; router frames carry the v2 surface — `topic_id` (per camera / per stream), `seq`, `timestamp_ns`, and the in-frame sideband descriptor (`sideband_idx`, `sideband_seq`, `sideband_len`) per ADR 0008
- Sideband SHM name convention documented (ties to ADR 0005)
- **`[[peers.sideband]] memory_class`** field lands here (ADR 0008 forward declaration realized): `shm` | `cuda_managed` | `cuda_host` | `nvbufsurface`, with optional `cuda_device`. Subscribers pick the right access path (zero-copy borrow / DMA mapping / memcpy) by reading the topology entry indexed by the frame's `sideband_idx`

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
- Put JPEG / NV12 / point clouds / model tensors in the inline payload (`kRouterPayloadSize = 32 B`) — they belong in a sideband region referenced by the v2 frame's `sideband_idx` / `sideband_seq` / `sideband_len`

## Session prompt

```
Execute Phase F from robotics-ipc-module/plans/F-interoperability-bridges.md.
Read SYSTEM-VISION.md and DESIGN-PRINCIPLES.md. Update STATUS.md.
```
