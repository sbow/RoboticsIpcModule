# `vision_peer` — Phase F F5

> **Status:** Sketch only — Phase F F5 deliverable. No code in this directory yet.

A separate process that captures from CSI cameras (Jetson NVMM / nvargus) or V4L2 USB cameras, encodes frames into a sideband SHM region, and publishes a metadata-only [`RouterFrame` v2](../../../docs/adr/0008-router-frame-v2.md) on the control plane. The router stays unaware of pixel data.

**Peer ID:** 4 (`vision_capture`)

## Contract

- [Peer catalog row](../../../docs/robotics-reference-layout.md#peer-catalog) — `vision_capture` peer 4, "metadata only on RouterFrame; NV12 / JPEG via SHM region"
- [Integration pattern — Vision capture](../../../docs/robotics-reference-layout.md#vision-capture-vision_capture-peer-4) — frame field contract (which `topic_id` / `seq` / `timestamp_ns` / `sideband_idx` / `sideband_seq` / `sideband_len` mean for vision), sideband region naming

## Phase F plan

[`robotics-ipc-module/plans/F-interoperability-bridges.md` → F5](../../../robotics-ipc-module/plans/F-interoperability-bridges.md#f5--vision-metadata-peer-sketch) specifies:

- Separate capture binary (CSI vs V4L are different builds; share the same publish path)
- Router frames carry the v2 surface: `topic_id` (per camera / per stream), `seq`, `timestamp_ns`, and the in-frame sideband descriptor (`sideband_idx`, `sideband_seq`, `sideband_len`)
- Sideband SHM name convention documented (ties to [ADR 0005](../../../docs/adr/0005-payload-policy-and-sideband.md): classes `vision_nv12` and `vision_jpeg`)
- **`[[peers.sideband]] memory_class`** parsing lands here (ADR 0008 forward declaration realized): `shm` | `cuda_managed` | `cuda_host` | `nvbufsurface`, with optional `cuda_device`. Subscribers pick the right access path (zero-copy borrow / DMA mapping / `memcpy`) by reading the topology entry indexed by the frame's `sideband_idx`

## Open considerations parked for revisit

This bridge intersects multiple items in the [post-phases robotics-integration review](../../../robotics-ipc-module/plans/post-phases-robotics-review.md):

- [C1 — TensorRT integration contract](../../../robotics-ipc-module/plans/post-phases-robotics-review.md#c1--tensorrt-integration-contract): the vision peer feeds `ml_inference`; the contract surface between the two lives in this bridge's publish path
- [C2 — CUDA / sideband `memory_class` parsing](../../../robotics-ipc-module/plans/post-phases-robotics-review.md#c2--cuda--sideband-memory_class-parsing): F5 is the natural home for the parser
- [C9 — Camera / GStreamer integration shape](../../../robotics-ipc-module/plans/post-phases-robotics-review.md#c9--camera--gstreamer--v4l2-integration-shape): this directory is precisely C9

When Phase F closes, walk these considerations and confirm they are addressed; if not, decide whether to defer or scope into a Phase G.

## Boundary (do not violate)

Per [ADR 0004](../../../docs/adr/0004-robotics-module-boundaries.md) and the [bridges index](../README.md):

- **No GStreamer / V4L2 / NVMM headers in `ipc/src/`.** Camera capture stays inside this bridge process.
- **No pixel data in the 32 B inline payload.** Pixels go in a sideband region; the inline payload carries compact metadata (width, height, exposure, drop counter).
- **The router does not own the sideband region's contents.** This bridge owns lifecycle of `/dev/shm/rim_vision_*` (create on start, populate on each frame, optional cleanup on stop). The router's `ExecStopPost` cleanup script [`rim-router-cleanup.sh`](../../../robotics-ipc-module/deploy/systemd/rim-router-cleanup.sh) proactively `rm -f`s the anticipated names so the next deployment starts clean.
