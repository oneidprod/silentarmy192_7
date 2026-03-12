/*
** Solution extraction from GPU collision trees (new architecture, Step 5)
**
** All stages use uniform uint32_t attr:
**   tree0: attr = xi (raw hash index 0..2^25-1)
**   tree1-7: attr = (src_bucket << 12) | (slot_i << 6) | slot_j
**
** Stage 7 writes ONLY true solutions (all 48 XOR bits = 0) to bucket 0.
** cpu_attrs[r] = flat uint32_t array of attrs for tree r.
** NSLOTS, NBUCKETS, BUCKBITS, SLOTBITS inherited from sa-tromp.c (including file).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define PARAM_K   7
#define PROOFSIZE (1 << PARAM_K)   /* 128 */

/* Stage slot structures — must match GPU kernel struct layout (same ABI).
 * attr is always the first field (uint32_t, 4 bytes). */
typedef struct { uint32_t attr; uint8_t hash[24];                          } stage0_slot_t;
typedef struct { uint32_t attr; unsigned char hash[21]; unsigned char pad[3]; } stage1_slot_t;
typedef struct { uint32_t attr; unsigned char hash[18]; } stage2_slot_t;
typedef struct { uint32_t attr; unsigned char hash[15]; } stage3_slot_t;
typedef struct { uint32_t attr; unsigned char hash[12]; } stage4_slot_t;
typedef struct { uint32_t attr; unsigned char hash[9];  } stage5_slot_t;
typedef struct { uint32_t attr; unsigned char hash[6];  } stage6_slot_t;
typedef struct { uint32_t attr; unsigned char hash[3];  } stage7_slot_t;

/* Decode stages 1-7 attr → flat index in the previous tree.
 * which=0: returns flat for slot_i (the "left" child)
 * which=1: returns flat for slot_j (the "right" child) */
static inline uint32_t flat_idx_of(uint32_t attr, int which)
{
    uint32_t bucket = attr >> 12;
    uint32_t slot   = which ? (attr & 0x3F) : ((attr >> 6) & 0x3F);
    return bucket * NSLOTS + slot;
}

/* Compare helper for qsort */
static int compu32(const void *pa, const void *pb) {
    uint32_t a = *(const uint32_t *)pa;
    uint32_t b = *(const uint32_t *)pb;
    return (a > b) - (a < b);
}

/* Recursively collect leaf hash indices (xi) from the tree.
 *
 * cpu_attrs[r]  — uint32_t* flat array of attrs for tree r (r=0..PARAM_K).
 * round         — current tree level (PARAM_K downto 0).
 * flat          — flat slot index into cpu_attrs[round].
 * indices       — output array (PROOFSIZE elements).
 * cnt           — current write count (starts at 0, ends at PROOFSIZE).
 * tree_size     — NBUCKETS*NSLOTS (bounds check). */
static void listindices(uint32_t **cpu_attrs, int round, uint32_t flat,
                        uint32_t *indices, int *cnt, uint32_t tree_size)
{
    if (flat >= tree_size) {
        fprintf(stderr, "listindices: OOB flat=%u tree_size=%u round=%d\n",
                flat, tree_size, round);
        return;
    }
    if (round == 0) {
        if (*cnt < PROOFSIZE)
            indices[(*cnt)++] = cpu_attrs[0][flat]; /* leaf xi */
        return;
    }
    uint32_t attr = cpu_attrs[round][flat];
    listindices(cpu_attrs, round - 1, flat_idx_of(attr, 0), indices, cnt, tree_size);
    listindices(cpu_attrs, round - 1, flat_idx_of(attr, 1), indices, cnt, tree_size);
}

/* Sort indices into canonical Equihash order (recursive).
 * At each level, the half-block with the smaller min-index must come first.
 * block: pointer to block of 2^depth indices, depth: tree depth (PARAM_K downto 0). */
static void canonical_sort(uint32_t *block, int depth)
{
    if (depth == 0) return;
    int half = 1 << (depth - 1);
    canonical_sort(block,        depth - 1);
    canonical_sort(block + half, depth - 1);
    /* Swap halves if left[0] > right[0] */
    if (block[0] > block[half]) {
        uint32_t tmp[half]; /* VLA, depth <= PARAM_K=7, max half=64 */
        memcpy(tmp,         block,        half * sizeof(uint32_t));
        memcpy(block,       block + half, half * sizeof(uint32_t));
        memcpy(block + half, tmp,         half * sizeof(uint32_t));
    }
}

/* Extract 128 solution indices from tree7 candidate at flat index flat7.
 * Returns 1 if 128 distinct indices found in tree order, 0 on duplicates.
 * Indices are in tree traversal order (NOT canonical); caller may canonical_sort
 * before output but must verify BEFORE sorting. */
int extract_solution(uint32_t **cpu_attrs, uint32_t flat7,
                     uint32_t *solution_indices, uint32_t tree_size)
{
    int cnt = 0;
    listindices(cpu_attrs, PARAM_K, flat7, solution_indices, &cnt, tree_size);
    if (cnt != PROOFSIZE) {
        fprintf(stderr, "extract_solution: expected %d indices, got %d\n",
                PROOFSIZE, cnt);
        return 0;
    }
    /* Check all 128 indices are distinct */
    uint32_t sorted[PROOFSIZE];
    memcpy(sorted, solution_indices, PROOFSIZE * sizeof(uint32_t));
    qsort(sorted, PROOFSIZE, sizeof(uint32_t), compu32);
    int dup = 0;
    for (int i = 1; i < PROOFSIZE; i++) {
        if (sorted[i] <= sorted[i-1]) { dup = 1; break; }
    }
    return dup ? 0 : 1;
}
