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
5. Update this doc → append to Completed Steps, update Immediate Next Step (no separate commit)
6. Report back → tell user what happened, wait for go-ahead

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
**Batch size is 90x too small for Equihash 192,7.**
- Current: 187K nonces = 374K hashes per batch
- Required: ~16.5M nonces = 33M hashes (2^25) for birthday bound
- The 20-bit attr encoding limits batch to 1M hashes max
- Even at 1M hashes, expected Stage 2 collisions ≈ 0.5 (not enough cascade)
- Previous "Stage 7 candidates" were artifacts of the +2 offset bug (fake collisions)
- **Need architectural change**: either wider attr encoding or different approach

## Completed Steps
| # | Date | Commit | Description |
|---|------|--------|-------------|
| 1 | 2026-03-06 | cf4bfc3 | Macro fix: removed conflicting #defines from solution_extraction.c |
| 2 | 2026-03-06 | 1137264 | XOR offset +2→+3 in all 7 stages + Stage 7 loop bound fix |

## History Reference
- **CLAUDE_SONNET_4.5.md**: 2,852-line session log from Sonnet 4.5 (Mar 4-6)
- **ZERO_ATTR_BUG.md**: Opus's macro redefinition analysis
- **CLAUDE.md**: Project governance and anti-spiral protocol
