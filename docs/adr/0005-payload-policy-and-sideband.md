# ADR 0005: Payload policy and sideband regions

- **Status:** Accepted (active); refined by [ADR 0008](0008-router-frame-v2.md) which adds an in-frame sideband descriptor (`sideband_idx`, `sideband_len`, `sideband_seq`) so subscribers no longer need out-of-band region resolution
- **Date:** 2026-05-25
- **Builds on:** [ADR 0001](0001-ipc-and-router.md), [ADR 0004](0004-robotics-module-boundaries.md)
- **Scope:** What goes in `RouterFrame` vs out-of-band; naming, layout, and lifecycle rules for sideband regions; minimal in-tree API (`ipc/src/router/sideband.hpp`).

> **Refinement:** the original ADR text below was written against the
> v1 frame (22 B payload, no in-frame sideband descriptor). With
> [ADR 0008](0008-router-frame-v2.md) the v2 frame carries
> `sideband_idx`, `sideband_len`, and `sideband_seq` directly, plus a
> `has_sideband` flag bit. The publisher → subscriber contract is now:
>
> 1. Publisher writes bulk bytes to its sideband region at slot N.
> 2. Publisher emits a `RouterFrame` with `has_sideband=1`,
>    `sideband_idx = <region index>`, `sideband_seq = N`,
>    `sideband_len = <bytes>`.
> 3. Subscriber reads the frame, indexes
>    `topology.peers[source].sideband[sideband_idx]` to find the region
>    name and (Phase F) memory class, then pulls/borrows the bytes.
>
> The naming convention, `SidebandHeader` layout, and lifecycle rules in
> the body of this ADR are unchanged. The Phase F `memory_class` TOML
> field is forward-declared in ADR 0008.

## Context

ADR 0004 froze `RouterFrame` at version 1: 32 bytes wire-side, 22 byte
payload window after the source / timestamp header. That window is sized
for **control plane** traffic — small scalars, command acks, sensor digests,
frame metadata — and is intentionally too small for the data plane the
robotics stack actually needs:

- Vision peers must move **NV12 / JPEG / depth frames** between the camera
  pipeline (CSI / V4L / GStreamer) and downstream consumers (recorder, ML).
  Even one 1080p NV12 frame is ~3 MB.
- ML inference peers exchange **tensors** (input batches, output logits, KV
  caches) that range from kilobytes to tens of MB per inference.
- The MAVLink gateway may need to forward **MAVLink message batches** that
  exceed the 22 B payload window (parameter dumps, mission uploads).

If the module tried to stretch `RouterFrame` to carry these, every change
would be a wire-format break, every Jetson SHM ring would burn cache for
a worst-case-sized slot, and the router's hot path would have to learn to
fragment / reassemble. [SYSTEM-VISION.md](../../robotics-ipc-module/SYSTEM-VISION.md)
already commits us to a different model: cameras and ML peers publish
*metadata* through the router and the *bulk bytes* through a separate
named region per producer.

Phase B (this ADR + B2 topology loader) is when we write down what
"sideband region" means so peers can rely on a fixed contract.

## Decision

### 1. Two planes, separated by name and lifetime

The module distinguishes:

| Plane | Carrier | Owner | Size policy |
|-------|---------|-------|-------------|
| **Control plane** | `RouterFrame` (32 B wire, 22 B payload) over UDP / UDS / SHM SPSC | Router server | Frozen at v1 ([ADR 0004](0004-robotics-module-boundaries.md)). Used for commands, status, scalar telemetry, references *to* sideband data (frame_id, sequence). |
| **Data plane** | Named **sideband regions** (typically `/dev/shm/<name>`; may be filesystem path, DMA-BUF, GPU allocator handle, etc.) | The *producer peer* (camera, ML inference, MAVLink gateway) — **not** the router | Producer-declared; consumers read up to the descriptor's `max_payload_bytes`. |

The router never opens, reads, writes, or unlinks sideband regions. It only
forwards 32 B control frames. Consumers and producers agree on sideband
names through topology / config data (`SidebandRegion` descriptor), and
synchronize on a small in-region header.

### 2. Naming convention

Sideband regions follow:

```text
/robot_<peer>_<class>
```

Examples:

