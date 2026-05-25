---
name: ipc-robotics-phase-d
description: >-
  Executes Phase D (validation and stress): gtest unit tests, extended
  router_test scenarios, soak scripts, fault injection. Use for IPC router
  testing, soak tests, or phase D of robotics-ipc-module.
---

# Phase D — Validation & stress

## Plan

`robotics-ipc-module/plans/D-validation-stress.md`

**Do one sub-phase per session** unless user asks for full D.

## Sub-phase order

1. **D1** — `make test-ipc-unit` (frame, routing, resolver, topology)
2. **D2** — extend `router_test.cpp`
3. **D3** — `robotics-ipc-module/scripts/soak_router.sh`
4. **D4** — fault cases

## Workflow (D1 example)

```
- [ ] Add gtest or Catch2 to Makefile
- [ ] frame_test, routing_test, resolver_test
- [ ] make test-ipc-unit green
- [ ] Update STATUS.md D1 checkbox
```

## Acceptance

```bash
make all
make test-ipc-unit
make test-router
bash robotics-ipc-module/scripts/soak_router.sh 10
```

Make scripts executable: `chmod +x robotics-ipc-module/scripts/*.sh`
