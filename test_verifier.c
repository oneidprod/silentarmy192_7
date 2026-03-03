// Test program to verify a known-good solution from eq1927
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

typedef uint8_t uchar;
typedef uint32_t uint;

#include "param.h"
#include "blake.h"

// Verification functions (from main.c)
static void eh_genhash(const blake2b_state_t *ctx, uint32_t idx, uint8_t *hash)
{
    blake2b_state_t st = *ctx;
    const uint32_t hashes_per_blake = 512 / PARAM_N;
    const uint32_t hash_bytes = PARAM_N / 8;
    uint8_t full_hash[ZCASH_HASH_LEN];
    uint8_t block[128];  // Must be zero-padded to 128 bytes for final block
    uint32_t g = idx / hashes_per_blake;

    // Zero-pad the 4-byte index to full block size
    memset(block, 0, sizeof(block));
    block[0] = (uint8_t)(g & 0xff);
    block[1] = (uint8_t)((g >> 8) & 0xff);
    block[2] = (uint8_t)((g >> 16) & 0xff);
    block[3] = (uint8_t)((g >> 24) & 0xff);

    zcash_blake2b_update(&st, block, 4, 1);  // msg_len=4 (actual data), buffer zero-padded to 128, is_final=1
    zcash_blake2b_final(&st, full_hash, sizeof(full_hash));
    memcpy(hash, full_hash + (idx % hashes_per_blake) * hash_bytes, hash_bytes);
}

static uint32_t eh_verifyrec(const blake2b_state_t *ctx, uint32_t *indices, uint8_t *hash, int r)
{
    const uint32_t hash_bytes = PARAM_N / 8;

    if (r == 0) {
        eh_genhash(ctx, *indices, hash);
        return 1;
    }

    uint32_t *indices1 = indices + (1 << (r - 1));
    if (*indices >= *indices1) {
        printf("FAIL at r=%d: ordering violation indices[0]=%u >= indices[%d]=%u\n",
               r, *indices, (1 << (r - 1)), *indices1);
        return 0;
    }

    uint8_t hash0[hash_bytes], hash1[hash_bytes];
    if (!eh_verifyrec(ctx, indices, hash0, r - 1)) {
        printf("FAIL at r=%d: left subtree failed\n", r);
        return 0;
    }
    if (!eh_verifyrec(ctx, indices1, hash1, r - 1)) {
        printf("FAIL at r=%d: right subtree failed\n", r);
        return 0;
    }

    for (uint32_t i = 0; i < hash_bytes; i++)
        hash[i] = hash0[i] ^ hash1[i];

    int b = r < PARAM_K ? r * PREFIX : PARAM_N;
    int i;
    for (i = 0; i < b / 8; i++) {
        if (hash[i]) {
            printf("FAIL at r=%d: XOR byte %d is %02x (expected 00), need %d zero bits\n",
                   r, i, hash[i], b);
            return 0;
        }
    }
    if ((b % 8) && (hash[i] >> (8 - (b % 8)))) {
        printf("FAIL at r=%d: XOR partial byte %d has non-zero high bits: %02x, need %d zero bits\n",
               r, i, hash[i], b);
        return 0;
    }
    return 1;
}

static uint32_t verify_equihash_full(uint32_t *indices, uint8_t *header)
{
    blake2b_state_t ctx;
    uint8_t hash[PARAM_N / 8];

    zcash_blake2b_init(&ctx, ZCASH_HASH_LEN, PARAM_N, PARAM_K);
    zcash_blake2b_update(&ctx, header, 128, 0);
    zcash_blake2b_update(&ctx, header + 128, ZCASH_BLOCK_HEADER_LEN - 128, 0);

    return eh_verifyrec(&ctx, indices, hash, PARAM_K);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <header_hex> <solution_file>\n", argv[0]);
        return 1;
    }
    
    // Parse header
    const char *header_hex = argv[1];
    uint8_t header[ZCASH_BLOCK_HEADER_LEN];
    if (strlen(header_hex) != 2 * ZCASH_BLOCK_HEADER_LEN) {
        fprintf(stderr, "Header must be %d hex chars (got %zu)\n", 
                2 * ZCASH_BLOCK_HEADER_LEN, strlen(header_hex));
        return 1;
    }
    for (int i = 0; i < ZCASH_BLOCK_HEADER_LEN; i++) {
        sscanf(header_hex + 2*i, "%2hhx", &header[i]);
    }
    
    // Parse solution file
    FILE *f = fopen(argv[2], "r");
    if (!f) {
        fprintf(stderr, "Cannot open solution file: %s\n", argv[2]);
        return 1;
    }
    
    uint32_t indices[1 << PARAM_K];
    for (int i = 0; i < (1 << PARAM_K); i++) {
        if (fscanf(f, "%x", &indices[i]) != 1) {
            fprintf(stderr, "Failed to read index %d\n", i);
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    
    printf("Testing verification with header and %d indices...\n", 1 << PARAM_K);
    printf("Header: %.32s...\n", header_hex);
    printf("First few indices: %x %x %x %x\n", 
           indices[0], indices[1], indices[2], indices[3]);
    
    if (verify_equihash_full(indices, header)) {
        printf("\n✓ VERIFICATION PASSED - Our verifier accepts the reference solution!\n");
        printf("This means our verification code is CORRECT.\n");
        printf("The problem is in GPU candidate extraction.\n");
        return 0;
    } else {
        printf("\n✗ VERIFICATION FAILED - Our verifier rejects the reference solution!\n");
        printf("This means our verification code has a BUG.\n");
        printf("We need to fix the verifier before debugging GPU.\n");
        return 1;
    }
}
