# ADR 0013: Per-topic routing — `topic_id` as a dispatch key

- **Status:** Accepted
- **Date:** 2026-06-01
- **Builds on:** [ADR 0008](0008-router-frame-v2.md) (the v2 frame already carries `topic_id` as a u16 wire field), [ADR 0006](0006-shm-backpressure-and-metrics.md) (deterministic per-peer fan-out the dispatch must preserve), [closed C5 Scope A](../../robotics-ipc-module/plans/post-phases-robotics-review.md#closure--scope-a-lift-the-2-destination-cap-2026-05-28) (`kMaxRouteDests = 8` + `make_route`), [closed C5 Scope B](../../robotics-ipc-module/plans/post-phases-robotics-review.md#closure--scope-b-declarative-topic-registry-2026-05-28) (the declarative `[[topics]]` registry this routing key references)
- **Closes:** [Phase G](../../robotics-ipc-module/plans/G-declarative-routing.md) (per-topic dispatch) and [post-phases review C5 Scope C](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c5--declarative-transport-layer-gaps) (promoted to Phase G during the Scope A+B closure)
- **Scope:** Promote `topic_id` from an opaque wire field + declarative-only registry entry to a **first-class routing key**. The router can dispatch on `(source, topic_id)`, not just `source`. No wire-format change; no new frame field; backward compatible at the TOML surface.

## Context

Through Phase F the router dispatched on exactly one key: the **source peer id**. `route_targets_for(rules, count, source)` walked a `RouteRule[]` table, first-match-wins, and returned the fan-out set for whichever rule matched the source. `topic_id` rode along in the 64 B frame ([ADR 0008](0008-router-frame-v2.md)) but the router never read it; [closed C5 Scope B](../../robotics-ipc-module/plans/post-phases-robotics-review.md#closure--scope-b-declarative-topic-registry-2026-05-28) added a declarative `[[topics]]` registry so bridges could *name* and *validate* topics, but explicitly stopped short of letting it drive dispatch.

That leaves a real gap for the bridges Phase F just shipped. A consumer that only wants one stream from a multi-topic publisher has no declarative way to say so:

- A dashboard wants `imu_proprio` (topic 100) from the sensor but not every frame the sensor emits.
- A MAVLink command consumer ([ADR 0011](0011-device-bridge-transports.md)) wants a specific `msgid`, not the controller's entire output.
- A vision/ML consumer ([ADR 0012](0012-sideband-memory-class.md)) wants one camera stream / tensor topic, not all of peer 4's frames.

Today the only workarounds are source-side process forking (one publisher peer id per topic — wastes peer-id space and ring memory) or subscriber-side filtering (every consumer receives every frame and discards most — wastes the hot path's fan-out budget and SHM ring slots). Both push a *routing* concern into the wrong layer.

The fix is to let the deployment profile express `(source, topic_id) → destinations`, which it already half-can: Scope B gave us the topic registry; this ADR makes the route table reference it.

## Decision

**`RouteRule` gains an optional `topic_id` selector, and `route_targets_for` becomes a two-dimensional first-match-wins lookup on `(source, topic_id)`.** A rule with the `kRouteTopicAny` sentinel (the default) matches every topic — identical to today's source-only behavior.

### The match rule

A `RouteRule` matches a frame when **both** hold:

1. `rule.source == frame.source`, **and**
2. `rule.topic_id == kRouteTopicAny` (matches any topic) **or** `rule.topic_id == frame.topic_id`.

The table is walked in declaration order; the **first** matching rule wins and supplies the fan-out set. This is the same first-match-wins contract Phase D relied on — a topic-specific rule placed *before* a source-only catch-all for the same source takes precedence, and the lookup stays deterministic.

```cpp
// ipc/src/router/routing.hpp
constexpr uint16_t kRouteTopicAny = 0xFFFF;   // reserved sentinel

struct RouteRule {
    uint8_t  source     = kEndpointInvalid;
    uint8_t  dest_count = 0;
    uint16_t topic_id   = kRouteTopicAny;      // Phase G
    std::array<uint8_t, kMaxRouteDests> dest{};
};

inline RouteTargets route_targets_for(
    const RouteRule* rules, size_t rule_count,
    uint8_t source, uint16_t topic_id = kRouteTopicAny) {
    for (size_t i = 0; i < rule_count; ++i) {
        if (rules[i].source != source) continue;
        if (rules[i].topic_id != kRouteTopicAny
         && rules[i].topic_id != topic_id) continue;
        /* …copy dest[] into RouteTargets, return… */
    }
    return {};
}
```

The two hot-path callers — `link.hpp` (datagram) and `shm_router_link.hpp` (SHM) — pass `frame.topic_id()`, which is already in hand at the point of dispatch (the router sets `source` and `timestamp_ns` on forward but never touches `topic_id`).

### Why `kRouteTopicAny = 0xFFFF`

`topic_id` is a u16, so a sentinel must reserve a value rather than add a field. `0xFFFF` is reserved as "match any" at the topology level — the loader **rejects** `topic = 0xFFFF` as a literal in a profile, exactly as peer ids `0` and `255` are reserved. This keeps `RouteRule` a flat POD (no extra `bool has_topic_selector` + padding), keeps `make_route` constexpr, and keeps the hot path one extra compare that a source-only rule short-circuits.

(It shares the numeric value of `kSidebandIdxNone`, but the two live in unrelated namespaces — a sideband-table index vs. a routing topic key — so there is no coupling.)

### TOML surface

`[[routes]]` gains an optional `topic` field:

```toml
# imu_proprio (topic 100) from the sensor reaches the dashboard too…
[[routes]]
source = 1
topic  = 100
dest   = [2, 3, 8]

# …every other sensor topic goes only to controller + recorder.
[[routes]]
source = 1
dest   = [2, 3]
```

- **Omitting `topic`** yields `kRouteTopicAny` → source-only semantics. Every Phase A–F profile loads unchanged.
- The loader **resolves `topic` against `[[topics]]`** (a second pass, since topics are parsed after routes) and rejects an unknown id with `route topic id N does not match any [[topics]] entry` — mirroring the existing "route dest does not match any peer" rejection. The topic registry stays the source of truth.
- `topic = 0xFFFF` is rejected (reserved sentinel); valid topics are `0..65534`.

### `make_topic_route` factory

Compile-time topologies get a sibling to `make_route`:

```cpp
constexpr RouteRule r = make_topic_route(1, 100, 2, 3); // (source 1, topic 100) -> {2, 3}
```

`make_route` is untouched (source-only, `kRouteTopicAny`), so every existing call site compiles and behaves identically.

## Alternatives considered

### A — Separate `[[topic_routes]]` array next to `[[routes]]`

Keep source-only and per-topic rules in visually distinct TOML arrays.

- **Reject reason:** Doubles the lookup machinery (two tables, two walks, a precedence rule *between* them) for no expressive gain. A single ordered table with an optional selector already expresses every topology — including "topic X overrides, everything else falls through" — via first-match-wins. One table, one walk, one precedence rule.

### B — Compound `(source, topic)` key with multi-match union

Let *every* matching rule contribute to the fan-out (union of all matches) instead of first-match-wins.

- **Reject reason:** Breaks the deterministic single-fan-out contract `ShmRouterLink` and the per-peer drop-attribution metrics ([ADR 0006](0006-shm-backpressure-and-metrics.md)) rely on. Multi-match also makes "send topic X everywhere *except* peer Y" awkward (you'd need negative rules). First-match-wins keeps the existing mental model and the existing metrics semantics. Topic-driven multi-routing is explicitly parked as a follow-on.

### C — Move dispatch into the topic registry (`[[topics]].dest = [...]`)

Collapse routing into the topic table: each `[[topics]]` entry carries its own destination list.

- **Reject reason:** Collapses two concepts (a topic's *identity / schema* vs. a topic's *routing*) into one, and can't express source-dependent routing — "topic 100 from sensor goes to A; topic 100 from a replay peer goes to B" — without re-introducing a source key anyway. It also breaks Scope B's invariant that the registry is **declarative-only** (names + schema, never behavior). Keeping routing in `[[routes]]` and identity in `[[topics]]` preserves both contracts; the route merely *references* the registry.

## Consequences

### Positive

- **Declarative per-topic subscription.** A profile expresses "peer X sees only topic T from source S" without process forking or subscriber-side filtering. The bridges shipped in F3–F5 and the device bridges in ADR 0011/0012 can now be wired precisely.
- **Backward compatible, opt-in per rule.** Omitting `topic` is exactly today's behavior; an operator migrates one rule at a time. Every shipped profile and every `make_route` call site is unaffected.
- **Wire format untouched.** `topic_id` was already in the frame ([ADR 0008](0008-router-frame-v2.md)); Phase G only adds a *read* of it on the forward path. Subscribers need no recompile.
- **Deterministic and branch-light.** First-match-wins is preserved; a source-only rule costs one short-circuited compare more than before. No measurable hot-path change.
- **Registry-validated.** Routes can't reference a topic the deployment never declared — the loader catches it at startup, same as an unknown peer id.

### Negative

- **Rule ordering now carries semantics for multi-rule sources.** A topic-specific rule must precede the source-only catch-all for the same source, or the catch-all shadows it. This is the inherent cost of first-match-wins; it's documented, and the loader could grow a "shadowed rule" lint later if it bites.
- **No multi-match / wildcards in this cut.** A frame matches exactly one rule. Topic-driven fan-out via multiple rules, and pattern/wildcard topic matching, are deliberately parked (see Out of scope).

### Neutral

- **`RouteRule` grew by 2 bytes** (the u16 selector). It's a config-time table, not a hot-path frame; the size is irrelevant, and the struct stays a flat POD.
- **The topic registry stays declarative-only for *registration*.** Phase G only makes the *route table* read it at load; `[[topics]]` itself still declares names + schema, never destinations (Scope B contract intact).

## Out of scope (parked / elsewhere)

- **Priority-aware QoS** (former C5 Scope D) — merged into [C7 — Real-time / production knobs](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c7--real-time--production-knobs-mlockall-cpu-pinning-sched_fifo): QoS and RT pinning both address latency-under-contention, a different problem domain from the shape-of-the-config that Phase G settles.
- **Topic-level delivery attributes** (`reliability`, `history_depth` à la ROS 2) — a topic's *dispatch* is Phase G; a topic's *delivery semantics* belongs with the C7 union.
- **Wildcard / pattern topic matching** (`topic = "imu_*"`, regex) — first cut is exact u16 match. Patterns are a follow-on phase, not Phase G ("do not reach for a query language").
- **Topic-driven multi-routing** (one frame fanning out via several matching rules) — keeps first-match-wins; multi-match is a follow-on.

## Verification

- `make test-ipc-unit`:
  - `routing_test` — new cases: a topic-specific rule matches its topic and is skipped for others; a `kRouteTopicAny` rule matches every topic; first-match-wins across mixed topic / source-only rules; ordering precedence (topic-specific before catch-all). All pre-Phase-G cases stay green (the 3-arg `route_targets_for` calls keep source-only semantics via the defaulted sentinel) — the backward-compatibility gate.
  - `topology_loader_test` — new cases: `topic = N` parses onto `RouteRule::topic_id`; a route referencing an undeclared topic id is rejected; `topic = 0xFFFF` is rejected; a route with no `topic` defaults to `kRouteTopicAny`; the `x86_dev.toml` per-topic demonstration loads with the expected rule shape.
- `topic_dispatch_test` (new integration scenario) — one sensor publishes three different `topic_id`s over SHM; three subscribers each declare a per-topic route; each subscriber receives **only** its declared topic. Proves the `link` wiring reads `frame.topic_id()` correctly end-to-end, not just the unit-level lookup.
- `make all` clean; `make test-router` UDS/UDP/SHM green (the demo route table is source-only `make_route`, so its behavior is unchanged).

## References

- [ADR 0008 — RouterFrame v2](0008-router-frame-v2.md): defines `topic_id` as a u16 wire field — the key this ADR promotes to dispatch
- [ADR 0006 — SHM backpressure and metrics](0006-shm-backpressure-and-metrics.md): the deterministic per-peer fan-out + drop attribution that first-match-wins preserves
- [Phase G plan](../../robotics-ipc-module/plans/G-declarative-routing.md): the deliverable this ADR's G1 item specifies
- [Post-phases review C5](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c5--declarative-transport-layer-gaps): Scope A + B closure trail and the Scope C → Phase G promotion
- [ADR 0011 — Device-bridge transports](0011-device-bridge-transports.md) and [ADR 0012 — Sideband memory_class](0012-sideband-memory-class.md): the F4/F5 bridges that gain precise per-topic subscription from this change
- `ipc/src/router/routing.hpp` — `RouteRule`, `kRouteTopicAny`, `make_topic_route`, `route_targets_for`
- `ipc/src/router/topic_table.hpp` — the `[[topics]]` registry (Scope B) that routes reference
