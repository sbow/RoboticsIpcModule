# `vision_peer` — Phase F F5

> **Status:** Interface + realized `memory_class` parser. The control-plane contract (metadata frame fields), the sideband region convention, and the **`[[peers.sideband]] memory_class` parser** ([ADR 0012](../../../docs/adr/0012-sideband-memory-class.md)) are shipped and tested. **No capture binary in this directory yet** — the camera-side code needs Argus / V4L2 / CUDA SDKs that stay out of the IPC core, so the working binary is operator/downstream code built against this contract.

A separate process that captures from CSI cameras (Jetson NVMM / nvargus) or V4L2 USB cameras, stages each frame into a sideband region, and publishes a metadata-only [`RouterFrame` v2](../../../docs/adr/0008-router-frame-v2.md) on the control plane. The router stays unaware of pixel data — it forwards the 64 B metadata frame and never touches the sideband region.

**Peer ID:** 4 (`vision_capture`)

**Transport (control plane):** whatever the profile gives peer 4 — UDS on `x86_dev.toml`, SHM on `jetson_prod.toml`. The **bulk pixel data does not ride the control plane**; it lives in a separate sideband SHM / DMA-buf region (ADR 0005), referenced by the frame's `sideband_idx` / `sideband_seq`.

## What F5 actually ships (and what it doesn't)

