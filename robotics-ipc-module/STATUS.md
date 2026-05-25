# Execution status

> **Agents:** Update every session. Read [DESIGN-PRINCIPLES.md](DESIGN-PRINCIPLES.md) before coding.

## Configuration

| Key | Value |
|-----|-------|
| `IPC_ROOT` | `cpp_tricks/ipc` |
| `PLANS_ROOT` | `robotics-ipc-module` |
| Baseline tag / commit | `243ede0d905e0e5365073b83feff06ba3427db07` (initial commit, `main`, `github.com/sbow/RoboticsIpcModule`) |
| Target platforms | Jetson (embedded), x86+CUDA (dev), HIL/sim (UDP) |

## Current phase

**Next:** `A` — Module packaging

## Phase completion

| Phase | Name | Status | Notes |
|-------|------|--------|-------|
| A | Module packaging | `[ ]` | |
| B | Message & config | `[ ]` | Profiles + sideband ADR |
| C | Transport hardening | `[ ]` | Jetson SHM |
| D | Validation & stress | `[ ]` | |
| E | Robotics integration | `[ ]` | Jetson + x86 layout |
| F | Interoperability bridges | `[ ]` | Python, Node, MAVLink, vision |

### Phase A deliverables

- [ ] A1 `MODULE.md` (Jetson + x86, bridge exclusion)
- [ ] A2 Public vs example split
- [ ] A3 Wire format version / ADR
- [ ] A4 ADR 0004 (references SYSTEM-VISION)

### Phase B deliverables

- [ ] B1 Payload + sideband ADR (vision/ML/MAVLink metadata)
- [ ] B2 Topology loader + `config/profiles/*.yaml`
- [ ] B3 Logging callback
- [ ] B4 Last-value cache (optional)

### Phase C deliverables

- [ ] C1 SHM `try_send` / bounded wait
- [ ] C2 Idle wake (eventfd or ADR deferral)
- [ ] C3 Router metrics
- [ ] C4 Datagram seq (optional)

### Phase D deliverables

- [ ] D1 Unit tests (+ cli_args regression)
- [ ] D2 Integration (restart, profile smoke)
- [ ] D3 Stress/soak
- [ ] D4 Fault injection

### Phase E deliverables

- [ ] E1 Reference layout (vision, ML, MAVLink, dashboard peers)
- [ ] E2 systemd (Jetson-oriented)
- [ ] E3 Bridge pointers to Phase F
- [ ] E4 Monotonic/PTP timestamp ADR

### Phase F deliverables

- [ ] F1 Profile templates + `deployment-profiles.md`
- [ ] F2 Python bridge example
- [ ] F3 Node gateway example
- [ ] F4 MAVLink gateway sketch
- [ ] F5 Vision metadata peer sketch

## Blockers

_None._

## Session log

| Date | Phase | Summary | Agent/human |
|------|-------|---------|-------------|
| | | Plan pack created | initial |
| | | Added DESIGN-PRINCIPLES, LESSONS-LEARNED, SYSTEM-VISION, Phase F | plan update |
| 2026-05-25 | — | Repo init + push to `github.com/sbow/RoboticsIpcModule`; baseline `243ede0` recorded | orchestrator |
