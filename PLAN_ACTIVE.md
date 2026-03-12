# PLAN_ACTIVE — Equihash 192,7 GPU Miner
**Last updated**: 2026-03-12 (Session 7 end)
**Branch**: rewrite
**Status**: PROTOCOL MISMATCH DISCOVERED — sa-tromp uses wrong header format, must fix before pool use

---

## Session Startup Command
```
Session#7  Run your map tool, read CLAUDE_SONNET_4.6.md and PLAN_ACTIVE.md. Resume from IN PROGRESS marker.
```

---

## Session 6 Summary (context ran out — no code written)

### Research completed — do NOT repeat:
- `test_verifier.c` already exists as cross-check tool (uses Tromp blake2b + ZERO_PoW)
- `EQ1927_USAGE_GUIDE.md` documents the existing workflow
- **CRITICAL FINDING**: `test_verifier.c:109` uses one 140-byte update (WRONG for sa-tromp)
- sa-tromp uses TWO updates: `blake2b_update(header, 128)` + `blake2b_update(&nonce_idx, 4)` — incompatible
- `param.h:ZCASH_BLOCK_HEADER_LEN=140` — legacy; sa-tromp uses 128, NOT 140
- sa-tromp header = `"test_block_header_data_192_7"` (28 bytes) + 100 zero bytes
- nheqminer interface: ISolver.h, solver1927 = working reference, MinerFactory pattern understood
- Full plan in `/home/mine/.claude/plans/sunny-sprouting-codd.md`

### Disk space note:
- Deleted `core` dump (~300MB) to free space
- `make clean && rm -f _kernel.h` needed before building (object files may be stale)

---

## ✅ DONE — verify_equihash_full working (Session 5)

- **Fix**: `canonical_sort(indices, PARAM_K)` must run BEFORE `verify_equihash_full`
- All failures were ordering violations — tree traversal order ≠ canonical order
- XOR chain preserved: swapping whole subtrees is valid (XOR is commutative)
- Result: 4 verified solutions in 10 nonces (commit 703dd24)

---

## NEXT STEPS — START HERE Session 8

### Step 0 — Verify Zero coin nonce location (research only, no code)
Check what nonce offset Zero coin actually uses in its 140-byte header:
- `equihash_tromp/equi.c` uses `((u32*)headernonce)[32]` = byte 128 (4-byte nonce)
- Original silentarmy (Zcash) used 32-byte nonce at bytes 108-139
- Zero coin likely uses the Tromp convention: 4-byte nonce at byte 128
Run: `./equihash_tromp/eq1927 -s -p "ZERO_PoW" -n 0 2>&1 | head -5` to confirm it works
Then check eq1927 source to confirm byte offset

### Step 1 — Fix blake2b to handle 140-byte header
Option A (simpler): Use Tromp's blake2b for the initial state setup in sa-tromp.c
- `#include "equihash_tromp/blake/blake2.h"` in sa-tromp.c
- Replace the two `zcash_blake2b_update` calls with one Tromp `blake2b_update(ctx, header, 140)`
- Extract `ctx.h[0..7]` (8 × uint64_t) to upload to GPU — same as before
- GPU kernel receives same 8-word state, continues as-is

Option B (harder): extend zcash_blake2b_update to buffer partial blocks

**Recommend Option A** — minimal change, proven code path.

### Step 2 — Fix header construction in sa-tromp.c main()
- Change header to 140 bytes
- Embed nonce at correct offset (byte 128 for Zero coin)
- Iterate nonce as uint32_t

### Step 3 — Fix GPU kernel counter
- Currently `v[12] ^= ZCASH_BLOCK_HEADER_LEN + 4` = 144
- For 140-byte header + 4-byte index: `v[12] ^= 144` is actually CORRECT if total = 140+4
- But the Tromp approach is: blake state after 140-byte header is the starting state,
  then GPU adds 4-byte index → total = 144. **No kernel change needed** if Option A used.

### Step 4 — Rebuild + cross-check with eq1927
```bash
./equihash_tromp/eq1927 -s -p "ZERO_PoW" -n 0 2>&1 | grep '^Solution' | head -1 | sed 's/Solution //' > /tmp/eq_sol.txt
HEADER=$(printf '%280s' | tr ' ' '0')  # 140-byte zero header
./test_verifier "$HEADER" /tmp/eq_sol.txt   # should PASS with same blake init
```

