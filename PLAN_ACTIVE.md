# PLAN_ACTIVE — Equihash 192,7 GPU Miner
**Last updated**: 2026-03-11
**Branch**: rewrite
**Status**: Pipeline working, extraction stubbed (crashed SSH — see below)

---

## Current State (start here every session)

```
./sa-tromp 1    → runs 1.85s, NSLOTS=40, Stage 7: 942 candidates, 40 in bucket 0
```

Pipeline is complete and correct. Only missing piece: solution extraction.

**What compiles and runs safely**: `./sa-tromp 1` — lean cascade, extraction is a stub (safe, prints message, returns).
**What crashed SSH**: the `mine_batch_extract()` rerun approach — ran full cascade twice back-to-back. OOM + GPU overload killed SSH server. That code is now stubbed out.

---

## Hardware Constraints (CRITICAL — read before any GPU change)

- **GPU**: Intel integrated (Beignet driver). GPU and CPU **share the same RAM**.
- **RAM**: 7.6 GB total, ~5.4 GB available.
- **Beignet GPU watchdog**: NDRange dispatches > ~2^18–2^20 work items with heavy compute HANG GPU and DROP SSH. Always batch in ≤ 2^18 work items per clEnqueueNDRangeKernel. Current code batches at 2^18 — do not increase.
- **Current NSLOTS=40**: tree0 = tree1 = 1M×40×28B = **1.07 GB each on GPU**. Two trees simultaneously = 2.14 GB. Safe within 5.4 GB free.
- **Never** run two full cascades back-to-back (the broken rerun approach).

---

## Key Constants (sa-tromp.c + input.cl must match)

| Constant | Value | Notes |
|----------|-------|-------|
| PARAM_N | 192 | WN |
| PARAM_K | 7 | WK |
| NBUCKETS | 1<<20 = 1,048,576 | 1M buckets |
| NSLOTS | 40 | per bucket; tree0/tree1 = 1.07 GB each |
| BUCKBITS | 20 | bits used for bucket index |
| RESTBITS | 4 | bits matched within bucket per stage |
| SLOTBITS | 6 | bits for slot index in attr (log2(64) >= log2(40)) |
| PROOFSIZE | 128 | 2^K indices |

---

## File Map

| File | Role |
|------|------|
| `sa-tromp.c` | Host: mine_batch(), mine_batch_extract() stub, main() |
| `input.cl` | GPU kernels: kernel_round0_gen, kernel_stage1..7_collisions |
| `solution_extraction.c` | CPU: extract_solution(), listindices(), flat_idx_of() |
| `solution_extraction.h` | Declares extract_solution() |
| `param.h` | All #defines (PARAM_N, NBUCKETS, NSLOTS, etc.) |
| `blake.c` / `blake.h` | CPU blake2b for zcash_blake2b_init/update |

---

## Attr Encoding

Every GPU stage stores `uint32_t attr` as first field of each slot:

| Stage | attr encodes |
|-------|-------------|
| 0 (tree0) | `xi` = raw hash index (0..2^25-1) |
| 1-7 | `(src_bucket << 12) | (i << 6) | j` where src=parent bucket, i/j=slot indices |

`solution_extraction.c:flat_idx_of()` uses `NSLOTS` stride — must match GPU NSLOTS=40.

---

## THE TASK: Implement extraction in mine_batch() directly (no rerun)

### Memory budget (NSLOTS=40, safe to proceed)
- Per stage: prev_tree(1.07GB GPU) + curr_tree(1.07GB GPU) + tmp_readback(1.07GB CPU) = 3.2GB peak
- After step: prev freed, tmp freed → only cpu_attrs[r](160MB) remains
- All 8 cpu_attrs live during extraction: 8 × 160MB = 1.28GB
- Total worst case: baseline(0.5GB) + 3.2GB = **3.7GB** — within 5.4GB available

### Implementation (in mine_batch(), NOT a separate function)

**Step 1** — Declare `cpu_attrs` at top of mine_batch():
```c
uint32_t *cpu_attrs[8] = {NULL};
const size_t slot_sz[8] = {28,28,22,19,16,13,10,7};
```

