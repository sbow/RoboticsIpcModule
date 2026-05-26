# ADR 0008: RouterFrame v2 — typed, sequenced, sideband-aware header

- **Status:** Accepted
- **Date:** 2026-05-25
- **Builds on:** [ADR 0001](0001-ipc-and-router.md), [ADR 0004](0004-robotics-module-boundaries.md) (which froze `kRouterFrameVersion = 1` and explicitly anticipated a v2 with a migration ADR), [ADR 0005](0005-payload-policy-and-sideband.md) (sideband regions)
- **Supersedes:** the v1 layout block in ADR 0004 "Frozen v1 wire contract"
- **Scope:** Wire format of `ipc/src/router/frame.hpp`. Defines v2 layout, accessors, byte order, sideband descriptor fields, migration policy from v1. Does **not** change ring-slot sizing (`max_payload`) — that is a separate ADR.

## Context

`RouterFrame` v1 (32 B: 1 source + 9 ts + 22 payload) was originally sized
for raw control-plane throughput — small, cache-friendly, no per-frame
type information. After Phase B (sideband regions, ADR 0005) and Phase C
(per-link metrics, backpressure) shipped, three structural gaps in v1
are now blocking progress on the production peer catalog
([SYSTEM-VISION.md](../../robotics-ipc-module/SYSTEM-VISION.md)):

1. **No topic / message-kind field.** The only "type" the router
   exposes is the source peer id. A peer that wants to publish multiple
   message kinds (sensor publishing IMU + GNSS + battery on one ring)
   either has to run as multiple processes or push type discrimination
   into the 22 B payload — defeating the "zero type-erasure on hot path"
   principle in [DESIGN-PRINCIPLES.md](../../robotics-ipc-module/DESIGN-PRINCIPLES.md).

2. **No sequence number.** Subscriber-side loss / reorder detection is
   impossible. C4 (datagram seq) was deferred precisely because it was
   the wrong field to bolt on outside the frame header. Without a seq,
   the UDP profile in particular can drop frames silently.

3. **No in-frame sideband descriptor.** Today a subscriber must consult
   the topology table to discover *which* of the source peer's sideband
   regions a metadata frame refers to. For Phase F vision (NV12 + JPEG
   + depth from the same camera peer) and ML (input + output tensors
   from the same inference peer), that is ambiguous — multiple
   sidebands per peer require an in-frame descriptor.

The 22 B v1 payload is also below the natural size of common robotics
messages (poses, IMU samples, twists, joint states all exceed 22 B).
A bigger inline scratchpad would absorb most control-plane messages
without forcing them through sideband indirection.

ADR 0004 explicitly anticipated this:

> Any breaking change to layout must bump `kRouterFrameVersion` and ship
> a migration ADR.

The 9-byte timestamp in v1 is also non-standard (a `uint64_t` ns is
8 bytes and covers 584 years from epoch) — a legacy oversight that we
correct here without extra cost.

## Decision

### Frame size: 64 bytes (one cache line)

`kRouterFrameVersion = 2`, `kRouterFrameSize = 64`. Every field is
naturally aligned, the entire frame fits in one 64 B cache line on
x86_64 and aarch64 (the two platforms of intent — see ADR 0004).

Per-frame copy cost on the SHM hot path rises from ~32 B to ~64 B, but
the SHM slot stride (`max_payload = 1024` default in `ShmSpsc`) is
already an order of magnitude larger than v1 frames, so the cache lines
touched per forward are unchanged. Benchmarks: see "Verification" below.

### Layout

```
offset  size  field           type    notes
------  ----  -------------   ------  ----------------------------------------------
 0       1    source          u8      router-stamped on forward; same semantics as v1
 1       1    flags           u8      bit 0 = has_sideband
                                       bit 1 = keyframe / sync point
                                       bit 2 = is_ack (correlated by topic_id + seq)
                                       bit 3 = end_of_stream
                                       bits 4..6 = priority (0=normal..7=critical)
                                       bit 7 = reserved (must be zero on publish)
 2       2    topic_id        u16     publisher-set; subscribers dispatch on this
 4       4    seq             u32     per-source monotonic; wraps; subscriber uses
                                       modular arithmetic for gap detection
 8       8    timestamp_ns    u64     monotonic ns; host byte order (little-endian)
16       2    sideband_idx    u16     index into source peer's topology sideband table;
                                       0xFFFF = kSidebandIdxNone (no sideband for this frame)
18       6    sideband_len    u48     bulk byte length; little-endian 6 bytes; caps at 256 TB
24       8    sideband_seq    u64     slot index / sequence within the sideband region
32      32    payload         u8[32]  inline scratchpad; zero-padded
```

