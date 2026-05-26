# ADR 0006: SHM router backpressure policy and link metrics

- **Status:** Accepted
- **Date:** 2026-05-25
- **Builds on:** [ADR 0001](0001-ipc-and-router.md) (router stages), [ADR 0004](0004-robotics-module-boundaries.md) (module surface), [ADR 0005](0005-payload-policy-and-sideband.md) (control plane vs data plane)
- **Scope:** Bounded behavior of `ShmRouterLink::send_to_peer` when a destination ring is full; the metrics surface (`ShmRouterMetrics`) consumers can sample to detect overload; alternatives considered and rejected for Phase C.

## Context

Phase A vendored a Phase 1 SPSC ring (`ipc/src/ipc/shm_spsc.hpp`). The
producer-side helper `shm_push_slot` retried *forever* on a full ring:

```cpp
while (true) {
    if (h - t >= slot_count) {
        continue;          // ← infinite spin on a slow consumer
    }
    ...
}
```

For the in-process echo benchmark this is acceptable: producer and consumer
are paired, the consumer is always alive, and saturating the ring on
purpose is the *point* of the benchmark. But the SHM transport is also
the production fabric for the **Jetson profile** (see
`config/profiles/jetson_prod.toml` and SYSTEM-VISION.md). On a real robot:

- A subscriber peer may crash, hang, or be SIGSTOPped while the router and
  every other peer keep producing.
- The router serves *every* peer on a single forwarding thread. If
  `send_to_peer(slow_peer, ...)` spins, every other producer is starved.
  Head-of-line blocking on a robotics fabric means a control loop misses
  its deadline because the recorder hung — unacceptable per
  [DESIGN-PRINCIPLES.md](../../robotics-ipc-module/DESIGN-PRINCIPLES.md)
  ("Determinism").
- The router has no policy to even **report** overload back to apps; the
  failure is "everything stops" with no log line, no counter, no signal.

Phase C closes both gaps: bounded send on the hot path, and observable
counters so ops/test code can detect drops.

Phase A worked around the spin by setting `IPC_SKIP_SHM=1` in
`test-ipc`. That escape hatch is removed in Phase D (echo client gains an
interruptible exchange path); ADR 0006 documents the underlying fix.

## Decision

### Drop-on-full at the router

`ShmRouterLink::send_to_peer` now uses non-blocking publish:

```cpp
void send_to_peer(uint8_t dest, const Buffer& payload) {
    for (auto& channel : peer_channels_) {
        if (channel.peer_id == dest) {
            if (try_send_shm_buffer(channel.endpoint, payload)
                == ShmSendResult::Ok) {
                metrics_->forwarded.fetch_add(1, std::memory_order_relaxed);
            } else {
                metrics_->dropped_full.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
    }
    throw std::runtime_error("shm router: no channel for peer id "
        + std::to_string(dest));
}
```

The destination ring is checked once. A full ring yields
`ShmSendResult::Full`, the frame is **dropped for that destination only**,
the `dropped_full` counter is bumped, and the router moves on to the next
target. Other destinations on the same forward call are unaffected.

Rationale: in a robotics fabric we prefer **a lost frame to a stalled
fabric**. The publisher of the dropped frame keeps publishing fresh data;
the subscriber that was full was already too far behind. Operationally
this matches video pipelines (drop frames on the floor) and CAN/MAVLink
ground stations (newest-state wins).

### Wire-level helper: `ShmSpsc::try_send`

Adds a Linux-only static method to the transport:

```cpp
enum class ShmSendResult : uint8_t { Ok = 0, Full = 1 };

static ShmSendResult try_send(
    const Handle& handle,
    const SendParams& params,
    const Buffer& payload);
```

Implemented in terms of a new non-blocking helper `shm_try_push_slot`
that returns `false` instead of looping when `head - tail >= slot_count`.
The legacy blocking `shm_push_slot` is reimplemented as
`while (!shm_try_push_slot(...)) {}` so existing call sites (echo
benchmark, client→router publish) are byte-for-byte unchanged.

`IpcEndpoint<T>::try_send` is exposed via a C++20 `requires` clause so
only transports that declare `try_send` get the method at the type level
(UDP/UDS do not — their kernel-managed send buffer plays the role of the
SPSC ring and is not exposed to the user via a Full enum).

### Link-level metrics

```cpp
struct ShmRouterMetrics {
    std::atomic<uint64_t> forwarded{0};
    std::atomic<uint64_t> dropped_full{0};
    std::atomic<uint64_t> recv_empty{0};
    std::atomic<uint64_t> recv_truncated{0};
};
```

Atomics live in a heap-allocated `ShmRouterMetrics` owned by
`ShmRouterLink` via `std::unique_ptr`. This keeps the link itself movable
(required by the factory pattern — `ShmRouterLink::server(topo)` returns
by value) without having to hand-write a copy-via-load move constructor
for each counter.

Semantics:

