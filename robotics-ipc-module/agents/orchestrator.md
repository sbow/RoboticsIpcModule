# Subagent: Plan orchestrator

Use with Task tool (`subagent_type=explore` or `generalPurpose`).

## Prompt

```
You are the orchestrator for the robotics IPC module plan in robotics-ipc-module/.

1. Read robotics-ipc-module/STATUS.md and plans/00-MASTER.md
2. Report: current phase, completed deliverables, blockers
3. Recommend exactly one next skill: ipc-robotics-phase-a through e
4. List acceptance commands from that phase plan

Do not implement code. Return a 10-line execution brief for the parent agent.
```
