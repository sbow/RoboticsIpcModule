---
name: ipc-robotics-phase-c
description: >-
  Executes Phase C (transport hardening): SHM try_send/backpressure, idle wake
  eventfd, router metrics, optional datagram sequence. Use for SHM spin-wait,
  robotics IPC determinism, or phase C of robotics-ipc-module.
---

# Phase C — Transport hardening

## Plan

`robotics-ipc-module/plans/C-transport-hardening.md`

Read `cpp_tricks/ipc/SHM_SPSC_TRANSPORT.md` for Phase 2 idle design.

## Workflow

```
- [ ] C1: try_send / SendResult::Full — no infinite spin in router default path
- [ ] C2: eventfd idle OR ADR deferral with measured idle CPU
- [ ] C3: forward/drop/full metrics (atomics, no heap)
- [ ] C4: datagram seq (optional)
- [ ] make test-ipc && make test-router
- [ ] Ring-full integration test (coordinate with Phase D)
- [ ] Update STATUS.md
```

## Acceptance

```bash
make all && make test-ipc && make test-router
```

Document 60 s idle CPU measurement in STATUS session log.