| Peer (SYSTEM-VISION ID) | Class | Region name |
|-------------------------|-------|-------------|
| `vision` (4)            | NV12 frames    | `/robot_vision_nv12` |
| `vision` (4)            | JPEG frames    | `/robot_vision_jpeg` |
| `ml_inference` (5)      | model input    | `/robot_ml_tensor_in` |
| `ml_inference` (5)      | model output   | `/robot_ml_tensor_out` |
| `mavlink_gateway` (6)   | bulk MAVLink   | `/robot_mavlink_bulk` |

Rules:

- Leading `/` required for shm_open()-style regions.
- ASCII only; lowercase `[a-z0-9_]`.
- Length ≤ 250 bytes (`kSidebandNameMaxLen` in `ipc/src/router/sideband.hpp`;
  bounded conservatively below the Linux NAME_MAX).
- Class strings: free-form, but the convention constants in
  `sideband.hpp` (`kSidebandClassVisionNv12`, `kSidebandClassMlInput`,
  …) are the names the module *expects* to see in field deployments.

Non-SHM sideband carriers (filesystem paths, DMA-BUF, vendor allocators)
follow the same `<peer>_<class>` discipline but may live under different
roots (e.g. `/run/robot/vision_dump_<frame_id>.bin`). Topology loader
validation (B2) checks length and ASCII only — it does not enforce the
`/robot_*` prefix because non-SHM carriers legitimately deviate.

### 3. In-region header (`SidebandHeader`)

Every producer writes a fixed 16 B header at offset 0 of the region. The
layout is **frozen at v1** alongside `RouterFrame` v1:

| Offset | Size | Field            | Notes |
|--------|------|------------------|-------|
| 0..3   | 4    | magic            | `'R' 'S' 'B' '1'` |
| 4..7   | 4    | version          | host-endian uint32; `kSidebandVersion = 1` |
| 8..15  | 8    | payload_bytes    | host-endian uint64; bytes after the header |

Consumers MUST validate magic, version, and `payload_bytes ≤
SidebandRegion::max_payload_bytes` before reading past offset 16. The
producer-side helper `sideband_header_init()` and consumer-side
`sideband_header_is_valid()` are inline in `sideband.hpp`.

The header is host-endian on purpose: sideband regions are host-local
(SHM, DMA-BUF). Cross-host transfers go through the UDP control plane plus
a transport-layer file/object store, not a stretched RouterFrame.

### 4. Lifecycle

- **Producer creates and owns.** The peer that produces the bulk data
  (camera process, ML inference process, MAVLink gateway) calls
  `shm_open(..., O_CREAT | O_RDWR, ...)`, sizes the region via `ftruncate`,
  writes the `SidebandHeader`, and `shm_unlink`s on shutdown. The router
  has no role.
- **Consumers join read-only.** Recorder, dashboard bridge, ML training
  Python peer, etc., call `shm_open(..., O_RDONLY)` + `mmap(..., PROT_READ,
  ...)`. They never modify the header.
- **Crash recovery.** If the producer is SIGKILLed, the region stays in
  `/dev/shm` until reboot or until a new producer instance unlinks-then-
  recreates. Producers SHOULD unlink-before-create on startup (mirrors the
  UDS `unlink` rule in [LESSONS-LEARNED.md](../../robotics-ipc-module/LESSONS-LEARNED.md)).
- **Synchronization with the control plane.** The producer publishes a
  control frame referencing the sideband region (typically by `frame_id`
  in bytes 10..13 of the 22 B payload, definition deferred to per-peer
  ADR). Consumers wait for that control frame, then read the region.
  Producers MUST write payload bytes *before* the announcing control
  frame.

### 5. Minimum in-tree API

`ipc/src/router/sideband.hpp` (header-only) ships:

```cpp
struct SidebandHeader { char magic[4]; uint32_t version; uint64_t payload_bytes; };
constexpr uint32_t kSidebandVersion = 1;
constexpr size_t   kSidebandNameMaxLen = 250;
constexpr const char* kSidebandClassVisionNv12  = "vision_nv12";
// ... (vision_jpeg, ml_tensor_in, ml_tensor_out, mavlink_bulk)

struct SidebandRegion {
    const char* name;
    size_t      max_payload_bytes;
    uint32_t    version;
};

inline void sideband_header_init(SidebandHeader&, uint64_t payload_bytes);
inline bool sideband_header_is_valid(const SidebandHeader&, const SidebandRegion&);
```

