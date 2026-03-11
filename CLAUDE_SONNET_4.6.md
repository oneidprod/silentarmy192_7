# Claude Sonnet 4.6 — Session Log

## Operating Rules
1. **One step at a time.** Make one change, test it, commit it, report back.
2. **Commit before proceeding.** No multi-step investigations without a checkpoint.
3. **Explain before running.** State what I expect and why before each test.
4. **Follow CLAUDE.md governance.** Time-boxing, commit format, anti-spiral protocol.
5. **CLAUDE_OPUS_4.6.md is read-only history.** Reference, don't append.

## Mandatory Workflow (every change)
1. Plan → explain what I'll do and what I expect
2. Edit → make the code change
3. Test → run the test, check result
4. Commit code → `git add <files> && git commit`
5. Update this doc → append to Completed Steps, update Immediate Next Step (no separate commit)
6. Report back → tell user what happened, wait for go-ahead

## Hardware
- **GPU**: Intel integrated GPU, Beignet OpenCL driver
- **Shared RAM**: ~5.8GB available to GPU
- **Beignet limits**: No GPU watchdog — large NDRange dispatches (>~2^20 work items with heavy compute) hang the GPU and drop SSH. Must batch large kernels into ≤2^18–2^20 work items per dispatch.

## Current Verified State (2026-03-09)

**Last Commit**: ae68936 — `feat: add kernel_round0_gen GPU hash generation`
**Branch**: rewrite

**Active Plan**: [PLAN_KERNEL_ROUND0.md](PLAN_KERNEL_ROUND0.md)
(Full architectural rewrite — GPU hash generation + source-bucket kernel pattern)

### What Works
- ✅ All 7 GPU kernel stages implemented (Tromp bucket algorithm)
- ✅ Blake2b CPU/GPU parity proven (compare_blake2b tool)
- ✅ Macro fix (cf4bfc3): solution_extraction.c inherits correct constants
- ✅ XOR offset fix (1137264): all stages use correct +3 offset
- ✅ Stage 1 ulong attr fix (9f85090): correct idx0/idx1 encoding

### Root Cause (Sonnet 4.6 Analysis — 2026-03-09)
- ❌ **Batch architecture**: each batch = ~374K hashes; solution needs all 33.5M hashes of ONE mining nonce. P(solution/batch) ≈ 10^-256. Cannot find solutions by design.
- ❌ **Stage 2-7 kernels**: O(NBUCKETS × NSLOTS) global scan per work item. With NBUCKETS=1M, completely intractable.
- ❌ No valid solutions verified locally

## Root Cause Analysis (Sonnet 4.6 Code Review)

### Bug 1: Stage 1 attr — 20-bit idx0 truncation

In `input.cl` Stage 1 kernel:
```c
// Line 1241
slot->attr = (idx0 << 12) | delta;
// 20-bit idx0 | 12-bit delta = 32-bit uint
```

`idx0` is the global hash index of the first collision partner. It is 20 bits wide, so it can only address hashes 0..1,048,575 (max 524,288 nonces before overflow).

With `./sa-tromp 1000000` (1M nonces = 2M hashes):
- Hash indices run 0..1,999,999 — requires 21 bits
- Indices > 1,048,575 get their upper bits truncated → attr is WRONG
- Corrupted attr → solution_extraction reads wrong positions → duplicate indices

**Also broken**: `delta = (idx1 - idx0) & 0xFFF` (12 bits, max 4095). Within a bucket, two colliding hash indices can be millions apart. Delta wraps → non-unique.

### Bug 2: Batch size is too small for full cascade

Even with correct attr, the birthday math shows Stage 2+ cascade at 2M nonces (4M hashes):
- 4M hashes / 16K buckets = 244 per bucket at Stage 1 input
- Stage 1 collisions: C(244,2)/1024 ≈ 29 per bucket → 475K total Stage 1 slots
- Stage 2 input: 475K/16K ≈ 30 per bucket → Stage 2 total: C(30,2)/1024 × 16K ≈ 7K
- Stage 3 input: 7K/16K ≈ 0.4 per bucket → cascade dies

