# Phase D — Validation & stress

**Skill:** `@ipc-robotics-phase-d`  
**Depends on:** A; B1 for schema tests; C1 for ring-full tests  
**Read:** [LESSONS-LEARNED.md](../LESSONS-LEARNED.md) (test pitfalls)

## Objective

Automated evidence: correctness, restart safety, load, faults— including **profile switch** smoke (load `hil.yaml` vs `jetson_prod.yaml` if B2 done).

## Sub-phases (can split across sessions)

### D1 — Unit tests

Framework: **GoogleTest** or **Catch2** (pick one; document in MODULE.md).

| Suite | Targets |
|-------|---------|
| `frame_test` | payload max, timestamp round-trip, version byte |
| `routing_test` | `route_targets_for` edge cases |
| `resolver_test` | UDS/UDP peer_id_from_recv |
| `topology_test` | loader valid/invalid YAML; profile files parse |
| `cli_args_test` | log_path_for_role arity (regression for lessons learned) |

Makefile target: `make test-ipc-unit`

### D2 — Integration extensions

Extend `router_test.cpp` or sibling:

| Scenario | Pass criteria |
|----------|---------------|
| SIGKILL router | cleanup + restart binds OK |
| Slow recorder | no deadlock; metrics show drops if ring full |
| Burst sensor | 1 kHz × 10 s, thresholds met |

### D3 — Stress / soak

Scripts under `robotics-ipc-module/scripts/` or `cpp_tricks/ipc/scripts/`:

- `soak_router.sh` — loop `router_test` N times
- `shm_leak_check.sh` — count `/dev/shm` entries before/after
- Optional: `latency_histogram.sh` wrapping `echo_client_benchmark`

### D4 — Fault injection

| Fault | Expected |
|-------|----------|
| Truncated UDP datagram | drop, no crash |
| Wrong UDS path | bind error at startup |
| kill -9 router | clients exit on signal; shm_unlink on next cleanup |

## Review checklist

- [ ] CI workflow or documented nightly command
- [ ] Unit tests run < 30 s
- [ ] No flaky sleeps without comment (prefer polling thresholds)

## Acceptance

```bash
make all
make test-ipc-unit
make test-router
bash robotics-ipc-module/scripts/soak_router.sh 10   # 10 iterations
```

## Session prompt

```
Execute Phase D (sub-phase D1 first) from robotics-ipc-module/plans/D-validation-stress.md.
Update STATUS.md with which D1–D4 items completed.
```
