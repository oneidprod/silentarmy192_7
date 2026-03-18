# Claude Governance — Equihash 192,7

## Anti-Spiral Rules
- **Max 2 hours per investigation** before mandatory commit + reassessment
- **If stuck**: stop, commit current state, document what was tried, ask user
- **If infinite loop**: immediate stop — don't retry same approach
- **Must get valid local solutions BEFORE pool testing**

## Circuit Breakers
| Symptom | Action |
|---------|--------|
| Build corruption | `make clean && rm -f _kernel.h && make` |
| OpenCL errors / clCreateBuffer fails | OpenCL reset (see below) |
| Segfault | Revert to last stable commit |
| >2 hours no progress | Force commit + reassess |

## OpenCL Reset Procedure
When: `clCreateBuffer` errors, zero solutions, driver instability
```
git add -A && git commit -m "checkpoint before reset"
make clean && rm -f _kernel.h
# check dmesg for GPU errors; restart driver if needed
make -j4
./sa-tromp 1   # verify pipeline works
```
No git commit needed for the reset itself.

## Git Commit Format
```
<category>: <brief description>

- <specific change 1>
- <specific change 2>
- Status: <working|broken|partial>
- Next: <next planned step>
```
Commit every 30-45 min or at logical breakpoints.

## Context Management
- **MANDATORY: Run `/context` at the START of every turn before doing any work**
- If at or above 40%: warn the user immediately before proceeding
- If at or above 50%: stop immediately — commit all in-progress work, update CLAUDE_SONNET_4.6.md
- Notify user: "Context at ~X% — stopping to preserve session limit. Committed checkpoint."
- Do NOT continue coding after 50%; wait for user to start a new session

## Active Session Doc
See [CLAUDE_SONNET_4.6.md](CLAUDE_SONNET_4.6.md) for current state, active plan, and next step.
