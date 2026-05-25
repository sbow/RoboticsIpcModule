---
name: ipc-robotics-phase-f
description: >-
  Executes Phase F (interoperability bridges): deployment profiles for
  Jetson/HIL/sim, Python and Node bridge examples, MAVLink and vision peer
  sketches. Use for multi-language robotics IPC, dashboard gateways, or phase F
  of robotics-ipc-module.
---

# Phase F — Interoperability & bridges

## Required reading

- `robotics-ipc-module/plans/F-interoperability-bridges.md`
- `robotics-ipc-module/SYSTEM-VISION.md`
- `robotics-ipc-module/DESIGN-PRINCIPLES.md` (interop section)

## Plan

Implement F1–F5 under `examples/bridges/` and `config/profiles/` only.

## Workflow

```
- [ ] F1 profile YAML + docs/deployment-profiles.md
- [ ] F2 python_peer example
- [ ] F3 node_gateway example
- [ ] F4 mavlink_gateway README (+ stub if time)
- [ ] F5 vision_peer README (CSI/V4L note)
- [ ] make test-router
- [ ] Update STATUS.md Phase F
```

## Constraints

- No Python/Node/MAVLink includes in `src/ipc/` or `src/router/`

## Acceptance

See plan file.
