# ADR 0012: Sideband `memory_class` — realizing the ADR 0008 forward declaration

- **Status:** Accepted
- **Date:** 2026-05-31
- **Builds on:** [ADR 0004](0004-robotics-module-boundaries.md) ("the router transports; neighbours interpret"; no CUDA / vendor SDKs in `ipc/src/`), [ADR 0005](0005-payload-policy-and-sideband.md) (sideband regions carry bulk data; the inline 32 B payload carries metadata), [ADR 0008](0008-router-frame-v2.md) (the v2 frame's `sideband_idx` indexes a per-peer topology table; `memory_class` was **forward-declared** there and explicitly deferred to Phase F)
- **Closes:** [Phase F F5](../../robotics-ipc-module/plans/F-interoperability-bridges.md#f5--vision-metadata-peer-sketch) (the `memory_class` parser deliverable) and the loader portion of [post-phases review C2 — CUDA / sideband `memory_class` parsing](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c2--cuda--sideband-memory_class-parsing)
- **Scope:** Add a `memory_class` (+ optional `cuda_device`) field to `[[peers.sideband]]`, parse it in `topology_loader.hpp`, surface it on `SidebandRegion`, and define the consumer-side access-path semantics each class implies. **No CUDA / NvBufSurface headers enter the IPC core** — the loader records a tag; only bridge / consumer processes link the GPU SDKs. No wire-format change; no router hot-path change.

## Context

[ADR 0008](0008-router-frame-v2.md) carved a `sideband_idx` field into the 64 B `RouterFrame` v2 and said it indexes "the source peer's topology sideband table." It then forward-declared a `memory_class` field on that table entry, with this note:

> This ADR forward-declares that field so the v2 frame's `sideband_idx` has a well-defined meaning all the way to the bulk-byte access path, even though `topology_loader.hpp` does not parse `memory_class` yet. […] The actual parser + sideband.hpp helper change for memory class is deferred to Phase F (it requires CUDA headers at the bridge level, not in the header-only IPC core).

Phase F F5 (the vision metadata peer) is where that deferral comes due. The vision and ML bridges on a Jetson Orin publish bulk data — camera frames, inference tensors — that does **not** live in plain CPU shared memory:

- **Camera frames** come out of Argus / V4L2 as `NvBufSurface` (DMA-buf) handles. A GPU consumer imports them zero-copy; a CPU consumer maps the surface's CPU-side pointer.
- **Inference tensors** want to be in CUDA unified memory (`cudaMallocManaged`) so the TensorRT engine reads them without an explicit host→device copy and the controller can read the result on the CPU side.
- **DMA-friendly staging** sometimes wants pinned host memory (`cudaHostAlloc`) so a copy engine can move it without bounce buffers.

A subscriber holding a `RouterFrame` with `has_sideband()` set needs to know *which* of these its `sideband_idx` points at, because the access code is completely different (`mmap` a CPU pointer vs. `cudaImportExternalMemory` a DMA-buf FD vs. dereference a managed pointer). That "which" is a **deployment-time fact** — it depends on how the host is wired, not on the message — so it belongs in topology, exactly where ADR 0008 said it would.

What was missing until now: the loader threw the field away (the sideband block parsed `name`, `max_payload_bytes`, `version`, ignored everything else). This ADR makes the loader record it.

## Decision

**Add an optional `memory_class` enum field (+ optional `cuda_device`) to `[[peers.sideband]]`, parse and validate it in `topology_loader.hpp`, and expose it on `SidebandRegion`. The IPC core records the tag and links no GPU SDK; consumers read the tag to pick their access path.**

### TOML surface

```toml
[[peers.sideband]]
class             = "vision_nv12"
name              = "/rim_vision_nv12"
max_payload_bytes = 8388608
memory_class      = "nvbufsurface"   # one of: shm, cuda_managed, cuda_host, nvbufsurface
cuda_device       = 0                # optional; GPU-backed classes only
```

- `memory_class` is **optional** and **defaults to `shm`** — so every profile written before this ADR loads with identical behaviour (plain CPU SHM is what a sideband always implicitly was).
- Unknown spellings are a **hard load error**, not a silent fallback. A typo like `cuda_unified` must fail loudly: silently falling back to `shm` would hand a GPU consumer a CPU pointer and crash it (or worse, read garbage) at runtime.
- `cuda_device` is **optional**, an integer `0..255`, and **only valid on a GPU-backed class** (`cuda_managed`, `cuda_host`, `nvbufsurface`). Setting it on a `shm` region is a load error, so an operator can't fool themselves into thinking a CPU region is pinned to a GPU.

### The four classes and their consumer access paths

| `memory_class` | Backing allocation | Producer (bridge) | Consumer access path | `cuda_device` |
|----------------|--------------------|-------------------|----------------------|---------------|
| `shm` (default) | `shm_open` + `mmap` | writes bytes into the mapped region | `mmap` the named region read-only; read after validating `SidebandHeader` | n/a (rejected) |
| `cuda_managed` | `cudaMallocManaged` | writes via the managed pointer | dereference the managed pointer directly (CPU or GPU); page migration is automatic | optional device hint |
| `cuda_host` | `cudaHostAlloc` (pinned) | writes into pinned host memory | read the host pointer; DMA / copy engines move it without a bounce buffer | optional device hint |
| `nvbufsurface` | `NvBufSurface` / DMA-buf | Argus / V4L2 / DeepStream fills the surface | GPU: `cudaImportExternalMemory` the DMA-buf FD (zero-copy); CPU: read the surface's mapped CPU pointer | iGPU ordinal (0 on Orin) |

The `SidebandHeader` (magic + version + `payload_bytes`, [ADR 0005](0005-payload-policy-and-sideband.md)) convention is unchanged and class-independent — every region, CPU or GPU, still starts with the 16 B header so a consumer can validate before reading. `memory_class` only tells the consumer *how to get a pointer to* those bytes.

### Code shape

- **`ipc/src/router/sideband.hpp`** gains:
  - `enum class SidebandMemoryClass : uint8_t { Shm, CudaManaged, CudaHost, NvBufSurface }`
  - `sideband_memory_class_name()` / `parse_sideband_memory_class()` — round-tripping helpers, no allocation, no throw
  - `sideband_memory_class_is_gpu()` — true for everything but `Shm`
  - `kSidebandCudaDeviceUnset = -1`
  - two new fields on `SidebandRegion`: `memory_class` (default `Shm`) and `cuda_device` (default `-1`)
- **`ipc/src/router/topology_loader.hpp`** parses + validates the two fields in the existing `[[peers.sideband]]` loop.
- **No new headers, no new link dependencies.** `<cstring>` (already included) backs the string compares. The enum is a `uint8_t` tag.

## The boundary this protects (ADR 0004)

The crucial line ADR 0008 drew — and the reason this is its own ADR rather than a footnote — is **recording a memory class is not the same as using it.**

- The **IPC core** (header-only, no SDKs) records `memory_class` as an 8-bit enum, validates the TOML, and hands it to whoever calls `sidebands_for()`. It never includes `cuda_runtime.h`, `nvbufsurface.h`, or any vendor header. It cannot allocate, map, or import GPU memory and does not try.
- The **bridge / consumer process** — `vision_capture`, `ml_inference`, a recorder — is where CUDA / NvBufSurface actually gets linked. It reads `region.memory_class` and `region.cuda_device` and calls the matching SDK function. That process already depends on the camera / inference SDK; adding the access path there changes nothing about the core's dependency surface.

This is the same delegation rule [ADR 0010](0010-router-timestamp-clock.md) used for time and [ADR 0011](0011-device-bridge-transports.md) used for device protocols: the core carries an opaque descriptor; the neighbour with the right dependencies interprets it. The forward declaration in ADR 0008 was precisely a promise that this field would land *in topology, not in the frame, and not behind a CUDA dependency in the core* — and this ADR keeps that promise.

## Profiles

- **`jetson_prod.toml`** — the production target — now carries real GPU classes: `vision_nv12` → `nvbufsurface` (`cuda_device = 0`, the integrated GPU); `ml_tensor_in` / `ml_tensor_out` → `cuda_managed` (`cuda_device = 0`).
- **`x86_dev.toml`** — the dev laptop — declares the **same logical sidebands** with `memory_class = "shm"`, because a workstation has no Orin iGPU / NvBufSurface. This is the payoff of putting the class in topology: the *bridge code is identical*; only the deployment file changes which path the consumer takes. (It also makes x86_dev internally consistent — its `[[topics]]` already referenced `sideband_idx` 0 / 1, which now resolve to real per-peer sideband tables.)
- `hil.toml` / `sim_cloud.toml` don't declare vision / ML sidebands, so they're untouched.

## Alternatives considered

### A — Put `memory_class` in the frame instead of topology

Carry the memory class in the `RouterFrame` itself (spend bits in `flags` or steal from a reserved field).

- **Reject reason:** ADR 0008 already settled this — the frame "stays a stupid pointer, the topology stays a deployment-time contract." Memory class is a property of *the region*, fixed at deployment, identical for every frame that points at it. Putting it in the frame would burn hot-path bytes to re-send a constant, and would let two frames disagree about the same region's class (incoherent). Topology is the single source of truth.

### B — Keep it a free-form string (like `class`)

Store `memory_class` as an uninterpreted string, the way the `class` field is "informational; we don't enforce a whitelist."

- **Reject reason:** `class` is a human-facing label with no behavioural contract, so a typo is harmless. `memory_class` *drives code selection* — a consumer branches on it to pick `mmap` vs. `cudaImportExternalMemory`. A silent typo there is a runtime crash or a garbage read. A closed enum with a hard parse error is the correct strictness for a field that selects an access path.

### C — Defer until a real CUDA bridge ships (stay forward-declared)

Leave the loader ignoring the field until `vision_capture` / `ml_inference` are real binaries.

- **Reject reason:** That's the trap the forward declaration was meant to avoid. The *parser* needs no CUDA — it's a string→enum map. Decoupling "record the contract" (loader, header-only, shippable now) from "use the contract" (bridge, needs CUDA, deferred) is exactly the layering ADR 0008 set up. Shipping the loader half now means profiles can express their real intent and the eventual bridge has a parsed field waiting, with tests guarding it.

### D — Add `cuda_device` as mandatory for GPU classes

Require `cuda_device` whenever `memory_class` is GPU-backed.

- **Reject reason:** On a single-GPU host (every Jetson Orin) the device is unambiguous, so forcing the field is noise. Leaving it optional (`-1` = "not pinned") keeps the common case clean while still allowing a multi-GPU x86 inference box to pin a region to a specific ordinal. The loader validates the range when present and rejects it on CPU regions — that's the right amount of strictness.

## Consequences

### Positive

- **The ADR 0008 forward declaration is now real.** `sideband_idx` has a defined meaning all the way to the access path: index → `SidebandRegion` → `memory_class` → access strategy.
- **Same bridge code, different deployment.** A vision bridge written once runs on x86 dev (`shm`) and Jetson prod (`nvbufsurface`) with no code change — it reads the class from topology. This is the whole point of a deployment-time contract.
- **Typos fail at load, not at runtime.** An unknown `memory_class` or a `cuda_device` on a CPU region is caught by `load_topology_from_*` before the router starts, with a specific error message.
- **Zero new dependencies in the core.** The IPC module stays header-only and SDK-free. CUDA lives only where it always had to — in the bridge.
- **Backward compatible.** Omitting `memory_class` yields `shm`, which is the exact behaviour every existing profile already had. No migration needed.

### Negative

- **The core can't validate that a region's bytes actually match its class.** A profile can claim `nvbufsurface` for a region a buggy bridge fills with plain SHM; the loader can't tell. This is inherent — the core doesn't link the SDK that would know. The check is a bridge-level responsibility (validate the DMA-buf FD / managed pointer when mapping).
- **`cuda_device` semantics are advisory in the core.** The core range-checks `0..255` but has no way to know how many GPUs exist. A consumer that reads `cuda_device = 3` on a single-GPU host gets a CUDA error when it tries to use it — correctly, but at runtime.

### Neutral

- **No wire-format change.** `RouterFrame` v2 is byte-identical; the field lives in topology TOML, which is parsed at startup, not on the hot path.
- **No router behaviour change.** The router forwards frames the same way; it never reads `memory_class`. Only end-consumers do.
- **`SidebandRegion` grew by 8 bytes** (a `uint8_t` enum + an `int`, with padding). It's a config-time descriptor, not a hot-path struct; the size is irrelevant.

## Verification

- `make test-ipc-unit` — `topology_loader_test` extended with: parsed GPU values (`nvbufsurface` + `cuda_device`, `cuda_managed` + `cuda_device`); default-to-`shm` when omitted; enum round-trip helper; unknown-spelling error; `cuda_device`-on-`shm` error (both explicit and implicit-default); `cuda_device` out-of-range error; and an assertion that the shipped `jetson_prod.toml` carries the expected GPU classes on peers 4 and 5. 159/159 assertions pass.
- `make all` compiles clean (header-only change; no new link deps).
- `make test-router` UDS / UDP / SHM scenarios unaffected (the router never reads the field).
- Both shipped profiles (`x86_dev.toml`, `jetson_prod.toml`) load without error and demonstrate the CPU and GPU paths respectively.

## References

- [ADR 0004 — Robotics module boundaries](0004-robotics-module-boundaries.md): no vendor SDKs in `ipc/src/`; the rule this ADR's core/bridge split protects
- [ADR 0005 — Payload policy and sideband](0005-payload-policy-and-sideband.md): sideband region convention, `SidebandHeader`, class names
- [ADR 0008 — RouterFrame v2](0008-router-frame-v2.md): forward-declared `memory_class`; this ADR realizes it
- [ADR 0010 — Router timestamp clock](0010-router-timestamp-clock.md): precedent for "core carries an opaque value, neighbour interprets"
- [ADR 0011 — Device-bridge transports](0011-device-bridge-transports.md): the F4 sibling; vision capture (this ADR's driving use case) is a device bridge whose bulk path uses these memory classes
- [Phase F plan F5](../../robotics-ipc-module/plans/F-interoperability-bridges.md#f5--vision-metadata-peer-sketch)
- [Post-phases review C1 — TensorRT integration contract](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c1--tensorrt-integration-contract): the `cuda_managed` ML tensors feed this; still parked
- [Post-phases review C2 — CUDA / sideband `memory_class` parsing](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c2--cuda--sideband-memory_class-parsing): loader portion closed here; the actual GPU mapping code stays in the (deferred) bridge
- [Post-phases review C9 — Camera / GStreamer / V4L2 integration shape](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c9--camera--gstreamer--v4l2-integration-shape): the `vision_peer` bridge is C9; this ADR gives it the memory-class vocabulary
- [`examples/bridges/vision_peer/README.md`](../../examples/bridges/vision_peer/README.md): F5 interface stub, the worked consumer of these classes