That is the entire module-level surface. Open / map / fd-passing /
DMA-BUF handling lives in each producer/consumer peer — not in `ipc/src/`.
This keeps the module focused on routing and lets bridges pick the right
allocator (host SHM, NVMM, vendor SDK) for their hardware.

### 6. Topology integration (Phase B2)

The Phase B topology loader allows per-peer `[[peer.sideband]]` entries:

```toml
[[peers]]
id = 4
name = "vision"
local = "shm:/robot_vision_router"

[[peers.sideband]]
class = "vision_nv12"
name  = "/robot_vision_nv12"
max_payload_bytes = 8388608   # 8 MB
```

The loader validates name length / ASCII and exposes `SidebandRegion`
descriptors next to the `PeerEntry`. Producers use these to know what
region to create; consumers use them to know what region to open.

The B2 loader treats sideband entries as **optional** and validates them
on a per-peer basis. Peers without sideband entries continue to use
control-plane-only messaging exactly as today.

## Consequences

### Positive

- `RouterFrame` stays frozen at v1. No pressure to grow the wire format
  every time a new sensor or ML model lands.
- Each producer picks the right allocator for its hardware (host SHM on
  x86 / Jetson host buffers, NVMM / DMA-BUF for Jetson camera-to-GPU
  pipelines, etc.) without the module having to know.
- Consumers have a single, ADR-pinned validation path (`magic` + `version`
  + `max_payload_bytes`). Mis-signed buffers are caught at the boundary.
- B2 topology loader gets a small, well-typed extension point (the
  `[[peer.sideband]]` table) without bloating the schema for peers that
  don't need it.

### Negative

- Two-plane synchronization (control frame must follow payload write) is
  a new failure mode peers have to handle. Existing single-plane demos
  don't hit it; bridges in Phase F will.
- The router gains no diagnostic visibility into sideband regions. If a
  producer crashes and the consumer is waiting on a stale `frame_id`,
  detection requires a separate liveness signal (out of scope — Phase D
  metrics may add one).
- `SidebandHeader` adds 16 B per region. For tiny sideband payloads
  (<1 KB) this is meaningful overhead; producers may opt out by
  documenting `version = 0` in the descriptor and skipping the header
  (escape hatch for embedded MCU peers, not the documented default).

### Neutral

- The `kSidebandClass*` constants are *conventions*, not enforcement.
  Field deployments can use other class names; the topology loader does
  not whitelist them.

## Alternatives considered

1. **Extend `RouterFrame` to 1 KB / 4 KB.** Rejected — re-opens the
   `kRouterFrameVersion` contract, doubles SHM ring memory, and still
   doesn't fit camera frames. The 32 B size was chosen specifically so
   that SPSC rings stay cache-friendly on Jetson.
2. **Fragment large payloads across multiple RouterFrames.** Rejected —
   adds reassembly state to the router hot path (forbidden by
   [DESIGN-PRINCIPLES.md](../../robotics-ipc-module/DESIGN-PRINCIPLES.md)
   "no per-message std::string in forward path"), and reordering / loss
   on UDP profiles makes reliable reassembly an entirely separate ADR.
3. **Make sideband a router-managed resource.** Rejected — turns the
   module into a buffer pool, which it explicitly is not (ADR 0004 out-
   of-scope list). Vendor allocators (NVMM, DMA-BUF, CUDA host-pinned)
   are too varied to abstract here.
4. **Use ZeroMQ / nanomsg for the data plane.** Rejected — pulls in a
   compiled dependency for every bridge process, and the existing SHM
   SPSC transport already covers the host-local case. ZMQ may
   legitimately appear inside a Python bridge (Phase F decision), but
   not in `ipc/src/`.
5. **Versioning via separate "v1 magic" / "v2 magic" rather than a
   numeric version field.** Rejected — `version` as a uint32 lets
   consumers compare ranges (`if (h.version < 2) ...`) and is friendlier
   to forward-compat windows; magic stays constant.

## Verification

```bash
make all
grep -q 'kSidebandVersion' ipc/src/router/sideband.hpp
test -f docs/adr/0005-payload-policy-and-sideband.md
# Loader test (B2) exercises sideband descriptor parsing once it lands.
```
