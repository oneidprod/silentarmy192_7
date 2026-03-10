# Plan: Immediate Next Step — GPU Test + Solution Extraction

## Context

All architectural work is complete (commits through ad11e0d):
- `kernel_round0_gen`: 2^24 work items × 2 hashes each = 2^25 hashes/nonce, dispatched in 64 batches of 2^18
- Stage 1–7 kernels: source-bucket-per-work-item pattern, fully implemented in `input.cl` (lines 1186–1464)
- `sa-tromp.c` `mine_batch()`: wired correctly — kernel args match kernel signatures, progressive tree alloc/free

What remains is to **verify the pipeline runs** and then **add solution extraction** (Step 5 of PLAN_KERNEL_ROUND0.md).

---

## Critical Files

| File | Role |
|------|------|
| [sa-tromp.c](sa-tromp.c) | Host code — `mine_batch()` at line 273, `main()` at line 463 |
| [input.cl](input.cl) | All GPU kernels — `kernel_round0_gen` at line 1481, stages 1–7 at lines 1186–1464 |
| [solution_extraction.c](solution_extraction.c) | Needs full rewrite for new attr encoding — included by sa-tromp.c at line 44 |
| [param.h](param.h) | Constants (NBUCKETS, NSLOTS etc.) |

---

## Part A: GPU Test (`./sa-tromp 1`)

No code changes needed. This validates the Phase 1 + Phase 2 pipeline.

### Steps

1. **Build**
   ```
   make clean && rm -f _kernel.h && make sa-tromp
   ```
   Expected: clean build, no warnings.

2. **Run**
   ```
   ./sa-tromp 1
   ```
   Expected output per stage:
   - `tree0`: ~32 hashes/bucket avg, max ~100, overflow=0 (NSLOTS=64 > 32 expected)
   - `Stage 1`: ~32 collisions/bucket
   - `Stage 2–6`: ~32 each
   - `Stage 7`: 0–2 solution candidates

### Fallback Options

| Symptom | Fix |
|---------|-----|
| GPU hang / SSH drop | In `sa-tromp.c` line 276: change `DISPATCH = (1<<18)` to `(1<<17)`, rebuild |
| Stage 1 = 0 | Add printf of `cnt[0]` after tree0 read to verify hash gen; check arg order |
| `clCreateBuffer` fails | GPU OOM — tree0 is 1.75 GB; check available memory |

---

## Part B: Fix Solution Extraction (Step 5)

After Stage 1 confirms ~32 collisions/bucket, update `solution_extraction.c` for the new architecture.

### Problem with Current solution_extraction.c

1. **Line 19**: `stage1_slot_t` uses `uint64_t attr` (8B) — GPU uses `uint32_t` (4B) → 32B vs 28B struct
2. **Lines 51–59**: `tree_idx0_stages27` / `tree_idx1_stages27` use old widths (14-bit bucket, 9-bit slots, stride=512) — new widths are 20-bit bucket, 6-bit slots, stride=64
3. **`tree_store_t`**: Wrong structure — alternating even/odd trees is an old pattern; new pattern has 7 sequential trees

### New attr Encoding (all stages 1–7)

```
attr = (src_bucket << 12) | (slot_i << 6) | slot_j
src_bucket = attr >> 12         // 20 bits (0..1M-1)
slot_i     = (attr >> 6) & 0x3F // 6 bits (0..63)
slot_j     = attr & 0x3F        // 6 bits (0..63)
flat_i     = src_bucket * NSLOTS + slot_i
flat_j     = src_bucket * NSLOTS + slot_j
```

Stage 0 base case: `attr = xi` (hash index 0..2^25-1)

### Rewrite Plan for solution_extraction.c

1. Fix `stage1_slot_t.attr` → `uint32_t`
2. Replace `tree_store_t` with flat pointer array `void *tree[8]` (tree[0]=tree0, ..., tree[7]=tree7)
3. Replace `tree_idx0_stages27` with:
   ```c
   static inline uint32_t flat_idx(uint32_t attr, int which) {
       uint32_t bucket = attr >> 12;
       uint32_t slot = which ? (attr & 0x3F) : ((attr >> 6) & 0x3F);
       return bucket * NSLOTS + slot;
   }
   ```
4. Rewrite `listindices` as a single recursive function (not 0/1 split):
   - Base case (tree0): return `((stage0_slot_t*)tree[0])[flat].attr` for both i and j
   - Recursive case: attr from tree[r] → descend into tree[r-1] at flat_i and flat_j
5. Update `mine_batch()` in sa-tromp.c to:
   - Keep `buf_tree1..tree7` alive after Stage 7
   - Read `buf_counts[6]` (tree7 count at bucket 0) and `buf_tree7` back to CPU
   - For each candidate: call `extract_solution()`, then `verify_equihash_full()`
   - Also need to keep `buf_tree0` available for base case lookups (or read tree0 back to CPU first)

> **Note**: tree0 is 1.75 GB — reading it back to CPU is expensive. Alternative: keep `buf_tree0` alive until extraction completes, then do a targeted GPU-side lookup. For simplicity, read to CPU (one-time cost per nonce).

### Test After Step 5

```
./sa-tromp 100
```
Expected: ≥1 `VALID SOLUTION #1 FOUND!` with 0 `dup_fail`.

---

## Session Continuity Protocol

To survive a 5-hour session limit mid-task, follow this pattern **before every file edit or shell command**:

### Step 0 (always first): Update CLAUDE_SONNET_4.6.md

Update the `## Immediate Next Step` section to the finest-grained sub-task that's currently *in progress*, not just "what's next." Example:

```markdown
## Immediate Next Step
**IN PROGRESS** (session may have ended mid-task):
- Editing solution_extraction.c: fixing stage1_slot_t.attr uint64_t → uint32_t
- flat_idx() helper written ✓
- listindices rewrite: NOT STARTED
- mine_batch() wiring: NOT STARTED
```

This means a new Claude session reading CLAUDE_SONNET_4.6.md will know the exact sub-step even if the session died mid-edit.

### Plan File Persistence

The plan file at `/home/mine/.claude/plans/dynamic-marinating-avalanche.md` is outside the project.
After plan approval, copy it into the project as `PLAN_ACTIVE.md` (git-tracked) so any future session finds it with `ls *.md`.

```
cp /home/mine/.claude/plans/dynamic-marinating-avalanche.md PLAN_ACTIVE.md
git add PLAN_ACTIVE.md && git commit -m "chore: save active plan for session continuity"
```

When a plan is complete, rename it to `PLAN_DONE_<date>.md` for history.

### New Session Startup (what to tell Claude)

If a session is interrupted, start the next session with:
> "Run your map tool, read CLAUDE_SONNET_4.6.md and PLAN_ACTIVE.md. Resume from IN PROGRESS marker."

---

## Commit Format

After Part A (if Stage 1 confirmed):
```
test: verify GPU pipeline — Stage 1 N collisions/bucket avg
- Status: working|partial
- Next: Step 5 solution extraction
```

After Part B:
```
fix: solution_extraction for new uint32 attr BUCKBITS=20 NSLOTS=64
- Status: working
- Next: pool testing
```
