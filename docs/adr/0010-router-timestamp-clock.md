# ADR 0010: Router timestamp clock (`CLOCK_MONOTONIC_RAW`)

- **Status:** Accepted
- **Date:** 2026-05-27
- **Builds on:** [ADR 0004](0004-robotics-module-boundaries.md) (router stays transport-only; no cross-cutting subsystems), [ADR 0008](0008-router-frame-v2.md) (`uint64_t timestamp_ns` is one of the v2 frame fields, host little-endian, stamped by the router on forward)
- **Closes (single-host portion):** Phase E E4
- **Partially closes:** [post-phases robotics-integration review C8](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c8--cross-host-time-sync-ptp--ntp) — single-host scope only; cross-host correlation is delegated (see Out of scope below)
- **Scope:** What clock fills the 64 B `RouterFrame` v2's `timestamp_ns` field, and what semantics that field provides. No wire-format change; no API break; one small library helper + one demo update.

## Context

[ADR 0008](0008-router-frame-v2.md) reserves a `uint64_t timestamp_ns` slot in the 64 B `RouterFrame` v2 and says the router stamps it on the forward path. The current demo router in [`ipc/test/router_server.cpp`](../../ipc/test/router_server.cpp) fills it with:

```cpp
using Clock = std::chrono::steady_clock;
const Clock::time_point kStartTime = Clock::now();

uint64_t ns_since_start() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - kStartTime).count());
}
```

This is **router-process-local** time, measured from the moment `router_server` started. Two problems surface in real robotics deployment:

1. **It does not survive a router restart.** systemd's `Restart=on-failure` ([deploy/systemd/rim-router.service](../../robotics-ipc-module/deploy/systemd/rim-router.service)) will reset `kStartTime` and previously recorded `timestamp_ns` values are no longer comparable to fresh ones. Subscribers that latch the highest seen timestamp (e.g. `LastValueCache`) cross-check is fine; subscribers that ratchet a wall-clock interpretation are not.
2. **`std::chrono::steady_clock` is implementation-defined.** On glibc Linux it is `CLOCK_MONOTONIC` (slew-aware, NTP-adjusted at sub-µs scale); the standard does not guarantee that. For a robotics control loop, a clock that quietly slews under NTP correction is the wrong choice.

