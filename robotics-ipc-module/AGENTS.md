# Agent contract — Robotics IPC module plan

You are implementing a **phased roadmap** for a Linux robotics **message fabric** (C++ IPC/router) inside a larger stack: Jetson, x86+CUDA, HIL/sim/cloud, cameras, MAVLink, Python ML, Node dashboards.

## Session startup (required)

1. Read [STATUS.md](STATUS.md) — phase, deliverables, `IPC_ROOT`.
2. Read [DESIGN-PRINCIPLES.md](DESIGN-PRINCIPLES.md) — non-negotiables for every change.
3. Read [SYSTEM-VISION.md](SYSTEM-VISION.md) — if phase B, E, or F (config, deploy, bridges).
4. Skim [LESSONS-LEARNED.md](LESSONS-LEARNED.md) — avoid repeated bugs.
5. Read [CONTEXT.md](CONTEXT.md) — current code baseline.
6. Read [plans/00-MASTER.md](plans/00-MASTER.md) + **active phase plan**.
7. Load skill: `@ipc-robotics-phase-<letter>` or `@ipc-robotics-orchestrator`.

## Execution rules

| Rule | Detail |
|------|--------|
| **One phase per PR/session** | Complete one phase or sub-deliverable before scope creep |
| **Acceptance gates** | Run all commands in the phase plan; fix until green |
| **Preserve ADR style** | New decisions → `docs/adr/NNNN-<slug>.md` |
| **No safety claims** | Non–safety-critical only |
| **Minimal diffs** | Header-only, no type-erased router, adapters for transport specifics |
| **Interop outside core** | Python/Node/MAVLink/GStreamer only under `examples/bridges/` |
| **Stable peer IDs** | Profiles swap addresses, not route table semantics |
| **Update STATUS.md** | Checkboxes + session log every session |

## Environment swapping (HIL / sim / Jetson)

- Same `RouteRule[]` and peer **IDs** across `config/profiles/*.yaml`
- Change `PeerAddress` + default `TransportKind` per profile
- Do not hardcode `/tmp/rim_*` (or any concrete socket / SHM path) in library code under `ipc/src/`

## Default verify

```bash
make all && make test-ipc && make test-router
```

## Out of scope (unless user overrides)

- SIL / safety certification
- In-core DDS, ROS 2, Node, Python, CUDA, V4L in `src/`
- TLS on localhost UDS
- Raw image bytes or MAVLink wire format in 32 B `RouterFrame` payload

## Reporting format

```markdown
## Session result
- Phase: <letter>
- Principles checked: <which>
- Deliverables: <list>
- Acceptance: pass / fail
- STATUS.md: updated yes/no
- Next: <phase + task>
```
