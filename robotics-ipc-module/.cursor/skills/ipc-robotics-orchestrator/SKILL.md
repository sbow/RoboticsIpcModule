---
name: ipc-robotics-orchestrator
description: >-
  Orchestrates the robotics IPC module roadmap: reads STATUS.md, picks the next
  phase (A–E), and points to the correct phase skill and plan file. Use when
  starting a new session, continuing the robotics IPC plan, or when the user
  asks what to do next for the embedded robotics module.
---

# IPC robotics plan orchestrator

## Startup

1. Read `robotics-ipc-module/STATUS.md`
2. Read `robotics-ipc-module/DESIGN-PRINCIPLES.md` (summary)
3. Read `robotics-ipc-module/plans/00-MASTER.md`
4. If next phase is B, E, or F: read `SYSTEM-VISION.md`
5. Find first phase with Status `[ ]` in order: **A → B → C → D → E → F**
   - Phase D: prefer **D1** before D3 if B2 exists
   - Phase C can start after A if B is blocked
   - Phase F only after B1 + E1 recommended

## Dispatch

| Next phase | Skill to run |
|------------|--------------|
| A | `@ipc-robotics-phase-a` |
| B | `@ipc-robotics-phase-b` |
| C | `@ipc-robotics-phase-c` |
| D | `@ipc-robotics-phase-d` |
| E | `@ipc-robotics-phase-e` |
| F | `@ipc-robotics-phase-f` |

Tell the user: *"Next phase is X. I'll follow plans/X-....md"*

## End of session

Update `STATUS.md`:

- Check deliverable boxes
- Set `Current phase` to next incomplete
- Append session log row

## Verify (always)

```bash
make all && make test-ipc && make test-router
```

## References

- [AGENTS.md](../../../AGENTS.md)
- [CONTEXT.md](../../../CONTEXT.md)
