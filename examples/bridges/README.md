# Bridges — peer processes outside the C++ core

This directory holds **bridge processes** that interoperate with the C++ router from another language or domain (Python, Node, MAVLink-over-serial, GStreamer / V4L2 video). Each bridge is a **separate process** that speaks the [`RouterFrame` v2 wire layout](../../docs/adr/0008-router-frame-v2.md); the C++ router has no language-specific headers or bridge code in its include graph.

> **Status today:** This directory is **scaffolding** created in Phase E E3. Each subdirectory currently contains only a README pointing at its Phase F deliverable. **No bridge code exists yet** — the actual implementations land in Phase F (F2 – F5) per [`robotics-ipc-module/plans/F-interoperability-bridges.md`](../../robotics-ipc-module/plans/F-interoperability-bridges.md).

## Bridges in this tree

| Directory | Peer ID | Bridge | Status | Phase F deliverable |
|-----------|---------|--------|--------|---------------------|
| [`python_peer/`](python_peer/) | 7 | Python subscriber/publisher matching the v2 frame layout | **Sketch only** | F2 |
| [`node_gateway/`](node_gateway/) | 8 | UDS → WebSocket gateway for browser dashboards | **Sketch only** | F3 |
| [`mavlink_gateway/`](mavlink_gateway/) | 6 | Serial / MAVLink ↔ router status frames | **Sketch only** | F4 |
| [`vision_peer/`](vision_peer/) | 4 | CSI / V4L2 vision metadata + sideband NV12 / JPEG | **Sketch only** | F5 |

## Principles (from [ADR 0004](../../docs/adr/0004-robotics-module-boundaries.md))

These are **inviolable**. They are the reason bridges live here and not in `ipc/src/`:

- **No language-specific headers in `libipc`.** No `Python.h`, no `node.h`, no `mavlink.h`, no GStreamer / V4L2 headers in the C++ core. Each bridge depends on its own runtime; `libipc` stays header-only C++20 with no extra system deps beyond `-lrt` and `-pthread`.
- **Same wire format as C++ peers.** Bridges reimplement (or ctypes-bind) the 64 B [`RouterFrame` v2](../../docs/adr/0008-router-frame-v2.md) layout. Host little-endian, 32 B inline payload, in-frame sideband descriptor (`sideband_idx` / `sideband_seq` / `sideband_len`).
- **Same peer IDs and route rules as every other profile.** The [peer catalog](../../docs/robotics-reference-layout.md#peer-catalog) is stable across profiles — bridges plug into the existing IDs (4 / 6 / 7 / 8).
- **Bulk data through sidebands, not inline payload.** Images, tensors, point clouds, encoded video — none of these go in the 32 B inline payload. They go in a [sideband region](../../docs/adr/0005-payload-policy-and-sideband.md) referenced by the in-frame descriptor.
- **No ROS in the C++ core unless a separate ADR adds it.** ROS 2 bridges (rclpy / rclcpp wrappers) are a *separate* project decision; today this module is ROS-free by design.

## How bridges connect to the router

Bridges are clients of the router server, using one of the transports the router supports. Recommended transport per deployment:

| Deployment | Bridge transport | Resource path / port |
|------------|------------------|----------------------|
| x86 dev | UDS | `/tmp/rim_router.sock` |
| Jetson on-robot | SHM | `/dev/shm/rim_router_<peer>` (all 6 F1 peers on SHM — see [docs/deployment-profiles.md §Known limitations](../../docs/deployment-profiles.md#known-limitations) for why Node-style bridges currently need a native SHM addon or a separate UDS bridge daemon) |
| HIL bench | UDP loopback | `127.0.0.1:19100` (ports 19101–19108 per peer; see [hil.toml](../../config/profiles/hil.toml)) |
| Sim / CI cloud | UDP | container subnet per `sim_cloud.toml` |

For the operator-facing breakdown — selector, per-profile shape, route topology, known limitations — see [`docs/deployment-profiles.md`](../../docs/deployment-profiles.md) (Phase F F1). For per-bridge contract details (which `RouterFrame` fields a Python peer reads / writes, sideband region layout for vision, etc.), see the **Integration patterns** section in [`docs/robotics-reference-layout.md`](../../docs/robotics-reference-layout.md#integration-patterns) — that's the authoritative contract surface, written in Phase E E1.

## Layout convention (Phase F target)

When Phase F lands the bridge code, each subdirectory will follow this convention:

```
examples/bridges/<bridge>/
├── README.md              # how to build + run; ABI / wire layout notes
├── frame_layout.<ext>     # language-specific port of RouterFrame v2 (ctypes / TS interface / struct)
├── <source files>         # the bridge implementation
└── Makefile or build.{sh,toml}   # standalone build, NOT wired into the top-level Makefile
```

The top-level `make all` builds **only** the C++ core. Bridge builds are intentionally separate so a missing Python / Node / GStreamer toolchain does not break the C++ workflow on a fresh Jetson or CI runner.

## Why this scaffolding exists in E3 and not F

Phase E ships the **deployment contract** (peer catalog, frame fields, sideband regions, resource paths). The bridges are the customers of that contract. Reserving these directory names in E3 — with stub READMEs pointing at the Phase F plan — keeps the layout discoverable without committing to a single implementation language or library version. The Phase F session can drop in the actual code without renegotiating the directory shape.

## Related documents

- [docs/deployment-profiles.md](../../docs/deployment-profiles.md) — Phase F F1 operator-facing companion to `config/profiles/` (selector + per-profile shape + 2-dest-cap and single-transport-per-router limitations)
- [docs/robotics-reference-layout.md](../../docs/robotics-reference-layout.md) — Phase E reference layout (peer catalog + per-bridge integration patterns + RouterFrame field contract)
- [robotics-ipc-module/plans/F-interoperability-bridges.md](../../robotics-ipc-module/plans/F-interoperability-bridges.md) — Phase F deliverables F1 – F5
- [docs/adr/0004-robotics-module-boundaries.md](../../docs/adr/0004-robotics-module-boundaries.md) — module-boundary policy (what's in libipc vs in `examples/bridges/`)
- [docs/adr/0005-payload-policy-and-sideband.md](../../docs/adr/0005-payload-policy-and-sideband.md) — sideband classes (`vision_nv12`, `vision_jpeg`, `ml_tensor_in`, `ml_tensor_out`, `mavlink_bulk`)
- [docs/adr/0008-router-frame-v2.md](../../docs/adr/0008-router-frame-v2.md) — wire layout that every bridge must mirror
- [robotics-ipc-module/plans/post-phases-robotics-review.md](../../robotics-ipc-module/plans/post-phases-robotics-review.md) — open considerations parked for after Phase F closes (TensorRT contract depth, CUDA `memory_class`, replay peer, etc.)
