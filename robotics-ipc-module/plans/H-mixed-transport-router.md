# Phase H — Mixed-transport router (single-host heterogeneous links)

**Skill:** `@ipc-robotics-phase-h` (install after adding skill file)
**Depends on:** A (the templated `RouterServer<Link>` + `RouterLink` concept), B2 (topology loader — already accepts mixed transports), D2a (per-peer metrics the roll-up must preserve), **closed C5 Scope A** (`kMaxRouteDests`, `make_route`), **delivered [Phase G](G-declarative-routing.md)** (per-topic dispatch — `RouteTargets` are transport-agnostic peer ids, so per-topic routing composes with mixed transport for free).
**Read:** [post-phases-robotics-review.md §C11](post-phases-robotics-review.md#c11--mixed-transport-networks) for the full options analysis (3 options + variants + decision rubric) this plan executes, and [docs/deployment-profiles.md §Known limitations](../../docs/deployment-profiles.md) for the F1 workaround this phase removes.

> **Provenance.** Phase H is the **promotion of parked C11 ("mixed-transport networks")** into a first-class phase plan, scoped to **Option 2 (single-host mixed-transport router instance)** per the C11 decision rubric's recommended first stage. The C11 analysis recommends a two-stage migration: **Option 2 first** (single-host heterogeneous links — this phase), then **Option 1b** (declarative multi-router + bridge daemons for cross-host federation) stacked on top when multi-host fanout becomes necessary. Phase H is stage one only; cross-host is explicitly out of scope (see below).

## Objective

Lift the **single-transport-per-router-instance** constraint. Today `RouterServer<T>` is templated on one `Transport`; a profile that mixes SHM + UDS + UDP peers loads fine but **crashes the forward loop** on the first cross-transport route (`ShmRouterLink::send_to_peer` throws on a peer it has no channel for). Phase H introduces a `MixedRouterServer` that holds a heterogeneous set of links and dispatches each forwarded frame to the link that owns the destination peer — so one router process can serve SHM (control-loop hot path) + UDS (stateful subscribers: recorder, dashboard, language bridges) + UDP (cross-host / HIL) **at the same time**, which is the deployment shape F1 originally implied and had to revert.

## Why this is a phase, not a sub-scope

| Surface | Change |
|---------|--------|
| Forwarding model | **Split `forward()`** into receive-and-resolve (no send) + an explicit router-level send, so a frame received on link A can target peers on link B |
| Server type | New non-templated **`MixedRouterServer`** holding `optional<ShmRouterLink>` + `optional<DatagramRouterLink<Uds>>` + `optional<DatagramRouterLink<Udp>>` |
| Egress dispatch | New `peer_id → TransportKind` map; `send_to_peer(dest)` routes to the owning link |
| Threading / poll model | Datagram links must become **non-blocking** so a single thread can cooperatively poll all links (preserves SPSC egress invariant — see ADR) |
| TOML schema | `[router]` gains **multiple listen endpoints** (one per datagram transport present); SHM rings stay per-peer |
| Metrics | **Roll-up** across heterogeneous links behind one `metrics()` surface |
| Profiles | At least one profile (`jetson_prod.toml`) moves to the originally-intended **mixed** shape |
| ADR | **New ADR** — the de-templating + forward-split is an architectural commitment, not an implementation refinement |

This is the transport-layer sibling of the routing-layer change Phase G made: Phase G added a *dispatch key*; Phase H makes *dispatch span transports*. Both touch the hot path and the wire-facing schema — phase-shaped work.

## Principles

- **Backward compatible.** The single-transport `RouterServer<T>` template, `ShmRouterServer`, and `DatagramRouterServer<T>` stay exactly as they are. A homogeneous profile keeps using the templated path (zero behavior change, zero new dispatch cost). `MixedRouterServer` is the **opt-in** path the entry point selects only when a profile declares more than one transport.
- **Single forwarding hop.** Cross-transport delivery is one hop (publisher → router → subscriber), same as same-transport. No bridge daemon, no two-hop tax, no re-stamping decision (single process = single `timestamp_ns` source per [ADR 0010](../../docs/adr/0010-router-timestamp-clock.md)).
- **One thread, cooperative non-blocking poll.** The mixed loop polls each link in round-robin in **one** thread and sleeps (idle backoff per [ADR 0007](../../docs/adr/0007-router-idle-wake.md)) only when *all* links are idle. This preserves the SHM **SPSC single-producer invariant** on every egress ring without locks — a per-link-thread model would need egress serialization. Datagram recv becomes non-blocking (`MSG_DONTWAIT` / `SO_RCVTIMEO = 0`) to fit a cooperative poll.
- **The schema already half-supports this.** The loader accepts mixed transports today; it just doesn't validate that the router can serve them. Phase H makes mixed profiles *valid and served*, and adds the validation that was missing (every peer's transport must have a matching router listen endpoint).
- **Routes stay transport-agnostic.** `RouteRule` / `RouteTargets` reference peer ids, never transports. The transport is resolved at **send** time from `PeerEntry::local.kind`. Phase G's `(source, topic_id)` dispatch is unaffected — it resolves targets, then the mixed server resolves each target's transport.