| Piece | State | Where |
|-------|-------|-------|
| `memory_class` + `cuda_device` on `[[peers.sideband]]` | **Shipped + tested** | [`sideband.hpp`](../../../ipc/src/router/sideband.hpp), [`topology_loader.hpp`](../../../ipc/src/router/topology_loader.hpp), [`topology_loader_test.cpp`](../../../ipc/test/topology_loader_test.cpp) |
| Access-path semantics per class | **Documented** | [ADR 0012](../../../docs/adr/0012-sideband-memory-class.md) + [table below](#sideband-memory_class--access-path) |
| Sideband regions in profiles (vision + ML) | **Shipped** | `jetson_prod.toml` (GPU classes), `x86_dev.toml` (shm) |
| Capture binary (Argus / V4L2 → sideband) | **Deferred** | downstream; needs camera SDKs the core excludes (ADR 0004) |
| GPU mapping code (`cudaImportExternalMemory`, etc.) | **Deferred** | the bridge process, not the IPC core |

The thing that "lands" in F5 — per the [F plan](../../../robotics-ipc-module/plans/F-interoperability-bridges.md#f5--vision-metadata-peer-sketch) — is the **`memory_class` parser**, realizing the [ADR 0008](../../../docs/adr/0008-router-frame-v2.md) forward declaration. That's real, tested code. The camera capture loop is downstream because it pulls in Argus / V4L2 / CUDA, which [ADR 0004](../../../docs/adr/0004-robotics-module-boundaries.md) keeps out of `ipc/src/`.

## Control-plane frame contract

The capture process publishes one metadata `RouterFrame` per captured frame. The 32 B inline payload carries compact metadata; the pixels are in the sideband region.

| Field | Meaning for vision |
|-------|--------------------|
| `source` | Rewritten to `4` by the router on forward. |
| `flags` | `has_sideband` (bit 0) **set** — every vision frame references a sideband region. |
| `topic_id` | Per camera / per stream (e.g. `300` = `vision_frame` front camera; allocate more for stereo / multi-cam). |
| `seq` | Monotonic capture counter — lets a consumer detect dropped frames. |
| `timestamp_ns` | Router-stamped on forward ([ADR 0010](../../../docs/adr/0010-router-timestamp-clock.md)); the bridge may also stamp capture time (SOF) inside the inline payload. |
| `sideband_idx` | Index into **peer 4's** `[[peers.sideband]]` table — selects *which* region (and thus its `memory_class`). `0` = the `vision_nv12` region in both shipped profiles. |
| `sideband_seq` | Slot index within the sideband region — *which frame* in the ring the consumer should read. |
| `sideband_len` | Byte length of the staged frame (u48; one 1080p NV12 frame ≈ 3.1 MB). |
| `payload` (32 B) | Compact metadata: `u16 width`, `u16 height`, `u8 pixfmt`, `u8 plane_count`, `u32 stride`, `u64 capture_ns`, `u32 exposure_us`, `u16 gain`, `u16 drop_count`. Schema lives in the bridge's `frame_layout.h`. |

A subscriber's read sequence:

1. Receive the metadata frame; check `has_sideband()`.
2. Look up the producer peer's sideband table at `sideband_idx` → get the `SidebandRegion` (`name`, `max_payload_bytes`, **`memory_class`**, `cuda_device`).
3. Pick the access path from `memory_class` (table below).
4. Read slot `sideband_seq`, validate the `SidebandHeader` (magic + version + `payload_bytes ≤ max_payload_bytes`), consume `sideband_len` bytes.

## Sideband `memory_class` → access path

Realized in [ADR 0012](../../../docs/adr/0012-sideband-memory-class.md). The producer and consumer agree on the access strategy via the topology entry — **not** the frame.

| `memory_class` | Backing | Consumer access path | `cuda_device` |
|----------------|---------|----------------------|---------------|
| `shm` (default) | `shm_open` + `mmap` | `mmap` the named region read-only; read after validating `SidebandHeader` | n/a (rejected by loader) |
| `cuda_managed` | `cudaMallocManaged` | dereference the managed pointer directly (CPU or GPU); pages migrate on access | optional device hint |
| `cuda_host` | `cudaHostAlloc` (pinned) | read the pinned host pointer; DMA / copy engines move it without a bounce buffer | optional device hint |
| `nvbufsurface` | `NvBufSurface` / DMA-buf | GPU: `cudaImportExternalMemory` the DMA-buf FD (zero-copy); CPU: read the surface's mapped CPU pointer | iGPU ordinal (0 on Orin) |

**The same bridge binary runs on dev and prod** — it reads `region.memory_class` at startup and picks the path. That's the entire point of recording the class in topology: see the two profiles below.

## Profile shape (shipped)

**`jetson_prod.toml`** — production, GPU-backed:

```toml
[[peers]]
id              = 4
name            = "vision_capture"
local           = "shm:/rim_router_vision_capture"
shm_slot_count  = 256
shm_max_payload = 64                 # control plane is metadata; pixels are sideband

  [[peers.sideband]]
  class             = "vision_nv12"
  name              = "/rim_vision_nv12"
  max_payload_bytes = 8388608        # 8 MB — one 1080p NV12 frame
  memory_class      = "nvbufsurface" # Argus / V4L2 DMA-buf
  cuda_device       = 0              # integrated GPU
```

**`x86_dev.toml`** — dev laptop, no iGPU, same logical region on the CPU path:

```toml
[[peers]]
id    = 4
name  = "vision_capture"
local = "uds:/tmp/rim_router_vision_capture.sock"

  [[peers.sideband]]
  class             = "vision_nv12"
  name              = "/rim_vision_nv12"
  max_payload_bytes = 8388608
  memory_class      = "shm"          # CPU staging — no NvBufSurface on x86
```

`ml_inference` (peer 5) follows the same pattern with `cuda_managed` tensors on Jetson / `shm` on x86 — see both profiles.

## CSI vs V4L2 (capture binary, downstream)

Two builds, one publish path. The capture front-end differs; everything from "stage frame into sideband + publish metadata" onward is identical.

```
   CSI (Jetson)                          V4L2 (USB / x86)
┌──────────────┐                       ┌──────────────┐
│ nvargus /    │  NvBufSurface         │ /dev/video*  │  mmap'd V4L2
│ Argus API    │  (DMA-buf)            │ VIDIOC_DQBUF │  buffers
└──────┬───────┘                       └──────┬───────┘
       │  acquire frame                       │  dequeue buffer
       ▼                                      ▼
┌─────────────────────────────────────────────────────────┐
│ vision_capture bridge (peer 4)                            │
│  - stage pixels into the sideband region named in topology│
│    (NvBufSurface on Jetson, /dev/shm region on x86)       │
│  - pack metadata into the 32 B inline payload             │
│  - set has_sideband, sideband_idx, sideband_seq,          │
│    sideband_len; publish RouterFrame to the router        │
└──────────────────────────┬────────────────────────────────┘
                           │  metadata RouterFrame (64 B)
                           ▼
                    router → ml_inference (5), recorder (3), dashboard (8)
                             each reads the sideband by memory_class
```

The CSI build links Argus / NvBufSurface; the V4L2 build links plain V4L2. **Neither links into `ipc/src/`** — they're separate binaries that include the header-only router client + `sideband.hpp` for the `SidebandRegion` / `memory_class` vocabulary.

## Open considerations parked for revisit

This bridge intersects multiple items in the [post-phases robotics-integration review](../../../robotics-ipc-module/plans/post-phases-robotics-review.md):

- [C1 — TensorRT integration contract](../../../robotics-ipc-module/plans/post-phases-robotics-review.md#c1--tensorrt-integration-contract): the vision peer feeds `ml_inference`; the `cuda_managed` tensor regions in `jetson_prod.toml` are the handoff surface. Still parked (needs the real TensorRT engine).
- [C2 — CUDA / sideband `memory_class` parsing](../../../robotics-ipc-module/plans/post-phases-robotics-review.md#c2--cuda--sideband-memory_class-parsing): **loader portion closed by [ADR 0012](../../../docs/adr/0012-sideband-memory-class.md)**; the GPU mapping code stays in the (deferred) bridge.
- [C9 — Camera / GStreamer integration shape](../../../robotics-ipc-module/plans/post-phases-robotics-review.md#c9--camera--gstreamer--v4l2-integration-shape): this directory is precisely C9; the capture binary is the deferred part.

## Boundary (do not violate)

Per [ADR 0004](../../../docs/adr/0004-robotics-module-boundaries.md), [ADR 0012](../../../docs/adr/0012-sideband-memory-class.md), and the [bridges index](../README.md):

- **No GStreamer / V4L2 / NVMM / CUDA headers in `ipc/src/`.** Camera capture and GPU mapping stay inside this bridge process. The core records `memory_class` as an opaque enum tag; it never links a GPU SDK.
- **No pixel data in the 32 B inline payload.** Pixels go in a sideband region; the inline payload carries compact metadata (width, height, exposure, drop counter).
- **The router does not own the sideband region's contents.** This bridge owns the lifecycle of `/dev/shm/rim_vision_*` (create on start, populate on each frame, optional cleanup on stop). The router's `ExecStopPost` cleanup script [`rim-router-cleanup.sh`](../../../robotics-ipc-module/deploy/systemd/rim-router-cleanup.sh) proactively `rm -f`s the anticipated names so the next deployment starts clean.
- **`memory_class` is a deployment-time contract, not a per-frame field.** A region's class is fixed in topology; never try to carry it in the frame (ADR 0008 / 0012).

## Related documents

- [ADR 0012 — Sideband `memory_class`](../../../docs/adr/0012-sideband-memory-class.md) — **the** record for the parser this bridge relies on
- [ADR 0005 — Payload policy and sideband](../../../docs/adr/0005-payload-policy-and-sideband.md) — region convention, `SidebandHeader`, `vision_nv12` / `vision_jpeg` classes
- [ADR 0008 — RouterFrame v2](../../../docs/adr/0008-router-frame-v2.md) — `sideband_idx` / `sideband_seq` / `sideband_len`; the forward declaration F5 realizes
- [ADR 0004 — Robotics module boundaries](../../../docs/adr/0004-robotics-module-boundaries.md) — no vendor SDKs in the core
- [ADR 0010 — Router timestamp clock](../../../docs/adr/0010-router-timestamp-clock.md) — `timestamp_ns` semantics
- [Phase F plan F5](../../../robotics-ipc-module/plans/F-interoperability-bridges.md#f5--vision-metadata-peer-sketch)
- [`docs/robotics-reference-layout.md`](../../../docs/robotics-reference-layout.md#vision-capture-vision_capture-peer-4) — peer-4 integration pattern
- [`docs/deployment-profiles.md`](../../../docs/deployment-profiles.md) — profile selector + sideband region table
- [`examples/bridges/mavlink_gateway/README.md`](../mavlink_gateway/README.md) — F4 sibling device bridge ([ADR 0011](../../../docs/adr/0011-device-bridge-transports.md))
