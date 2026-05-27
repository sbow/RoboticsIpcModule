# Robotics IPC module — AI-executable plan pack

Evolve a **header-only Linux IPC/router** into the C++ **message fabric** for a robotics stack: Jetson, x86+CUDA, HIL/sim/cloud, with bridges to Python (ML), Node (dashboards), cameras (CSI/V4L), and MAVLink (MCU).

## Documentation map

| Doc | Purpose |
|-----|---------|
| [AGENTS.md](AGENTS.md) | Session entry contract |
| [DESIGN-PRINCIPLES.md](DESIGN-PRINCIPLES.md) | **Must-read** rules for all phases |
| [LESSONS-LEARNED.md](LESSONS-LEARNED.md) | Bugs and fixes from the IPC/router lineage (cpp_tricks → RoboticsIpcModule) |
| [SYSTEM-VISION.md](SYSTEM-VISION.md) | North star: peers, profiles, bridges |
| [CONTEXT.md](CONTEXT.md) | Current code baseline |
| [STATUS.md](STATUS.md) | Progress tracker |
| [plans/00-MASTER.md](plans/00-MASTER.md) | Phases A–F |

## Naming convention

- **Module name (long form):** `RoboticsIpcModule` — used in prose, repo URL, and the heading of every doc.
- **Resource prefix (short form):** `rim` — used in every runtime identifier this module owns:
  - SHM regions under `/dev/shm/` → `/dev/shm/rim_router_sensor`, `/dev/shm/rim_router_controller`, etc.
  - UDS sockets → `/tmp/rim_router.sock` (x86 dev) or `/run/robot/rim_router.sock` (production).
  - Log files → `/tmp/rim_router.log`, `/tmp/rim_router_c.log` (recorder).
  - systemd units → `rim-router.service`, `rim-peer@.service` (Phase E onward).
  - Test-private namespaces → `rim_burst_sensor_*`, `rim_slow_recorder_*`, `rim_fault_*`, etc.
- **Origin note:** the `ipc/` tree was vendored from [`sbow/cpp_tricks`](https://github.com/sbow/cpp_tricks) at Phase A baseline (`243ede0`). Runtime resources originally carried the `cpp_tricks_*` prefix; they were renamed to `rim_*` in a dedicated commit immediately before Phase E so the module stops carrying a dead-repo identifier (see [LESSONS-LEARNED.md](LESSONS-LEARNED.md) and [docs/adr/0004-robotics-module-boundaries.md](../docs/adr/0004-robotics-module-boundaries.md) "Resource-name note").

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