Total: 64 bytes. No reserved bytes (decision: keep payload window
generous; rely on the version bump for any future field).

### Byte order: host (little-endian)

v1's big-endian timestamp was a holdover that paid byte-shuffle cost
in `set_timestamp_ns`/`timestamp_ns` for zero benefit. Every target in
ADR 0004's scope (Linux x86_64, Linux aarch64 Jetson, HIL/sim/cloud
Linux x86_64) is little-endian. UDP profiles run within the same host
arch class.

`frame.hpp` carries a `static_assert(std::endian::native ==
std::endian::little)` to fail closed on any future big-endian port
attempt — the policy is then to add an explicit byte-swap path for
that platform, not to make the canonical path pay for a case that
never occurs in the supported deployments.

### Sideband length: uint48

A `uint48` little-endian length covers up to 256 TB per sideband chunk.
`uint32` (4 GiB cap) is uncomfortable for future point-cloud /
depth-volume peers (raw depth + RGB from a multi-camera rig already
approaches gigabytes per second of bulk data). `uint64` is symmetric
but burns 2 extra bytes for a cap nobody will ever reach.

Access cost: one 6-byte `std::memcpy` to a stack `uint64_t` plus a
mask. About 1 ns; not on the router's hot path (the router never reads
or writes this field — it's a producer/subscriber contract).

### Request/response semantics: single `is_ack` flag bit

A 1-bit `is_ack` flag in `flags` is enough. Correlation between a
request and its response is by `(topic_id, seq)` pair. No dedicated
mode field — that would have required carving 2 bits more deliberately
out of `flags` for an in-tree consumer that does not yet exist (MAVLink
bridge is Phase F).

If Phase F MAVLink work shows we need a richer mode taxonomy, we can
either add another flag bit (`heartbeat`, `cancellation`) or carve a
2-bit mode field — the version bump cost will be the same either way
and we'll have empirical motivation by then.

### Sideband memory_class (forward declaration)

Phase F vision and ML bridges on Jetson will publish into sideband
regions that may be CPU SHM, CUDA-mapped (`cudaMallocManaged`,
`cudaHostAlloc`), or NvBufSurface-backed (DMA-buf integrated with
V4L2 / DeepStream).

The v2 frame's `sideband_idx` references a `[[peers.sideband]]` table
entry in the topology TOML. That entry is the right place to record
the memory class, *not* the frame itself — the frame stays a stupid
pointer, the topology stays a deployment-time contract.

Phase F will add a `memory_class` field to `[[peers.sideband]]`:

```toml
[[peers.sideband]]
class             = "vision_nv12"
name              = "/robot_vision_nv12"
max_payload_bytes = 8388608
memory_class      = "nvbufsurface"   # one of: shm, cuda_managed, cuda_host, nvbufsurface
cuda_device       = 0                # optional, when memory_class = cuda_*
```

This ADR forward-declares that field so the v2 frame's `sideband_idx`
has a well-defined meaning all the way to the bulk-byte access path,
even though `topology_loader.hpp` does not parse `memory_class` yet.
Subscribers that don't care about memory class (recorder, log
forwarder) ignore the field; subscribers that do (GPU consumers, DMA
bridges) read it to pick the right access strategy.

The actual parser + sideband.hpp helper change for memory class is
deferred to Phase F (it requires CUDA headers at the bridge level, not
in the header-only IPC core).

### API on `RouterFrame`

The accessors map directly onto the layout. The hot-path accessors
(`source`, `timestamp_ns`, `set_source`, `set_timestamp_ns`) are
identical in name to v1 so the router code path is unchanged. New
accessors are explicit:

