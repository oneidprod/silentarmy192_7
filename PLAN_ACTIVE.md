# Plan: Resume — Fix OOM-at-startup + Verify Solutions

## CURRENT STATE (2026-03-11)

Steps A and B code are done. Blocked on exit-137 before first printf.

### Commits this session:
- e3f1347: NSLOTS 48→40
- 140aa52: pipeline verified (Stage 7: 942 candidates, 1.38s, no OOM)

### Uncommitted changes (in sa-tromp.c + solution_extraction.c):
- solution_extraction.c: full rewrite (flat_idx_of, listindices, extract_solution, flat uint32 attrs)
- sa-tromp.c: cpu_attrs[8] collected via 64MB chunks before each release; Stage 7 extraction loop

### Current blocker: `./sa-tromp 1` killed (exit 137, SIGKILL) before first printf
`./sa-tromp --help` works. Init_opencl() suspect. Possible Beignet heap leak from prior session.

### Debug + fix plan:
1. `dmesg | grep -i "oom\|killed" | tail -20` → confirm OOM killer
2. `git stash` → revert to last working binary → test: `./sa-tromp 1` → if it works, the extraction code changes themselves cause OOM at init
3. Or: add `printf("A\n"); fflush(stdout);` BEFORE `init_opencl()`, rebuild, rerun → see if "A" prints
4. If Beignet init OOMs: `sudo swapoff -a && sudo swapon -a` clears swap, then retry
5. If still OOM: reduce NSLOTS to 32 or investigate Beignet driver memory leak

---

## OLD CONTEXT (kept for reference):

Architecture is complete through commit ad11e0d. The pipeline (kernel_round0_gen → Stage 1-7) is implemented. Two blocking issues remain before valid solutions can be found:

1. **OOM crash**: NSLOTS=64 allocates 3.5 GB peak (tree0+tree1), total ~5.4 GB — at system limit; crashes SSH.
2. **solution_extraction.c is broken**: uses old attr encoding (uint64 stage1 attr, 14-bit bucket, 9-bit slots, stride=512) vs new encoding (uint32 everywhere, 20-bit bucket, 6-bit slots, stride=NSLOTS).

---

## Step 0: Plan Persistence (do this first, before any code change)

Before touching any source file, copy this plan into the project and update the session doc:

```bash
cp /home/mine/.claude/plans/eager-tumbling-cascade.md /home/mine/silentarmy192_7/PLAN_ACTIVE.md
cd /home/mine/silentarmy192_7
git add PLAN_ACTIVE.md
```

Then update `CLAUDE_SONNET_4.6.md` → `## Immediate Next Step` to show the exact sub-task now in progress (e.g., "Editing sa-tromp.c NSLOTS 64→48") before every code edit.

Commit plan + doc together:
```
git commit -m "chore: update PLAN_ACTIVE + session doc before NSLOTS fix"
```

This ensures a new session launched mid-task can find the plan with `ls *.md` and resume.

---

## Critical Files

| File | Purpose |
|------|---------|
| [sa-tromp/sa-tromp.c](sa-tromp/sa-tromp.c) | Host — NSLOTS define (line 35), mine_batch() (line 273) |
| [sa-tromp/input.cl](sa-tromp/input.cl) | GPU kernels — NSLOTS_STAGE1 define (line 1153) |
| [sa-tromp/solution_extraction.c](sa-tromp/solution_extraction.c) | Needs full rewrite — included by sa-tromp.c |

---

## Step A: Fix OOM — NSLOTS 64 → 48

**Two line changes:**

1. `sa-tromp/sa-tromp.c` line 35: `#define NSLOTS 64` → `#define NSLOTS 48`
2. `sa-tromp/input.cl` line 1153: `#define NSLOTS_STAGE1 64` → `#define NSLOTS_STAGE1 48`

Memory after fix: peak = 2 × 1M × 48 × 28B = 2.69 GB → total ~4.59 GB → 0.81 GB margin.
SLOTBITS stays 6 (6 bits holds 0–47 fine).

**Build + test:**
```
cd sa-tromp && make clean && rm -f _kernel.h && make sa-tromp && ./sa-tromp 1
```

