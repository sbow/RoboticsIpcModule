---
name: ipc-robotics-phase-e
description: >-
  Executes Phase E (robotics integration): reference peer layout, systemd units,
  optional ROS2 bridge doc, monotonic timestamps ADR. Use for deploying IPC
  router on robot Linux or phase E of robotics-ipc-module.
---

# Phase E — Robotics integration

## Plan

`robotics-ipc-module/plans/E-robotics-integration.md`

**Prerequisite:** B2, C1, D2 done per STATUS.md (or user override).

## Workflow

```
- [ ] E1: docs/robotics-reference-layout.md
- [ ] E2: deploy/systemd/*.service examples
- [ ] E3: examples/ros2_bridge/README.md (optional)
- [ ] E4: timestamp ADR update
- [ ] make test-router
- [ ] Update STATUS.md Phase E → [x]
```

## Constraints

- Examples only — no ROS headers in `src/router/`
- Non-safety wording in all docs

## Acceptance

```bash
make all && make test-router
```
