/*
** Solution extraction from GPU collision trees
** 
** Reads GPU tree buffers and recursively extracts hash indices
** Matches CPU baseline listindices0/listindices1 algorithm
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define PARAM_K 7
#define PROOFSIZE (1 << PARAM_K)  // 128 indices
// NBUCKETS, NSLOTS, SLOTBITS inherited from including file (sa-tromp.c)

// Stage slot structures (must match GPU kernels)
typedef struct { uint32_t attr; unsigned char hash[21]; } stage1_slot_t;
typedef struct { uint32_t attr; unsigned char hash[18]; } stage2_slot_t;
typedef struct { uint32_t attr; unsigned char hash[15]; } stage3_slot_t;
typedef struct { uint32_t attr; unsigned char hash[12]; } stage4_slot_t;
typedef struct { uint32_t attr; unsigned char hash[9]; } stage5_slot_t;
typedef struct { uint32_t attr; unsigned char hash[6]; } stage6_slot_t;
typedef struct { uint32_t attr; unsigned char hash[3]; } stage7_slot_t;

// Trees (odd stages use trees1, even stages use trees0)
typedef struct {
    stage1_slot_t *trees1_stage1;  // Stage 1 output
    stage2_slot_t *trees0_stage2;  // Stage 2 output  
    stage3_slot_t *trees1_stage3;  // Stage 3 output
    stage4_slot_t *trees0_stage4;  // Stage 4 output
    stage5_slot_t *trees1_stage5;  // Stage 5 output
    stage6_slot_t *trees0_stage6;  // Stage 6 output
    stage7_slot_t *trees1_stage7;  // Stage 7 output (candidates)
} tree_store_t;

// Decode tree attribution
// Stage 1: 20-bit idx0 + 12-bit delta format (hash indices)
// Stages 2-7: 14-bit bucket + 9-bit slot0 + 9-bit slot1 format (tree positions)
static inline uint32_t tree_idx0_stage1(uint32_t attr) {
    return attr >> 12;
}

static inline uint32_t tree_idx1_stage1(uint32_t attr) {
    uint32_t idx0 = attr >> 12;
    uint32_t delta = attr & 0xFFF;
    return idx0 + delta;
}

// Stages 2-7: Decode bucket+slot triplet → flat tree position
static inline uint32_t tree_idx0_stages27(uint32_t attr) {
    uint32_t bucket = attr >> 18;           // Bits 31:18 (14 bits)
    uint32_t slot0 = (attr >> 9) & 0x1FF;   // Bits 17:9 (9 bits)
    return bucket * 512 + slot0;
}

static inline uint32_t tree_idx1_stages27(uint32_t attr) {
    uint32_t bucket = attr >> 18;           // Bits 31:18 (14 bits)
    uint32_t slot1 = attr & 0x1FF;          // Bits 8:0 (9 bits)
    return bucket * 512 + slot1;
}

// Compare function for qsort
static int compu32(const void *pa, const void *pb) {
    uint32_t a = *(uint32_t *)pa;
    uint32_t b = *(uint32_t *)pb;
    return (a > b) - (a < b);
}

// Order indices (XOR + sort pattern from CPU baseline)
static void orderindices(uint32_t *indices, uint32_t size) {
    if (indices[0] > indices[size]) {
        for (uint32_t i = 0; i < size; i++) {
            uint32_t tmp = indices[i];
            indices[i] = indices[size + i];
            indices[size + i] = tmp;
        }
    }
}

// Forward declarations
static void listindices0(tree_store_t *trees, uint32_t r, uint32_t attr, uint32_t *indices);
static void listindices1(tree_store_t *trees, uint32_t r, uint32_t attr, uint32_t *indices);

// Extract solution indices recursively (even stages - read from trees1, recurse to listindices1)
static void listindices0(tree_store_t *trees, uint32_t r, uint32_t attr, uint32_t *indices) {
    if (r == 0) {
        // Base case: attr contains 20-bit idx0 + 12-bit delta (Stage 1 hash indices)
        uint32_t idx0 = attr >> 12;
        uint32_t delta = attr & 0xFFF;
        uint32_t idx1 = idx0 + delta;
        indices[0] = idx0;
        indices[1] = idx1;
        return;
    }
    
    // Stages 2-7: Use bucket+slot decoding
    uint32_t parent0 = tree_idx0_stages27(attr);
    uint32_t parent1 = tree_idx1_stages27(attr);
    uint32_t size = 1 << r;
    uint32_t *indices1 = indices + size;
    
    // Read from the appropriate trees0 stage
    uint32_t attr0, attr1;
    
    if (r == 2) {
        // Stage 2 output (even) - read from trees0_stage2
        attr0 = trees->trees0_stage2[parent0].attr;
        attr1 = trees->trees0_stage2[parent1].attr;
    } else if (r == 4) {
        // Stage 4 output (even) - read from trees0_stage4
        attr0 = trees->trees0_stage4[parent0].attr;
        attr1 = trees->trees0_stage4[parent1].attr;
    } else if (r == 6) {
        // Stage 6 output (even) - read from trees0_stage6
        attr0 = trees->trees0_stage6[parent0].attr;
        attr1 = trees->trees0_stage6[parent1].attr;
    } else {
        fprintf(stderr, "listindices0: invalid stage r=%u\n", r);
        return;
    }
    
    // Recurse to odd stages
    listindices1(trees, r - 1, attr0, indices);
    listindices1(trees, r - 1, attr1, indices1);
    orderindices(indices, size);
}

// Extract solution indices recursively (odd stages - read from trees0, recurse to listindices0)
static void listindices1(tree_store_t *trees, uint32_t r, uint32_t attr, uint32_t *indices) {
    // Stages 1-7: Use bucket+slot decoding
    uint32_t parent0 = tree_idx0_stages27(attr);
    uint32_t parent1 = tree_idx1_stages27(attr);
    uint32_t size = 1 << r;
    uint32_t *indices1 = indices + size;
    
    // Read from the appropriate trees1 stage
    uint32_t attr0, attr1;
    
    if (r == 1) {
        // Stage 1 output (odd) - read from trees1_stage1
        attr0 = trees->trees1_stage1[parent0].attr;
        attr1 = trees->trees1_stage1[parent1].attr;
    } else if (r == 3) {
        // Stage 3 output (odd) - read from trees1_stage3
        attr0 = trees->trees1_stage3[parent0].attr;
        attr1 = trees->trees1_stage3[parent1].attr;
    } else if (r == 5) {
        // Stage 5 output (odd) - read from trees1_stage5
        attr0 = trees->trees1_stage5[parent0].attr;
        attr1 = trees->trees1_stage5[parent1].attr;
    } else if (r == 7) {
        // Stage 7 output (odd) - read from trees1_stage7
        attr0 = trees->trees1_stage7[parent0].attr;
        attr1 = trees->trees1_stage7[parent1].attr;
    } else {
        fprintf(stderr, "listindices1: invalid stage r=%u\n", r);
        return;
    }
    
    // Recurse to even stages
    listindices0(trees, r - 1, attr0, indices);
    listindices0(trees, r - 1, attr1, indices1);
    orderindices(indices, size);
}

// Extract solution from Stage 7 candidate
// Returns 1 if valid (no duplicates), 0 if invalid
int extract_solution(tree_store_t *trees, uint32_t candidate_attr, uint32_t *solution_indices) {
    // Extract all 128 indices
    listindices1(trees, PARAM_K, candidate_attr, solution_indices);
    
    // Check for duplicates
    uint32_t sorted[PROOFSIZE];
    memcpy(sorted, solution_indices, PROOFSIZE * sizeof(uint32_t));
    qsort(sorted, PROOFSIZE, sizeof(uint32_t), compu32);
    
    for (uint32_t i = 1; i < PROOFSIZE; i++) {
        if (sorted[i] <= sorted[i-1]) {
            return 0;  // Duplicate found
        }
    }
    
    return 1;  // Valid solution
}