**Expected output:**
- tree0: ~32 hashes/bucket avg, max ~55, 0 overflow
- Stage 1: ~32 collisions/bucket
- Stages 2–6: ~32 each
- Stage 7: 0–2 solution candidates

**Fallback if still OOM:** reduce to NSLOTS=40.
**Fallback if GPU hang:** reduce DISPATCH from 1<<18 to 1<<17 in sa-tromp.c.

**Commit after confirmed:**
```
test: verify GPU pipeline NSLOTS=48 — Stage 1 N col/bucket
- Status: working|partial
- Next: Step 5 solution extraction
```

---

## Step B: Fix solution_extraction.c

Current bugs (confirmed by reading the file):

1. **Line 19**: `stage1_slot_t` uses `uint64_t attr` (8B) — GPU uses `uint32_t` (4B) → 32B vs 28B struct
2. **Lines 51–59**: `tree_idx0_stages27` / `tree_idx1_stages27` use 14-bit bucket + 9-bit slots + stride=512 — new is 20-bit bucket + 6-bit slots + stride=NSLOTS(48)
3. **`tree_store_t`**: Named struct with alternating even/odd trees — old pattern; new has 7 sequential trees

### Rewrite Plan

**A. Fix `stage1_slot_t`:**
```c
typedef struct { uint32_t attr; unsigned char hash[21]; unsigned char pad[3]; } stage1_slot_t;
// Change uint64_t → uint32_t (attr field only; pad stays)
```

**B. Replace `tree_store_t` with flat pointer array:**
```c
// Remove tree_store_t entirely.
// In extract_solution() / listindices(), receive void *tree[8] where:
//   tree[0] = stage0_slot_t* (tree0)
//   tree[1] = stage1_slot_t* (tree1)
//   ...
//   tree[7] = stage7_slot_t* (tree7)
```

**C. Replace tree_idx helpers with unified flat_idx:**
```c
static inline uint32_t flat_idx(uint32_t attr, int which) {
    uint32_t bucket = attr >> 12;             // 20-bit bucket
    uint32_t slot = which ? (attr & 0x3F)     // slot_j (6 bits)
                          : ((attr >> 6) & 0x3F); // slot_i (6 bits)
    return bucket * NSLOTS + slot;
}
```

**D. Rewrite `listindices` as single recursive function:**
```c
// Base case (round=0): return tree0[flat].attr for both i and j
// Recursive: attr from tree[round] → descend tree[round-1] at flat_i and flat_j
static void listindices(void **tree, int round, uint32_t flat, uint32_t *indices, int *cnt) {
    if (round == 0) {
        stage0_slot_t *t0 = (stage0_slot_t *)tree[0];
        indices[(*cnt)++] = t0[flat].attr;  // raw hash index (xi)
        return;
    }
    // get attr from this round's tree
    uint32_t attr = ((uint32_t *)((char *)tree[round] + flat * slot_size[round]))[0];
    uint32_t fi = flat_idx(attr, 0);
    uint32_t fj = flat_idx(attr, 1);
    listindices(tree, round-1, fi, indices, cnt);
    listindices(tree, round-1, fj, indices, cnt);
}
```

**E. Wire mine_batch() in sa-tromp.c to call extraction:**
- Keep buf_tree0..tree7 alive after Stage 7
- Read buf_tree7 and buf_counts back to CPU
- For each candidate at tree7: read-back tree0..tree6 as needed (or keep all alive)
- Call listindices() → sort → verify_equihash_full()
- Note: tree0 = 1.34 GB — read back once per nonce (acceptable)

### Test After Step B
```
./sa-tromp 100
```
Expected: ≥1 `VALID SOLUTION #1 FOUND!` with 0 `dup_fail`.

**Commit after confirmed:**
```
fix: solution_extraction for uint32 attr BUCKBITS=20 NSLOTS=48
- Status: working
- Next: pool testing
```

---

## Verification Sequence

1. `make clean && rm -f _kernel.h && make sa-tromp` — clean build, 0 warnings
2. `./sa-tromp 1` — pipeline runs, Stage 1 shows ~32 col/bucket, no OOM
3. `./sa-tromp 100` — ≥1 valid solution, 0 dup_fail

---

## Update CLAUDE_SONNET_4.6.md After Each Sub-task

Per session continuity protocol: update `## Immediate Next Step` before any edit so new sessions know exact sub-step.
