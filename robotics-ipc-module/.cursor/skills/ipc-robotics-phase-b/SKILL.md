---
name: ipc-robotics-phase-b
description: >-
  Executes Phase B (message and config) for the robotics IPC module: payload
  policy ADR, topology YAML/TOML loader, logging callback. Use for phase B of
  robotics-ipc-module or external router topology configuration.
---

# Phase B — Message & config

## Plan

`robotics-ipc-module/plans/B-message-and-config.md`

**Prerequisite:** Phase A marked complete in STATUS.md (or user override).

## Workflow

```
- [ ] B1: ADR payload policy + minimal API
- [ ] B2: topology loader + example config + test
- [ ] B3: RouterLogFn callback; remove string hot-path deps from library
- [ ] B4: Last-value cache (optional — note deferral in STATUS if skipped)
- [ ] make test-router (default demo still works)
- [ ] Update STATUS.md
```

## Key files (typical)

- `ipc/src/router/topology_loader.hpp`
- `config/profiles/*.toml` (Phase B shipped jetson_prod / x86_dev / hil / sim_cloud)
- `docs/adr/0005-*.md` (payload) if needed

## Acceptance

See plan file; minimum:

```bash
make all && make test-router
```