| Counter           | When it increments                                             |
|-------------------|----------------------------------------------------------------|
| `forwarded`       | one per (source frame × destination peer) successfully placed in a peer ring |
| `dropped_full`    | destination ring was full at `try_send`; this destination's copy was dropped |
| `recv_empty`      | `forward()` iteration where **no** peer channel had a frame   |
| `recv_truncated`  | peer sent a buffer smaller than `kRouterFrameSize`            |

All loads use `std::memory_order_relaxed` because the publish ordering
between counters is not meaningful for monitoring (each counter is an
independent monotonic stream).

Exposed via `ShmRouterLink::metrics() const noexcept -> const
ShmRouterMetrics&`. The reference is stable for the lifetime of the link
(moves transfer the `unique_ptr` but the heap-allocated metrics block
itself does not relocate).

Datagram links (`DatagramRouterLink<Udp>`, `DatagramRouterLink<Uds>`) do
not own metrics yet — their drop semantics live in the kernel and are
surfaced differently (`SO_RXQ_OVFL`, `/proc/net/udp`). A unified metrics
struct that abstracts both is a Phase E concern (status/health bridge).

### Module API surface

`ipc/src/router/metrics.hpp` is a new public header (see `ipc/MODULE.md`
"Phase C headers"). `ShmRouterMetrics` is now part of the module's
versioned API; the field layout is documented in the header and intended
to be stable across Phase D test additions. Adding counters is
backwards-compatible (appending atomics); removing or renaming requires
a new ADR.

## Consequences

### Positive

- The Phase A failure mode ("router server spins forever, must
  `IPC_SKIP_SHM=1`") is gone; the router never blocks on a slow peer.
- Apps gain a single-call observability surface (`link.metrics()`) for
  forwarding health without parsing logs.
- Drop policy is explicit and auditable: every frame the router decides
  not to deliver is counted, never silently swallowed.
- The metrics-via-`unique_ptr` pattern can be reused for
  `DatagramRouterLink` in Phase D / E without changing the link's move
  semantics.

### Negative

- Subscribers must tolerate gaps. The recorder peer and any downstream
  ML/MAVLink consumer must be designed for "newest state wins", not
  "every frame is delivered". This is consistent with SYSTEM-VISION.md
  but worth restating in MODULE.md.
- `dropped_full` alone does not tell you **which** peer was slow. Phase D
  may extend metrics with a small per-peer fixed array; for now ops must
  correlate drops with peer log offsets.
- The client→router direction is *still* blocking — `send_to_router`
  calls the legacy `shm_push_slot`. If the router crashes while a peer
  has its req ring full, that peer hangs until killed. This is documented
  in `ipc/MODULE.md` "Known limitations"; the fix is a separate ADR
  (TBD) because it requires a return type change on the client API.

### Neutral

- `forwarded` is per-destination, not per-frame. If a route forwards one
  source frame to two destinations, `forwarded` increments twice. This
  matches `ForwardResult::targets.count` semantics and is documented in
  the header.

## Alternatives considered

### A. Bounded retry with yield

Try `try_send` up to N times with `std::this_thread::yield()` between
attempts. Caps the worst case to roughly N × yield latency but doesn't
solve the head-of-line problem: while looping on peer 2, peers 3..N are
not serviced. Rejected — head-of-line blocking is the core failure mode
ADR 0006 fixes.

### B. Per-peer worker threads

One thread per destination ring; producer hands off to a per-peer queue.
Eliminates head-of-line blocking but adds a thread per peer (Jetson has
~10 peers in SYSTEM-VISION.md → 10+ threads), plus a software queue per
peer, plus context-switch costs. Out of scope for header-only Phase C;
revisit if a non-trivial number of peers need bounded-retry semantics
in Phase F.

### C. Overwrite oldest

Producer advances `tail` itself to evict the oldest slot before writing,
turning the ring into a "newest wins" buffer. Subscribers may see frames
disappear mid-read. Rejected because SPSC correctness depends on a single
writer of `tail`, and the router is not the consumer.

### D. Block with timeout

`try_send` with a millisecond timeout. Slightly better than retry-N but
adds a sleep_for to the hot path of every forward when even one peer is
slow. Rejected for the same head-of-line reason as A; the timeout policy
becomes a per-peer concern that's better handled by the future eventfd
path (ADR 0007) than baked into the synchronous router loop.

## Verification

```bash
# Builds the new try_send/metrics surface and runs the dedicated test:
make test-shm-backpressure
# expects: "27/27 assertions passed",
#          "published=1024 forwarded=256 dropped_full=768"
#          (slot_count=256 default ring fills, every excess frame is dropped)

# End-to-end SHM router scenario still passes:
make test-router

# Unit aggregate now includes Phase C:
make test-ipc-unit
```

The full-ring scenario in `ipc/test/shm_backpressure_test.cpp` includes
a 2-second deadline guard — if a future change reintroduces the spin,
the test fails with "deadline exceeded (spin?)".
