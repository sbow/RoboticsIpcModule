# ADR 0014: Mixed-transport router — one process, heterogeneous links

- **Status:** Accepted
- **Date:** 2026-06-02
- **Builds on:** [ADR 0006](0006-shm-backpressure-and-metrics.md) (per-peer drop attribution the roll-up preserves), [ADR 0007](0007-router-idle-wake.md) (the idle-sleep / CPU dial the cooperative poll reuses), [ADR 0010](0010-router-timestamp-clock.md) (single process ⇒ single `timestamp_ns` source — no re-stamp), [ADR 0013](0013-per-topic-routing.md) (per-topic dispatch — `RouteTargets` are transport-agnostic peer ids, so it composes with mixed transport for free)
- **Closes:** [Phase H](../../robotics-ipc-module/plans/H-mixed-transport-router.md) and the single-host half of [post-phases review C11](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c11--mixed-transport-networks) (Option 2). Cross-host federation (Option 1b) stays parked as the stage-two follow-on.
- **Scope:** Lift the **single-transport-per-router-instance** constraint. A single router process can now serve SHM + UDS + UDP peers at once, delivering a frame received on one transport to a peer on another in **one in-process hop**. The templated single-transport path is untouched; the mixed path is opt-in, selected only when a profile's peers span more than one transport.

## Context

Through Phase G the router served exactly one transport per instance. `RouterServer<Link>` is templated on a single `Link` ([ADR Phase A devirtualization](0001-transport-concept.md)); `ShmRouterServer` iterates SHM rings, `DatagramRouterServer<Uds>` binds a UDS socket, `DatagramRouterServer<Udp>` a UDP socket. The transport kind is fixed at compile time and selected at startup from `router_listen.kind`.

A profile is free to *declare* mixed transports — the loader has accepted `local = "shm:…"` next to `local = "uds:…"` since B2 — but the router cannot *serve* them. The forward loop binds one link; a route whose destination lives on a different transport hits `ShmRouterLink::send_to_peer`, which throws `no channel for peer id N` because that peer was never in its channel set. The mismatch surfaced as the F1 dashboard limitation: `jetson_prod.toml` *wanted* SHM for the control-loop hot path and UDS for the stateful subscribers (recorder, dashboard, language bridges), but had to revert to **all-SHM** (commit `3797789`), forcing Node.js / Python consumers onto a SHM addon or a separate bridge daemon.

This is the deployment shape embedded robotics actually needs: a Jetson runs a sub-millisecond SHM control loop *and* IO-side subscribers that are naturally socket clients. Routing between them should be one hop in one process — not a two-hop bridge tax, not a second router instance with a duplicated route table.

The transport-layer counterpart to Phase G: Phase G added a *dispatch key* (`topic_id`); Phase H makes *dispatch span transports*. Both touch the hot path and the wire-facing schema.

## Decision

**Introduce a non-templated `MixedRouterServer` that holds one optional link per transport present in the topology and drives them from a single cooperative, single-threaded, non-blocking poll loop.** Egress is dispatched by a `peer_id → transport` map so route rules stay transport-agnostic. The entry point selects `MixedRouterServer` only when a profile's peers span more than one transport kind; homogeneous profiles keep the templated path unchanged.

### 1 — Split `forward()` into receive-and-resolve + send

The templated links combined three steps in one `forward()`: receive a frame, resolve `(source, targets)`, and send to each target on the *same* link. Cross-transport egress requires separating "pull the next frame and tell me who sent it" from "send", because the sending link is no longer the receiving link.

Both links gained two public methods (the combined `forward()` is retained and now composes them, so the templated path is byte-for-byte unchanged):

```cpp
// Receive-and-resolve, no egress. Stamps source + timestamp into `frame`.
// Returns the resolved source peer id, or kEndpointInvalid when nothing was
// available / the frame was truncated / the source was unknown.
uint8_t try_receive(RouterFrame& frame, uint64_t timestamp_ns);

// Egress half, now public so a different link's receive can drive this
// link's send. Bumps forwarded (SHM also bumps drop-on-full per ADR 0006).
void send_to_peer(uint8_t dest, const Buffer& payload);
```

