/*
** Compare GPU and CPU Blake2b implementations on same input
** Goal: Identify the exact difference causing verification failures
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "blake.h"

// Define minimal param checks without param.h to avoid OpenCL types
#define PARAM_N 192
#define PARAM_K 7
#define ZCASH_HASH_LEN 32
#define ZCASH_BLOCK_HEADER_LEN 140
#define PREFIX (PARAM_N / (PARAM_K + 1))

int main()
{
    // Prepare test header (140 bytes, all zeros)
    uint8_t header[140];
    memset(header, 0, 140);
    
    printf("=== Blake2b Implementation Comparison ===\n");
    printf("Header: 140 bytes (all zeros)\n");
    printf("Personalization: ZERO_PoW + N=192 + K=7 (little-endian)\n\n");
    
    // Test 1: Full cipher state after header processing
    printf("TEST 1: Blake2b state after header compression\n");
    printf("-------\n");
    
    blake2b_state_t ctx;
    zcash_blake2b_init(&ctx, ZCASH_HASH_LEN, PARAM_N, PARAM_K);
    
    printf("After init, state h[0-7]:\n");
    for (int i = 0; i < 8; i++) {
        printf("  h[%d] = 0x%016lx\n", i, ctx.h[i]);
    }
    printf("  bytes = %lu\n", ctx.bytes);
    
    // Process header
    printf("\nProcessing header (140 bytes) as final block...\n");
    uint8_t block[128];
    memcpy(block, header, 128);
    memset(block + 128, 0, 0);  // First 128 bytes only
    
    zcash_blake2b_update(&ctx, block, 128, 0);  // Mark as NOT final yet
    
    printf("After first update:\n");
    for (int i = 0; i < 8; i++) {
        printf("  h[%d] = 0x%016lx\n", i, ctx.h[i]);
    }
    printf("  bytes = %lu\n", ctx.bytes);
    
    // Process remaining 12 bytes
    memset(block, 0, 128);
    memcpy(block, header + 128, 12);
    zcash_blake2b_update(&ctx, block, 128, 1);  // Mark as FINAL
    
    printf("After second update (final):\n");
    for (int i = 0; i < 8; i++) {
        printf("  h[%d] = 0x%016lx\n", i, ctx.h[i]);
    }
    printf("  bytes = %lu\n", ctx.bytes);
    
    // Test 2: Verify h[] state is consistent
    printf("\n\nTEST 2: Verify personalization was applied correctly\n");
    printf("------\n");
    
    printf("Expected personalization: 'ZERO_PoW' (8 bytes) + c0 00 00 00 + 07 00 00 00\n");
    printf("This affects h[6] and h[7] in init\n");
    
    blake2b_state_t ctx_test;
    zcash_blake2b_init(&ctx_test, ZCASH_HASH_LEN, PARAM_N, PARAM_K);
    
    printf("h[6] = 0x%016lx (should incorporate 'ZER' part of personalization)\n", ctx_test.h[6]);
    printf("h[7] = 0x%016lx (should incorporate 'O_P' part of personalization)\n", ctx_test.h[7]);
    
    printf("\n\nTEST 3: Parameter Summary\n");
    printf("-----\n");
    printf("PARAM_N = %d\n", PARAM_N);
    printf("PARAM_K = %d\n", PARAM_K);
    printf("PREFIX = %d (= N / (K+1) = %d / 8 = 24 bits)\n", PREFIX, PREFIX);
    printf("ZCASH_HASH_LEN = %d\n", ZCASH_HASH_LEN);
    printf("ZCASH_BLOCK_HEADER_LEN = %d\n", ZCASH_BLOCK_HEADER_LEN);
    
    return 0;
}
