# ADR 0016: Real-time hardening hooks + priority-aware QoS

- **Status:** Accepted (C7 closure, production-hardening cluster)
- **Date:** 2026-06-03
- **Builds on:** [ADR 0004](0004-robotics-module-boundaries.md) (dependency-light boundary), [ADR 0006](0006-shm-backpressure-and-metrics.md) (drop-on-full + metrics), [ADR 0007](0007-router-idle-wake.md) (forward-loop structure), [ADR 0008](0008-router-frame-v2.md) (the 3-bit `priority` flag), [ADR 0015](0015-systemd-readiness-notification.md) (the C6 sibling in this cluster)
- **Closes:** [post-phases review C7 — real-time / production knobs + priority-aware QoS](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c7--real-time--production-knobs-mlockall-cpu-pinning-sched_fifo-priority-aware-qos)
- **Scope:** Two latency-under-contention knobs that the C7 backlog entry bundled: (1) opt-in real-time hardening init hooks (`mlockall`, CPU pinning); (2) priority-aware drop-lowest-first behavior on the SHM hot path, plus per-priority drop attribution.

## Context

C7 collected the production-hardening gaps that share one problem domain —
**latency under contention** — and one surface (the `forward()` hot path + the
systemd unit files):

- The router never called `mlockall` / `sched_setaffinity` / `sched_setscheduler`.
  On a Jetson the control-loop IPC path wants its pages locked and its thread
  pinned to an isolated core to avoid page-fault and migration jitter. The E2
  units shipped these as *commented-out* `MemoryLock=` / `CPUAffinity=` /
  `LimitRTPRIO=` directives with a pointer here.
- The 3-bit `priority` field in `RouterFrame::flags` ([ADR 0008](0008-router-frame-v2.md))
  was **advisory only** — `ShmRouterLink`/`DatagramRouterLink` never inspected
  it. Under backpressure the router dropped whatever frame arrived when a ring
  was full, regardless of priority, so a flood of low-priority telemetry could
  starve high-priority control frames.

The C7 entry's decision rubric offered, for QoS, "drop low-priority frames first
on backpressure **or** drain a priority queue." We take the first: it is
SPSC-safe and needs no buffering/reordering.

## Decision

### 1. Opt-in real-time hardening hooks (no new dependencies)

`ipc/src/router_app.h` (app-only header) gains two inline hooks:

```cpp
bool router_lock_memory();          // mlockall(MCL_CURRENT | MCL_FUTURE)
bool router_pin_to_core(int core);  // sched_setaffinity to {core}
```

Both return `false` on failure (missing `CAP_IPC_LOCK`, out-of-range core, …),
never throw, and are **not called automatically**. The `router_server` demo
invokes them only when env vars are set (`RIM_MLOCK`, `RIM_CPU`), so the default
build is byte-for-byte unchanged. `sched_setaffinity` / `cpu_set_t` are glibc
extensions gated on `_GNU_SOURCE`; since the build uses `-std=c++20` (not
`gnu++20`), `router_app.h` defines `_GNU_SOURCE` at its top (it is always the
first include in the app translation units, and library headers must not include
it). No `librt`-beyond-existing, no `libsystemd`, no new link inputs.

**`SCHED_FIFO` is intentionally left to systemd**, not a code hook. Real-time
scheduling is a deployment-policy decision that belongs in the unit
(`LimitRTPRIO=` + a wrapper / `CPUSchedulingPolicy=`), not baked into the
binary: enabling it in-process risks a runaway `SCHED_FIFO` thread wedging a
core during development, and it needs privileges the unit already governs. The
unit files document `LimitRTPRIO=80` alongside the `RIM_*` env vars.

### 2. Priority-aware drop-lowest-first (SHM hot path)

`ShmRouterLink` gains `set_priority_drop_floor(uint8_t floor)` (default `0` =
disabled). The mechanism is **admission control under congestion**, not eviction
(eviction would violate SPSC — only the consumer pops):

- Each per-peer channel tracks a `congested` flag: set when a `try_send` finds
  the ring full, cleared on the next successful `try_send`.
- In `send_to_peer`, while a destination is congested and `floor > 0`, an
  incoming frame whose 3-bit priority is **below the floor** is **shed before
  the ring is touched**. This reserves the ring's freed-slot bandwidth for
  higher-priority frames behind it.