## Deliverables

### H1 — ADR: mixed-transport router

`docs/adr/0014-mixed-transport-router.md` (next-available — 0013 was taken by [per-topic routing](../../docs/adr/0013-per-topic-routing.md) during Phase G). Records:

- The decision: **Option 2 — single-process `MixedRouterServer` with cooperative single-threaded non-blocking poll** and `peer_id → transport` egress dispatch.
- The `forward()` split (receive-and-resolve vs. send) and why it's required (cross-link egress).
- The cooperative-poll-vs-per-link-threads decision and why cooperative wins (SPSC egress invariant preserved without locks; single timestamp source; matches the existing idle-sleep loop).
- **The datagram-latency trade-off this introduces, and its bounds.** The templated `DatagramRouterServer<T>` keeps its blocking `recvfrom()` (`SO_RCVTIMEO`), so the kernel wakes it the instant a datagram lands (~syscall-return latency). `MixedRouterServer` must instead poll datagram sockets **non-blocking** (`MSG_DONTWAIT` / `SO_RCVTIMEO = 0`) so one thread can service SHM + UDS + UDP without one transport starving another. The cost:
  - **Added datagram ingress latency is bounded by `idle_sleep_us`** (default 1 ms, [ADR 0007](../../docs/adr/0007-router-idle-wake.md)) — worst case ≈ one sleep interval, average ≈ half — and **only on the idle → first-frame wakeup**. Under sustained traffic the loop never sleeps (it keeps finding work each pass), so steady-state datagram latency collapses back toward syscall cost. This is a cold-pickup cost, not a throughput cost.
  - **Tunable, not fixed.** `idle_sleep_us` is a `RouterRunOptions` knob; a latency-pinned mixed router can lower it or set `0` (yield-only) for near-instant pickup at ~100 % of one core — the same CPU/latency dial ADR 0007 introduced.
  - **SHM hot path unaffected.** SHM was always non-blocking spin-poll; the control loop is unchanged. The latency hit lands only on UDS/UDP ingress — which in the intended mixed `jetson_prod` shape carries the *stateful subscribers* (recorder, dashboard), exactly the peers that do not need sub-millisecond pickup.
  - **The clean elimination is the parked eventfd doorbell.** SHM rings have no fd to `epoll`, so blocking on all sources at once is impossible without the [ADR 0007](../../docs/adr/0007-router-idle-wake.md) eventfd deferral. With per-ring eventfds, the mixed loop could `epoll` datagram fds *and* SHM doorbells and block → instant kernel wakeup across every transport, erasing the idle-poll latency. ADR 0014 records cooperative poll-with-sleep as the **interim** model and eventfd-driven `epoll` as the documented future optimization.
