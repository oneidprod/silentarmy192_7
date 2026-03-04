// Compare first index hash between our code and Tromp's
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <endian.h>

typedef uint8_t uchar;
typedef uint32_t uint;

#include "param.h"
#include "equihash_tromp/blake/blake2.h"

#ifndef htole32
#define htole32(x) ((uint32_t)(x))
#endif

static void eh_genhash(const blake2b_state *ctx, uint32_t idx, uint8_t *hash) {
    blake2b_state state = *ctx;
    const uint32_t hashes_per_blake = 512 / PARAM_N;
    const uint32_t hash_bytes = PARAM_N / 8;
    uint8_t full_hash[ZCASH_HASH_LEN];
    uint32_t g = idx / hashes_per_blake;

    blake2b_update(&state, (uchar *)&g, sizeof(uint32_t));
    blake2b_final(&state, full_hash, ZCASH_HASH_LEN);
    memcpy(hash, full_hash + (idx % hashes_per_blake) * hash_bytes, hash_bytes);
}

void print_hex(const char *label, uint8_t *data, int len) {
    printf("%s: ", label);
    for (int i = 0; i < len; i++) printf("%02x", data[i]);
    printf("\n");
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <header_hex> <first_index_hex>\n", argv[0]);
        return 1;
    }

    // Parse header
    const char *header_hex = argv[1];
    uint8_t header[ZCASH_BLOCK_HEADER_LEN];
    if (strlen(header_hex) != 2 * ZCASH_BLOCK_HEADER_LEN) {
        fprintf(stderr, "Header must be %d hex chars\n", 2 * ZCASH_BLOCK_HEADER_LEN);
        return 1;
    }
    for (int i = 0; i < ZCASH_BLOCK_HEADER_LEN; i++) {
        sscanf(header_hex + 2*i, "%2hhx", &header[i]);
    }
    print_hex("Header (first 32)", header, 32);

    // Parse index
    uint32_t idx = (uint32_t)strtol(argv[2], NULL, 16);
    const uint32_t hash_bytes = PARAM_N / 8;  // 24 bytes for 192-bit
    printf("\nTest index: 0x%x (%u)\n", idx, idx);
    printf("hash_bytes for index: %u\n", hash_bytes);
    printf("PARAM_N=%u (bits), PARAM_K=%u\n", PARAM_N, PARAM_K);
    printf("512 / PARAM_N = %u (hashes_per_blake)\n", 512 / PARAM_N);
    
    // Initialize blake2b state like test_verifier does
    printf("\n=== Blake2b Initialization ===\n");
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

    blake2b_state ctx;
    blake2b_init_param(&ctx, &P);
    printf("✓ Initialized blake2b with personalization\n");

    blake2b_update(&ctx, header, ZCASH_BLOCK_HEADER_LEN);
    printf("✓ Updated with header (%u bytes)\n", ZCASH_BLOCK_HEADER_LEN);

    // Generate hash for index
    uint8_t hash[hash_bytes];
    eh_genhash(&ctx, idx, hash);
    print_hex("\nResult hash (24 bytes)", hash, hash_bytes);

    return 0;
}
