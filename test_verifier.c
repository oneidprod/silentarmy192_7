// Test program to verify a known-good solution from eq1927
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <endian.h>

typedef uint8_t uchar;
typedef uint32_t uint;

#include "param.h"
#include "equihash_tromp/blake/blake2.h"

/* Define htole32 for little-endian conversion if not available */
#ifndef htole32
#define htole32(x) ((uint32_t)(x))  /* Assume little-endian host */
#endif

// Verification functions (using Tromp's blake2b)
static void eh_genhash(const blake2b_state *ctx, uint32_t idx, uint8_t *hash)
{
    blake2b_state state = *ctx;
    const uint32_t hashes_per_blake = 512 / PARAM_N;
    const uint32_t hash_bytes = PARAM_N / 8;
    uint8_t full_hash[ZCASH_HASH_LEN];
    
    /* Tromp's genhash: convert index to little-endian before hashing */
    uint32_t leb = htole32(idx / hashes_per_blake);
    blake2b_update(&state, (uchar *)&leb, sizeof(uint32_t));
    blake2b_final(&state, full_hash, ZCASH_HASH_LEN);
    memcpy(hash, full_hash + (idx % hashes_per_blake) * hash_bytes, hash_bytes);
}

static uint32_t eh_verifyrec(const blake2b_state *ctx, uint32_t *indices, uint8_t *hash, int r, int depth_indent)
{
    const uint32_t hash_bytes = PARAM_N / 8;

    if (r == 0) {
        eh_genhash(ctx, *indices, hash);
        printf("[r=0] idx=%u hash: %02x%02x%02x%02x%02x%02x\n", *indices, hash[0], hash[1], hash[2], hash[3], hash[4], hash[5]);
        return 1;
    }

    uint32_t *indices1 = indices + (1 << (r - 1));
    if (*indices >= *indices1) {
        printf("[r=%d] FAIL: ordering violation indices[0]=%u >= indices[%d]=%u\n",
               r, *indices, (1 << (r - 1)), *indices1);
        return 0;
    }

    uint8_t hash0[hash_bytes], hash1[hash_bytes];
    if (!eh_verifyrec(ctx, indices, hash0, r - 1, depth_indent + 1)) {
        printf("[r=%d] FAIL: left subtree failed\n", r);
        return 0;
    }
    if (!eh_verifyrec(ctx, indices1, hash1, r - 1, depth_indent + 1)) {
        printf("[r=%d] FAIL: right subtree failed\n", r);
        return 0;
    }

    for (uint32_t i = 0; i < hash_bytes; i++)
        hash[i] = hash0[i] ^ hash1[i];

    if (r == 1) {
        printf("[r=1] XOR result: %02x%02x%02x%02x%02x%02x (from %02x%02x%02x ^ %02x%02x%02x)\n",
               hash[0], hash[1], hash[2], hash[3], hash[4], hash[5],
               hash0[0], hash0[1], hash0[2], hash1[0], hash1[1], hash1[2]);
    }

    int b = r < PARAM_K ? r * PREFIX : PARAM_N;
    int i;
    for (i = 0; i < b / 8; i++) {
        if (hash[i]) {
            printf("[r=%d] FAIL: XOR byte %d is %02x (expected 00), need %d zero bits\n",
                   r, i, hash[i], b);
            return 0;
        }
    }
    if ((b % 8) && (hash[i] >> (8 - (b % 8)))) {
        printf("[r=%d] FAIL: nonzero partial byte %d: %02x\n", r, i, hash[i]);
        return 0;
    }
    return 1;
}

static uint32_t verify_equihash_full(uint32_t *indices, uint8_t *header)
{
    blake2b_state ctx;
    uint8_t hash[PARAM_N / 8];
    
    /* Initialize blake2b with proper personalization for Zero Equihash 192,7 */
    /* Tromp's setheader logic */
    char personals[16];
    memcpy(personals + 0, "ZERO_PoW", 8);
    uint32_t le_N = htole32(PARAM_N);
    memcpy(personals + 8, &le_N, 4);
    uint32_t le_K = htole32(PARAM_K);
    memcpy(personals + 12, &le_K, 4);
    
    blake2b_param P;
    memset(&P, 0, sizeof(blake2b_param));
    P.digest_length = ZCASH_HASH_LEN;
    P.fanout = 1;
    P.depth = 1;
    memcpy(P.personal, (const uint8_t *)personals, 16);
    
    blake2b_init_param(&ctx, &P);
    blake2b_update(&ctx, header, ZCASH_BLOCK_HEADER_LEN);

    return eh_verifyrec(&ctx, indices, hash, PARAM_K, 0);
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
