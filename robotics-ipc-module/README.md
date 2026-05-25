# Robotics IPC module — AI-executable plan pack

Evolve a **header-only Linux IPC/router** into the C++ **message fabric** for a robotics stack: Jetson, x86+CUDA, HIL/sim/cloud, with bridges to Python (ML), Node (dashboards), cameras (CSI/V4L), and MAVLink (MCU).

## Documentation map

| Doc | Purpose |
|-----|---------|
| [AGENTS.md](AGENTS.md) | Session entry contract |
| [DESIGN-PRINCIPLES.md](DESIGN-PRINCIPLES.md) | **Must-read** rules for all phases |
| [LESSONS-LEARNED.md](LESSONS-LEARNED.md) | Bugs and fixes from cpp_tricks development |
| [SYSTEM-VISION.md](SYSTEM-VISION.md) | North star: peers, profiles, bridges |
| [CONTEXT.md](CONTEXT.md) | Current code baseline |
| [STATUS.md](STATUS.md) | Progress tracker |
| [plans/00-MASTER.md](plans/00-MASTER.md) | Phases A–F |

## Quick start

```bash
cp -r robotics-ipc-module /path/to/repo/
./robotics-ipc-module/install.sh
```

In Cursor:

> Read `robotics-ipc-module/AGENTS.md`, `DESIGN-PRINCIPLES.md`, and `STATUS.md`. `@ipc-robotics-orchestrator`

## Skills (phases A–F)

| Skill | Phase |
|-------|-------|
| `@ipc-robotics-orchestrator` | Pick next work |
| `@ipc-robotics-phase-a` … `phase-f` | Execute one phase |

Run `./robotics-ipc-module/install.sh` to copy skills to `.cursor/skills/`.

## Execution order

```
A → B ─┬→ D1
       ├→ C
       └→ D2–D4 → E → F
```

Phase **F** adds Python/Node/MAVLink/vision **examples** and deployment profiles—no core bloat.

## Prerequisites

- `make test-ipc` && `make test-router` green
- C++20, Linux, `-lrt` for SHM

## Portable copy

Vendor this entire `robotics-ipc-module/` directory into a new repo with your IPC tree; set `IPC_ROOT` in STATUS.md.