**Step 2** — After kernel_round0_gen loop + clFinish (~line 340), readback tree0 attrs:
```c
{
    size_t gsz = tree_size * slot_sz[0];
    void *tmp = malloc(gsz);
    clEnqueueReadBuffer(queue, buf_tree0, CL_TRUE, 0, gsz, tmp, 0, NULL, NULL);
    cpu_attrs[0] = malloc(tree_size * sizeof(uint32_t));
    for (size_t k = 0; k < tree_size; k++)
        cpu_attrs[0][k] = *(uint32_t *)((char*)tmp + k * slot_sz[0]);
    free(tmp);
    /* keep buf_tree0 alive as 'prev' input to stage 1 */
}
```

**Step 3** — In the stage 1-7 loop, after clFinish(), BEFORE clReleaseMemObject(prev):
```c
{
    size_t gsz = tree_size * slot_sz[s];
    void *tmp = malloc(gsz);
    clEnqueueReadBuffer(queue, curr, CL_TRUE, 0, gsz, tmp, 0, NULL, NULL);
    clReleaseMemObject(prev);        /* free prev GPU FIRST to make room */
    if (s == 1) clReleaseMemObject(prev_cnt);
    cpu_attrs[s] = malloc(tree_size * sizeof(uint32_t));
    for (size_t k = 0; k < tree_size; k++)
        cpu_attrs[s][k] = *(uint32_t *)((char*)tmp + k * slot_sz[s]);
    free(tmp);
    prev = curr;
}
```
Remove the existing `clReleaseMemObject(prev)` calls that currently appear after the loop.

**Step 4** — Replace the `if (nsol > 0)` block:
```c
if (nsol > 0) {
    printf("  [Stage 7] %u candidate(s) — extracting...\n", nsol);
    uint32_t tree_sz = (uint32_t)tree_size;
    for (uint32_t s = 0; s < nsol && s < NSLOTS; s++) {
        uint32_t indices[PROOFSIZE];
        if (extract_solution(cpu_attrs, s, indices, tree_sz)) {
            printf("  SOLUTION nonce=%u:", nonce_idx);
            for (int i = 0; i < PROOFSIZE; i++) printf(" %08x", indices[i]);
            printf("\n");
            if (verify_equihash_full(indices, header, 0))
                printf("  VERIFIED OK\n");
            else
                printf("  VERIFY FAILED\n");
            valid_solutions++;
        }
    }
}
for (int r = 0; r < 8; r++) { free(cpu_attrs[r]); cpu_attrs[r] = NULL; }
```

**Step 5** — Remove `mine_batch_extract` forward decl and stub entirely.

### Slot sizes verification
Run once after compile to confirm sizes match:
```c
printf("slot_sz: %zu %zu %zu %zu %zu %zu %zu %zu\n",
    sizeof(stage0_slot_t), sizeof(stage1_slot_t), sizeof(stage2_slot_t),
    sizeof(stage3_slot_t), sizeof(stage4_slot_t), sizeof(stage5_slot_t),
    sizeof(stage6_slot_t), sizeof(stage7_slot_t));
// Expected: 28 28 22 19 16 13 10 7
```

---

## Test Sequence (safe)

```bash
# A: compile only
make clean && rm -f _kernel.h && make sa-tromp 2>&1 | grep -i error

# B: pipeline + extraction (nonce 0 always has 40 bucket-0 candidates)
./sa-tromp 1

# C: if B survives without dropping SSH, run more
./sa-tromp 50
```

If B crashes SSH: reduce NSLOTS from 40 to 32 in both `param.h` and `input.cl` line with `NSLOTS_STAGE1`.

---

## extract_solution() signature
```c
// solution_extraction.c
int extract_solution(uint32_t **cpu_attrs, uint32_t flat7,
                     uint32_t *indices, uint32_t tree_sz);
// flat7 = bucket*NSLOTS + slot  (bucket 0 → flat7 = slot index)
// tree_sz = NBUCKETS * NSLOTS
// returns 1 if valid solution, 0 otherwise
```

---

## Session startup command (copy-paste)
```
Session#1  Run your map tool, read CLAUDE_SONNET_4.6.md and PLAN_ACTIVE.md. Resume from IN PROGRESS marker.
```
