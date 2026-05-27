# Phase A — Module packaging

**Skill:** `@ipc-robotics-phase-a`  
**Status tracking:** [../STATUS.md](../STATUS.md) → Phase A deliverables  
**Read:** [DESIGN-PRINCIPLES.md](../DESIGN-PRINCIPLES.md) (layering), [SYSTEM-VISION.md](../SYSTEM-VISION.md) (targets)

## Objective

Draw a clear **library vs example** boundary so the IPC/router can ship as a module in a robotics repo (Jetson + x86 + future bridges).

## Prerequisites

- [CONTEXT.md](../CONTEXT.md) baseline tests pass
- Phase A is `[ ]` in STATUS.md

## Deliverables

### A1 — `MODULE.md`

Create `${IPC_ROOT}/MODULE.md` (default `ipc/MODULE.md`) containing:

- Supported platforms (Linux, glibc, `-lrt` for SHM)
- Minimum compiler (C++20)
- Include graph: minimal includes for router-only apps vs full `ipc.hpp`
- Link flags: header-only except SHM (`-lrt`)
- Thread model: one thread per `RouterServer` / `RouterClient` instance
- Shutdown contract: `install_app_stop_handlers` / `router_stop_flag`

### A2 — Public vs example split

Document and apply:

| Library | Examples |
|---------|----------|
| `src/ipc/`, `src/router/`, umbrella headers | `test/router_*.cpp`, `test/echo_*.cpp`, `router_client_config.h` |

Actions:

- Add `examples/` or keep `test/` but state in MODULE.md that **apps must not include `router_client_config.h`**
- Ensure no library header includes `router_app.h` (app layer only)
- Optional: `router/public.hpp` slim include list

### A3 — Wire format versioning

Pick one (document in ADR):

1. Reserve byte or add `frame_version` field in `RouterFrame`, or
2. ADR stating v1 frozen with `kRouterFrameVersion = 1` constant

No breaking change to existing demo without migration note.

### A4 — ADR 0004

Create `docs/adr/0004-robotics-module-boundaries.md`:

- In scope: Linux SBC (Jetson), x86 dev, multi-process, profile-swappable transports, non-safety
- Out of scope: safety cert, DDS/Python/Node in-core, raw camera/MAVLink in 32 B frame
- Module API promise; bridges live under `examples/bridges/` (Phase F)
- Reference [SYSTEM-VISION.md](../SYSTEM-VISION.md) peer catalog as illustrative, not mandatory implementation

## Review checklist

- [ ] `grep -r router_client_config` only under test/examples
- [ ] `grep -r router_app` only in test/apps, not in `src/router/` or `src/ipc/`
- [ ] MODULE.md lists all public entry headers
- [ ] ADR 0004 linked from README or MODULE.md

## Acceptance

```bash
make all
make test-ipc
make test-router
test -f ipc/MODULE.md
test -f docs/adr/0004-robotics-module-boundaries.md
```

## Do not

- Add ROS 2 or DDS implementation
- Refactor transports (Phase C)
- Add YAML loader yet (Phase B)

## Session prompt (copy-paste)

```
Execute Phase A from robotics-ipc-module/plans/A-module-packaging.md.
Read AGENTS.md and STATUS.md first. Update STATUS.md when done.
```
