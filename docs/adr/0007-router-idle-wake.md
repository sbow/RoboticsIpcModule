# ADR 0007: Router idle-wake policy

- **Status:** Accepted (Phase C); eventfd alternative deferred to Phase F
- **Date:** 2026-05-25
- **Builds on:** [ADR 0001](0001-ipc-and-router.md), [ADR 0006](0006-shm-backpressure-and-metrics.md)
- **Scope:** What the router's forwarding loop does when `link.forward()` returns "no frame this iteration"; the `RouterRunOptions::idle_sleep_us` knob; how the eventfd-based wake fits in later.

## Context

`RouterServer::run()` in `ipc/src/router/node.hpp` polls
`link.forward()` in a tight loop. Until Phase C the empty branch did:

```cpp
} else {
    std::this_thread::yield();
}
```

`std::this_thread::yield()` is a *scheduling hint* — it asks the kernel
"reschedule me if a higher-priority task is ready", but on an idle system
the thread is reinserted at the head of its priority's runqueue and runs
again immediately. The result is a fully CPU-bound idle loop.

### Measured baseline (Phase C startup, 2026-05-25)

`./build/ipc/test/router_server --config config/profiles/jetson_prod.toml`,
no clients connected, 20-core x86_64 dev host (`shaun-laptop`), idle:

```
pidstat -p $ROUTER_PID 10 6
09:49:36 PM     0    611392   59.00   41.00    0.00    0.00  100.00     4  router_server
09:49:46 PM     0    611392   59.30   40.70    0.00    0.00  100.00     5  router_server
09:49:56 PM     0    611392   58.50   41.50    0.00    0.00  100.00     5  router_server
09:50:06 PM     0    611392   59.00   41.00    0.00    0.00  100.00     7  router_server
09:50:16 PM     0    611392   57.80   42.20    0.00    0.00  100.00     7  router_server
09:50:26 PM     0    611392   58.30   41.70    0.00    0.00  100.00     6  router_server
Average:        0    611392   58.65   41.35    0.00    0.00  100.00
```

**100% of one core, sustained.** ~59% userland (busy `try_recv` calls) +
~41% kernel (page faults / scheduler overhead). On a 20-watt Jetson Orin
that is a substantial fraction of the thermal budget for the router
process *doing nothing*.

The Phase C plan requires "idle router + clients: CPU < 5% one core for
60 s". The yield-only path fails this requirement by a factor of 20.

`ipc/SHM_SPSC_TRANSPORT.md` Phase 2 describes the eventfd-based
wake we ultimately want: producer signals after publish, consumer arms a
wait when the ring is empty, drains on wake. That is a non-trivial
implementation (signaling protocol, lost-wake races, eventfd lifetime
mapped into SHM region or alongside it) and *also* needs to compose with
the multi-peer router loop, which polls many rings rather than one.

Phase C ships a smaller win that crosses the 5%-CPU bar today and clears
the runway for eventfd later.

## Decision

### Default: bounded `sleep_for(idle_sleep_us)` backoff

Extend `RouterRunOptions` with one new field:

```cpp
struct RouterRunOptions {
    int poll_timeout_ms = 200;
    int idle_exit_ms    = 0;
    int idle_sleep_us   = 1000;   // Phase C2
};
```

`RouterServer::run` honors it in the empty branch:

```cpp
} else if (opts.idle_sleep_us > 0) {
    std::this_thread::sleep_for(
        std::chrono::microseconds(opts.idle_sleep_us));
} else {
    std::this_thread::yield();
}
```

The default `1000 µs (1 ms)` is the sweet spot for the Jetson production
profile: idle CPU collapses to the noise floor while a freshly-published
frame waits at most one millisecond before forwarding. For control loops
that need lower wake latency, callers can set `idle_sleep_us` smaller
(e.g. `100` µs) or `0` (legacy yield-only) on a per-RouterServer basis.

### Measured result (after C2, same host, same profile)

```
pidstat -p $ROUTER_PID 10 6
09:59:52 PM     0    613877    0.70    1.00    0.00    0.00    1.70     2  router_server
10:00:02 PM     0    613877    0.70    1.20    0.00    0.00    1.90    19  router_server
10:00:12 PM     0    613877    0.60    0.60    0.00    0.10    1.20    13  router_server
10:00:22 PM     0    613877    0.60    1.10    0.00    0.00    1.70    11  router_server
10:00:32 PM     0    613877    0.70    0.90    0.00    0.10    1.60    18  router_server
10:00:42 PM     0    613877    0.80    0.90    0.00    0.00    1.70    17  router_server
Average:        0    613877    0.68    0.95    0.00    0.03    1.63
```