`route_targets_for` ([ADR 0013](0013-per-topic-routing.md)) moves up to the *router*, which calls it once with `frame.topic_id()` and then dispatches each target to the owning link. Per-topic dispatch therefore composes with mixed transport with zero extra work: the router resolves targets (transport-agnostic peer ids), then resolves each target's transport at send time.

### 2 — `MixedRouterServer` (static dispatch, no vtables)

```cpp
class MixedRouterServer {
    std::optional<ShmRouterLink>           shm_;
    std::optional<DatagramRouterLink<Uds>> uds_;
    std::optional<DatagramRouterLink<Udp>> udp_;
    std::array<bool, 256>           peer_known_;      // dest guard
    std::array<PeerAddressKind, 256> peer_transport_; // egress dispatch
};
```

Only the links for transports actually present among the topology's peers are instantiated. The hot path stays **statically dispatched** — `optional`-per-transport, not a polymorphic `IpcLink` vtable (which would reintroduce the virtual dispatch Phase A removed). Egress is an `O(1)` array lookup `peer_transport_[dest]` → the owning link's `send_to_peer`.

### 3 — Cooperative single-threaded non-blocking poll

The mixed loop polls each present link's `try_receive` in round-robin in **one** thread; for each ready frame it resolves targets and sends; it idle-sleeps (per [ADR 0007](0007-router-idle-wake.md)) only when *every* link returned empty.

One thread is deliberate. The SHM egress rings are SPSC ([ADR 0006](0006-shm-backpressure-and-metrics.md)); a per-link-thread model would have multiple threads writing the same peer's rep-ring and would need egress serialization (a lock on the hot path). A single writer preserves the single-producer invariant for free. A single process also means a single `timestamp_ns` clock source ([ADR 0010](0010-router-timestamp-clock.md)) — no cross-boundary re-stamp.

### 4 — The datagram-latency trade-off and its bounds

The templated `DatagramRouterServer<T>` keeps its **blocking** `recvfrom()` (`SO_RCVTIMEO`): the kernel wakes it the instant a datagram lands. A cooperative single-threaded loop cannot block on one transport without starving the others, so `MixedRouterServer` polls datagram sockets **non-blocking** (`MSG_DONTWAIT`, via `Udp::try_recv` / `Uds::try_recv`). The cost is bounded and narrow:

- **Added datagram ingress latency ≤ `idle_sleep_us`** (default 1 ms), worst case ≈ one sleep interval, average ≈ half, and **only on the idle → first-frame pickup**. Under sustained traffic the loop keeps finding work each pass and never sleeps, so steady-state datagram latency collapses back toward syscall cost. This is a cold-pickup cost, not a throughput cost.
- **Tunable, not fixed.** `idle_sleep_us` is a `RouterRunOptions` knob; a latency-pinned mixed router lowers it or sets `0` (yield-only) for near-instant pickup at ~100 % of one core — the same CPU/latency dial [ADR 0007](0007-router-idle-wake.md) introduced.
- **SHM hot path unaffected.** SHM was always a non-blocking spin-poll; nothing about the control loop changes. The latency hit lands only on UDS/UDP ingress — which in the intended mixed shape (`jetson_mixed.toml`) carries the *stateful subscribers* (recorder, dashboard), exactly the peers that do not need sub-millisecond pickup.
- **The clean elimination is the parked eventfd doorbell.** SHM rings have no fd to `epoll`, so blocking on all sources at once is impossible without the [ADR 0007](0007-router-idle-wake.md) eventfd deferral. With per-ring eventfds the mixed loop could `epoll` datagram fds *and* SHM doorbells and block → instant kernel wakeup across every transport, erasing the idle-poll latency. This ADR records cooperative poll-with-sleep as the **interim** model and eventfd-driven `epoll` as the documented future optimization.

### 5 — Multi-listen `[router]` schema

A datagram peer can only be served if the router listens on that transport. SHM peers need no router-level listen (per-peer rings are derived from the topology); each datagram transport in use needs one listen socket. `[router]` keeps its single back-compatible `listen` key and gains optional `listen_uds` / `listen_udp`:

```toml
[router]
listen     = "shm:/rim_router"            # primary (SHM compute rings)
listen_uds = "uds:/tmp/rim_router.sock"   # UDS subscribers dial this
# listen_udp = "udp:0.0.0.0:19100"        # add when UDP peers are present
```