### Step 5 — Commit

## NEXT STEPS (in order) — COMPLETED Session 7

### Step A — Fix test_verifier.c nonce embedding (Part A of plan)
**File**: `test_verifier.c`
**Change**: Line 109 — replace single 140-byte `blake2b_update` with:
```c
blake2b_update(&ctx, header, 128);           // 128-byte header
uint32_t nonce_le = htole32(nonce_idx);
blake2b_update(&ctx, (uchar*)&nonce_le, 4);  // nonce separate
```
Also: change function signature to accept `uint32_t nonce_idx`, update `main()` to parse `-n N`.

### Step B — Add `Solution` output line to sa-tromp.c
**File**: `sa-tromp.c` line ~477 — after existing SOLUTION printf, add:
```c
printf("Solution");
for (int i = 0; i < PROOFSIZE; i++) printf(" %x", indices[i]);
printf("\n");
```

### Step C — Rebuild + cross-check
```bash
make clean && rm -f _kernel.h && make sa-tromp
gcc -o test_verifier test_verifier.c equihash_tromp/blake/blake2b.cpp \
    -DWN=192 -DWK=7 -I. -Iequihash_tromp -Iequihash_tromp/blake -lstdc++ -lm
./sa-tromp 10 | grep '^Solution' | head -1 | sed 's/Solution //' > /tmp/sol.txt
# then: ./test_verifier <header_hex_256chars> /tmp/sol.txt -n <nonce>
```

### Step D — Commit, then start nheqminer integration (Part B of plan)
See full plan at `/home/mine/.claude/plans/sunny-sprouting-codd.md`

---

### (STALE — was pre-Session-5) Step A — Add r-level verbose to verify
In `verify_equihash_full` / `eh_verifyrec`: print which round (r) first fails and the two
XOR-input hash values. This will confirm whether it's r=2 or elsewhere.

```c
// In eh_verifyrec, before the XOR check at each level, print:
fprintf(stderr, "verify r=%d: h0=", r); for(int i=0;i<n;i++) fprintf(stderr,"%02x",h0[i]);
fprintf(stderr, " h1="); for(int i=0;i<n;i++) fprintf(stderr,"%02x",h1[i]); fprintf(stderr,"\n");
```

### Step B — Compare with known-good extraction
If r=2+ XOR fails: the attr chain in cpu_attrs[2] is corrupted OR canonical_sort is scrambling
indices in a way that breaks the XOR chain.

Key hypothesis: canonical_sort must preserve the PAIRWISE relationship at each level, not just
sort globally. The tree has a strict structure: left subtree XOR right subtree = 0 at each depth.
Sorting may swap indices in ways that break this pairing.

Alternative: DON'T canonical_sort. Instead fix eh_verifyrec to tolerate either ordering.
Or: verify BEFORE canonical_sort, then sort only for output.

### Step C — Fix + retest
Apply fix, rebuild, run `./sa-tromp 1` → expect VERIFIED OK.

### Step D — Commit
```bash
git add sa-tromp.c solution_extraction.c PLAN_ACTIVE.md CLAUDE_SONNET_4.6.md
git commit -m "fix: verifier working

- canonical_sort + verify pipeline
- Status: working
- Next: pool testing"
```

---

## Hardware Constraints (CRITICAL — read before any GPU change)

- **GPU**: Intel integrated (Beignet driver). GPU and CPU **share the same RAM**.
- **RAM**: 7.6 GB total, ~5.4 GB available.
- **Beignet watchdog**: NDRange > ~2^18–2^20 work items HANG GPU and DROP SSH. Current code batches at 2^18 — do not increase.
- **Memory model**: Beignet maps GPU buffer pages into process RSS via `clEnqueueReadBuffer`. `clReleaseMemObject` does NOT immediately unmap. Must use double-buffer ping-pong (see Architecture).
- **NEVER** run two full cascades back-to-back (OOM + SSH death).
- **ALWAYS** build with: `make clean && rm -f _kernel.h && make sa-tromp`

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

## Architecture: mine_batch() (sa-tromp.c)

