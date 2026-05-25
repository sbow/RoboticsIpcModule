# Subagent: Phase implementer

Use when a phase is large (B2 loader + tests, D full suite).

## Prompt template

Replace `<PHASE>` with A, B, C, D, or E.

```
Implement Phase <PHASE> of the robotics IPC module plan.

Required reads:
- robotics-ipc-module/AGENTS.md
- robotics-ipc-module/DESIGN-PRINCIPLES.md
- robotics-ipc-module/LESSONS-LEARNED.md
- robotics-ipc-module/SYSTEM-VISION.md (if phase B, E, or F)
- robotics-ipc-module/CONTEXT.md
- robotics-ipc-module/plans/<PHASE>-*.md
- robotics-ipc-module/STATUS.md

Rules:
- Follow the plan deliverables and "Do not" section
- Run all Acceptance commands; fix until pass
- Update STATUS.md checkboxes and session log
- Add ADRs for architectural decisions
- Minimal diffs; preserve header-only + no type-erased RouterLink

Return: files changed, acceptance output, remaining checklist items.
```

## Phase-specific hints

| Phase | Focus |
|-------|--------|
| A | grep include graph; MODULE.md |
| B | YAML schema tests first |
| C | try_send before eventfd |
| D | D1 unit tests before soak |
| E | deploy/ only; Jetson + x86 layout per SYSTEM-VISION |
| F | examples/bridges/ only; profile YAML |
