# Phase D — Validation & stress

**Skill:** `@ipc-robotics-phase-d`
**Depends on:** A; B1 (sideband ADR), B2 (profiles), B3 (logger), B4 (LV cache) for the schema/cache tests; C1 (drop-on-full) + C2 (idle backoff) + C3 (metrics) for the ring-full / per-peer tests; ADR 0008 (RouterFrame v2)
**Read:** [LESSONS-LEARNED.md](../LESSONS-LEARNED.md) (test pitfalls)

## Objective

Automated evidence: correctness, restart safety, load, faults — including **profile switch** smoke (load `hil.toml` vs `jetson_prod.toml`), **subscriber-side seq attribution**, **per-peer drop attribution**, and **right-sized ring** validation.

## Sub-phases (can split across sessions)

### D0 — Topology schema: per-peer ring sizing (ADR 0009)

**Why now.** With RouterFrame v2 at 64 B (ADR 0008), `ShmSpsc`'s default
`max_payload = 1024` is ~15× larger than needed for router-frame peers,
inflating per-peer ring memory and pushing rings out of L2. Right-sizing
the slot stride is the precondition for D2's "burst sensor" cache-resident
target and D3's soak measurement. It also lets sideband-bridge peers
(Phase F vision / ML) keep large slots without affecting control-plane
peers.

| Deliverable | Detail |
|---|---|
| ADR 0009 | "Per-peer ring sizing: `shm_slot_count` / `shm_max_payload` overrides" — rationale, default behavior preserved, schema, migration impact (none — additive) |
| Topology schema | `[[peers]]` gains optional `shm_slot_count` (default 256) and `shm_max_payload` (default 64 for router-frame peers, ≥ frame size enforced) |
| Loader pass-through | `bind_router({slot_count, max_payload, ...})` and `bind_client(...)` consume per-peer values; missing keys fall back to existing `ShmSpsc::BindParams` defaults |
| Unit test extension | `topology_loader_test` gains 3+ assertions for the new fields (default, override, validation error when `shm_max_payload < kRouterFrameSize`) |
| Acceptance | `shm_region_size(slot_count, max_payload)` for a router-frame peer drops from `2 × 256 × 1028 = ~526 KB` to `2 × 256 × 68 = ~35 KB`; existing `make test-router` still green on all three transports |

### D1 — Unit tests

**Framework decision (locks the choice deferred in the original plan):**
the existing lightweight in-repo `EXPECT` / `EXPECT_EQ` macros (one
`#define` block per test file, ~10 lines, counts assertions, prints
file:line on failure) are the supported pattern. **No GoogleTest, no
Catch2** — both would add a dependency for marginal ergonomic gain and
violate "header-only, no extra deps." The pattern is now documented in
`ipc/MODULE.md` "Testing".

| Suite | Status | Targets |
|-------|--------|---------|
| `frame_test` | **Done** (ADR 0008) — `ipc/test/frame_test.cpp`, 163 assertions | source / flags helpers / topic_id / seq (incl. wrap arithmetic) / timestamp / sideband_idx / uint48 sideband_len / neighbor-non-clobber / sideband_seq / payload max + overflow / version byte / full round-trip |
| `topology_loader_test` | **Done** (Phase B + extended in D0) | profile parse + invalid TOML rejection paths (negative tests for missing required fields, malformed addresses, peer id collisions, `shm_max_payload < kRouterFrameSize`) |
| `last_value_cache_test` | **Done** (Phase B4) | per-source replacement, multiple sources, FIFO eviction |
| `shm_backpressure_test` | **Done** (Phase C / ADR 0006) | forwarded / recv_empty / drop-on-full + deadline guard |
| `datagram_seq_test` | **New** | publisher emits seq 0..N; subscriber drives `LastValueCache` + a small gap detector; assert: monotonic-modulo-wrap detection, gap count under simulated drops, correct count across 2³² wrap |
| `routing_test` | **New** | `route_targets_for` edge cases: source unknown, no rules match, multiple rules match, self-routing rejected, broadcast vs unicast |
| `resolver_test` | **New** | UDS / UDP `peer_id_from_recv`: known peer, unknown peer (returns `kEndpointInvalid`), truncated address, address from wrong family |
| `cli_args_test` | **New** | `log_path_for_role` arity (regression for LESSONS-LEARNED: controller `argv[5]`, recorder `argv[4]`) |

Makefile target: `make test-ipc-unit` — must continue to aggregate
**every** unit test target. Build budget: <5 s clean rebuild, <30 s run.

### D2 — Integration extensions

Extend `router_test.cpp` (or sibling integration binaries). Each scenario
runs against UDS / UDP / SHM unless transport-specific.