```
1. Allocate buf_tree0, buf_tree1 (1.07GB each, once, never reallocated)
2. kernel_round0_gen loop: fills buf_tree0 in 2^18-WI batches → cpu_attrs[0] readback
3. Stage 1 kernel: buf_tree0 → buf_tree1 → cpu_attrs[1] readback
4. Stages 2-7 loop (s=2..7):
     curr = (s%2==0) ? buf_tree0 : buf_tree1
     dispatch kernel (prev → curr)
     readback curr → cpu_attrs[s]
     prev = curr
5. Read nsol count from Stage 7
6. For each candidate s in 0..nsol-1:
     extract_solution(cpu_attrs, s, indices, tree_sz)
     if valid: print + verify
7. Free all cpu_attrs, release buf_tree0/buf_tree1
```

**Stage 7 candidate flat index**: bucket 0 slot s → flat7 = s (flat = bucket*NSLOTS + slot = 0*40 + s = s).

**Stage 7 kernel cap**: `if (sl >= 65536) continue;` — allows all ~1000 Stage 7 candidates through, not just NSLOTS=40.

**Nonce variation**: `zcash_blake2b_update(&blake_gen, (uint8_t*)&nonce_idx, sizeof(nonce_idx), 0)` is called per nonce — each nonce produces different hashes.

---

## Attr Encoding

| Stage | attr encodes |
|-------|-------------|
| 0 (tree0) | `xi` = raw hash index (0..2^25-1) |
| 1-7 | `(src_bucket << 12) | (i << 6) | j` where src=parent bucket, i/j=slot indices |

`solution_extraction.c:flat_idx_of()` uses `NSLOTS` stride — must match GPU NSLOTS=40.

---

## Slot Sizes (Beignet 4-byte padded — CRITICAL)

| Stage | struct fields | _slot_sz | note |
|-------|--------------|---------|------|
| 0 | u32 + u8[24] | 28 | exact fit |
| 1 | u32 + u8[21] + u8[3]pad | 28 | explicit pad in struct |
| 2 | u32 + u8[18] | **24** | Beignet adds 2B tail pad |
| 3 | u32 + u8[15] | **20** | Beignet adds 1B tail pad |
| 4 | u32 + u8[12] | 16 | exact fit |
| 5 | u32 + u8[9]  | **16** | Beignet adds 3B tail pad |
| 6 | u32 + u8[6]  | **12** | Beignet adds 2B tail pad |
| 7 | u32 + u8[3]  | **8**  | Beignet adds 1B tail pad |

sa-tromp.c line 282: `const size_t _slot_sz[8] = {28,28,24,20,16,16,12,8};`

---

## File Map

| File | Role |
|------|------|
| `sa-tromp.c` | Host: mine_batch(), main() |
| `input.cl` | GPU kernels: kernel_round0_gen, kernel_stage1..7_collisions |
| `solution_extraction.c` | CPU: extract_solution(), listindices(), flat_idx_of() |
| `solution_extraction.h` | Declares extract_solution() |
| `param.h` | All #defines (PARAM_N, NBUCKETS, NSLOTS, etc.) |
| `blake.c` / `blake.h` | CPU blake2b |

---

## Debugging Guide

### If still all-duplicate leaf indices after _slot_sz fix
The strides may still be wrong. Add stride probe after Stage 3 readback in sa-tromp.c:
```c
{
    for (int stride = 18; stride <= 24; stride++) {
        void *tmp2 = malloc(stride * 100);
        clEnqueueReadBuffer(queue, curr, CL_TRUE, 0, stride*100, tmp2, 0, NULL, NULL);
        uint32_t a = *(uint32_t *)((char*)tmp2 + 10*stride);
        uint32_t bkt_a = a >> 12, i_a = (a>>6)&0x3F, j_a = a&0x3F;
        printf("stride=%d slot10 attr=%08x bkt=%u i=%u j=%u (i>j=%d)\n",
               stride, a, bkt_a, i_a, j_a, i_a>j_a);
        free(tmp2);
    }
}
```
Valid attr has `i > j` (slot_i > slot_j in collision pairs). Correct stride shows `i>j=1`.

### If OOM (EXIT=137)
Double-buffer is in place — should not happen. Check no extra GPU allocations were added in the loop.

### If Stage 7 produces 0 candidates
Check `NSLOTS_STAGE1` in input.cl is 40. Check Stage 7 kernel cap is 65536, not NSLOTS.

### Circuit breaker
```bash
make clean && rm -f _kernel.h && make sa-tromp
```