```cpp
constexpr uint16_t kSidebandIdxNone = 0xFFFF;

struct RouterFrame {
    uint8_t bytes[kRouterFrameSize]{};

    void init(uint8_t source_id);

    uint8_t  source() const;        void set_source(uint8_t);
    uint8_t  flags() const;         void set_flags(uint8_t);
    uint16_t topic_id() const;      void set_topic_id(uint16_t);
    uint32_t seq() const;           void set_seq(uint32_t);
    uint64_t timestamp_ns() const;  void set_timestamp_ns(uint64_t);

    uint16_t sideband_idx() const;  void set_sideband_idx(uint16_t);
    uint64_t sideband_len() const;  void set_sideband_len(uint64_t);   // truncates to 48 bits
    uint64_t sideband_seq() const;  void set_sideband_seq(uint64_t);

    bool has_sideband() const;      // helper: flags & 0x01
    bool is_keyframe() const;       // helper: flags & 0x02
    bool is_ack() const;            // helper: flags & 0x04
    bool is_eos() const;            // helper: flags & 0x08
    uint8_t priority() const;       // helper: (flags >> 4) & 0x07

    void set_payload(const void* data, size_t len);
    void set_payload(std::string_view payload);
    std::string payload() const;

    Buffer writable();
    Buffer read_only() const;
};
```