Phase E E4 in [the E plan](../../robotics-ipc-module/plans/E-robotics-integration.md#e4--time-sync) reserved the ADR slot but listed two options: "`CLOCK_MONOTONIC` raw or PTP offset field." A third option (a two-field frame with separate publisher + router timestamps) surfaced during the Phase E "fit for purpose" review captured in [post-phases-robotics-review.md C8](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c8--cross-host-time-sync-ptp--ntp). All three were on the table going into this ADR.

The decision below picks one — and explicitly delegates the remaining problem space to neighbouring components rather than letting it grow inside the router.

## Decision

**The router stamps `RouterFrame::timestamp_ns` with `CLOCK_MONOTONIC_RAW` in nanoseconds, read directly from `clock_gettime(CLOCK_MONOTONIC_RAW, &ts)` with no per-process offset subtraction.**

Three concrete deliverables:

1. **New library helper:** [`ipc/src/router/timestamp.hpp`](../../ipc/src/router/timestamp.hpp) exposes `inline uint64_t router_now_ns()` returning the canonical reading. Header-only, no extra link deps. User code that wants to stamp its own frames with the same clock the router uses includes this header.
2. **Demo update:** `router_server.cpp` drops `kStartTime` / `ns_since_start`, includes the helper, and passes `router_now_ns` as the `now_ns_fn` callback to `server.run(...)`. The router library itself (`link.hpp`, `shm_router_link.hpp`) is unchanged — the clock has always been a callback parameter; this ADR only fixes which callback the demo + recommended path uses.
3. **Stamping semantics on the wire are unchanged.** Same field, same width, same endianness, same offset in the frame. Subscribers do not need to recompile.

### Why `CLOCK_MONOTONIC_RAW` specifically

| Property | `steady_clock` (today) | `CLOCK_MONOTONIC` | `CLOCK_MONOTONIC_RAW` | `CLOCK_REALTIME` / `CLOCK_TAI` |
|---|---|---|---|---|
| Slew-free (immune to `adjtime` / NTP) | implementation-defined | no — NTP slew | **yes** | no |
| Survives router restart | no (reset to 0) | yes (since boot) | **yes (since boot)** | yes (wall clock) |
| Comparable across processes on same host | no | yes | **yes** | yes |
| Comparable across hosts | no | no | no | yes (if NTP/PTP sync'd) |
| `clock_gettime` availability on L4T / Ubuntu | available | available | **available** | available |
| Cost per call (typical) | ~10 ns vDSO | ~10 ns vDSO | **~10 ns vDSO** | ~10 ns vDSO |

`CLOCK_MONOTONIC_RAW` wins on the two properties that matter to a hot-path stateless router:

- **Slew-free** — control loops that compute deadlines from frame timestamps don't get spooky NTP-induced jitter.
- **Process-independent** — a peer that calls `router_now_ns()` and the router that re-stamps on forward share the same epoch. Subscribers that compute `recv_now - frame.timestamp_ns()` get a real one-way-latency estimate (modulo the slew between *publisher* and *router* clock domains, which on a single host is zero since they both read the same kernel hardware counter).

Cost is irrelevant — `clock_gettime` on the `CLOCK_MONOTONIC_RAW` ID is a vDSO call on modern kernels, costing the same as `steady_clock::now()`.

## Out of scope (delegated)

Cross-host time correlation, PTP integration, capture-time vs forward-time disambiguation, replay-grade timestamps — **none of these are router responsibilities.**

The router is the stateless hot path. Adding cross-host time logic to it would mean:

- New runtime dependency (`ptp4l` / `phc2sys`) on the production host
- New failure modes (PTP grandmaster unreachable, clock domain disagreement)
- New configuration surface (which `CLOCK_TAI` offset to use, how to refresh it)
- Bigger frames or out-of-band metadata to carry the second clock domain

None of these belong inside the forward loop. They belong in components that already have relaxed latency budgets and the right state:

1. **Application-level (user code).** A peer that needs cross-host correlation adds its own clock reading (PTP-anchored monotonic, host wall clock, sim-time, whatever) inside its custom payload. The 32 B inline payload has room; sideband regions have much more. The router transports those bytes opaquely.
2. **Future dedicated recorder module.** A stateful recorder (the kind needed for replay per [parked review C4](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c4--playback--simulation-testing-on-x86)) is the natural home for richer time semantics. Its statelessness and latency requirements are *relaxed* relative to the router — it can afford to read `CLOCK_TAI`, query `phc2sys`, and emit per-frame metadata that lets replay reconstruct the original cadence with PTP precision. This recorder does not exist yet; when it lands (or when a user chooses to build one), it inherits the cross-host time problem from the router by design.

This delegation rule is explicit in the project's policy: **the router transports; neighbours interpret.**

## Alternatives considered

### A — `steady_clock` (status quo)

- **Reject reason:** implementation-defined (`CLOCK_MONOTONIC` on glibc; not guaranteed elsewhere), slew-prone on some implementations, and the `- kStartTime` offset destroys cross-process comparability + restart survival.

### B — `CLOCK_TAI` + PTP / NTP

- **Reject reason:** drags in a host-level dependency (`chronyd` / `ptp4l` / `phc2sys`) the module has no business managing. Cross-host correlation is real, but it is *not* a router problem — see Out of scope above. Note: a user who *does* run PTP on their host can still stamp `CLOCK_TAI` themselves in their peer code; this ADR does not preclude it, it just refuses to make it the router's recommended default.

### C — Two-clock frame (publisher + router)

- **Reject reason:** would re-carve [ADR 0008](0008-router-frame-v2.md)'s 64 B layout. Either we shrink the 32 B inline payload (regression for ADR 0005 control-plane budgeting) or we drop another field. The marginal value — being able to compute "router-induced latency" — is real but available *today* by having the peer call `router_now_ns()` before send and the subscriber call `router_now_ns()` on recv, then comparing both to `frame.timestamp_ns()`. No frame change needed.

### D — `CLOCK_BOOTTIME`

- **Reject reason:** identical to `CLOCK_MONOTONIC_RAW` except it includes time spent in suspend. Robotics deployments don't suspend; the distinction is irrelevant. `CLOCK_MONOTONIC_RAW` is the more conservative, more portable name to pick.

## Consequences

### Positive

- **Slew-free, restart-surviving timestamps within a single host's boot epoch.** Subscribers that look at frame timestamps see monotonically advancing nanoseconds-since-boot, regardless of router lifetime or NTP activity.
- **Cross-process comparable on the same host.** A user-written peer that calls `router_now_ns()` before send and a recorder that decodes `frame.timestamp_ns()` after recv are reading the same kernel counter. Per-hop latency measurement becomes trivial.
- **Smaller demo code.** `router_server.cpp` loses the `kStartTime` global and the `ns_since_start` function. The clock semantics move from the demo into a documented, dedicated library helper.
- **Clean delegation boundary.** Anyone who reads this ADR knows exactly which problems the router refuses to solve and where they live instead. This protects future phases from scope creep — a "let's add PTP to the router" suggestion has a written answer to point at.

### Negative

- **Numerical timestamps grow.** Today's demo output shows `timestamp_ns ≈ a few ms` (since router start); after this ADR, the same field carries `timestamp_ns ≈ seconds-since-boot × 1e9`, i.e. typically in the 10¹⁰ – 10¹² range. Anything that parses the demo log expecting small numbers needs to widen its display. CSV parsers, JSON consumers — all see a `uint64`, all should already handle the full range.
- **Resets on reboot.** Acceptable for in-process control loops. Long-running tapes that span reboots cannot be timestamp-correlated across the gap without external context — exactly the case where a stateful recorder is the right answer.
- **No cross-host correlation.** Stated explicitly. Users who need it implement it; the router does not.

### Neutral

- **No wire-format change.** Existing subscribers continue to decode `frame.timestamp_ns()` as a `uint64`; they have no way to know (or care) which clock filled it.
- **No library code change.** The router's forward path was always clock-agnostic — `link.hpp` line 95 and `shm_router_link.hpp` line 91 both write whatever the callback returned. This ADR only changes which callback the demo and the recommended path uses.

## Code change shape

[`ipc/src/router/timestamp.hpp`](../../ipc/src/router/timestamp.hpp):

```cpp
#pragma once

#include <cstdint>
#include <ctime>

// Returns CLOCK_MONOTONIC_RAW in nanoseconds since boot. See ADR 0010.
inline uint64_t router_now_ns() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull
         + static_cast<uint64_t>(ts.tv_nsec);
}
```

[`ipc/test/router_server.cpp`](../../ipc/test/router_server.cpp) (delta):

```diff
-using Clock = std::chrono::steady_clock;
-const Clock::time_point kStartTime = Clock::now();
-
-uint64_t ns_since_start() {
-    return static_cast<uint64_t>(
-        std::chrono::duration_cast<std::chrono::nanoseconds>(
-            Clock::now() - kStartTime).count());
-}
+#include "router/timestamp.hpp"
...
-        ns_since_start,
+        router_now_ns,
```

No other library or test file changes. The header is opt-in for downstream peers that want to stamp their own frames with the same clock.

## Verification

- `make all` compiles clean.
- `make test-ipc-unit` (frame + LVC tests): assertions use literal values + `fake_now_ns()` counters, so this ADR has no behavioral impact on the test surface.
- `make test-router` UDS / UDP / SHM scenarios continue to pass; the demo recorder's CSV now contains since-boot ns values instead of since-start ns values — the format and count of records is unchanged.
- Subjective check: `journalctl -u rim-router.service -f` shows monotonically increasing timestamps across restarts of `rim-router.service`.

## References

- [ADR 0004 — Robotics module boundaries](0004-robotics-module-boundaries.md): "the router transports; neighbours interpret" rule
- [ADR 0008 — RouterFrame v2](0008-router-frame-v2.md): defines the 64 B layout including `timestamp_ns`
- [Phase E plan E4](../../robotics-ipc-module/plans/E-robotics-integration.md#e4--time-sync)
- [Post-phases robotics-integration review C8](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c8--cross-host-time-sync-ptp--ntp): cross-host time backlog — single-host portion closed by this ADR; cross-host portion delegated and remains in the backlog
- [Post-phases robotics-integration review C4](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c4--playback--simulation-testing-on-x86): replay / dedicated recorder — the natural home for richer time semantics, once it exists
- `man 2 clock_gettime` — `CLOCK_MONOTONIC_RAW` semantics
