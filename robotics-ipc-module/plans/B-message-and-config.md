# Phase B — Message & config

**Skill:** `@ipc-robotics-phase-b`  
**Depends on:** Phase A complete  
**Read:** [SYSTEM-VISION.md](../SYSTEM-VISION.md) (profiles, sideband), [LESSONS-LEARNED.md](../LESSONS-LEARNED.md) (CLI arity)

## Objective

Deployable **topology** and **payload policy** without recompiling per robot or per environment (Jetson vs HIL vs sim).

## Deliverables

### B1 — Payload policy (ADR + API)

Document and implement minimal API:

- **Control plane:** `RouterFrame` v1 (or version byte) for commands, status, small telemetry
- **Data plane:** sideband SHM/socket per [SYSTEM-VISION.md](../SYSTEM-VISION.md) — vision (CSI/V4L), ML tensors, not 22 B payload

ADR must state: max sizes, alignment, versioning, naming convention for sideband regions (`/robot_vision_nv12`, etc.).

### B2 — Topology loader + deployment profiles

- Format: YAML or TOML (pick one; document schema)
- Load: peers (`id`, `name`, `local` address), `router_listen`, `routes[]`
- **Profiles:** `config/profiles/jetson_prod.yaml`, `hil.yaml`, `sim_cloud.yaml`, `x86_dev.yaml` — same peer IDs, different `PeerAddress` / default transport (see SYSTEM-VISION)
- Validate: duplicate IDs, unknown `PeerAddressKind`, bounded string lengths
- Wire into `router_server` / test via `--config path` (keep demo_topology as fallback)
- **Lesson:** CLI paths must match loaded topology or fail at startup (ADR 0003 limitation)

Files (suggested):

- `cpp_tricks/ipc/src/router/topology_loader.hpp` (or `.cpp` if you accept one TU)
- `cpp_tricks/ipc/config/demo_topology.yaml` example
- Unit test: load valid + invalid fixtures

### B3 — Logging callback

Replace ad-hoc `router_log` dependency in library with optional:

```cpp
using RouterLogFn = void(*)(int level, const char* msg, size_t len);
void router_set_log_fn(RouterLogFn fn);  // in router_app or thin runtime header
```

Demo apps register stderr logger; library headers must not require `std::string` on forward hot path (on_forward callback may still log).

### B4 — Last-value cache (optional)

If timeboxed: subscriber-side cache for latest frame per `source_id` in `RouterClient` or small helper class. ADR note if deferred.

## Review checklist

- [ ] Config file changes do not require recompile of library
- [ ] Invalid config fails at startup with clear error
- [ ] `make test-router` still passes with default demo config
- [ ] New test loads YAML topology for one transport (SHM recommended)

## Acceptance

```bash
make all
make test-ipc
make test-router
# After D1 exists:
make test-ipc-unit   # or gtest target you add
./build/ipc/test/router_server shm --config cpp_tricks/ipc/config/demo_topology.yaml  # example CLI
```

## Do not

- Implement DDS
- Change SHM spin behavior (Phase C)

## Session prompt

```
Execute Phase B from robotics-ipc-module/plans/B-message-and-config.md.
Read AGENTS.md, STATUS.md, CONTEXT.md. Update STATUS.md when done.
```