- The multi-listen `[router]` schema extension.
- Backward-compatibility guarantee (templated single-transport routers untouched; mixed is opt-in).
- Alternatives considered and rejected (carried from [C11 §Options](post-phases-robotics-review.md#c11--mixed-transport-networks)):
  - **Option 1 / 1a / 1b — factory-generated bridge daemons / SHM inter-router channel / declarative multi-router.** Two-hop latency tax + stateful bridge + route-table duplication. **Deferred, not rejected** — Option 1b is the explicit cross-host follow-on stacked *on top of* Phase H. Phase H is single-host.
  - **Option 2a — polymorphic `IpcLink` vtable interface.** Clean, but reintroduces vtable dispatch on the hot path that Phase A deliberately devirtualized. The `optional<link>`-per-transport approach keeps static dispatch.
  - **Option 3 — peer-side dual-protocol bridging.** Duplicates bridging logic per peer, doesn't help ingress, hides the topology. The bridging-by-fiat fallback only.
  - **Option 4 — kernel-assisted routing (XDP / io_uring / eBPF).** Linux-specific, large scope, breaks the header-only model. Explicitly closed.

### H2 — Link-layer surgery + `MixedRouterServer`

`ipc/src/router/link.hpp`, `shm_router_link.hpp`, `link_concept.hpp`, and a new `mixed_router_server.hpp`:

- **Split forwarding.** Extract the receive-and-resolve half of each link's `forward()` into a method that returns `ForwardResult` (source + targets) **and** exposes the received frame bytes **without sending**. The existing combined `forward()` is kept (composes resolve + the per-link send loop) so the templated `RouterServer<Link>` path is byte-for-byte unchanged.
- **Non-blocking datagram recv.** Add a non-blocking poll mode to `DatagramRouterLink` (`MSG_DONTWAIT` or `SO_RCVTIMEO = 0`) so it returns immediately when no datagram is queued. `ShmRouterLink::forward` is already non-blocking (poll-then-return), so no change there beyond the split.
- **`MixedRouterServer`** (non-templated, header-only): holds `std::optional<ShmRouterLink>`, `std::optional<DatagramRouterLink<Uds>>`, `std::optional<DatagramRouterLink<Udp>>`, instantiating only the links for transports present among the topology's peers. Builds a `std::array<TransportKind, 256>` `peer_transport_` map from `PeerEntry::local.kind`. Its `run()`-equivalent loop: poll each present link's receive-and-resolve; for each target dest, look up `peer_transport_[dest]` and call the owning link's `send_to_peer(dest, buf)`; idle-sleep only when all links returned empty.
- **`RouterLink` concept** (`link_concept.hpp`): add the non-blocking receive-and-resolve + standalone `send_to_peer` requirements so both links continue to satisfy the concept and `MixedRouterServer` can be written against it generically where useful.
- **Metrics roll-up:** `MixedRouterServer::metrics()` returns an aggregate view summing the present links' counters (forwarded / dropped / recv_truncated / recv_unknown_source / per-peer drops). The per-link metrics stay individually reachable for debugging.

### H3 — Multi-listen schema + loader

`ipc/src/router/topology_loader.hpp` + `[router]` schema:

- `[router]` gains the ability to declare **one listen endpoint per datagram transport** that has peers (the UDS socket and/or the UDP socket the router binds to receive from those peers). SHM peers need no router-level listen beyond their per-peer rings. Exact spelling is an H1 design call — candidates: parallel keys (`listen_uds = "uds:/…"`, `listen_udp = "udp:host:port"`) alongside the existing `listen`, or a `[[router.listen]]` array of addresses. The single `listen` key stays valid (homogeneous profiles unchanged).
- **Validation:** for every transport kind present among `[[peers]]`, the `[router]` table must declare a matching listen endpoint; a peer whose transport has no router listen is rejected at load time (`peer N transport <kind> has no matching [router] listen endpoint`) — replacing today's silent-skip-then-runtime-crash with a clear load-time error.
- A homogeneous profile (one transport) continues to validate and load exactly as today.

### H4 — Profile(s)

`config/profiles/`:

- `jetson_prod.toml` moves to the **originally-intended mixed shape** (the F1 design that was reverted to all-SHM in commit `3797789`): SHM for compute peers (sensor / controller / vision / ml — control-loop hot path), UDS for the stateful subscribers (recorder, dashboard_feed), demonstrating mixed transport end-to-end on the production profile. The all-SHM variant is preserved (renamed or commented) for hosts that want it.
- At least the mixed profile loads, binds all its listen endpoints, and forwards a cross-transport route in `make ci`.

### H5 — Tests

- **New `ipc/test/mixed_transport_test.cpp`** (integration): a `MixedRouterServer` with SHM peers + UDS peers; a SHM publisher's frame routed to a UDS subscriber **and** a UDS publisher's frame routed to a SHM subscriber; assert cross-transport delivery counts and that same-transport routes still work in the same router.
- `topology_loader_test` — new cases: multi-listen `[router]` parses; a peer whose transport lacks a router listen is rejected; a homogeneous profile still loads (backward-compat gate).
- `profile_switch_test` — exercise the mixed `jetson_prod.toml` bind + a cross-transport forward.
- Existing single-transport `router_test` (UDS / UDP / SHM) + all integration tests stay green (backward-compatibility gate — the templated path is untouched).

### H6 — Documentation

- `docs/adr/0014-mixed-transport-router.md` (H1).
- `docs/deployment-profiles.md` — new §Mixed-transport routing section; flip the C11 "single-transport-per-router" §Known limitations entry from a hard limitation to **resolved (Phase H)**; document the multi-listen `[router]` schema.
- `ipc/MODULE.md` — `MixedRouterServer` row + `mixed_router_server.hpp` entry + ADR 0014 cross-link; note the forward() split in the link rows.
- `docs/robotics-reference-layout.md` — per-deployment shapes updated (Jetson is now SHM-compute + UDS-subscribers); scope-vs-deferred + summary references updated.
- `robotics-ipc-module/plans/post-phases-robotics-review.md` — C11 entry marked **closed (single-host) / cross-host follow-on parked**, following the Scope-A/B/C closure template; the cross-host Option 1b explicitly remains parked.

## Out of scope (parked / follow-on)

- **Cross-host federation (Option 1 / 1a / 1b).** Bridge daemons, SHM inter-router channels, and the declarative `[[routers]]` multi-router schema that stitches per-host routers together over UDP. This is the **explicit stage-two follow-on** the C11 rubric names — stacked on top of Phase H once multi-host / sim_cloud federation becomes necessary. It also drags in the cross-host timestamp-epoch problem ([parked C8](post-phases-robotics-review.md#c8--cross-host-time-sync-ptp--ntp) + [ADR 0010](../../docs/adr/0010-router-timestamp-clock.md)). Not Phase H.
- **Per-link failure isolation.** A bug in one link can still take down the single mixed process (the C11 rubric notes Option 1 wins on isolation). Process-level isolation is an Option-1 property, deferred with cross-host.
- **Polymorphic `IpcLink` vtable interface** (Option 2a) — the `optional`-per-transport approach is the chosen implementation; a virtual interface is a later refactor only if a fourth transport makes the trio unwieldy.
- **eventfd idle-wake** (the [ADR 0007](../../docs/adr/0007-router-idle-wake.md) deferral) — Phase H keeps the idle-sleep poll. eventfd-driven wake of the cooperative loop is a natural *future* optimization but is not required for correctness and stays parked.
- **Priority-aware drain order across links** — how QoS interacts with round-robin link polling belongs with [C7](post-phases-robotics-review.md#c7--real-time--production-knobs-mlockall-cpu-pinning-sched_fifo-priority-aware-qos), not Phase H.

## Review checklist

- [ ] Templated single-transport `RouterServer<T>` / `ShmRouterServer` / `DatagramRouterServer<T>` are byte-for-byte unchanged in behavior (all existing `router_test` + integration cases green).
- [ ] A mixed SHM + UDS profile binds all listen endpoints and forwards a cross-transport route end-to-end.
- [ ] Loader rejects a peer whose transport has no matching `[router]` listen endpoint (replaces the silent-skip-then-crash).
- [ ] SPSC egress invariant preserved (single thread writes each peer's ring; no data races under `make test-soak` / leak-check).
- [ ] `make ci` clean; new ADR 0014 linked from `docs/adr/` and from this plan.
- [ ] `mixed_transport_test` + updated `topology_loader_test` + `profile_switch_test` all green.

## Acceptance

```bash
make all
make ci          # full unit + integration + router + leak-check gate
# Manual smoke: a mixed profile (SHM compute + UDS subscribers)
./build/ipc/test/router_server --config config/profiles/jetson_prod.toml &
# Start a SHM sensor and a UDS recorder; publish from the sensor and assert
# the UDS recorder receives the SHM-originated frame (cross-transport hop).
```

## Do not

- Break the templated single-transport path. `RouterServer<Link>` stays; `MixedRouterServer` is additive and opt-in.
- Add per-link threads (egress SPSC contention → locks on the hot path). Cooperative single-thread poll is the decision.
- Reach for vtables on the hot path (Option 2a). Static `optional`-per-transport dispatch.
- Solve cross-host in this phase. Single-host mixed transport only; cross-host is the Option 1b follow-on.
- Re-stamp `timestamp_ns` at a boundary (single process = single clock source; the re-stamp problem is a cross-host / bridge concern that Phase H does not introduce).

## Session prompt

```
Execute Phase H from robotics-ipc-module/plans/H-mixed-transport-router.md.
Read post-phases-robotics-review.md §C11 (the options analysis + decision
rubric this plan executes) and the ADR drafted in H1. Update STATUS.md.
```
