# Phase G — Declarative routing (per-topic dispatch)

**Skill:** `@ipc-robotics-phase-g` (install after adding skill file)
**Depends on:** B2 (topology loader), D1 (`route_targets_for` semantics), F2 (Python bridge — first real consumer of `topic_id`), **closed C5 Scope A** (`kMaxRouteDests = 8` + `make_route` factory), **closed C5 Scope B** (declarative `[[topics]]` registry).
**Read:** [post-phases-robotics-review.md §C5](post-phases-robotics-review.md#c5--declarative-transport-layer-gaps) for the closure trail of Scopes A + B and the promotion rationale for Scope C.

> **Provenance.** Phase G is the **promotion of parked C5 Scope C** ("per-topic routing") from the post-phases review into a first-class phase plan. The promotion happened during the C5 Scope A + B closure session (2026-05-28) when it became clear that per-topic routing is **not** a follow-on close-out task: it changes the router's dispatch primitive, the wire-facing TOML schema, every shipped profile, and the integration-test surface. That is phase-shaped work, not a sub-scope.

## Objective

Promote `topic_id` from "u16 wire field opaque to the router" + "declarative-only registry" (today, post-Scope-B) to a **first-class routing key**: the router can dispatch on `(source, topic_id)` pairs, not just `source`. Bridges, recorders, and the dashboard get per-topic subscription semantics without per-peer process forking.

## Why this is a phase, not a sub-scope

| Surface | Scope A change | Scope B change | Scope C (Phase G) change |
|---------|---------------|---------------|--------------------------|
| `RouteRule` shape | widen `dest[]` | none | **add a topic-key dimension** |
| `route_targets_for` | iterate `dest_count` | none | **two-dimensional lookup** (source, topic_id) |
| Hot path (`link.hpp` / `shm_router_link.hpp`) | none | none | **reads `frame.topic_id()` before dispatch** |
| TOML schema | `dest` array bound | new optional `[[topics]]` | **`[[routes]]` gains `topic` selector** |
| Profile rewrites | additive (dashboard tap) | optional `[[topics]]` example in one profile | **every shipped profile re-keys its routes** |
| Tests | additive (fan-out + topic registry) | additive | **integration tests rebuilt** for per-topic recorder + dashboard |
| ADR | none (implementation artifact) | none (additive) | **new ADR required** for the dispatch-key change |

Scopes A and B were close-out work. Scope C is a routing-primitive change — it deserves the same phase-shaped treatment Phase D got for transport hardening.

## Principles

- **Backward compatible at the TOML surface.** A rule that omits the topic selector keeps source-only semantics (everything matches). Existing Phase F profiles continue to load without modification, so an operator can move to per-topic routing one rule at a time.
- **Wire format unchanged.** The 64 B `RouterFrame` v2 already carries `topic_id` (ADR 0008). Phase G adds a *dispatch* dependency on it; it does not add a new wire field.
- **Topic registry is the source of truth.** Routes that name a `topic` selector reference an `id` declared in the `[[topics]]` section (closed C5 Scope B). The loader rejects routes that reference an unknown topic id at load time, matching the existing "route source / dest does not match any peer" pattern.
- **First-match-wins still wins.** The lookup remains deterministic — the table is walked in declaration order, the first rule whose `(source, topic_id)` matches is used. This keeps the hot-path behavior identical for routes that don't gain a topic selector.

## Deliverables

### G1 — ADR: per-topic routing as a dispatch key

`docs/adr/0012-per-topic-routing.md` (next-available — ADR 0011 was taken by [device-bridge transports](../../docs/adr/0011-device-bridge-transports.md) during F4, 2026-05-31). Records:

- The dispatch-key change (source → (source, topic_id)).
- Why first-match-wins is preserved.
- The TOML schema extension (`topic` optional on a `[[routes]]` entry; default = "match any topic" = source-only behavior).
- Alternatives considered and rejected:
  - **(A)** Separate `[[topic_routes]]` array next to `[[routes]]` — keeps the two cases visually distinct but doubles the lookup machinery; rejected.
  - **(B)** Compound source × topic key without first-match-wins (e.g. multi-match union) — breaks the deterministic-fan-out contract that ShmRouterLink relies on; rejected.
  - **(C)** Move dispatch into the topic registry (`[[topics]].dest = [...]`) — collapses two concepts into one; harder to express "all peers see topic X, except peer Y" patterns; rejected.

### G2 — `RouteRule` surgery

`ipc/src/router/routing.hpp`:

- `RouteRule` gains a `topic_id` selector with a `kTopicAny` sentinel (proposed: `0xFFFF` — but `RouterFrame` `topic_id` is itself u16, so a sentinel needs a free value; either reserve `0xFFFF` in the topic-id space at the topology level, or add an explicit `bool has_topic_selector` field).
- `make_route(source, d0, …)` keeps the existing surface (topic-less, source-only). A new `make_topic_route(source, topic_id, d0, …)` (or a defaulted second template parameter) covers the per-topic case.
- `route_targets_for(rules, count, source, topic_id)` becomes two-argument. Existing one-argument call sites in `link.hpp` / `shm_router_link.hpp` are updated to read `frame.topic_id()` and pass it in.
- The hot path stays branch-light: a topic-less rule (`has_topic_selector = false`) matches any topic_id without an extra compare.

### G3 — Loader extension

`ipc/src/router/topology_loader.hpp`:

- `[[routes]]` schema gains optional `topic` (u16) field. Loader resolves the value against `[[topics]]` ids at load time and rejects unknown ids with a frame-aware error message ("route topic id X does not match any [[topics]] entry").
- Rejects `topic = 0xFFFF` in a profile (reserved as `kTopicAny`), matching the existing peer-id 0/255 rejections.
- A profile that declares `[[topics]]` but never references a topic in routes is legal (Scope B already permits declarative-only registries).
- A profile with no `[[topics]]` section is still legal — it just cannot use the `topic` selector on routes.

### G4 — Profile rewrites

All four `config/profiles/*.toml`:

- Every profile that declares `[[topics]]` (today: `x86_dev.toml` only) gains per-topic example routes alongside the existing source-only routes. The other three profiles get a `[[topics]]` section + at least one per-topic route per Phase G's "demonstrate the capability" discipline.
- Recorder (peer 3) keeps subscribe-all source-only routes — it is by design the central tap.
- Dashboard (peer 8) moves from the source-only "tap on every compute rule" pattern (closed C5 Scope A) to a per-topic pattern where appropriate (e.g. dashboard subscribes to `topic = imu_proprio` from source = sensor, but not the full sensor frame stream). The exact topology is a Phase G design decision; the existing dashboard tap is the fallback.

### G5 — Integration tests rebuilt

- `routing_test` — new in-process cases for per-topic dispatch: topic-matching rule wins, topic-mismatching rule is skipped, `kTopicAny` rule still matches every topic, first-match-wins across mixed topic / no-topic rules.
- `topology_loader_test` — new TOML cases: `topic = N` parses, `topic = N` referencing an unknown topic id rejected, `topic = N` with no `[[topics]]` section rejected (or accepted with a warning — design call in G1).
- `profile_switch_test` — exercise a per-topic route end-to-end on SHM and UDP profiles.
- New `topic_dispatch_test` — full router scenario: 1 sensor publishes 3 different `topic_id`s, 3 subscribers each declare a per-topic route, each subscriber receives only its declared topic.
- Existing `routing_test` + `topology_loader_test` cases stay green (backward-compatibility gate).

### G6 — Documentation

- `docs/deployment-profiles.md` — new section on per-topic routes alongside the existing §Topic registry section; flip the "Per-topic routing" forward-references row from parked to **implemented**.
- `ipc/MODULE.md` — `router/routing.hpp` row updated to mention the topic selector; ADR cross-link added.
- `docs/robotics-reference-layout.md` — `ml_inference` integration pattern's per-source caveat removed (per-topic dispatch is now available); scope-vs-deferred table row updated.
- `robotics-ipc-module/plans/post-phases-robotics-review.md` — C5 entry marks Scope C **closed**; close-out follows the same template as Scopes A + B.

## Out of scope (still parked or moved elsewhere)

- **Priority-aware QoS** (former C5 Scope D) — moved to C7 ("Real-time / production knobs") because both QoS and RT pinning / `mlockall` solve the same problem domain: latency under contention. Phase G is about **shape of the config**; QoS is about **scheduling under load**. They share no code surface and very little test surface; coupling them would muddy both.
- **Topic-level QoS attributes in `[[topics]]`** (`reliability`, `history_depth`, etc. à la ROS 2) — declared out of scope for Phase G's first cut. A topic's *dispatch* is a Phase G concern; a topic's *delivery semantics* is the C5-Scope-D / C7 union.
- **Wildcard / subscription patterns** (e.g. `topic = "imu_*"`, regex matching) — the first cut uses exact `topic_id` integer match. Pattern matching is a follow-on, not Phase G.
- **Topic-driven multi-routing** (one publish event fanning out via multiple matching rules) — keeps first-match-wins. Multi-match is a follow-on.

## Review checklist

- [ ] `RouteRule` change does not break any existing Phase A–F call site that does not opt in to per-topic dispatch.
- [ ] Loader rejects unknown topic ids referenced from routes (mirrors existing "unknown peer id in dest" rejection).
- [ ] At least one shipped profile demonstrates per-topic dispatch end-to-end.
- [ ] `make ci` clean; new ADR linked from `docs/adr/` and from this plan.
- [ ] `routing_test` + `topology_loader_test` + `topic_dispatch_test` all green.

## Acceptance

```bash
make all
make ci          # full unit + integration + router + leak-check gate
# Manual smoke (document command in deployment-profiles.md):
./build/ipc/test/router_server --config config/profiles/x86_dev.toml &
# Then start two subscribers that subscribe to different topic_ids
# and one publisher that publishes both topics — assert each subscriber
# sees only its declared topic.
```

## Do not

- Break the wire format. `RouterFrame::topic_id` is already u16; Phase G consumes it for dispatch but does not redefine it.
- Couple per-topic routing to QoS / priority handling. Those problems share no code surface — see Out of scope above.
- Reach for a query language. Exact integer match is the first cut; patterns / wildcards are Phase H or later.
- Drop the topic registry's declarative-only invariant for *registration*. The registry stays declarative (Scope B contract); only the routing table reads it at dispatch.

## Session prompt

```
Execute Phase G from robotics-ipc-module/plans/G-declarative-routing.md.
Read post-phases-robotics-review.md §C5 (closure trail of Scopes A + B)
and the ADR drafted in G1. Update STATUS.md.
```