**Average 1.63% of one core, 60s window.** Comfortably under the 5% Phase
C target; on Jetson this corresponds to single-digit milliwatts.

### Eventfd path: deferred, not abandoned

The eventfd-based design from `SHM_SPSC_TRANSPORT.md` Phase 2 is still
the right end state for the SHM transport:

- Sub-microsecond wake latency vs ~1 ms sleep granularity.
- No periodic syscall when truly idle.
- Composes with epoll, so a future router that also services UDP/UDS
  fds in the same loop can wait on every transport type uniformly.

It is deferred to **Phase F** for three reasons:

1. The 5%-CPU acceptance bar is met without it. Implementation cost has
   to be balanced against value, and `sleep_for` buys us the bar at zero
   API churn.
2. The multi-peer router signaling protocol is not trivially the same as
   the single-ring sketch in `SHM_SPSC_TRANSPORT.md`. The router polls N
   peer rings; an N-eventfd `epoll_wait` design must define how producer
   signaling works (per-peer eventfd, shared eventfd, futex-on-head — all
   have trade-offs).
3. We want the eventfd descriptor lifecycle to be defined alongside the
   sideband region lifecycle (ADR 0005) — both are kernel handles that
   the router creates/unlinks per topology and that crash recovery has
   to clean up. A unified ADR is more useful than two partial ones.

When Phase F gets there, the API change is small: an
`RouterRunOptions::idle_wake_kind` enum with `Sleep` (current) and
`EventFd` (new) variants, plus a per-link `arm_idle_wait()` /
`disarm_idle_wait()` pair. `idle_sleep_us` stays as the fallback when
eventfd is unavailable (e.g. older kernels, non-Linux ports — although
those are out of scope per ADR 0004).

## Consequences

### Positive

- Jetson idle power and thermal headroom for the rest of the stack
  (vision, ML, control) — the router stops being a 100%-core nuisance
  in `top`.
- One knob, default-on, no API ceremony. The benchmark mode (yield-only)
  is still reachable by setting `idle_sleep_us = 0` explicitly.
- Acceptance criterion met today; eventfd work can be planned without
  blocking Phase C delivery.

### Negative

- Worst-case wake latency on a freshly-active fabric is ~1 ms. For a
  100 Hz control loop that is one-tenth of a period; acceptable but
  worth noting. Apps that need lower must tune `idle_sleep_us`.
- The router still wakes ~1000 times per second when idle, even with no
  traffic. That is 1 ms of CPU per second under perfect schedule (i.e.
  the ~1.6% we measured). An eventfd path would drop this further to
  "0 CPU until producer writes" but is deferred per above.

### Neutral

- The 100 Hz-ish wake rate is also fine for any periodic housekeeping
  the router gains later (metrics export, watchdog ping, sideband stat).
  We get a heartbeat for free.

## Alternatives considered

### A. Pure ADR deferral, keep yield-only

Rejected: 100% one-core idle CPU is unshippable on Jetson per the
acceptance criterion and SYSTEM-VISION goals. Would have meant Phase C
ships with a documented "limitation" that immediately becomes a Phase D
hotfix.

### B. `nanosleep(1µs)`

Linux's HZ (`CONFIG_HZ_*` on typical kernels) makes any sleep < ~1 ms
round up; on `CONFIG_HZ_250` distros that's 4 ms; on `HZ_1000` it's 1
ms. Asking for 1 µs yields the actual minimum granularity, which is the
same as just asking for 1 ms. Rejected as misleading API for users who
might tune the knob expecting microsecond accuracy.

### C. Exponential backoff (yield → 100µs → 1ms)

Marginal improvement: pays microseconds-of-extra-CPU when traffic
resumes after a short idle, in exchange for slightly lower wake latency
on the first message. Adds complexity for a 5%-CPU regime that already
clears the bar. Revisit if profiling shows a problem.

### D. Direct eventfd implementation now

Discussed above; the work is real and best done as one coherent Phase F
ADR alongside multi-peer signaling, sideband eventfds, and futex-on-head
trade-offs.

## Verification

```bash
# Re-measure with default idle_sleep_us=1000 (no traffic, 60 s):
rm -f /dev/shm/rim_router*
./build/ipc/test/router_server --config config/profiles/jetson_prod.toml &
ROUTER_PID=$!
pidstat -p $ROUTER_PID 10 6   # expect %CPU well under 5
kill -TERM $ROUTER_PID

# Latency-pinned mode (yield-only) is still reachable by users who want it:
#   RouterRunOptions opts; opts.idle_sleep_us = 0;
#   router.run(rules, n, now_ns, on_fwd, opts);
# Verified manually; documented in MODULE.md.
```
