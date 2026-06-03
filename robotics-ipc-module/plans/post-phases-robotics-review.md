# Post-phases robotics-integration review

> **Status:** deferred — revisit when Phase F closes (or earlier if explicitly reopened)
> **Source:** chat analysis dated 2026-05-26 ([Phase E fit-for-purpose review](f325cb57-00db-4d4e-a19c-2c45473839d1))
> **Trigger question:** _"Analyze Phase E — is it fit for purpose? Consider TensorRT, CUDA, ARM, x86 playback / simulated inputs, declarative transport."_
> **Later additions:** C11 (mixed-transport networks) surfaced during Phase F F1 — 2026-05-27.
> **Partial closures:** **C5 Scopes A + B** closed early during Phase F (2026-05-28) — 2-destination cap lifted to `kMaxRouteDests = 8`; declarative `[[topics]]` registry added. See the C5 entry for the closure notes.
> **Re-classifications (2026-05-30):** **C5 Scope C (per-topic routing)** promoted to its own phase plan — see [Phase G — Declarative routing](G-declarative-routing.md). **C5 Scope D (priority-aware QoS)** merged into [C7 — Real-time / production knobs](#c7--real-time--production-knobs-mlockall-cpu-pinning-sched_fifo) because both Scope D and C7 are latency-under-contention problems and share the same systemd-directives surface; see C7 §Options for the merged scope.

## Scope

This document captures the analysis surfaced when the user asked whether [Phase E](E-robotics-integration.md) as currently written is fit for purpose against real robotics-deployment requirements (10 considerations, C1–C10), with one Phase F F1 addition (C11) for the mixed-transport network gap that F1 discovered while authoring `jetson_prod.toml`. The verdict was:

> Phase E as written is **necessary but not sufficient**. It is a deployment-shape phase (systemd units, reference layout, timestamp ADR) — it does not unlock the robotics-integration capabilities the user named.

Some considerations are already in [Phase F](F-interoperability-bridges.md) scope, some are underspecified inside Phase E itself, and some are not in any current plan. This document is the parking lot for all of them so nothing is lost while the existing phases close out.

## Revisit trigger

Reopen this document when **all** of:

- Phase E (E1–E4) marked complete in [STATUS.md](../STATUS.md)
- Phase F (F1–F5) marked complete in [STATUS.md](../STATUS.md)

At that point, walk each consideration, decide close / defer / scope into a new phase.

## How to reopen

1. Re-read this document end to end.
2. For each consideration in **Group 1** and **Group 2**, verify whether the closing E/F deliverables addressed the concern. Mark the outcome inline (e.g. `**Closed 2026-XX-XX:** ...`).
3. For **Group 3**, decide per item: close with a documented rationale, defer further, or promote into a new `plans/G-*.md`.
4. Update [STATUS.md](../STATUS.md) — remove the breadcrumb pointing here once every consideration is resolved.

## Group 1 — Verify when Phase F closes (already in Phase F scope)

- **C1** TensorRT integration contract — addressed by F5 / `examples/bridges/`?
- **C2** CUDA / sideband `memory_class` parsing — addressed by F5 + ADR 0008 forward declaration?
- **C9** Camera / GStreamer integration shape — addressed by F5 `vision_peer` sketch?

## Group 2 — Verify when Phase E closes (in plan but underspecified)

- **C6** systemd readiness signaling — E2 names the units but does not yet specify `Type=notify` / `sd_notify`. Confirm before E2 closes.
- **C8** Cross-host time sync — E4 forward-declares PTP / `CLOCK_MONOTONIC` raw. Confirm which path E4 actually lands.

## Group 3 — Not in any current plan (needs explicit decision)

- **C3** ARM / aarch64 verification
- **C4** Playback / simulation testing on x86
- **C5** Declarative transport gaps — Scopes A + B closed 2026-05-28; Scope C promoted to [Phase G](G-declarative-routing.md) 2026-05-30; Scope D merged into C7 below
- **C7** Real-time / production knobs (`mlockall`, CPU pinning, SCHED_FIFO) — now also covers priority-aware QoS (former C5 Scope D)
- **C10** Module consumption model (`make install` / CMake export vs. vendor-as-submodule)
- **C11** Mixed-transport networks — fan out a single logical topology across SHM + UDS + UDP peers (today the router is single-transport per instance; surfaced during F1)


---

## Considerations in detail

### C1 — TensorRT integration contract

**Finding.** The module is, by design, CPU/transport-only (per [docs/adr/0004-robotics-module-boundaries.md](../../docs/adr/0004-robotics-module-boundaries.md)). There is no TensorRT code today, and there shouldn't be — `ml_inference` is a separate peer process. **But** the contract for "how does a TensorRT peer plug in?" is not concretely documented anywhere. [robotics-ipc-module/SYSTEM-VISION.md](../SYSTEM-VISION.md) names the peer; that's it.

**Evidence.**

- [robotics-ipc-module/SYSTEM-VISION.md](../SYSTEM-VISION.md) — `ml_inference` named in peer catalog.
- [docs/adr/0004-robotics-module-boundaries.md](../../docs/adr/0004-robotics-module-boundaries.md) — explicitly excludes CUDA / TensorRT from core.
- [robotics-ipc-module/plans/F-interoperability-bridges.md](F-interoperability-bridges.md) (F5) — vision peer sketch only; no TensorRT example.

**Coverage today.** Partial — peer is named, no contract written.

**Options.**

1. Phase E E1 reference layout adds a written contract: which `RouterFrame` v2 fields a TensorRT peer reads/writes, which sideband regions carry input vs output tensors, who allocates, who frees, `topic_id` semantics.
2. Push contract into a worked Phase F example under `examples/bridges/ml_inference/`.
3. Document only as "user responsibility" and close.

---

### C2 — CUDA / sideband `memory_class` parsing

**Finding.** [docs/adr/0008-router-frame-v2.md](../../docs/adr/0008-router-frame-v2.md) forward-declares `[[peers.sideband]] memory_class` (`shm` / `cuda_managed` / `cuda_host` / `nvbufsurface`), but the topology loader does **not** parse it. The `SidebandRegion` struct has no memory-class field. This is currently a Phase F deliverable (F5).

**Evidence.**

- [ipc/src/router/topology_loader.hpp](../../ipc/src/router/topology_loader.hpp) — `[[peers.sideband]]` parser (~lines 321–358 as of 2026-05-26): reads `name`, `max_payload_bytes`, optional `version`. No `class` / `memory_class` / `cuda_device` extraction.
- [ipc/src/router/sideband.hpp](../../ipc/src/router/sideband.hpp) — `SidebandRegion` struct (~lines 69–83): only `name`, `max_payload_bytes`, `version`.
- [docs/adr/0008-router-frame-v2.md](../../docs/adr/0008-router-frame-v2.md) lines ~129–161 — forward declares the schema for Phase F.
- [robotics-ipc-module/plans/F-interoperability-bridges.md](F-interoperability-bridges.md) line ~57 — F5 commits to landing the field.
- Repo grep for CUDA / NvBufSurface in `ipc/src/`: zero hits (only ADRs, plan docs, and a `#ifdef __CUDACC__` guard inside `third_party/tomlplusplus/toml.hpp`).

**Coverage today.** No — only SHM-class sidebands are usable.

**Options.**

1. **Promote `memory_class` parsing out of Phase F into Phase E.** ~30-line parser change + `SidebandRegion` field. The CUDA-aware *consumer* code stays in Phase F.
2. Keep deferred to Phase F as designed — and during Phase F closure, verify F5 actually lands the parser + struct field, not just the README sketch.
3. Document explicitly that only `class = "shm"` sidebands are supported in v1.

---

### C3 — ARM / aarch64 verification

**Finding.** Jetson (aarch64, L4T) is the named target ([ipc/MODULE.md](../../ipc/MODULE.md) line ~16, [docs/adr/0004-robotics-module-boundaries.md](../../docs/adr/0004-robotics-module-boundaries.md) line ~117, [docs/adr/0008-router-frame-v2.md](../../docs/adr/0008-router-frame-v2.md) lines ~57, ~94) but the module has never actually been built or tested on aarch64 hardware in CI.

**Evidence.**

- Zero `#ifdef __aarch64__` / `NEON` / `tegra` / `Jetson` symbols in `ipc/src/` library or test code (only documentation mentions + toml++ ARM64 compile guards in `third_party/`).
- [.github/workflows/ci.yml](../../.github/workflows/ci.yml) — `ubuntu-latest` x86_64 runner only; no aarch64 matrix.
- Jetson validation today is **indirect**: running `jetson_prod.toml` profile on an x86 host + idle-CPU script.

**Coverage today.** No — code *should* be aarch64-clean (header-only C++20 atomics, no x86 intrinsics), but "should be" is not "is".

**Options.**

1. **Document only** — add an aarch64 build recipe to `ipc/MODULE.md`. Run it on actual hardware out-of-band; record result in [STATUS.md](../STATUS.md). No CI change.
2. **Cross-compile CI** — add `aarch64-linux-gnu-g++` cross-build matrix to [.github/workflows/ci.yml](../../.github/workflows/ci.yml). Builds only; no execution.
3. **Full aarch64 test CI** — use `qemu-user-static` or GitHub's ARM runners to actually execute the suite. Highest confidence, highest cost.

---

### C4 — Playback / simulation testing on x86

**Finding.** This is the largest delta between the plan and the stated requirements. There is **no replay / playback engine** in the repo (grep for `replay`, `playback`, `tape`, `simulator` — zero hits in code). The recorder peer writes a 3-field CSV that lacks the fields needed for replay. The `hil.toml` profile is just an address swap, not a sim harness.

**Evidence.**

- Recorder output format ([ipc/test/router_client.cpp](../../ipc/test/router_client.cpp) lines ~37–44):

  ```cpp
  void append_record(const std::string& log_path, const RouterFrame& frame) {
      ...
      out << static_cast<int>(frame.source()) << ','
          << frame.timestamp_ns() << ','
          << frame.payload() << '\n';
  }
  ```

  Missing for replay: `topic_id`, `seq`, `flags`, sideband refs, sideband bulk bytes, original cadence, capture-time (router-relative monotonic only — see C8).

- [config/profiles/hil.toml](../../config/profiles/hil.toml) — line ~3 comment: "plant/sensors replaced by mock processes (Phase F examples)". Those mock binaries do not exist; `examples/` tree is absent entirely.
- Recorder peer ([ipc/test/router_client.cpp](../../ipc/test/router_client.cpp) lines ~119–137): passive `recv` loop, no sideband capture.

**Coverage today.** No — no replay, no extended log format, no mock peers.

**Options.**

1. **Minimum (Phase E or new Phase G):** Define a canonical recorder tape format (ADR + a small `tape_format.hpp`): binary, length-prefixed records covering every `RouterFrame` field + sideband descriptor bytes. Don't implement the player yet.
2. **Full (new Phase G or expansion of Phase F):** Land the recorder upgrade *and* a `replay_peer` under `examples/sim/` that reads the tape and re-emits at original cadence (or scaled). Most useful for "test on x86 with playback inputs."
3. Treat playback as user-responsibility infrastructure; document the wire fields a user-written recorder would need to capture; close.

> Recommended by the analysis as the **single highest-leverage** missing capability for the stated robotics use case.

---

### C5 — Declarative transport layer (gaps)

> **Status update — 2026-05-28:** Scopes A (lift 2-destination cap) and B (topic registry) closed during Phase F as a single deliverable. Scopes C (per-topic routing) and D (priority-aware QoS) remained parked.
>
> **Status update — 2026-06-01:** Scope C **delivered as [Phase G — Declarative routing](G-declarative-routing.md)** ([ADR 0013](../../docs/adr/0013-per-topic-routing.md)). Scope D remains parked, now owned by [C7](#c7--real-time--production-knobs-mlockall-cpu-pinning-sched_fifo-priority-aware-qos). **With Scope C delivered and Scope D merged, the C5 entry has no remaining sub-scopes.** The original four-gap analysis is preserved below for context; closure notes follow.

**Finding (original).** A declarative transport layer exists at the per-peer level (TOML profiles cover address, ring sizing, sideband regions). What is **not** declarative: topic registry, per-topic routing, QoS, transport-kind as first-class.

**Evidence (original).**

- TOML schema today ([ipc/src/router/topology_loader.hpp](../../ipc/src/router/topology_loader.hpp) `build_from_`): `[router].listen`, `[[peers]]` (`id`, `name`, `local`, optional SHM ring sizing), `[[peers.sideband]]` (name, max_payload_bytes, optional version), `[[routes]]` (source, dest = array of 1–2 peer IDs).
- Routes are **per-source**, not per-topic ([ipc/src/router/routing.hpp](../../ipc/src/router/routing.hpp) `RouteRule { source, dest0, dest1 }`, ~lines 17–21). Lookup is `route_targets_for(rules, rule_count, source)` — first matching rule wins.
- Topic registry: **none.** `topic_id` is a `u16` wire field in `RouterFrame` ([ipc/src/router/frame.hpp](../../ipc/src/router/frame.hpp) lines ~87–94) with no central table mapping `topic_id → name + sideband_idx + payload schema`. Publishers magic-number it.
- QoS: 3-bit `priority` field in frame `flags` (ADR 0008) is set but the router does **not** act on it. Drop policy is fixed at drop-on-full per destination ([ShmRouterMetrics::dropped_full_per_peer](../../ipc/src/router/metrics.hpp)).
- Transport kind is embedded in `listen`/`local` URI string (`uds:/...`, `udp:host:port`, `shm:/name`) rather than first-class.

**Coverage today.** Partial — Scope A + B closed (see below); peer + address + ring + sideband still declarative; per-topic routing and QoS still not declarative.

**Options (original; numbering preserved for traceability).**

1. **Document the current schema authoritatively** in [docs/robotics-reference-layout.md](../../docs/robotics-reference-layout.md) (E1) and treat the rest as publisher discipline.
2. **Add a `[[topics]]` registry** (topic_id → name + sideband_idx + default payload class). Router behavior unchanged; tooling and bridges can validate. — **Closed by Scope B below.**
3. **Make routes per-topic** (changes `RouteRule` shape, routing dispatch, all profiles). — **Delivered as [Phase G](G-declarative-routing.md) ([ADR 0013](../../docs/adr/0013-per-topic-routing.md)), 2026-06-01.**
4. **Make the router act on `priority`** (priority queue draining order in `forward_loop` and `ShmRouterLink::forward`). — **Parked as Scope D.**

---

#### Closure — Scope A: lift the 2-destination cap (2026-05-28)

**Trigger.** F1 surfaced a clean follow-on: `RouteRule { source, dest0, dest1 }` could not express `source = 1 → [controller, recorder, dashboard]`, leaving `dashboard_feed` (peer 8) without an inbound route across all four shipping profiles. Documented in F1's session log and in `docs/deployment-profiles.md` §Known limitations §2-destination cap.

**Change.**

- `ipc/src/router/routing.hpp` — `RouteRule` is now `{ uint8_t source; uint8_t dest_count; std::array<uint8_t, kMaxRouteDests> dest; }` with `kMaxRouteDests = 8` matching the existing `RouteTargets::ids` width (so no downstream change to `ForwardResult` or the link hot paths).
- A variadic constexpr factory `make_route(source, d0, d1, …)` deduces `dest_count` from the argument list and refuses zero-dest or >8-dest rules at compile time.
- `route_targets_for` copies `dest_count` slots into the `RouteTargets` (defaults preserve the array-tail-is-kEndpointInvalid contract; defensive coverage in `routing_test`).
- TOML loader (`topology_loader.hpp`) accepts `dest = [...]` arrays of length 1..kMaxRouteDests, rejects duplicates within a rule, and continues to reject self-routing per Phase D1.
- All four `config/profiles/*.toml` profiles widened uniformly: every compute-side rule now has `dashboard_feed` (8) as a trailing destination, closing the F1 dashboard limitation without changing route count or pre-C5 delivery order to existing destinations.

**Tests.** `routing_test` (60 assertions; +5 for Scope A: fan-out-to-3, fan-out-to-kMaxRouteDests, mixed-width rules, `dest_count` truncation safety, `make_route` constexpr deduction). `topology_loader_test` (126 assertions; +5 for Scope A: 3-dest TOML accepts, 8-dest TOML accepts, >8-dest TOML rejects, duplicate-dest TOML rejects, new dashboard-tap assertions in `test_load_from_file`). `profile_switch_test` updated to acknowledge the wider fan-out in its comments.

#### Closure — Scope B: declarative topic registry (2026-05-28)

**Trigger.** Bridges (especially the F2 Python ctypes peer) were hard-coding `topic_id` magic numbers. Without a central table, a recorder cannot validate "frame.topic_id = 100 means imu_proprio with no sideband", and the system has no place to record the declared payload class per topic.

**Change.**

- New `ipc/src/router/topic_table.hpp` — `TopicEntry { uint16_t id; uint16_t sideband_idx; const char* name; const char* payload_class; }`. Free-form `payload_class` (matches the ADR 0005 convention for sideband classes — not a whitelist). `sideband_idx` defaults to `kSidebandIdxNone` so the registry round-trips with the `RouterFrame` "no sideband" default.
- `RouterTopology` gains `const TopicEntry* topics` + `size_t topic_count` (default-empty; every existing aggregate-init keeps compiling). Lookups via `topic_by_id` / `topic_by_name` mirror the peer-table helpers.
- TOML loader parses an optional `[[topics]]` section with `id` (required u16, unique), `name` (required, unique, ≤63 bytes), optional `payload_class` (≤63 bytes), and optional `sideband_idx`.
- `config/profiles/x86_dev.toml` ships a 4-topic worked example (`imu_proprio`, `controller_command`, `vision_frame` + sideband 0, `ml_result` + sideband 1). Other profiles intentionally omit `[[topics]]` to demonstrate that the section is optional.

**What Scope B does NOT do.** Routing is still per-source — the router never consults `topics_` at forward time. Consumers (bridges, recorder, dashboard) opt in to validation. Scope C will revisit dispatch.

**Tests.** New `ipc/test/topic_registry_test.cpp` (42 assertions): absent-section yields empty view, three-topic round-trip, lookup by id and by name, id=0 is legal, move-stable interned pointers, and seven loader-error cases (missing id, missing name, empty name, u16-overflow id, duplicate id, duplicate name, out-of-range `sideband_idx`, empty `payload_class`).

#### Closure — Scope C: per-topic routing → Phase G (2026-06-01)

**Trigger.** After the F4/F5 device bridges shipped, the gap was concrete: a consumer wanting one stream from a multi-topic publisher (dashboard wanting only `imu_proprio`; a MAVLink consumer wanting one `msgid`; an ML consumer wanting only the output tensor topic) had no declarative way to say so. The only workarounds — source-side process forking (one peer id per topic) or subscriber-side filtering (every consumer receives every frame) — push a *routing* concern into the wrong layer. Promoted to a phase on 2026-05-30 (see Re-classifications below) because the change touches the dispatch primitive, the wire-facing TOML schema, profiles, and the integration-test surface, and needs a new ADR.

**Change.**

- `ipc/src/router/routing.hpp` — `RouteRule` gains a `uint16_t topic_id` selector (default `kRouteTopicAny = 0xFFFF` = match-any). `route_targets_for(rules, count, source, topic_id = kRouteTopicAny)` is now a two-dimensional first-match-wins lookup: a rule matches when `source` matches **and** the rule's topic is `kRouteTopicAny` or equals the frame's topic. A `make_topic_route(source, topic, dests…)` constexpr factory joins `make_route` (which is unchanged and stays source-only). The hot path stays branch-light — a source-only rule short-circuits the topic compare.
- `ipc/src/router/link.hpp` + `shm_router_link.hpp` — the two `forward()` hot paths pass `frame.topic_id()` into `route_targets_for`.
- `ipc/src/router/topology_loader.hpp` — `[[routes]]` accepts an optional `topic` (u16, `0..65534`; `0xFFFF` rejected as the reserved sentinel). A second pass validates each per-topic route against the `[[topics]]` registry (topics are parsed after routes), rejecting unknown ids with `route topic id N does not match any [[topics]] entry` — mirroring the existing unknown-peer rejection.
- `config/profiles/x86_dev.toml` — the sensor (source 1) route is split by topic: `imu_proprio` (topic 100) reaches the dashboard; every other sensor topic goes only to controller + recorder. First-match-wins requires the topic-specific rule to precede the source-only catch-all.
- New ADR [0013](../../docs/adr/0013-per-topic-routing.md) records the dispatch-key change, the `kRouteTopicAny` sentinel choice, the TOML extension, and the three rejected alternatives (separate `[[topic_routes]]` array / multi-match union / dispatch-in-registry).

**What Scope C / Phase G does NOT do.** No wire-format change (`topic_id` was already a u16 frame field, ADR 0008). No wildcard / pattern topic matching (exact integer match only). No topic-driven multi-routing (one frame still matches exactly one rule, first-match-wins). No topic-level QoS / delivery attributes — those belong with the C7 union. The `[[topics]]` registry stays declarative-only for *registration*; only the route table reads it (at load time) for *dispatch* validation.

**Tests.** `routing_test` (85 assertions; +25 for Phase G: topic-specific match/skip, any-topic matches every topic, first-match-wins with topic-specific before catch-all, catch-all-shadows-topic hazard, multi-topic same-source selection, 3-arg backward-compat, `make_topic_route` constexpr). `topology_loader_test` (176 assertions; +22 for Phase G: per-topic route parses, default-to-match-any, unknown-topic rejection, no-`[[topics]]`-section rejection, `0xFFFF` rejection, and the `x86_dev.toml` per-topic rule shape). New `ipc/test/topic_dispatch_test.cpp` (8 assertions): 1 sensor publishes 3 topic_ids over SHM, 3 subscribers each declare a per-topic route, each receives **only** its declared topic (`recv[a=1000 b=1000 c=1000] wrong[0,0,0]`). All pre-Phase-G cases stay green (backward-compatibility gate).

#### Re-classifications (2026-05-30)

After the Scope A + B closure landed, the two remaining sub-scopes were re-evaluated against the "is this close-out work or is it a phase?" test. The answer was different for each:

- **Scope C — per-topic routing → promoted to [Phase G — Declarative routing](G-declarative-routing.md) (promoted 2026-05-30; delivered 2026-06-01, [ADR 0013](../../docs/adr/0013-per-topic-routing.md) — see Closure above).** Changing `RouteRule` semantics so the route table dispatches on `(source, topic_id)` pairs instead of just `source` is **not** close-out work: it changes the router's dispatch primitive, the wire-facing TOML schema (`[[routes]]` gains a `topic` selector), every shipped profile, and the integration-test surface. It also needs a new ADR (the dispatch-key change is an architectural commitment, not an implementation refinement). That is phase-shaped work, not a sub-scope. See the new plan file for the full deliverable list (G1 ADR, G2 `RouteRule` surgery, G3 loader, G4 profiles, G5 tests rebuilt, G6 docs) and the principle that source-only routes continue to load without modification (backward-compatible TOML surface, opt-in per rule).
- **Scope D — priority-aware QoS → merged into [C7 — Real-time / production knobs](#c7--real-time--production-knobs-mlockall-cpu-pinning-sched_fifo).** Priority-aware drain order and `SCHED_FIFO` / `MemoryLock` / `CPUAffinity` solve the same problem domain (latency under contention), share the same hot path (`ShmRouterLink::forward` + `DatagramRouterLink::forward`), and share the same systemd-directives surface in Phase E's unit files. They also share a failure mode: both manifest as p99 latency drift under load, not as a config-shape problem. Bundling them keeps the "what does it take to make this thing real-time-safe?" conversation in one place. See C7 §Options for the merged scope.

The Scope A + B delivery was already enough to (i) close the F1 dashboard limitation and (ii) unblock bridge-side validation of `topic_id` semantics, which were the two concrete asks that surfaced during Phase F. With Scope C promoted and Scope D merged, the C5 entry has no remaining parked sub-scopes; both follow-on items now have an owning plan (Phase G for dispatch, C7 for QoS).

---

### C6 — systemd readiness signaling (`sd_notify` / `Type=notify`)

**Finding.** Zero `sd_notify` / `libsystemd` / `Type=notify` / `NotifySocket` references in the repo. The router binds and enters its forward loop without signaling readiness. Phase E E2 lists the units but does not specify the notify type.

**Evidence.**

- [ipc/test/router_server.cpp](../../ipc/test/router_server.cpp) — `run_shm_router` (~lines 82–88) calls `bind_shm_router_listen` then `run_forward_loop`. No callback after SHM regions are mapped.
- [robotics-ipc-module/plans/E-robotics-integration.md](E-robotics-integration.md) E2 — names `rim-router.service` / `rim-peer@.service` but does not specify `Type=notify`.
- [robotics-ipc-module/README.md](../README.md) line ~24 — future unit names only.

**Coverage today.** No. **Why it matters.** Without `Type=notify`, peer units gated `After=rim-router.service` will start the moment `router_server` is `exec()`'d, not when the SHM regions are actually bound. Peers will race their first `shm_open` and intermittently fail. Real Jetson-production footgun.

**Options.**

1. Add `sd_notify("READY=1")` after bind in [ipc/test/router_server.cpp](../../ipc/test/router_server.cpp) — either via `<systemd/sd-daemon.h>` behind a `HAVE_LIBSYSTEMD` define, or a small inline socket-write to `$NOTIFY_SOCKET` to keep `libsystemd` out of the link line.
2. Document the race + require peer-side retry-with-backoff on `shm_open` (already the right defensive posture either way).
3. Both — `sd_notify` for clean startup ordering, plus document the retry as a backstop.

---

### C7 — Real-time / production knobs (`mlockall`, CPU pinning, SCHED_FIFO, priority-aware QoS)

> **Scope absorbed 2026-05-30.** This entry was extended to cover priority-aware QoS (formerly C5 Scope D). See §Options 4 below for the merged sub-scope and the reasoning.

**Finding.** Zero hits for `mlockall`, `sched_setaffinity`, `SCHED_FIFO`, `LimitRTPRIO`, `MemoryLock`, `CPUAffinity`, `Nice=`, `pthread_setaffinity_np`. The Phase E plan does not list these as targets. The 3-bit `priority` field in `RouterFrame::flags` (ADR 0008) is also today purely advisory — `ShmRouterLink::forward` and `DatagramRouterLink::forward` never inspect it.

**Evidence.**

- [ipc/test/router_server.cpp](../../ipc/test/router_server.cpp) — default scheduler, paged memory, 200 ms poll timeout, 1 ms idle sleep ([ADR 0007](../../docs/adr/0007-router-idle-wake.md)). No init hook for hardening.
- [robotics-ipc-module/plans/E-robotics-integration.md](E-robotics-integration.md) — no mention of `mlockall` / `CPUAffinity` / RT scheduling.
- [ipc/src/router/shm_router_link.hpp](../../ipc/src/router/shm_router_link.hpp) `forward` — drops on ring-full regardless of `frame.flags & kPriorityMask`; no priority-aware drain order in `ShmRouterMetrics::dropped_full_per_peer` accounting.

**Coverage today.** No. **Why it matters.** Robotics control loops typically pin the IPC hot path to an isolated core with `mlockall` + `SCHED_FIFO` to avoid page-fault and preemption jitter. Without an opt-in path, downstream users must patch. Priority-aware QoS belongs in the same conversation: both problems are **latency under contention**, share the same hot path (`forward()` in both link types), and share the same systemd-directives surface in Phase E's unit files. Solving one without the other still produces p99 drift on the wrong frames.

**Options.**

1. Add an opt-in init hook in `router_app.h` (`router_lock_memory()`, `router_pin_to_core(int)`) + a documented `[Service]` snippet (`MemoryLock=infinity`, `CPUAffinity=2`, `LimitRTPRIO=80`) in the Phase E2 unit files.
2. Treat as user responsibility — document the recommended systemd directives in E1 only; no code changes.
3. Promote into a new "production hardening" phase plan (working name; phase letter to be picked when the work is scheduled — Phase G is already taken by declarative routing, so this would be Phase H or later).
4. **Priority-aware QoS sub-scope (merged from former C5 Scope D, 2026-05-30).** Have `ShmRouterLink::forward` / `DatagramRouterLink::forward` consult the 3-bit `priority` field in `frame.flags` and either drop low-priority frames first on backpressure or drain a priority queue. Touches `ShmRouterMetrics::dropped_full_per_peer` accounting (needs per-priority bucket counters). Belongs here because:
   - **Shared problem domain.** Priority-aware drain and `SCHED_FIFO` / `mlockall` are both about behavior under contention. Bundling them lets a single design pass settle the question "what is the contract for latency under load?"
   - **Shared hot path.** Both edits land in `forward()` on both link types. Splitting them across two phases would touch the same code twice.
   - **Shared systemd surface.** A production unit that wants priority-aware QoS also wants `MemoryLock=infinity` + `CPUAffinity` + `LimitRTPRIO=80` — they are configured at the same level and tested with the same load generators.
   - **Shared failure signature.** Both manifest as p99 latency drift under load, not as a config-shape problem. The Phase G boundary (config-shape work) explicitly excludes this — see [G-declarative-routing.md §Out of scope](G-declarative-routing.md#out-of-scope-still-parked-or-moved-elsewhere).

---

### C8 — Cross-host time sync (PTP / NTP)

**Finding.** The router clock is `steady_clock::now() - kStartTime` — monotonic-relative-to-router-start. Useless across hosts and reset on every router restart. E4 forward-declares PTP but Phase E does not yet land an implementation choice.

**Evidence.**

- [ipc/test/router_server.cpp](../../ipc/test/router_server.cpp) lines ~16–22:

  ```cpp
  using Clock = std::chrono::steady_clock;
  const Clock::time_point kStartTime = Clock::now();
  uint64_t ns_since_start() {
      return static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              Clock::now() - kStartTime).count());
  }
  ```

  Stamped on forward ([ipc/src/router/link.hpp](../../ipc/src/router/link.hpp) line ~95; [ipc/src/router/shm_router_link.hpp](../../ipc/src/router/shm_router_link.hpp) line ~91).

- [robotics-ipc-module/plans/E-robotics-integration.md](E-robotics-integration.md) E4 — "replace router-relative ns with `CLOCK_MONOTONIC` raw or PTP offset field."

**Coverage today.** Forward reference only (E4). **Why it matters.** HIL/sim, recorded-tape replay (C4), and any latency measurement across hosts all break without a cross-host clock or at minimum a process-stable monotonic clock.

**Options.**

1. **`CLOCK_MONOTONIC_RAW`** — single-host monotonic immune to NTP slew. Small change. Insufficient for cross-host correlation.
2. **`CLOCK_TAI` + PTP** — cross-host, requires `ptp4l` / `phc2sys` on the host. Bigger system story; document the host config.
3. **Both fields in the frame** — publisher monotonic_ns + router_recv_ns. Enables end-to-end latency. Currently the frame has only one `timestamp_ns`. **Note:** ADR 0008 fixed the frame at 64 B; this would require carving the existing `timestamp_ns` into two narrower fields or stealing bytes from the 32 B payload — non-trivial.

---

### C9 — Camera / GStreamer / V4L2 integration shape

**Finding.** No camera-side code in the repo (grep `gstreamer`, `v4l2`, `nvbuf`, `dmabuf`, `csi` — only docs/plans). [robotics-ipc-module/SYSTEM-VISION.md](../SYSTEM-VISION.md) lines ~98–102 name `vision_capture` as a future peer; F5 places the sketch in `examples/bridges/vision_peer/README.md` — and `examples/` does not exist.

**Evidence.**

- [robotics-ipc-module/SYSTEM-VISION.md](../SYSTEM-VISION.md) — names peer, mentions NV12/JPEG/NVMM sideband classes.
- [robotics-ipc-module/plans/F-interoperability-bridges.md](F-interoperability-bridges.md) F5 — sketch only, README-shaped deliverable.
- [docs/adr/0004-robotics-module-boundaries.md](../../docs/adr/0004-robotics-module-boundaries.md) line ~152 — `examples/bridges/vision_metadata/` Phase F territory.

**Coverage today.** No.

**Options.**

1. Phase E E1 documents the contract (which `topic_id` / `sideband_idx` / `seq` semantics; who owns the SHM region; how frames are released back); leave code for Phase F.
2. Defer entirely to Phase F as scoped.
3. Treat as out-of-scope and explicitly close.

---

### C10 — Module consumption model

**Finding.** No `make install`, no CMake config-mode export, no pkg-config. [robotics-ipc-module/install.sh](../install.sh) installs Cursor *skills*, not the library. Downstream apps are expected to vendor `ipc/src/` as a submodule and add the include + link flags by hand.

**Evidence.**

- [Makefile](../../Makefile) lines ~1–31:

  ```makefile
  #   ipc/src/     header-only library
  # Public entry points:
  #   #include "ipc.hpp"
  #   #include "router_protocol.hpp"
  #   #include "router_app.h"
  IPC_INC := -I$(IPC_ROOT)/src -I$(THIRD_PARTY)/tomlplusplus
  IPC_TEST_LDFLAGS := -lrt
  ```

- No `CMakeLists.txt`, no `.pc`, no `*.cmake` export file anywhere.
- [ipc/MODULE.md](../../ipc/MODULE.md) §Toolchain / §Public entry points — documents `-Iipc/src`, optional `-Ithird_party/tomlplusplus`, link `-lrt -pthread`.

**Coverage today.** Implicit (header-only vendoring is the de facto policy).

**Options.**

1. Make policy explicit — add a "Consuming this module" section to [ipc/MODULE.md](../../ipc/MODULE.md) (or [robotics-ipc-module/README.md](../README.md)) covering submodule pin recipe, include/link flags, ABI policy (none — header-only), version selection.
2. Add a minimal CMake config-mode export (`RoboticsIpcModuleConfig.cmake` + `target_include_directories` + `INTERFACE` target) as a Phase F (or G) deliverable.
3. Add `make install` that lays headers into `$(PREFIX)/include/ipc/`.

---

### C11 — Mixed-transport networks

**Finding.** The current router architecture is **single-transport per instance**. `RouterServer<T>` is templated on one `Transport`; [`ShmRouterLink::bind_router`](../../ipc/src/router/shm_router_link.hpp) silently skips non-SHM peers in the topology; [`ShmRouterLink::send_to_peer`](../../ipc/src/router/shm_router_link.hpp) **throws** when a route targets a peer it has no channel for. A profile that mixes SHM + UDS peers therefore crashes the forward loop on the first cross-transport route hit. Real deployments want to mix: SHM for the control-loop hot path, UDS for stateful subscribers (recorder, dashboard bridges), UDP for cross-host / HIL — and today they cannot, within one router instance.

**Evidence.**

- Single-transport bind ([ipc/src/router/shm_router_link.hpp](../../ipc/src/router/shm_router_link.hpp) lines ~48–60):

  ```cpp
  for (size_t i = 0; i < topo_.peer_count; ++i) {
      const PeerEntry& entry = topo_.peers[i];
      if (entry.local.kind != PeerAddressKind::ShmRing) {
          continue;     // <-- non-SHM peers silently skipped
      }
      ...
  }
  if (peer_channels_.empty()) {
      throw std::runtime_error("shm router: no shm peers in topology");
  }
  ```

- Crash-on-unknown-dest ([ipc/src/router/shm_router_link.hpp](../../ipc/src/router/shm_router_link.hpp) lines ~169–185):

  ```cpp
  void send_to_peer(uint8_t dest, const Buffer& payload) {
      for (auto& channel : peer_channels_) { ... }
      throw std::runtime_error("shm router: no channel for peer id "
          + std::to_string(dest));
  }
  ```

- F1 design realization: the original `jetson_prod.toml` proposal placed recorder (3) and dashboard_feed (8) on UDS while the compute peers used SHM. This would have crashed the router on the first `sensor → [controller, recorder]` forward. The profile was reverted to all-SHM and the limitation captured in [docs/deployment-profiles.md §Known limitations](../../docs/deployment-profiles.md#known-limitations) (commit `3797789`).
- `DatagramRouterLink<T>` ([ipc/src/router/link.hpp](../../ipc/src/router/link.hpp)) mirrors the same single-transport assumption — its `peer_channels_` are all `T`.
- TOML schema does **not** validate transport homogeneity ([ipc/src/router/topology_loader.hpp](../../ipc/src/router/topology_loader.hpp) `build_from_`): it happily loads mixed-transport profiles, only failing at runtime when the bind/forward path discovers the mismatch.
- Adjacent gap: [C5 declarative-transport gaps](#c5--declarative-transport-layer-gaps) covers the routing-side limitations (per-source `RouteRule` with 2-dest cap, no topic registry, no QoS). C11 is the **transport-layer** sibling of C5: routes can't span transports, and that's a separate problem from "routes can't fan out to 3+ destinations."

**Coverage today.** No — single-transport per router instance is a hard architectural constraint, undocumented in [ADR 0001](../../docs/adr/0001-ipc-and-router.md) (which speaks of routing in the abstract) and acknowledged inline only in [docs/deployment-profiles.md](../../docs/deployment-profiles.md) and indirectly in [parked C5](#c5--declarative-transport-layer-gaps). **Why it matters.** Real robotics deployments have **intrinsically heterogeneous** ingress/egress shapes: cameras want SHM for zero-copy; Node dashboards want UDS / WebSocket; HIL benches want UDP; cross-host federation needs UDP. Without mixed-transport support, every cross-transport peer either (a) speaks the router's only transport even when it's wrong for the workload (e.g. Node speaking SHM via a native N-API addon — heavy and fragile), (b) lives in a separate router instance and depends on external glue (factory-bridge approach), or (c) bridges itself in user code (every peer reimplements bridging). F1 had to navigate all three.

**Options.**

1. **Factory-generated bridge daemons between single-transport routers** (the seeded option).
   - **Concept.** Each transport gets its own `RouterServer<T>` process + profile (`rim-router-shm.toml`, `rim-router-uds.toml`, `rim-router-udp.toml`). A small bridge daemon subscribes to one router as a peer and republishes received frames to another router as a peer. The "factory" piece is a build-time tool that reads a unified **logical topology** (peers, transports, routes) and emits the N per-transport profiles plus the bridge daemons (or a single parameterized bridge binary).
   - **Pros.** Zero router-internals change — each router stays the simple single-transport machine it is today. Process boundaries align with transport boundaries (good for failure isolation, security sandboxing, systemd unit granularity). Multi-host scales for free: SHM router on Jetson, UDP router on a sim_cloud container, UDS router on a dashboard host — bridge daemons stitch them together over UDS/UDP. Each router stays profile-isolated; per-router debugging is local. Matches the project's existing "process-based separation" philosophy ([ADR 0004](../../docs/adr/0004-robotics-module-boundaries.md)).
   - **Cons.** Cross-transport frames pay a **two-hop tax**: publisher → router-A → bridge → router-B → subscriber, where today same-transport is one hop. Bridge daemon is a new **stateful** component on what is otherwise a stateless hot path (peer-id ↔ peer-id mapping, per-route state). Route rules are **duplicated** across routers: a logical `sensor → [controller, dashboard]` becomes `[router-shm] sensor → controller, bridge_egress` + `[router-uds] bridge_ingress → dashboard`; without the factory tool keeping them in sync, drift is inevitable. Bridge process count scales as O(transport_pairs) — three transports means three bridges (or one omnidirectional bridge with its own internal routing table). systemd unit footprint roughly triples (router-shm + router-uds + router-udp + bridges + cleanup helpers). Frame `timestamp_ns` either gets **re-stamped at the bridge** (loses original capture time — bad for cross-transport latency measurement) or **preserved** (then the second router's `timestamp_ns = router_now_ns()` overwrite per [ADR 0010](../../docs/adr/0010-router-timestamp-clock.md) must be conditionalized for bridge-sourced frames — special-case logic on the hot path).

2. **Mixed-transport router instance** (heterogeneous links inside one `MixedRouterServer`).
   - **Concept.** Detemplate `RouterServer<T>` into a non-templated `MixedRouterServer` that holds a heterogeneous collection of links (one `ShmRouterLink`, optionally one `DatagramRouterLink<Uds>`, optionally one `DatagramRouterLink<Udp>`). `peer_channels_` becomes a heterogeneous map keyed by `peer_id` to `(transport_kind, link_index)`. `forward()` polls each link in turn; `send_to_peer(dest)` looks up the dest's `transport_kind` and dispatches to the matching link. `PeerEntry::local.kind` already carries the right tag — the routing layer just needs to use it.
   - **Pros.** Single forwarding hop regardless of transport mix (latency parity with same-transport). Single process — one TOML, one systemd unit, one set of metrics. Profile authors can mix transports freely (the design that F1 originally implied). Single timestamp source (no re-stamping decision). systemd footprint stays the same. The TOML schema **already** supports mixed transports (the loader accepts them today); only the link layer needs to catch up.
   - **Cons.** Bigger architectural change — touches [router/link.hpp](../../ipc/src/router/link.hpp), [router/shm_router_link.hpp](../../ipc/src/router/shm_router_link.hpp), the [link_concept](../../ipc/src/router/link_concept.hpp) header that was deliberately scoped to a single-transport API, and every integration test that constructs a router. Heterogeneous polling needs care: `ShmRouterLink::forward` is non-blocking (poll-then-sleep per [ADR 0007](../../docs/adr/0007-router-idle-wake.md)); `DatagramRouterLink::forward` is blocking on `recvfrom()` with `SO_RCVTIMEO`. Either both run in their own threads (per-link thread + a router-level merge step) or the datagram links must be made non-blocking + cooperatively polled (refactor of the UDS/UDP recv path). `bind_router(BindParams)` API needs to port across links (per-link bind params). Metrics need a roll-up across links. Hot-path dispatch becomes a small switch/jump on the `PeerAddressKind` tag (cost negligible in practice but it's not free).

3. **Peer-side dual-protocol bridging** (no router change).
   - **Concept.** Don't touch the router. For each peer that wants to span transports — recorder, dashboard_feed, anything that needs cross-protocol egress — the **peer itself** becomes a dual-protocol bridge: subscribes to the router on its single transport (SHM on Jetson), re-emits over UDS/HTTP/WebSocket inside the same process. The router never knows about the second protocol.
   - **Pros.** Zero router changes — smallest blast radius of any option. Encapsulates bridging in the peer that needs it; if no peer needs mixed-transport, no one pays. Each peer chooses its egress protocol freely (recorder may bridge to disk via file IO; dashboard may bridge to UDS or directly to WebSocket; no one is forced into a common protocol).
   - **Cons.** **Duplicates bridging logic** across every peer that needs it — recorder writes its own SHM-to-disk drain; dashboard writes its own SHM-to-WebSocket drain; future peers each reimplement the SHM-read half. Every non-C++ bridge author must write a SHM-aware client in their language (native N-API addon for Node, `ctypes` wrapper for Python). **Does not help ingress** at all — every publisher peer must speak the router's single transport, so a UDP-only sensor mock cannot publish into a SHM router without an external bridge daemon (which is just Option 1 reinvented for ingress). Hides the topology: an external observer reading `jetson_prod.toml` cannot tell that recorder is also a UDS server — that fact lives only inside the recorder source.

**Variants worth naming for the post-phases walk** (not standalone options, but refinements of the above):

- **1a. SHM-backed inter-router channel** (optimization of Option 1). Replace the bridge daemon with a dedicated SHM ring that both routers tap directly: router A writes outbound frames to `/dev/shm/rim_bridge_shm_to_uds`, router B reads from it as if it were a normal peer. Removes the bridge process at the cost of teaching both routers a "router-as-peer" mode.
- **1b. Declarative multi-router schema** (refinement of Option 1). Extend TOML with a `[[routers]]` table; routes can target a peer **or** a router-id. A build tool consumes a single logical topology and emits per-router profiles + bridge daemon config. Solves the route-table-drift problem at the cost of a more complex schema and validator.
- **2a. Polymorphic link interface** (implementation strategy for Option 2). Introduce a virtual `IpcLink` interface that each transport implements; the router holds `std::vector<std::unique_ptr<IpcLink>>`. Cleaner abstraction at the cost of vtable dispatch on the hot path — deliberately avoided in Phase A for that reason. Today's templated single-transport router devirtualizes everything; this gives that back.
- **4. Kernel-assisted routing** (XDP / io_uring / eBPF). Use Linux kernel primitives to route between transports without a userspace bridge daemon. Linux-specific, large scope, and breaks the header-only consumption model ([parked C10](#c10--module-consumption-model)) — listed only to explicitly close it as out-of-scope for this module.

**Decision rubric for the walk.**

When this consideration is reopened, the choice between Options 1 / 2 / 3 turns on these trade-offs:

| Concern | Favors |
|---------|--------|
| Latency budget (cross-transport hot path) | **2** (single hop) > 1 (two hops + bridge) ≈ 3 (depends on peer impl) |
| Architectural simplicity / staying close to today's code | **3** > 1 > 2 |
| Operator-facing config simplicity | **2** (one TOML) > 1b (declarative multi-router) > 1 (3 TOMLs + glue) > 3 (config invisible to topology) |
| Multi-host scaling | **1 / 1b** > 2 (Option 2 is still single-process; cross-host still needs Option 1's bridge pattern) |
| Failure isolation (one transport's faults don't take down others) | **1 / 1b** > 2 (a bug in `DatagramRouterLink` could crash the whole router) |
| Bridge-author burden (Python / Node / MAVLink) | **2** (router does the work) > 1 (router does the work) > 3 (every author reimplements SHM reads) |

A reasonable two-stage migration: **Option 2 first** (lands single-host mixed-transport, which unblocks F2/F3 properly and matches the operator mental model F1 documented), then **Option 1b stacked on top of Option 2** when cross-host fanout becomes necessary (multi-host robotics, sim_cloud federation, recorder-on-edge-router patterns). Option 3 is the **bridging-by-fiat fallback** if neither lands and the F2 / F3 sketches need to ship anyway — accept the per-peer duplication.

**Decision deferred.** This consideration was surfaced in F1 (the all-SHM `jetson_prod.toml` is a workaround, not a fix) and intentionally **not** scoped into a Phase F deliverable — F1's plan-text answer "UDS for logger/dashboard_feed" turned out to require this feature, and adding it under F1 would have ballooned the deliverable. Revisit alongside C5 during the post-phases walk; the two share enough surface area that their resolutions should be co-designed.

---

## Summary matrix

| ID | Consideration | Status today | Phase E/F coverage | Group |
|----|----|----|----|----|
| C1 | TensorRT integration contract | Peer named, no contract | Phase F (F5 sketch) | 1 |
| C2 | CUDA / sideband `memory_class` | Unparsed | Phase F (F5) | 1 |
| C3 | ARM / aarch64 verification | Docs only; no CI dim | Not planned | 3 |
| C4 | Playback / sim on x86 | None; CSV recorder unusable | Not planned | 3 |
| C5 | Declarative transport gaps | Per-peer only; no topic/QoS | A+B closed (F); C→Phase G delivered ([ADR 0013](../../docs/adr/0013-per-topic-routing.md)); D→C7 | — |
| C6 | systemd readiness | No `sd_notify` | E2 (underspecified) | 2 |
| C7 | RT pinning / `mlockall` | None | Not planned | 3 |
| C8 | Cross-host time sync | `steady_clock` relative ns | E4 (forward-decl only) | 2 |
| C9 | Camera / GStreamer shape | Docs only | Phase F (F5) | 1 |
| C10 | Module consumption model | Implicit submodule pattern | Not planned | 3 |
| C11 | Mixed-transport networks | Single-transport per router (hard) | Not planned; surfaced in F1 | 3 |

## References

- [robotics-ipc-module/plans/E-robotics-integration.md](E-robotics-integration.md)
- [robotics-ipc-module/plans/F-interoperability-bridges.md](F-interoperability-bridges.md)
- [robotics-ipc-module/SYSTEM-VISION.md](../SYSTEM-VISION.md)
- [robotics-ipc-module/STATUS.md](../STATUS.md)
- [docs/adr/0004-robotics-module-boundaries.md](../../docs/adr/0004-robotics-module-boundaries.md)
- [docs/adr/0005-payload-policy-and-sideband.md](../../docs/adr/0005-payload-policy-and-sideband.md)
- [docs/adr/0008-router-frame-v2.md](../../docs/adr/0008-router-frame-v2.md)
- [docs/adr/0009-per-peer-ring-sizing.md](../../docs/adr/0009-per-peer-ring-sizing.md)
- [docs/adr/0013-per-topic-routing.md](../../docs/adr/0013-per-topic-routing.md) — Phase G, closes C5 Scope C
- [robotics-ipc-module/plans/G-declarative-routing.md](G-declarative-routing.md) — Phase G plan (delivered)
- [ipc/MODULE.md](../../ipc/MODULE.md)
- Conversation: [Phase E fit-for-purpose review](f325cb57-00db-4d4e-a19c-2c45473839d1)
