# ADR 0009: Per-peer SHM ring sizing (`shm_slot_count`, `shm_max_payload`)

- **Status:** Accepted
- **Date:** 2026-05-25
- **Builds on:** [ADR 0001](0001-ipc-and-router.md), [ADR 0002](0002-ipc-router-refactor.md), [ADR 0004](0004-robotics-module-boundaries.md) (module boundaries), [ADR 0005](0005-payload-policy-and-sideband.md) (sideband regions are separate rings), [ADR 0006](0006-shm-backpressure-and-metrics.md) (drop-on-full), [ADR 0008](0008-router-frame-v2.md) (64 B RouterFrame v2)
- **Scope:** Per-peer override of `ShmSpsc::BindParams` (slot count, slot payload) at the TOML topology surface, propagated through `PeerEntry` to the SHM peer-address adapter. No change to wire format, no change to non-SHM transports, no change to ring algorithm.

## Context

After [ADR 0008](0008-router-frame-v2.md), the canonical `RouterFrame` is
**64 bytes** (one cache line). Every `[[peers]]` `local` address with
`shm:` scheme is, by construction, a **router-frame ring** — bulk data
(images, tensors, MAVLink streams) lives in `[[peers.sideband]]`
regions per [ADR 0005](0005-payload-policy-and-sideband.md), which are
**separate** SHM rings with their own sizing.

Yet `ShmSpsc::BindParams` still defaults to `slot_count = 256`,
`max_payload = 1024`, and `ShmRouterLink::bind_router` /
`bind_peer` use those defaults unconditionally. The
`bind_router(const BindParams&)` parameter is even explicitly ignored
in the source today:

```cpp
void bind_router(const BindParams& params) {
    ...
    (void)params;
    peer_channels_.clear();
    ...
}
```

That means every router-frame ring eats `2 × 256 × 1028 ≈ 514 KiB` of
`/dev/shm` per peer pair direction, of which **~94%** is wasted: each
slot is 1028 B (4 B atomic length + 1024 B payload) but only 64 B of
frame data is ever written. On a 10-peer Jetson topology this is
several MB of SHM, none of which fits in any cache level, and every
forward operation pulls in lines from main memory.

The fix is structural, not algorithmic: the slot stride must be sized
to the frame at *bind* time. The topology is the right place to
declare it because (a) it is already the per-deployment configuration
surface, and (b) different peers want different sizings — a vision
peer's *sideband* ring (Phase F) will want `max_payload = 8 MiB`, but
its router-frame ring stays at 64 B.

