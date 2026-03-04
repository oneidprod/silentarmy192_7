// Debug Blake2b initialization to compare with Tromp
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <endian.h>

typedef uint8_t uchar;
typedef uint32_t uint;

#include "param.h"
#include "equihash_tromp/blake/blake2.h"

#ifndef htole32
#define htole32(x) ((uint32_t)(x))
#endif

void print_hex(const char *label, uint8_t *data, int len) {
    printf("%s: ", label);
    for (int i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
    printf("\n");
}

void print_state(const char *label, blake2b_state *ctx) {
    printf("\n%s State:\n", label);
    printf("  h[0..7]: ");
    for (int i = 0; i < 8; i++) {
        printf("%016lx ", ctx->h[i]);
    }
    printf("\n");
}

int main() {
    printf("=== Blake2b Debug: Personalization & Initialization ===\n\n");
    
    // Setup personalization like Tromp
    printf("Personalization setup:\n");
    char personals[16];
    memcpy(personals + 0, "ZERO_PoW", 8);
    printf("  Base: 'ZERO_PoW' (8 bytes)\n");
    
    uint32_t le_N = htole32(PARAM_N);  // Should be 192 in LE
    printf("  PARAM_N=%u, LE bytes: %02x %02x %02x %02x\n", 
           PARAM_N, 
           ((uint8_t*)&le_N)[0], ((uint8_t*)&le_N)[1], 
           ((uint8_t*)&le_N)[2], ((uint8_t*)&le_N)[3]);
    memcpy(personals + 8, &le_N, 4);
    
    uint32_t le_K = htole32(PARAM_K);  // Should be 7 in LE
    printf("  PARAM_K=%u, LE bytes: %02x %02x %02x %02x\n",
           PARAM_K,
           ((uint8_t*)&le_K)[0], ((uint8_t*)&le_K)[1],
           ((uint8_t*)&le_K)[2], ((uint8_t*)&le_K)[3]);
    memcpy(personals + 12, &le_K, 4);
    
    print_hex("  Full personalization", (uint8_t *)personals, 16);
    
    // Initialize blake2b with personalization
    printf("\nInitializing blake2b with personalization:\n");
    blake2b_param P;
    memset(&P, 0, sizeof(blake2b_param));
    P.digest_length = ZCASH_HASH_LEN;  // Should be 48
    P.fanout = 1;
    P.depth = 1;
    printf("  digest_length: %u\n", P.digest_length);
    printf("  fanout: %u\n", P.fanout);
    printf("  depth: %u\n", P.depth);
    memcpy(P.personal, (const uint8_t *)personals, 16);
    
    blake2b_state ctx;
    blake2b_init_param(&ctx, &P);
    print_state("After init_param", &ctx);
    
    // Test header
    printf("\nTest header (real blockchain header):\n");
    uint8_t test_header[140] = {
        0x04, 0x00, 0x00, 0x00, 0x3d, 0xd4, 0xa5, 0x34, 0x40, 0xb1, 0x1d, 0xde, 
        0x6a, 0x27, 0x9a, 0x53, 0x18, 0xb2, 0xcc, 0xe2, 0x3b, 0xce, 0xf2, 0x06,
        0x55, 0xc6, 0x56, 0x7d, 0x20, 0xb4, 0x16, 0xb1, 0x83, 0x15, 0x00, 0x00,
        0xec, 0x45, 0xbc, 0xf3, 0x4a, 0x76, 0x1f, 0xe1, 0x60, 0xcc, 0x5b, 0xa3,
        0x98, 0xf5, 0xbf, 0x36, 0x6b, 0x68, 0xa8, 0x30, 0x44, 0x93, 0x6f, 0xc5,
        0x6c, 0x86, 0x84, 0x79, 0x14, 0xd8, 0x91, 0x8b, 0x32, 0x70, 0x1e, 0xfc,
        0xed, 0xb2, 0xbf, 0x54, 0xfe, 0xf5, 0x03, 0x97, 0x92, 0x35, 0x16, 0xe1,
        0xa6, 0xa2, 0xb5, 0xd1, 0x3b, 0x39, 0x5f, 0xb2, 0x18, 0x40, 0xee, 0x94,
        0xa3, 0xba, 0xb2, 0x23, 0x1d, 0x3a, 0xa7, 0x69, 0xeb, 0xbd, 0x2a, 0x1e,
        0xc0, 0x02, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc,
        0x6b, 0x10, 0x00
    };
    printf("  Header length: %lu bytes\n", sizeof(test_header));
    print_hex("  First 32 bytes", test_header, 32);
    printf("  ...\n");
    print_hex("  Last 16 bytes", test_header + 124, 16);
    
    // Update with header
    blake2b_state ctx_after_header = ctx;
    blake2b_update(&ctx_after_header, test_header, sizeof(test_header));
    print_state("After header update", &ctx_after_header);
    
    // Now test with first index (0x6cc1 = 27841)
    printf("\nTest with index 0x6cc1 (27841):\n");
    uint32_t idx_g = 0x6cc1 / (512 / PARAM_N);  // Which blake hash contains this
    printf("  Index: 0x6cc1, g = %u, sizeof(g) = %lu\n", idx_g, sizeof(idx_g));
    
    blake2b_state ctx_with_idx = ctx_after_header;
    blake2b_update(&ctx_with_idx, (uint8_t *)&idx_g, sizeof(idx_g));
    
    // Finalize
    uint8_t hash_out[ZCASH_HASH_LEN];
    blake2b_final(&ctx_with_idx, hash_out, ZCASH_HASH_LEN);
    print_hex("  Result hash (first 32 bytes)", hash_out, 32);
    
    return 0;
}