Full cascade to Stage 7 requires ~2^25 = 33M hashes (16M "nonces" in sa-tromp terminology). That requires:
- round0 buffer: 33M × 24 bytes = 800MB (large but potentially feasible)
- NSLOTS ≥ 2048 per bucket for Stage 0 (currently 512 → bucket overflow at full scale)

### What the attr fix enables

Fixing the attr encoding at minimum allows correct Stage 1→2 cascade for batches within the NSLOTS=512 limit (up to ~250K hashes per bucket → 4M nonces before overflow). This validates the pipeline correctness even if full cascade requires more architectural work.

## Immediate Next Step

**NEXT SESSION** — start with: "Session#1  Run your map tool, read CLAUDE_SONNET_4.6.md and PLAN_ACTIVE.md. Resume from IN PROGRESS marker."

### Current State (2026-03-11, end of session 2)
- Last commit: (see git log)
- `./sa-tromp 1` runs 1.85s, NSLOTS=40, Stage 7: 942 candidates, 40 in bucket 0
- `mine_batch_extract()` is a **safe stub** (prints message, returns 0 — does NOT crash)
- Cascade counts 32M→31M→29M→26M→20M→12M→942 are **mathematically correct** (not a bug)

### What crashed SSH this session
Attempted `mine_batch_extract()` rerun approach: ran full cascade twice back-to-back.
GPU + RAM overloaded → SSH server killed. Reverted to safe stub.

### NEXT STEP: Implement extraction INSIDE mine_batch() (no rerun)
See PLAN_ACTIVE.md for full step-by-step implementation with exact code snippets.
Summary: save attrs during the lean cascade (step 2-3 in the stage loop), then extract after stage 7.
Memory budget: 3.7GB worst case — safe with 5.4GB available.

### Sub-task checklist:
- [x] Pipeline runs 1.85s without OOM
- [x] Stage 7: 942 candidates
- [x] Cascade counts confirmed mathematically correct (no bug to fix)
- [ ] **NEXT**: Add cpu_attrs readback inside mine_batch() stage loop (see PLAN_ACTIVE.md)
- [ ] **THEN**: `./sa-tromp 1` → extraction fires, check for valid solutions
- [ ] **THEN**: `./sa-tromp 50` → find ≥1 verified solution

## Completed Steps
| # | Date | Commit | Description |
|---|------|--------|-------------|
| (inherited) | 2026-03-06 | cf4bfc3 | Macro fix: removed conflicting #defines from solution_extraction.c |
| (inherited) | 2026-03-06 | 1137264 | XOR offset +2→+3 in all 7 stages + Stage 7 loop bound fix |
| 1 | 2026-03-06 | 9f85090 | Stage 1 attr encoding: ulong replaces lossy 20+12 bit uint |
| 2 | 2026-03-09 | aba5111 | Step 1: RESTBITS 10→4, NSLOTS 512→64, SLOTBITS 9→6; Stages 2-7 attr shifts <<18→<<12, <<9→<<6 |
| 3 | 2026-03-09 | ae68936 | Step 2: kernel_round0_gen + stage0_slot_t; smoke test 524K hashes avg 0.5/bucket max 6 no overflow 0.26s |
| 4 | 2026-03-10 | ad11e0d | Step 3 (kernel side done in ae68936): fix sa-tromp host — mine_batch() uses kernel_round0_gen 2^24 WI batched at 2^18; Stage 1 args fixed; stages 2-7 progressive alloc/free; input.cl duplicate typedef removed |

## History Reference
- **CLAUDE_HAIKU.md**: Blake2b fix, stack overflow fix, test tooling
- **CLAUDE_SONNET_4.5.md**: 2,852-line session log — attr encoding, zero attr bug
- **CLAUDE_OPUS_4.6.md**: Macro fix analysis + XOR offset fix
- **ZERO_ATTR_BUG.md**: Opus's macro redefinition analysis
- **REFERENCE_MAP.md**: Full file/folder index (sections A-I)
- **PLAN_KERNEL_ROUND0.md**: Active plan — architectural rewrite
- **CLAUDE.md**: Project governance and anti-spiral protocol
- **.specstory/**: SpecStory VS Code extension chat logs (gitignored)