[Phase D's D0 plan item](../../robotics-ipc-module/plans/D-validation-stress.md)
makes this an explicit gate: D2's "Burst sensor" and D3's idle-CPU
re-baseline both want to measure on right-sized rings, not the
default-1024 status quo.

## Decision

### Schema additions (additive only — no migration required)

`[[peers]]` gains two optional integer fields:

```toml
[[peers]]
id              = 1
name            = "sensor"
local           = "shm:/cpp_tricks_router_sensor"
shm_slot_count  = 256        # optional; default = ShmSpsc::BindParams::slot_count (256)
shm_max_payload = 64         # optional; default = ShmSpsc::BindParams::max_payload (1024)
```

Both fields are **optional**. Existing profiles that omit them
continue to load unchanged and bind with the same `256 × 1024` slots
they did before — this is a **truly additive** schema extension and
ADR 0008-style "rip the band-aid" migration is **not** required.

### `PeerEntry` C++ representation

The values are propagated through a sentinel-zero pattern on
`PeerEntry`:

```cpp
struct PeerEntry {
    uint8_t      id;
    const char*  name;
    PeerAddress  local;
    uint32_t     shm_slot_count  = 0;   // 0 = use ShmSpsc::BindParams default
    uint32_t     shm_max_payload = 0;   // 0 = use ShmSpsc::BindParams default
};
```

The zero defaults keep all existing compile-time aggregate
initializers (e.g. `{kEndpointSensor, "sensor", peer_shm(name)}` in
`router_client_config.h` and `shm_backpressure_test.cpp`) compiling
**without source changes** — C++20 aggregate initialization fills the
trailing members from the in-class initializers.

`uint32_t` is plenty: slot count up to ~4 billion (we cap at 2²⁰ in
the loader, far below); slot payload up to ~4 GiB (we cap at 256 MiB
in the loader). 64-bit fields buy nothing on a 64 B `RouterFrame`'s
ring and would bloat the struct.

### Adapter overload

`ipc/src/router/shm_peer_address_io.hpp` gains a second
`bind_shm_endpoint` overload that takes a `PeerEntry`:

```cpp
inline void bind_shm_endpoint(
    IpcEndpoint<ShmSpsc>& endpoint,
    const PeerEntry& entry,
    bool create) {
    if (entry.local.kind != PeerAddressKind::ShmRing) {
        throw std::runtime_error("shm endpoint expected shm peer address");
    }
    ShmSpsc::BindParams params{
        .name = entry.local.u.shm_name,
        .create = create,
    };
    if (entry.shm_slot_count  != 0) params.slot_count  = entry.shm_slot_count;
    if (entry.shm_max_payload != 0) params.max_payload = entry.shm_max_payload;
    endpoint.bind(params);
}
```

The pre-existing `bind_shm_endpoint(endpoint, const PeerAddress&,
bool)` overload is kept for callers that don't have a `PeerEntry`
handy. Both call sites in `ShmRouterLink` (`bind_router` for each
peer channel, `bind_peer` for the client's own ring) switch to the
new `PeerEntry` overload so per-peer overrides flow end-to-end.

### Loader validation

The TOML loader rejects nonsensical combinations early, with explicit
error messages:

| Condition | Error |
|---|---|
| `shm_slot_count` or `shm_max_payload` set on a non-`shm:` peer | `"peer '<name>' has shm_* override but local address is not shm:"` |
| `shm_slot_count < 0` or `> 1 << 20` (≈ 1 M slots) | `"shm_slot_count <N> out of range 1..1048576"` (0 means "default") |
| `shm_max_payload != 0` and `< kRouterFrameSize` (64) | `"shm_max_payload <N> < kRouterFrameSize (64): cannot hold a RouterFrame"` |
| `shm_max_payload > 256 * 1024 * 1024` (256 MiB) | `"shm_max_payload <N> exceeds 256 MiB"` |

The lower bound `≥ kRouterFrameSize` is what ties this ADR to ADR
0008: the loader knows a peer's `local` ring carries `RouterFrame` v2
and refuses to bind a ring that can't hold one. Bulk-data rings live
under `[[peers.sideband]]`, are not subject to this validation, and
keep their existing `max_payload_bytes` semantics.

### Profile templates

`config/profiles/jetson_prod.toml` (the canonical SHM deployment) is
updated to set `shm_max_payload = 64` and `shm_slot_count = 256` on
all three demo peers (sensor, controller, recorder), demonstrating the
recommended sizing for router-frame rings. The other profiles are not
SHM and require no change.

The other reference profiles (`x86_dev.toml`, `hil.toml`,
`sim_cloud.toml`) use UDS / UDP transports and never touch
`shm_max_payload` at all.

## Migration

| Surface | Change | Migration |
|---|---|---|
| C++ headers | `PeerEntry` gains two trailing fields with in-class defaults | Source-compatible. Existing aggregate initializers continue to work. |
| Wire format | None | None |
| Topology TOML | Two new optional `[[peers]]` keys | Pure addition. Old profiles load unchanged. |
| `ShmRouterLink::bind_router`'s ignored `BindParams` | Still ignored (kept for API stability) | None; the override path goes through `PeerEntry` now |
| `make test-router` (demo path) | Compile-time `kDemoPeers` set the new fields to 0 → ShmSpsc defaults (256 × 1024) | None — behavior preserved |
| `jetson_prod.toml` (profile template) | Opts into 64 B slots | None — operators who copy this profile get the cache-friendly defaults |

## Consequences

### Positive

- Right-sized rings: router-frame rings on `jetson_prod.toml` drop
  from `~514 KiB` to `~34 KiB` per peer pair direction — a **~15×
  reduction** in SHM footprint and (more importantly) per-peer ring
  data that fits in L1/L2 on the platforms of intent.
- Memory pressure on Jetson Orin (limited LLC vs. discrete x86)
  becomes a tunable, not an unspoken default.
- Sideband rings (Phase F vision / ML) remain free to set
  `max_payload_bytes = 8 MiB` independently — the ADR cleanly
  separates "router-frame ring sizing" from "sideband region sizing."
- Loader validation catches a class of footguns at startup
  (`shm_max_payload < kRouterFrameSize` becomes impossible to
  deploy).

### Negative

- Two new optional fields slightly expand the TOML schema surface and
  the `PeerEntry` ABI. Both are additive; both are validated at load.
- `bind_router(const BindParams&)`'s already-ignored argument remains
  ignored. We could remove it, but that would be an API break for
  zero gain (it has no readers outside the link itself); leaving it
  in place keeps the door open if `BindParams` ever grows fields
  unrelated to per-peer sizing.
- The `PeerEntry` struct grows from 16 to 24 bytes (on a 64-bit
  target with `uint8_t id` + 7-byte padding + 8-byte `name` + 16-byte
  `PeerAddress` + two `uint32_t`s, aligned to 8). Topologies with
  hundreds of peers will see a few extra cache lines of static
  storage. Cost is trivial compared to the per-ring savings.

### Neutral

- The `ShmRouterMetrics` structure ([ADR 0006](0006-shm-backpressure-and-metrics.md))
  is unaffected. Drop-on-full continues to count global drops; per-peer
  attribution is its own line item ([Phase D2a](../../robotics-ipc-module/plans/D-validation-stress.md)).
- `static_assert` is intentionally **not** added at compile time on
  `shm_max_payload >= kRouterFrameSize` for the C++ struct because
  the field is allowed to be zero (sentinel for default). The check
  lives in the loader where the user-supplied value is known.

## Alternatives considered

### A. Hardcode the loader default to 64

The loader would set `shm_max_payload = 64` whenever the TOML omits
it, regardless of whether the operator wanted that. **Rejected.**
This is a behavior change for already-deployed profiles, the kind we
explicitly avoid without a wire-format-style migration ADR. The
opt-in route via profile template (`jetson_prod.toml`) gives operators
the cache win where it matters without surprising anyone.

### B. Make `shm_max_payload` a global `[router]` setting

A single global field would apply uniformly. **Rejected.** Phase F
vision peers will need an order-of-magnitude larger payload for their
*sideband* rings — that's separate config under `[[peers.sideband]]`
— but the principle stands: per-peer sizing is the only way to
support heterogeneous peer roles.

### C. Drop `slot_count` from the override and pin it to 256

`slot_count` matters less for cache (the atomic-header lines are
already separate from slot data), but matters for throughput
headroom: a fast publisher into a slow consumer needs more slots
before drop-on-full kicks in. Burst-prone profiles (e.g. recorder
behind a slow disk) will want larger slot counts. **Rejected.**
Exposing both is symmetric, parsed by the same code, and costs ~5
lines of validation.

### D. Make `shm_slot_count` / `shm_max_payload` mandatory in TOML

Forces every SHM profile to declare both. **Rejected.** Increases
boilerplate without value; sensible defaults exist and the loader
covers them.

## Verification

```bash
# Build and unit tests must stay green; new loader scenarios must pass.
make test-ipc-unit
# expects:
#   topology_loader_test: N/N assertions passed   (new D0 cases included)
#   frame_test           : 163/163
#   last_value_cache_test: 26/26
#   shm_backpressure_test: 27/27

# Demo path still binds with default 256 × 1024 (compile-time PeerEntry zeros).
make test-router
# expects: uds / udp / shm scenarios green, no regression.

# Loaded jetson_prod profile binds with 256 × 64.
./build/ipc/test/router_server --config config/profiles/jetson_prod.toml \
  & ROUTER_PID=$!
ls -la /dev/shm/cpp_tricks_router*    # files now ~35 KB each, down from ~526 KB
kill -TERM $ROUTER_PID
```

Quantitative acceptance (asserted in the new topology_loader_test
case `test_shm_ring_sizing_overrides`):

```
shm_region_size(256, 1024) = 64 + 2*256*1028 =  526'400 B  (~514 KiB)
shm_region_size(256,   64) = 64 + 2*256*  68 =   34'880 B  (~34 KiB)
ratio ≈ 15.1×
```

## Follow-ups

- **Phase D2a:** Per-peer `dropped_full` attribution
  (`ShmRouterMetrics` extension). Independent of this ADR but
  benefits from the same per-peer-config plumbing.
- **Phase D3:** Re-baseline `idle_cpu_check.sh` against
  `jetson_prod.toml`'s new sizing.
- **Phase F:** `[[peers.sideband]] memory_class` (CPU / CUDA /
  NvBufSurface), forward-declared in [ADR 0008](0008-router-frame-v2.md).
  Distinct from this ADR — that field describes *which memory*, this
  one describes *how much*.
