// Compare hash generation between our code and Tromp's
#include <stdio.h>
#include <string.h>
#include <stdint.h>

// Our implementation
typedef struct  blake2b_state_s { uint64_t h[8]; uint64_t bytes; } blake2b_state_t;
void zcash_blake2b_init(blake2b_state_t *st, uint8_t hash_len, uint32_t n, uint32_t k);
void zcash_blake2b_update(blake2b_state_t *st, const uint8_t *_msg, uint32_t msg_len, uint32_t is_final);
void zcash_blake2b_final(blake2b_state_t *st, uint8_t *out, uint8_t outlen);

void our_eh_genhash(const blake2b_state_t *ctx, uint32_t idx, uint8_t *hash) {
    blake2b_state_t st = *ctx;
    const uint32_t hashes_per_blake = 512 / 192;  // PARAM_N=192
    const uint32_t hash_bytes = 192 / 8;
    uint8_t full_hash[48];  // ZCASH_HASH_LEN
    uint8_t block[128];
    uint32_t g = idx / hashes_per_blake;

    memset(block, 0, sizeof(block));
    block[0] = (uint8_t)(g & 0xff);
    block[1] = (uint8_t)((g >> 8) & 0xff);
    block[2] = (uint8_t)((g >> 16) & 0xff);
    block[3] = (uint8_t)((g >> 24) & 0xff);

    zcash_blake2b_update(&st, block, 4, 1);
    zcash_blake2b_final(&st, full_hash, sizeof(full_hash));
    memcpy(hash, full_hash + (idx % hashes_per_blake) * hash_bytes, hash_bytes);
}

int main() {
    blake2b_state_t ctx;
    uint8_t header[140];
    memcpy(header, "zero", 4);
    memset(header + 4, 0, 136);

    zcash_blake2b_init(&ctx, 48, 192, 7);
    zcash_blake2b_update(&ctx, header, 128, 0);
    zcash_blake2b_update(&ctx, header + 128, 12, 0);

    uint8_t hash[24];
    our_eh_genhash(&ctx, 0x6cc1, hash);
    
    printf("Our hash for idx=0x6cc1: ");
    for (int i = 0; i < 24; i++) printf("%02x", hash[i]);
    printf("\n");
    
    return 0;
}