| Scenario | Pass criteria |
|----------|---------------|
| SIGKILL router | Clients exit on TERM within 1 s; next `router_server` bind succeeds (no stale `/dev/shm/cpp_tricks_*` blocking) |
| Slow recorder (SHM) | Sensor publishes at 1 kHz × 10 s; recorder drains at 100 Hz; **per-peer attribution gate** (D2a) asserts `dropped_full_per_peer[recorder] > 0` AND `dropped_full_per_peer[controller] == 0`; no deadlock; controller continues to receive |
| Burst sensor | 1 kHz × 10 s on right-sized rings (D0); subscribers see ≥ 99.5% of frames in steady state; seq gap count == published − received (closes deferred C4) |
| Profile switch smoke | Load `jetson_prod.toml` and `hil.toml` back-to-back; router starts, peers connect on each profile's transport, one round trip succeeds, clean shutdown |

#### D2a — Per-peer drop attribution (carryover from Phase C)

| Deliverable | Detail |
|---|---|
| `ShmRouterMetrics` extension | Add `std::array<std::atomic<uint64_t>, kMaxPeers> dropped_full_per_peer` (or a small flat map keyed on peer id) alongside the existing global `dropped_full`. Heap-owned via `unique_ptr` so `ShmRouterLink` stays movable (per Phase C lesson) |
| `send_to_peer` | On `ShmSendResult::Full`, increment **both** the global counter and `dropped_full_per_peer[dest_id]` |
| Backwards compat | Existing `shm_backpressure_test` asserts on the global counter still pass; new assertions added for per-peer |
| Documentation | `metrics.hpp` header comment; `ipc/MODULE.md` "Metrics" section updated |

### D3 — Stress / soak

Scripts under `robotics-ipc-module/scripts/`:

| Script | Purpose |
|---|---|
| `soak_router.sh N` | Loop `make test-router` N times; abort on first failure; print per-iteration timing |
| `shm_leak_check.sh` | Count `/dev/shm/cpp_tricks_*` and `/tmp/cpp_tricks_*.sock` before / after a full `make test-ipc-unit && make test-router`; assert delta == 0 |
| `idle_cpu_check.sh` | Start `router_server jetson_prod.toml`, sample `pidstat` for 60 s, assert idle CPU ≤ 5% (re-baselines the Phase C measurement on the current revision) |
| `latency_histogram.sh` (optional) | Wrap `echo_client_benchmark` with a quantile summarizer; emit p50 / p95 / p99 |

### D4 — Fault injection

| Fault | Expected |
|-------|----------|
| Truncated UDP datagram (< `kRouterFrameSize`) | Drop, no crash; `recv_truncated` metric increments (Phase C: same counter that already exists for SHM should be mirrored for datagrams) |
| Wrong UDS path in topology | Bind error at startup with a clear log line; no partial state on disk |
| `kill -9 router` mid-traffic | Clients exit on signal; next clean run of `router_server` succeeds via `shm_unlink` cleanup helper |
| Profile with `shm_max_payload < kRouterFrameSize` | Topology load rejects with a clear error; never reaches `bind_router` |

## CI wiring

| Deliverable | Detail |
|---|---|
| `.github/workflows/ci.yml` (or `Makefile` target named `ci`) | Runs on every PR: `make all`, `make test-ipc-unit`, `make test-router`, `bash scripts/shm_leak_check.sh`. Total budget ≤ 3 min on a standard runner |
| Cache | `ccache` if available; otherwise just `apt-get install build-essential` |
| Artifacts | Upload `build/ipc/test/*` on failure for triage |

## Review checklist

- [ ] CI workflow lands and is documented in `MODULE.md` ("CI" section)
- [ ] Unit tests run < 30 s in aggregate; documented in `MODULE.md` "Build & verify"
- [ ] No flaky sleeps without comment (prefer polling thresholds or deterministic deadlines, per Phase C echo refactor pattern)
- [ ] Every new test file follows the in-repo `EXPECT/EXPECT_EQ` pattern; no new test dependencies
- [ ] D0 ring-sizing schema is backwards-compatible (no required new field; defaults preserve current behavior)
- [ ] D2 slow-recorder scenario actually exercises per-peer attribution — global counter alone is insufficient
- [ ] `LESSONS-LEARNED.md` updated with whatever D surfaces

## Acceptance

```bash
make clean && make all
make test-ipc-unit                  # frame + topology_loader + LV cache + shm_backpressure
                                    # + datagram_seq + routing + resolver + cli_args
make test-ipc                       # echo benchmarks (no regression vs ADR 0008 baseline)
make test-router                    # uds + udp + shm scenarios, profile switch included
bash robotics-ipc-module/scripts/soak_router.sh 10
bash robotics-ipc-module/scripts/shm_leak_check.sh
bash robotics-ipc-module/scripts/idle_cpu_check.sh   # asserts ≤5% idle CPU
```

## Session prompt

```
Execute Phase D from robotics-ipc-module/plans/D-validation-stress.md.
Start with D0 (per-peer ring sizing ADR 0009) since later sub-phases
assume right-sized rings. Then D1 unit-test expansion, then D2 + D2a
integration + per-peer attribution, then D3 / D4 scripts and faults,
closing with the CI workflow.

Update STATUS.md after each completed sub-phase. New decisions land
as docs/adr/NNNN-*.md. Stick to the lightweight EXPECT/EXPECT_EQ test
pattern — no GoogleTest, no Catch2.
```
