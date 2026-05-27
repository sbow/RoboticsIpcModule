# Phase E — Robotics integration

**Skill:** `@ipc-robotics-phase-e`  
**Depends on:** B2, C1, D2 minimum  
**Read:** [SYSTEM-VISION.md](../SYSTEM-VISION.md) (full stack)

## Objective

Reference deployment for **Jetson on-robot** and **x86 lab** stacks: router + peers + optional CUDA/CSI notes.

## Deliverables

### E1 — Reference architecture doc

`docs/robotics-reference-layout.md`:

- Peer catalog from SYSTEM-VISION (sensor, controller, recorder, vision, ml_inference, mavlink_gateway, dashboard_feed)
- **Jetson:** SHM for high-rate C++ peers; UDS to logger
- **x86 + CUDA:** same topology, UDS/UDP profile; ML process optional
- **CSI vs USB camera:** separate `vision_capture` process; metadata only on router
- **MAVLink:** gateway process, not in router core
- Process diagram (mermaid) including Python/Node as bridge processes

### E2 — Process supervision

`deploy/systemd/` or `robotics-ipc-module/deploy/`:

- `router.service` starts first
- client units `After=router.service`
- `ExecStop=/bin/kill -TERM`, timeout, cleanup note for SHM/UDS

### E3 — Bridge pointers (optional)

Link to Phase F: `examples/bridges/python_peer`, `node_gateway`, `mavlink_gateway` — no ROS in core unless user adds separate ADR.

### E4 — Time sync

ADR section: replace router-relative ns with `CLOCK_MONOTONIC` raw or PTP offset field.

## Review checklist

- [ ] Fresh install: systemd units start router + 3 clients on bench hardware
- [ ] Shutdown leaves no stale `/dev/shm/rim_*` after clean stop
- [ ] MODULE.md links reference layout

## Acceptance

```bash
make all
make test-router
# Manual: systemd dry-run
systemd-analyze verify deploy/systemd/*.service 2>/dev/null || true
```

## Do not

- Claim safety certification
- Merge ROS into library headers

## Session prompt

```
Execute Phase E from robotics-ipc-module/plans/E-robotics-integration.md.
Update STATUS.md. Prefer deploy/ examples over modifying core library.
```
