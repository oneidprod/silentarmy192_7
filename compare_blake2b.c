/*
** Compare kernel_round0 hash construction against current CPU eh_genhash logic.
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <endian.h>
#include "blake.h"

#define PARAM_N 192
#define PARAM_K 7
#define ZCASH_HASH_LEN 48
#define ZCASH_BLOCK_HEADER_LEN 140

static const uint64_t blake_iv[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
    0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

static inline uint64_t rotr64(uint64_t x, int n)
{
    return (x >> n) | (x << (64 - n));
}

static inline void mix(uint64_t *a, uint64_t *b, uint64_t *c, uint64_t *d,
        uint64_t x, uint64_t y)
{
    *a = *a + *b + x;
    *d = rotr64(*d ^ *a, 32);
    *c = *c + *d;
    *b = rotr64(*b ^ *c, 24);
    *a = *a + *b + y;
    *d = rotr64(*d ^ *a, 16);
    *c = *c + *d;
    *b = rotr64(*b ^ *c, 63);
}

static void gpu_emulated_hash48(const blake2b_state_t *ctx, uint32_t idx, uint8_t out[48])
{
    uint64_t v[16];
    uint32_t g = idx / (512 / PARAM_N);
    uint64_t word1 = (uint64_t)g << 32;

    for (int i = 0; i < 8; i++) {
        v[i] = ctx->h[i];
        v[i + 8] = blake_iv[i];
    }
    v[12] ^= (uint64_t)(ZCASH_BLOCK_HEADER_LEN + 4);
    v[14] ^= (uint64_t)-1;

    mix(&v[0], &v[4], &v[8],  &v[12], 0, word1);
    mix(&v[1], &v[5], &v[9],  &v[13], 0, 0);
    mix(&v[2], &v[6], &v[10], &v[14], 0, 0);
    mix(&v[3], &v[7], &v[11], &v[15], 0, 0);
    mix(&v[0], &v[5], &v[10], &v[15], 0, 0);
    mix(&v[1], &v[6], &v[11], &v[12], 0, 0);
    mix(&v[2], &v[7], &v[8],  &v[13], 0, 0);
    mix(&v[3], &v[4], &v[9],  &v[14], 0, 0);

    mix(&v[0], &v[4], &v[8],  &v[12], 0, 0);
    mix(&v[1], &v[5], &v[9],  &v[13], 0, 0);
    mix(&v[2], &v[6], &v[10], &v[14], 0, 0);
    mix(&v[3], &v[7], &v[11], &v[15], 0, 0);
    mix(&v[0], &v[5], &v[10], &v[15], word1, 0);
    mix(&v[1], &v[6], &v[11], &v[12], 0, 0);
    mix(&v[2], &v[7], &v[8],  &v[13], 0, 0);
    mix(&v[3], &v[4], &v[9],  &v[14], 0, 0);

    mix(&v[0], &v[4], &v[8],  &v[12], 0, 0);
    mix(&v[1], &v[5], &v[9],  &v[13], 0, 0);
    mix(&v[2], &v[6], &v[10], &v[14], 0, 0);
    mix(&v[3], &v[7], &v[11], &v[15], 0, 0);
    mix(&v[0], &v[5], &v[10], &v[15], 0, 0);
    mix(&v[1], &v[6], &v[11], &v[12], 0, 0);
    mix(&v[2], &v[7], &v[8],  &v[13], 0, word1);
    mix(&v[3], &v[4], &v[9],  &v[14], 0, 0);

    mix(&v[0], &v[4], &v[8],  &v[12], 0, 0);
    mix(&v[1], &v[5], &v[9],  &v[13], 0, word1);
    mix(&v[2], &v[6], &v[10], &v[14], 0, 0);
    mix(&v[3], &v[7], &v[11], &v[15], 0, 0);
    mix(&v[0], &v[5], &v[10], &v[15], 0, 0);
    mix(&v[1], &v[6], &v[11], &v[12], 0, 0);
    mix(&v[2], &v[7], &v[8],  &v[13], 0, 0);
    mix(&v[3], &v[4], &v[9],  &v[14], 0, 0);

    mix(&v[0], &v[4], &v[8],  &v[12], 0, 0);
    mix(&v[1], &v[5], &v[9],  &v[13], 0, 0);
    mix(&v[2], &v[6], &v[10], &v[14], 0, 0);
    mix(&v[3], &v[7], &v[11], &v[15], 0, 0);
    mix(&v[0], &v[5], &v[10], &v[15], 0, word1);
    mix(&v[1], &v[6], &v[11], &v[12], 0, 0);
    mix(&v[2], &v[7], &v[8],  &v[13], 0, 0);
    mix(&v[3], &v[4], &v[9],  &v[14], 0, 0);

    mix(&v[0], &v[4], &v[8],  &v[12], 0, 0);
    mix(&v[1], &v[5], &v[9],  &v[13], 0, 0);
    mix(&v[2], &v[6], &v[10], &v[14], 0, 0);
    mix(&v[3], &v[7], &v[11], &v[15], 0, 0);
    mix(&v[0], &v[5], &v[10], &v[15], 0, 0);
    mix(&v[1], &v[6], &v[11], &v[12], 0, 0);
    mix(&v[2], &v[7], &v[8],  &v[13], 0, 0);
    mix(&v[3], &v[4], &v[9],  &v[14], word1, 0);

    mix(&v[0], &v[4], &v[8],  &v[12], 0, 0);
    mix(&v[1], &v[5], &v[9],  &v[13], word1, 0);
    mix(&v[2], &v[6], &v[10], &v[14], 0, 0);
    mix(&v[3], &v[7], &v[11], &v[15], 0, 0);
    mix(&v[0], &v[5], &v[10], &v[15], 0, 0);
    mix(&v[1], &v[6], &v[11], &v[12], 0, 0);
    mix(&v[2], &v[7], &v[8],  &v[13], 0, 0);
    mix(&v[3], &v[4], &v[9],  &v[14], 0, 0);

    mix(&v[0], &v[4], &v[8],  &v[12], 0, 0);
    mix(&v[1], &v[5], &v[9],  &v[13], 0, 0);
    mix(&v[2], &v[6], &v[10], &v[14], 0, word1);
    mix(&v[3], &v[7], &v[11], &v[15], 0, 0);
    mix(&v[0], &v[5], &v[10], &v[15], 0, 0);
    mix(&v[1], &v[6], &v[11], &v[12], 0, 0);
    mix(&v[2], &v[7], &v[8],  &v[13], 0, 0);
    mix(&v[3], &v[4], &v[9],  &v[14], 0, 0);

    mix(&v[0], &v[4], &v[8],  &v[12], 0, 0);
    mix(&v[1], &v[5], &v[9],  &v[13], 0, 0);
    mix(&v[2], &v[6], &v[10], &v[14], 0, 0);
    mix(&v[3], &v[7], &v[11], &v[15], 0, 0);
    mix(&v[0], &v[5], &v[10], &v[15], 0, 0);
    mix(&v[1], &v[6], &v[11], &v[12], 0, 0);
    mix(&v[2], &v[7], &v[8],  &v[13], word1, 0);
    mix(&v[3], &v[4], &v[9],  &v[14], 0, 0);

    mix(&v[0], &v[4], &v[8],  &v[12], 0, 0);
    mix(&v[1], &v[5], &v[9],  &v[13], 0, 0);
    mix(&v[2], &v[6], &v[10], &v[14], 0, 0);
    mix(&v[3], &v[7], &v[11], &v[15], word1, 0);
    mix(&v[0], &v[5], &v[10], &v[15], 0, 0);
    mix(&v[1], &v[6], &v[11], &v[12], 0, 0);
    mix(&v[2], &v[7], &v[8],  &v[13], 0, 0);
    mix(&v[3], &v[4], &v[9],  &v[14], 0, 0);

    mix(&v[0], &v[4], &v[8],  &v[12], 0, word1);
    mix(&v[1], &v[5], &v[9],  &v[13], 0, 0);
    mix(&v[2], &v[6], &v[10], &v[14], 0, 0);
    mix(&v[3], &v[7], &v[11], &v[15], 0, 0);
    mix(&v[0], &v[5], &v[10], &v[15], 0, 0);
    mix(&v[1], &v[6], &v[11], &v[12], 0, 0);
    mix(&v[2], &v[7], &v[8],  &v[13], 0, 0);
    mix(&v[3], &v[4], &v[9],  &v[14], 0, 0);

    mix(&v[0], &v[4], &v[8],  &v[12], 0, 0);
    mix(&v[1], &v[5], &v[9],  &v[13], 0, 0);
    mix(&v[2], &v[6], &v[10], &v[14], 0, 0);
    mix(&v[3], &v[7], &v[11], &v[15], 0, 0);
    mix(&v[0], &v[5], &v[10], &v[15], word1, 0);
    mix(&v[1], &v[6], &v[11], &v[12], 0, 0);
    mix(&v[2], &v[7], &v[8],  &v[13], 0, 0);
    mix(&v[3], &v[4], &v[9],  &v[14], 0, 0);

    uint64_t h[6];
    h[0] = ctx->h[0] ^ v[0] ^ v[8];
    h[1] = ctx->h[1] ^ v[1] ^ v[9];
    h[2] = ctx->h[2] ^ v[2] ^ v[10];
    h[3] = ctx->h[3] ^ v[3] ^ v[11];
    h[4] = ctx->h[4] ^ v[4] ^ v[12];
    h[5] = ctx->h[5] ^ v[5] ^ v[13];
    memcpy(out, h, 48);
}

static void cpu_current_hash48(const blake2b_state_t *ctx, uint32_t idx, uint8_t out[48])
{
    blake2b_state_t st = *ctx;
    uint64_t message[16] = {0};
    uint32_t g = idx / (512 / PARAM_N);

    message[1] = ((uint64_t)g) << 32;
    st.bytes = ZCASH_BLOCK_HEADER_LEN;
    zcash_blake2b_update(&st, (const uint8_t *)message, sizeof(uint32_t), 1);
    zcash_blake2b_final(&st, out, ZCASH_HASH_LEN);
}

static void dump_hex(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        printf("%02x", p[i]);
}

int main(void)
{
    uint8_t header[ZCASH_BLOCK_HEADER_LEN] = {0};
    blake2b_state_t ctx;

    zcash_blake2b_init(&ctx, ZCASH_HASH_LEN, PARAM_N, PARAM_K);
    zcash_blake2b_update(&ctx, header, 128, 0);

    printf("Comparing GPU-emulated vs current CPU path (N=192,K=7)\n");
    printf("ctx.bytes after header precompute: %lu\n\n", ctx.bytes);

    for (uint32_t idx = 0; idx < 6; idx++) {
        uint8_t gpu[48], cpu[48];
        gpu_emulated_hash48(&ctx, idx, gpu);
        cpu_current_hash48(&ctx, idx, cpu);
        int same = memcmp(gpu, cpu, 48) == 0;
        printf("idx=%u %s\n", idx, same ? "MATCH" : "MISMATCH");
        if (!same) {
            printf("  gpu: "); dump_hex(gpu, 48); printf("\n");
            printf("  cpu: "); dump_hex(cpu, 48); printf("\n");
            printf("  gpu_xi0: "); dump_hex(gpu, 24); printf("\n");
            printf("  cpu_xi0: "); dump_hex(cpu, 24); printf("\n");
            printf("  gpu_xi1: "); dump_hex(gpu + 24, 24); printf("\n");
            printf("  cpu_xi1: "); dump_hex(cpu + 24, 24); printf("\n");
        }
    }

    return 0;
}