Inline scratchpad is `kRouterPayloadSize = 32 B` (up from v1's 22 B).

## Migration

### Wire incompatibility

v1 and v2 frames cannot coexist on the same SHM ring or socket — both
sides must use the same version. There is **no on-wire negotiation
protocol**. The version is pinned per deployment:

- Module bumps `kRouterFrameVersion` from `1` to `2`. All callers
  recompiled against the new header use v2.
- No staged migration: this is a single-commit, single-deploy change.
- Any external bridge (Python, Node, future MAVLink) reads
  `kRouterFrameVersion` at compile time and is rebuilt against the new
  module.

The justification for "rip the band-aid" rather than maintaining v1+v2
in parallel:

- The project has zero external production consumers today (Phase F
  bridges don't exist yet).
- v1's in-tree consumers (`router_client.cpp`, `router_server.cpp`,
  tests) are all in this repo and updated atomically with the header
  bump.
- Maintaining two parallel formats would require version-tag bytes,
  branch logic in every accessor, and a compatibility shim that adds
  permanent complexity for one migration event.

### v1 history preserved

ADR 0004's "Frozen v1 wire contract" block is preserved as historical
documentation; this ADR (0008) becomes the active contract reference.
ADR 0004's wire-format-versioning section is updated to point readers
here for the current layout.

### Test surface

`ipc/test/frame_test.cpp` (new) verifies every field round-trip, the
literal layout (offsets, size, alignment), and bounds checking on
`set_payload`. Existing tests that incidentally constructed frames
(`router_test`, `last_value_cache_test`, `shm_backpressure_test`) are
updated to the new accessors; the topology loader is unaffected
(topology has no frame dependency).

## Consequences

### Positive

- Multi-message peers become first-class: one process publishes IMU,
  GNSS, battery telemetry on one ring; subscribers dispatch on
  `topic_id`.
- Subscriber-side loss / reorder detection closes Phase C's deferred
  C4; `LastValueCache` can additionally track `last_seq` per source.
- Sideband descriptor in-frame removes the topology lookup ambiguity
  for multi-region peers (multi-camera, multi-tensor).
- Inline payload window 22 B → 32 B fits the common robotics
  control-plane messages (twist, IMU, joint state≤8 floats, pose).
- Per-frame priority hint (3 bits) is now wireable, enabling future
  preemption / QoS heuristics in the router without another version
  bump.
- `is_ack` bit + topic_id + seq form a natural correlation key for
  request/response patterns when Phase F MAVLink lands.

### Negative

- **Single-commit wire break.** Anything that has the v1 layout
  hard-coded (which, today, is just our tests + demos in this repo)
  must be rebuilt simultaneously. No staged rollout possible. This is
  the explicit cost of a stable v2 instead of a forever-compatibility
  stack.
- **+32 B per frame on the wire / per copy.** At our measured 8.5M
  trips/s on SHM, that is ~270 MB/s additional memcpy bandwidth —
  trivially absorbed by L1 store bandwidth (tens of GB/s) but worth
  measuring after the change. See "Verification" below.
- **Big-endian platforms unsupported by canonical path.** Codified
  via `static_assert`. If a future port targets a big-endian box
  (none planned), the static_assert fires at compile time and the
  port author owns the byte-swap layer.

### Neutral

- Sideband length is `uint48`, which has no `<cstdint>` type. Access
  is via 6-byte `std::memcpy` to a `uint64_t` + mask. About 1 ns;
  off the router hot path (router never reads/writes this field).
- `kSidebandIdxNone = 0xFFFF` consumes one of the 65 535 possible
  sideband slots per peer as a sentinel. We do not expect any peer to
  exceed 64 K sideband regions; if some future system does, the
  sentinel can move and the type stays `uint16`.

### Out of scope (deferred)

- **Ring-slot right-sizing.** Today `ShmSpsc::BindParams` defaults to
  `max_payload = 1024` per slot, regardless of frame size. For the
  router fabric this is ~15× larger than needed; right-sizing it to
  `max_payload = 64` per peer (and possibly larger for sideband-bridge
  peers) is a topology-loader extension and its own ADR. Phase D
  candidate.
- **Datagram sequence on UDP.** v2's `seq` field is now in every
  transport, including UDP. UDP-specific reorder handling
  (out-of-order arrival → newest-only filter at subscriber) lives in
  subscriber code, not in the router. Phase D candidate; closes
  Phase C's deferred C4 with the right surface.
- **Sideband memory_class TOML field.** Forward-declared above;
  parsing + access strategy is Phase F.
- **CRC / integrity.** Optional `crc32` over the header could replace
  some of the priority/flag bits in a v3 if we ever ship cross-host
  UDP with hostile networks. Out of scope today.

## Alternatives considered

### A. Stay at 32 B and shrink fields aggressively

- 1 B source + 8 B ts + 1 B topic + 2 B seq + 20 B payload = 32 B.
- Pays the v1 cache-line story but loses the sideband descriptor,
  caps seq at 16-bit (wraps in seconds for high-rate sensors), shrinks
  the payload window further. Rejected — the value of the bigger
  fields exceeds the cost of the second cache line, and v1 is already
  inside a 1024-byte ring slot anyway.

### B. 128 B frame (2 cache lines)

- ~88 B payload window, room for many small messages inline.
- Crosses a cache-line boundary, complicates future atomic ops on
  any field that might straddle the line (none today, but pre-paid
  cost). Doubles memcpy bandwidth on the hot path. Payload window is
  bigger than we need given the sideband design. Rejected — 64 B is
  the right size and 128 B is for "what if we never had sideband",
  which we explicitly do.

### C. Variable-length frame

- Header fixed, payload trails as length-prefixed bytes.
- Breaks the "fixed frames for control" principle in
  DESIGN-PRINCIPLES.md, complicates ring-slot sizing (now every slot
  is worst-case), forces parsing on the hot path. Rejected — the
  point of having sideband is precisely that variable-length bulk
  data lives outside the frame.

### D. Compatibility tag byte + dispatch

- Reserve byte 0 as version, switch on it in accessors.
- Adds permanent complexity for a one-time migration. Today's external
  surface is zero, the cost of "rip the band-aid" is contained.
  Rejected.

## Verification

```bash
# Build the v2 frame and round-trip every field.
make test-frame
# expects: "frame_test: N/N assertions passed" with kRouterFrameVersion==2,
#          kRouterFrameSize==64, every field offset/size as declared.

# Phase B + C tests must keep passing on top of v2.
make test-ipc-unit
# expects: topology_loader_test + last_value_cache_test + shm_backpressure_test
#          + frame_test all green.

# End-to-end transports + benchmarks must keep working.
make test-ipc
# UDP / UDS / SHM trips per second; SHM should remain within ~10% of
# the Phase C number (8.5M trips/5s). A regression below ~7M trips
# would suggest the extra 32 B per frame is unexpectedly expensive.

make test-router
# All three transport scenarios still pass with the v2 wire.
```

Manual byte-layout audit:

```bash
# Decode a frame from the wire (hexdump of one SHM slot) and verify:
#   bytes[0]      == source_id
#   bytes[1]      == flags
#   bytes[2..4]   == topic_id (LE)
#   bytes[4..8]   == seq (LE)
#   bytes[8..16]  == timestamp_ns (LE)
#   bytes[16..18] == sideband_idx (LE)
#   bytes[18..24] == sideband_len (LE, 6B)
#   bytes[24..32] == sideband_seq (LE)
#   bytes[32..64] == payload (zero-padded)
```
