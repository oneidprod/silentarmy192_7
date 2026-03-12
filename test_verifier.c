// Test program to cross-check sa-tromp solutions using sa-tromp's own blake2b
// Replicates eh_genhash exactly as in sa-tromp.c to guarantee hash parity.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <endian.h>

typedef uint8_t uchar;
typedef uint32_t uint;

#include "param.h"
#include "blake.h"

/* Define htole32 for little-endian conversion if not available */
#ifndef htole32
#define htole32(x) ((uint32_t)(x))  /* Assume little-endian host */
#endif

// Replicates sa-tromp.c eh_genhash exactly (must stay in sync)
static void eh_genhash(const blake2b_state_t *ctx, uint32_t idx, uint8_t *hash)
{
    blake2b_state_t st = *ctx;
    const uint32_t hashes_per_blake = 512 / PARAM_N;   /* = 2 */
    const uint32_t hash_bytes = PARAM_N / 8;            /* = 24 */
    uint8_t full_hash[ZCASH_HASH_LEN];
    uint64_t message[16] = {0};
    uint32_t g = idx / hashes_per_blake;
    message[1] = ((uint64_t)g) << 32;
    st.bytes = ZCASH_BLOCK_HEADER_LEN;
    zcash_blake2b_update(&st, (const uint8_t *)message, sizeof(uint32_t), 1);
    zcash_blake2b_final(&st, full_hash, ZCASH_HASH_LEN);
    memcpy(hash, full_hash + (idx % hashes_per_blake) * hash_bytes, hash_bytes);
}

static uint32_t eh_verifyrec(const blake2b_state_t *ctx, uint32_t *indices, uint8_t *hash, int r)
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
    if (!eh_verifyrec(ctx, indices, hash0, r - 1)) return 0;
    if (!eh_verifyrec(ctx, indices1, hash1, r - 1)) return 0;

    for (uint32_t i = 0; i < hash_bytes; i++)
        hash[i] = hash0[i] ^ hash1[i];

    int b = r < PARAM_K ? r * PREFIX : PARAM_N;
    int i;
    for (i = 0; i < b / 8; i++) {
        if (hash[i]) {
            printf("[r=%d] FAIL: XOR byte %d is %02x (expected 00)\n", r, i, hash[i]);
            return 0;
        }
    }
    if ((b % 8) && (hash[i] >> (8 - (b % 8)))) {
        printf("[r=%d] FAIL: nonzero partial byte %d: %02x\n", r, i, hash[i]);
        return 0;
    }
    return 1;
}

static uint32_t verify_equihash_full(uint32_t *indices, uint8_t *header, uint32_t nonce_idx)
{
    blake2b_state_t ctx;
    uint8_t hash[PARAM_N / 8];

    zcash_blake2b_init(&ctx, ZCASH_HASH_LEN, PARAM_N, PARAM_K);
    /* Match sa-tromp's nonce embedding: zero-padded 128-byte block with nonce at bytes 0-3 */
    zcash_blake2b_update(&ctx, header, 128, 0);
    uint8_t nonce_block[128] = {0};
    uint32_t nonce_le = htole32(nonce_idx);
    memcpy(nonce_block, &nonce_le, 4);
    zcash_blake2b_update(&ctx, nonce_block, 4, 0);

    return eh_verifyrec(&ctx, indices, hash, PARAM_K);
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 5) {
        fprintf(stderr, "Usage: %s <header_hex> <solution_file> [-n nonce_idx]\n", argv[0]);
        return 1;
    }

    uint32_t nonce_idx = 0;
    if (argc >= 5 && strcmp(argv[3], "-n") == 0)
        nonce_idx = (uint32_t)atoi(argv[4]);

    // Parse header (128 bytes = 256 hex chars)
    const char *header_hex = argv[1];
    uint8_t header[128];
    if (strlen(header_hex) != 256) {
        fprintf(stderr, "Header must be 256 hex chars (128 bytes) (got %zu)\n", strlen(header_hex));
        return 1;
    }
    for (int i = 0; i < 128; i++) {
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

    printf("Testing verification with header (128 bytes) and %d indices, nonce=%u...\n", 1 << PARAM_K, nonce_idx);
    printf("Header: %.32s...\n", header_hex);
    printf("First few indices: %x %x %x %x\n",
           indices[0], indices[1], indices[2], indices[3]);

    if (verify_equihash_full(indices, header, nonce_idx)) {
        printf("\n✓ VERIFICATION PASSED - Independent verifier accepts the solution!\n");
        return 0;
    } else {
        printf("\n✗ VERIFICATION FAILED\n");
        return 1;
    }
}
