# Claude Opus 4.6 — Session Log

## Operating Rules
1. **One step at a time.** Make one change, test it, commit it, report back.
2. **Commit before proceeding.** No multi-step investigations without a checkpoint.
3. **Explain before running.** State what I expect and why before each test.
4. **Follow CLAUDE.md governance.** Time-boxing, commit format, anti-spiral protocol.
5. **CLAUDE_SONNET_4.5.md is read-only history.** Reference, don't append.

## Mandatory Workflow (every change)
1. Plan → explain what I'll do and what I expect
2. Edit → make the code change
3. Test → run the test, check result
4. Commit code → `git add <files> && git commit`
5. Update this doc → append to Completed Steps, update Immediate Next Step
6. Commit doc → `git add CLAUDE_OPUS_4.6.md && git commit`
7. Report back → tell user what happened, wait for go-ahead

## Current Verified State (2026-03-06)

**Last Commit**: cf4bfc3 — `fix: macro redefinition bug in solution_extraction.c (Opus analysis)`
**Branch**: rewrite

### What Works
- ✅ All 7 GPU kernel stages implemented (Tromp bucket-based algorithm)
- ✅ Blake2b CPU/GPU parity (proven by compare_blake2b tool)
- ✅ Macro fix applied: solution_extraction.c inherits correct constants from sa-tromp.c
- ✅ 50K nonce test: all 5 collision attrs now non-zero (was 4/5 zero before fix)
- ✅ Stage 1→2 cascade produces collisions (39 in 50K test)

### What Doesn't Work Yet
- ❌ Stage 2 shows 0 collisions in larger (1M+) tests — cascade dies
- ❌ No Stage 7 candidates reached since macro fix
- ❌ Unknown if duplicate index bug is resolved (need Stage 7 candidates to test)

### Key Constants (sa-tromp.c)
- RESTBITS=10, BUCKBITS=14, NBUCKETS=16384, NSLOTS=512, SLOTBITS=9
- Batch size: 187K nonces (Beignet driver limit)

## Immediate Next Step
**Investigate why Stage 2 produces 0 collisions in most batches.**
- 50K test: Stage 2 = 39 collisions ✓
- 1M test: Stage 2 = 0 in all batches ✗
- This is the current blocker preventing cascade from reaching Stage 7.

## Completed Steps
| # | Date | Commit | Description |
|---|------|--------|-------------|
| 1 | 2026-03-06 | cf4bfc3 | Macro fix: removed conflicting #defines from solution_extraction.c |

## History Reference
- **CLAUDE_SONNET_4.5.md**: 2,852-line session log from Sonnet 4.5 (Mar 4-6)
- **ZERO_ATTR_BUG.md**: Opus's macro redefinition analysis
- **CLAUDE.md**: Project governance and anti-spiral protocol
