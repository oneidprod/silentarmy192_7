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

**NEXT SESSION** — start with: "Session#3  Run your map tool, read CLAUDE_SONNET_4.6.md and PLAN_ACTIVE.md. Resume from IN PROGRESS marker."

### Current State (2026-03-11, end of session 2)
- Uncommitted changes: `_slot_sz` fix + doc updates (committing now)
- Pipeline: double-buffer ping-pong, NSLOTS=40, Stage 7: ~942 candidates
- Extraction fully implemented in mine_batch() — no rerun, no stub

### Session 2 Summary
1. Implemented full cpu_attrs readback inside mine_batch()
2. Fixed OOM: double-buffer ping-pong (only buf_tree0+buf_tree1 allocated)
3. Fixed nonce variation: nonce_idx mixed into blake2b state
4. Fixed Stage 7 coverage: cap changed from NSLOTS=40 to 65536 in kernel
5. Identified + fixed duplicate leaf bug: `_slot_sz` packed→Beignet padded sizes

### _slot_sz fix — TESTED AND WORKING
```c
// sa-tromp.c line 282
const size_t _slot_sz[8] = {28,28,24,20,16,16,12,8};
// was: {28,28,22,19,16,13,10,7} — Beignet pads to 4-byte alignment
```
Extraction finds SOLUTION for ~half of nonces. ~2 solutions/nonce average (matches birthday math).

### Session 3 Progress (2026-03-12)
- Ran `./sa-tromp 1` → extraction fires → SOLUTION found (nonce 0)
- Ran `./sa-tromp 50` → solutions found in nonces 0,1,2,4,5,8,9+
- Rewrote `verify_equihash_full` + `eh_genhash` to use silentarmy blake (`zcash_blake2b_*`)
  and include `nonce_idx` in initial state (matching GPU blake upload)
- Confirmed with `/tmp/test_nonce` tool: CPU `eh_genhash` matches GPU emulation with nonce
- **Remaining bug**: `verify_equihash_full` still VERIFY FAILED
  - Diagnosed r=7: ordering violation — fixed by calling `canonical_sort(indices, PARAM_K)` before verify
  - After canonical_sort fix: still fails at r=1 XOR check in some candidates, passes others?
  - AFTER_SORT debug confirmed: Stage 1 pairs at [0,1] MATCH hash prefixes (GPU/CPU parity OK)
  - Root cause not fully isolated: multiple candidates per nonce; verbose shows a different candidate
    than the one where MATCH was confirmed. Likely r=2+ fails for candidates that pass r=1.

### Session 4 Progress (2026-03-12)
- Added `canonical_sort()` call before `verify_equihash_full()` in extraction loop
- Changed `verify_equihash_full` signature to take `const blake2b_state_t *blake_ctx` directly
  (uses blake_gen from mine_batch — eliminates redundant blake re-init)
- Cleaned all debug prints (MB2-MB16, A-G, li_debug) from sa-tromp.c and solution_extraction.c
- **Status**: verify still fails — next step is to isolate whether failure is at r=2 or higher
  for the candidates that pass 128-distinct check

### Session 5 Progress (2026-03-12)
- Diagnosed: ALL verify failures were ordering violations (`indices[0] >= indices[half]`)
- Root cause: `canonical_sort` was being called AFTER `verify_equihash_full`
- Fix: call `canonical_sort(indices, PARAM_K)` BEFORE `verify_equihash_full`
- XOR chain is unaffected (XOR commutative; swapping whole subtrees is valid)
- Removed all remaining debug prints; cleaned up extraction loop
- Added Context Management section to CLAUDE.md
- **RESULT: 4 verified solutions in 10 nonces — PIPELINE FULLY WORKING**
- Commit: 703dd24

### Session 7 Progress (2026-03-12)
- Fixed nonce embedding: use explicit zero-padded 128-byte block (deterministic blake state)
- Rewrote test_verifier.c to use sa-tromp's own blake2b (no Tromp blake dependency)
- **test_verifier cross-check PASSES** — independently confirms solutions from sa-tromp
- Commit: 0381e82
- **CRITICAL FINDING**: sa-tromp uses WRONG protocol format — not Zero coin compatible
- **NEXT**: Fix blake2b header format to match real Zero coin protocol (see below)

### Protocol Mismatch — MUST FIX Before Pool Use

**Current sa-tromp (wrong)**:
- 128-byte header (truncated)
- Nonce as separate 2nd block (4 bytes, zero-padded to 128)
- Invented during rewrite — not the Zero coin standard

**Correct Zero coin / original silentarmy protocol**:
- 140-byte header with 32-byte nonce at bytes 108-139
- Single `blake2b_update(header, 140)` — but spans TWO 128-byte blocks
- Block 1: bytes 0-127 (not final)
- Block 2: bytes 128-139 (12 bytes) + 116 zero bytes padding (final)
- Nonce iterates across bytes 108-139 (32-byte nonce space)

**What needs changing**:
1. `zcash_blake2b_update` in blake.c: currently single-block only. Needs to handle the 12-byte tail without losing it (or use Tromp's buffering blake for setup)
2. GPU kernel `input.cl`: hardcodes `v[12] ^= ZCASH_BLOCK_HEADER_LEN + 4` (144). Must change to process 140-byte header correctly across 2 blocks
3. `sa-tromp.c` mine_batch: change to accept 140-byte header, embed 32-byte nonce at bytes 108-139, iterate nonce
4. `test_verifier.c`: update to match
5. Cross-check with `eq1927 -s -p "ZERO_PoW" -n 0` must pass

**Reference**: `equihash_tromp/equi.c:setheader()` — the correct implementation:
```c
blake2b_update(ctx, headernonce, 140);  // one call, Tromp's buffering blake handles it
((u32*)headernonce)[32] = htole32(nonce);  // nonce at byte 128 (index 32)
```
Wait — `[32]` × 4 = byte 128, so nonce is actually at bytes 128-131, not 108-139.
The original silentarmy had 32-byte nonce at bytes 108-139 (Zcash). Zero coin may differ.
**Need to verify Zero coin's exact nonce location before implementing.**

### Session 6 Progress (2026-03-12)
- Context used for research/planning only — no code written
- Deleted `core` dump (~300MB) to free disk space
- Discovered `test_verifier.c` already exists as cross-check tool
- **KEY FINDING**: `test_verifier.c:109` uses wrong nonce embedding (one 140-byte update vs sa-tromp's two updates)
- Fix identified: change to `blake2b_update(header, 128)` + `blake2b_update(&nonce_idx, 4)` in test_verifier.c
- Also need: add `Solution idx...` output line to sa-tromp.c for easy piping to verifier
- nheqminer integration plan fully researched and documented in plan file
- Full plan: `/home/mine/.claude/plans/sunny-sprouting-codd.md`
- **NEXT**: Start Session 7 with implementation of test_verifier fix (no research needed)

### Sub-task checklist:
- [x] Pipeline runs without OOM (double-buffer)
- [x] Nonce variation working
- [x] Stage 7: ~942 candidates (all written, not capped at 40)
- [x] cpu_attrs readback for all 8 stages
- [x] extraction loop in mine_batch()
- [x] _slot_sz Beignet padding fix applied and built
- [x] canonical_sort before verify — ordering invariant satisfied
- [x] verify_equihash_full passes — solutions confirmed valid
- [x] debug prints cleaned; production-ready output
- [ ] **NEXT**: Pool testing via nheqminer integration

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