- `floor == 0` skips the check entirely → identical to the legacy
  unconditional drop-on-full.

This is "drop lowest first" achievable without reordering or buffering: under
load, low-priority frames stop being admitted, so when the consumer frees a
slot it is claimed by the next high-priority frame rather than the next frame
to arrive.

**Datagram links are unchanged.** A `DatagramRouterLink` hands frames to the
kernel socket buffer; the router owns no bounded queue there, so there is no
backpressure signal to shed against (kernel drops are observable via
`SO_RXQ_OVFL` / `SO_RCVBUF`, a separate future concern). Priority-aware
*dropping* is therefore an SHM-link property by construction.

### 3. Per-priority drop attribution (always on)

`ShmRouterMetrics` gains `dropped_by_priority[8]` — a parallel breakdown of
`dropped_full` bucketed by the dropped frame's priority. It is maintained
unconditionally (whether or not a floor is set), so operators can see which
priority classes are being shed and confirm the floor is doing its job.
`dropped_full` (aggregate) and `dropped_full_per_peer` (Phase D2a) are
unchanged; the new array sums to `dropped_full`.

The floor is wired through `MixedRouterServer::set_priority_drop_floor` (SHM
link only) and exposed in the `router_server` demo via
`RIM_PRIORITY_DROP_FLOOR=<0..7>`.

## Consequences

### Positive

- Control-loop frames survive a low-priority flood: under backpressure the
  SHM router preferentially keeps high-priority traffic.
- RT placement is available without patching the module — env-gated hooks plus
  the documented systemd directives.
- Zero new dependencies; default behavior unchanged (floor 0, hooks off).
- Free observability: `dropped_by_priority` tells operators what is being shed.
- Backward compatible: every existing test stays green; the new behavior is
  opt-in.

### Negative / Neutral

- "Drop lowest first" is admission control, not eviction — a frame already
  queued is never displaced. Under sustained overload some low-priority frames
  may still get through between congestion episodes; the floor biases, it does
  not guarantee strict priority ordering.
- The policy is SHM-only. Datagram QoS would need kernel-queue introspection
  or a router-owned queue (rejected — adds the buffering this ADR avoids).
- `congested` is a single bool per peer, not an occupancy measure, so the floor
  engages on the binary "last send failed" signal rather than a high-water
  mark. This is deliberately simple; a hysteresis band can be added later if
  profiling shows flapping.

## Alternatives considered

### A. Per-priority multi-queue drain

Maintain N queues per peer and drain highest-priority first. Rejected for this
phase: adds router-owned buffering, reordering, and memory proportional to
priority levels × peers, and complicates the SPSC ring contract. The drop-first
policy delivers the "keep high-priority under load" goal at a fraction of the
complexity. Revisit if strict priority scheduling becomes a requirement.

### B. Unconditional priority floor (drop sub-floor always, even with room)

Simpler, but it sheds low-priority traffic even when the ring has space —
wasteful and surprising. Gating on `congested` means the floor only bites under
actual backpressure.

### C. In-process `SCHED_FIFO`

Rejected as a code hook (see Decision §1): a real-time policy belongs in the
deployment unit, not the binary. Documented as `LimitRTPRIO=` + wrapper.

### D. Link `libsystemd` / `libcap` for the RT knobs

Unnecessary: `mlockall` and `sched_setaffinity` are plain libc/glibc; the only
requirement is `_GNU_SOURCE` for the affinity declarations, handled in-header.

## Verification

```bash
make test-shm-backpressure   # incl. C7 QoS: per-priority sum, floor reserves
                             #   slot for high-priority, no-floor legacy control
make test-rt-hardening       # mlock / pin hooks: no-throw, input validation
make ci                      # full gate green

# Manual: run the router with the opt-in knobs under systemd or a shell:
RIM_MLOCK=1 RIM_CPU=2 RIM_PRIORITY_DROP_FLOOR=4 \
  ./build/ipc/test/router_server --config config/profiles/jetson_prod.toml
# logs: "mlockall: locked current + future pages", "pinned to core 2",
#       "priority drop floor = 4 (drop-lowest-first on backpressure)"
```
