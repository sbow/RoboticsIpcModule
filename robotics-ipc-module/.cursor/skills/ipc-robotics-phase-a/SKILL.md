---
name: ipc-robotics-phase-a
description: >-
  Executes Phase A (module packaging) for the robotics IPC library: MODULE.md,
  public API boundary, wire format version, ADR 0004. Use when implementing
  phase A of the robotics-ipc-module plan or module packaging for the RoboticsIpcModule `ipc/` tree.
---

# Phase A — Module packaging

## Plan

Read and follow every section in:

`robotics-ipc-module/plans/A-module-packaging.md`

Also read `AGENTS.md`, `DESIGN-PRINCIPLES.md`, `CONTEXT.md`.

## Workflow

```
- [ ] Confirm baseline: make test-ipc && make test-router
- [ ] A1: Write ipc/MODULE.md
- [ ] A2: Document library vs test/; fix stray includes if any
- [ ] A3: Frame version constant or reserved byte + ADR note
- [ ] A4: docs/adr/0004-robotics-module-boundaries.md
- [ ] Run Acceptance block in plan
- [ ] Update STATUS.md Phase A → [x] and deliverable checkboxes
```

## Constraints

- No YAML loader (Phase B)
- No SHM API changes (Phase C)
- Match existing header-only style

## Acceptance

```bash
make all && make test-ipc && make test-router
test -f ipc/MODULE.md
test -f docs/adr/0004-robotics-module-boundaries.md
```