Resolution rule (back-compatible): when `listen` is itself a datagram address it *is* that transport's listen; `listen_uds` / `listen_udp` add the others. The loader then **validates** that every datagram peer has a matching router listen, rejecting the most common mixed-profile mistake at load time:

> `peer 'recorder' is a uds peer but [router] has no uds listen (set listen or listen_uds to a uds: address)`

— replacing the old silent-skip-then-runtime-crash with a clear startup error. A homogeneous profile (one transport) validates and loads exactly as before.

### 6 — Metrics roll-up

`MixedRouterServer` exposes aggregate `forwarded()` / `dropped_full()` summing whichever links exist; the per-link `metrics()` stays individually reachable for debugging. Per-peer drop attribution ([ADR 0006](0006-shm-backpressure-and-metrics.md)) is unchanged — only the SHM link can drop on a full ring.

## Alternatives considered

Carried from the [C11 options analysis](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c11--mixed-transport-networks):

### Option 1 / 1a / 1b — bridge daemons / SHM inter-router channel / declarative multi-router

A factory-generated bridge process (or a second router) stitches transports together.

- **Deferred, not rejected.** This is the right answer for **cross-host** federation and is the explicit stage-two follow-on stacked *on top of* Phase H. But for single-host it imposes a two-hop latency tax, a stateful bridge process, and a duplicated route table — all unnecessary when one process can hold both links. Phase H is single-host; Option 1b owns cross-host (and drags in the cross-host timestamp-epoch problem, [parked C8](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c8--cross-host-time-sync-ptp--ntp)).

### Option 2a — polymorphic `IpcLink` vtable interface

A virtual `IpcLink` base so the router holds a `vector<unique_ptr<IpcLink>>`.

- **Reject reason:** Clean, but reintroduces vtable dispatch on the hot path that Phase A deliberately devirtualized. `optional<link>`-per-transport keeps static dispatch and a closed transport set (three is the whole universe today). A virtual interface is a later refactor only if a fourth transport makes the trio unwieldy.

### Option 3 — peer-side dual-protocol bridging

Each peer speaks two protocols and bridges itself.

- **Reject reason:** Duplicates bridging logic per peer, doesn't help ingress, and hides the topology from the router. The bridging-by-fiat fallback only.

### Option 4 — kernel-assisted routing (XDP / io_uring / eBPF)

- **Reject reason:** Linux-kernel-version-specific, large scope, and breaks the header-only model. Explicitly closed.

### Per-link threads instead of a cooperative single thread

- **Reject reason:** Multiple threads writing the same peer's SPSC egress ring need a lock on the hot path. Cooperative single-thread poll preserves the single-producer invariant for free and reuses the existing idle-sleep loop. Listed in Phase H's **Do not**.

## Consequences

### Positive

- **The originally-intended deployment shape works.** `jetson_mixed.toml` runs SHM compute + UDS subscribers in one process, one hop — the F1 design that had to be reverted to all-SHM.
- **Backward compatible, opt-in.** The templated `RouterServer<T>` / `ShmRouterServer` / `DatagramRouterServer<T>` are untouched; a homogeneous profile keeps the templated path with zero new dispatch cost. The mixed path is selected only when a profile mixes transports.
- **Per-topic routing composes for free.** `RouteTargets` are peer ids; transport is resolved at send time. ADR 0013 dispatch is unaffected.
- **Load-time validation replaces a runtime crash.** A datagram peer with no matching router listen is now a clear startup error, not a `send_to_peer` throw mid-forward.
- **Static dispatch preserved.** No vtables on the hot path; egress is an array lookup.

### Negative

- **Bounded idle-pickup latency on datagram ingress** (≤ `idle_sleep_us`, idle-only, tunable) — see Decision §4. The templated single-transport datagram router keeps its instant blocking wake; only the mixed router pays this, and only on cold pickup. eventfd is the documented elimination.
- **No per-link failure isolation.** A bug in one link can take down the single mixed process — an Option-1 (separate-process) property, deferred with cross-host.

### Neutral

