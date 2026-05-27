# Phase C — Transport hardening

**Skill:** `@ipc-robotics-phase-c`  
**Depends on:** Phase A; B1 helpful but not blocking  
**Read:** [DESIGN-PRINCIPLES.md](../DESIGN-PRINCIPLES.md) (interruptible I/O), [LESSONS-LEARNED.md](../LESSONS-LEARNED.md) (SHM idle/spin)

## Objective

Bounded SHM behavior for **Jetson** workloads (vision-adjacent throughput), measurable metrics, near-zero idle CPU on battery-powered compute.

Reference: `ipc/SHM_SPSC_TRANSPORT.md` Phase 2 (eventfd idle).

## Deliverables

### C1 — SHM backpressure API

In `shm_spsc.hpp`:

- `try_send` → enum or bool + `SendResult { Ok, Full }`
- Replace infinite spin in `shm_push_slot` for router path (keep blocking `send` optional for benchmarks)

Update `ShmRouterLink::send_to_peer` to handle Full (drop counter or return error per ADR).

### C2 — Idle wake

Implement **one** of:

1. `eventfd` + wait when both rings empty for N polls (preferred), or
2. ADR deferral + document max idle CPU with yield-only

Router and `EchoServer::run_until` should use same primitive.

### C3 — Metrics

Lock-free or relaxed atomic counters (no heap):

- `forwarded`, `recv_empty`, `send_full`, `dropped`

Expose `RouterServer::metrics()` or link-level struct.

### C4 — Datagram seq (optional)

- Add optional sequence byte in frame or side header
- Subscriber drops out-of-order / old seq for high-rate sensor

## Review checklist

- [ ] Idle router + clients: CPU < 5% one core for 60 s (document measurement command)
- [ ] Full ring: no infinite spin in default router config
- [ ] `make test-router` passes
- [ ] New test: fill SHM ring, assert drop/block policy

## Acceptance

```bash
make all
make test-ipc
make test-router
make test-ipc-stress   # add in Phase D if not yet present
```

Manual (document in PR):

```bash
# 60s idle CPU check — record top output in STATUS session log
./build/ipc/test/router_server shm & sleep 60; kill $!
```

## Do not

- Port to RTOS
- Add TLS

## Session prompt

```
Execute Phase C from robotics-ipc-module/plans/C-transport-hardening.md.
Read SHM_SPSC_TRANSPORT.md. Update STATUS.md when done.
```
