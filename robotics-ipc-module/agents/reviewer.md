# Subagent: Phase reviewer

Run after implementation, before marking STATUS complete.

## Prompt

```
Review the latest changes against robotics-ipc-module/plans/<PHASE>-*.md.

Check:
1. All deliverables present
2. Review checklist in plan
3. No scope violations (see plans/00-MASTER.md out of scope)
4. make all && make test-ipc && make test-router pass
5. ADR added if API/wire format changed

Output:
- Pass / fail per checklist item
- Gaps to fix (file:line if possible)
- Do not implement fixes unless a gap is critical
```