- **`RouterTopology` grew four fields** (`has_listen_uds` / `listen_uds` / `has_listen_udp` / `listen_udp`), defaulted off so every compile-time topology and homogeneous profile is unaffected.
- **`jetson_prod.toml` stays all-SHM** as the conservative variant; `jetson_mixed.toml` is the new mixed demonstrator (rather than mutating `jetson_prod.toml` in place — this preserves the all-SHM production default and its ring-sizing test, while still shipping a mixed profile end-to-end in `make ci`).

## Out of scope (parked / follow-on)

- **Cross-host federation (Option 1 / 1a / 1b)** — bridge daemons, SHM inter-router channels, the declarative `[[routers]]` multi-router schema, and the cross-host timestamp epoch. The explicit stage-two follow-on; not Phase H.
- **eventfd idle-wake** ([ADR 0007](0007-router-idle-wake.md) deferral) — the natural future optimization that erases the idle-pickup latency; not required for correctness.
- **Priority-aware drain order across links** — how QoS interacts with round-robin link polling belongs with [C7](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c7--real-time--production-knobs-mlockall-cpu-pinning-sched_fifo-priority-aware-qos).
- **Polymorphic `IpcLink`** (Option 2a) — only if a fourth transport arrives.

## Verification

- `make test-ipc-unit`:
  - `topology_loader_test` — new cases: `listen_uds` / `listen_udp` parse and set `has_listen_*`; the primary `listen` doubles as its own transport's listen; a uds/udp peer with no matching router listen is rejected; `listen_uds` of the wrong kind is rejected; a homogeneous profile is **not** flagged mixed (backward-compat gate); `jetson_mixed.toml` loads from file with SHM compute + UDS subscribers.
  - `routing_test` — unchanged and green (transport-agnostic targets; the refactor preserved `route_targets_for`).
- `mixed_transport_test` (new integration scenario) — a `MixedRouterServer` with SHM + UDS peers forwards an end-to-end **SHM → router → UDS → router → SHM** chain (1000 frames): a SHM publisher's frames reach a UDS node (SHM-ingress / UDS-egress), which relays them back so they reach a SHM subscriber (UDS-ingress / SHM-egress). Asserts ~100 % delivery both directions, that the UDS→SHM hop re-resolves the source peer id, in-order SHM delivery, and the metrics roll-up counts both hops.
- All existing single-transport tests (`router_test` UDS/UDP/SHM, `burst_sensor`, `slow_recorder`, `topic_dispatch`, `fault_injection`, `router_restart`, `profile_switch`) stay green — the templated path is unchanged.
- Manual smoke: `router_server --config config/profiles/jetson_mixed.toml` selects the mixed router, binds the SHM rings + the UDS listen, and logs `mixed-transport router serving: SHM UDS`.

## References

- [Phase H plan](../../robotics-ipc-module/plans/H-mixed-transport-router.md): the deliverable this ADR's H1 item specifies
- [Post-phases review C11](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c11--mixed-transport-networks): the full options analysis + decision rubric (Option 2 first, Option 1b cross-host follow-on)
- [ADR 0006 — SHM backpressure and metrics](0006-shm-backpressure-and-metrics.md): the SPSC egress invariant + per-peer drop attribution the cooperative single-thread loop preserves
- [ADR 0007 — Router idle wake](0007-router-idle-wake.md): the idle-sleep CPU/latency dial the poll reuses, and the eventfd deferral that is the future elimination of the idle-pickup latency
- [ADR 0010 — Router timestamp clock](0010-router-timestamp-clock.md): single process ⇒ single clock source (no cross-boundary re-stamp)
- [ADR 0013 — Per-topic routing](0013-per-topic-routing.md): transport-agnostic `RouteTargets` that compose with mixed transport
- `ipc/src/router/mixed_router_server.hpp` — `MixedRouterServer`, `topology_is_mixed`
- `ipc/src/router/link.hpp`, `shm_router_link.hpp` — the `try_receive` + public `send_to_peer` split
- `ipc/src/ipc/datagram.hpp` — `Udp::try_recv` / `Uds::try_recv` (non-blocking `MSG_DONTWAIT`)
- `ipc/src/router/topology_loader.hpp` — multi-listen parse + per-transport listen validation
- `config/profiles/jetson_mixed.toml` — the mixed deployment template; `config/profiles/jetson_prod.toml` — the preserved all-SHM variant
