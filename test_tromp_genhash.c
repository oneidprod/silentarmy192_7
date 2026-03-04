#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "equihash_tromp/blake/blake2.h"
#ifdef __APPLE__
#include <machine/endian.h>
#include <libkern/OSByteOrder.h>
#define htole32(x) OSSwapHostToLittleInt32(x)
#else
#include <endian.h>
#endif

#define WN 192
#define WK 7
#define HEADERNONCELEN 140
#define DIGITBITS (WN/(WK+1))
#define HASHESPERBLAKE (512/WN)
#define HASHOUT (HASHESPERBLAKE*WN/8)

typedef uint32_t u32;
typedef unsigned char uchar;

void genhash(const blake2b_state *ctx, u32 idx, uchar *hash) {
  blake2b_state state = *ctx;
  u32 leb = htole32(idx / HASHESPERBLAKE);
  blake2b_update(&state, (uchar *)&leb, sizeof(u32));
  uchar blakehash[HASHOUT];
  blake2b_final(&state, blakehash, HASHOUT);
  memcpy(hash, blakehash + (idx % HASHESPERBLAKE) * WN/8, WN/8);
}

void setheader(blake2b_state *ctx, const char *headernonce, const char *personal) {
  char personals[16];
  memcpy(personals+ 0, personal, 8);
  uint32_t le_N = htole32(WN);
  memcpy(personals+ 8, &le_N, 4);
  uint32_t le_K = htole32(WK);
  memcpy(personals+12, &le_K, 4);
  blake2b_param P[1];
  memset(P, 0, sizeof(blake2b_param));
  P->digest_length = HASHOUT;
  P->fanout        = 1;
  P->depth         = 1;
  memcpy(P->personal, (const uint8_t *)personals, 16);
  blake2b_init_param(ctx, P);
  blake2b_update(ctx, (const uchar *)headernonce, HEADERNONCELEN);
}

int main() {
    const char *header_hex = "040000003dd4a53440b11dde6a279a5356e70141edca0a83c5d7d5a81b0bcd0c0c5e7bbca1b6e5f306976e70a0b5c9a8de7f61e2d3c4a5b6c7d81926374e5f6a7b8c2d3e4f506172a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0";
    
    uchar header[HEADERNONCELEN];
    for (int i = 0; i < HEADERNONCELEN; i++) {
        sscanf(header_hex + i*2, "%2hhx", &header[i]);
    }
    
    blake2b_state ctx;
    setheader(&ctx, (const char *)header, "ZERO_PoW");
    
    u32 indices[] = {31311, 32183617};
    uchar hash0[WN/8], hash1[WN/8];
    
    genhash(&ctx, indices[0], hash0);
    genhash(&ctx, indices[1], hash1);
    
    printf("Tromp's genhash results:\n");
    printf("idx=%u hash:", indices[0]);
    for (int i = 0; i < 6; i++) printf("%02x", hash0[i]);
    printf("\n");
    
    printf("idx=%u hash:", indices[1]);
    for (int i = 0; i < 6; i++) printf("%02x", hash1[i]);
    printf("\n");
    
    printf("XOR first bytes: %02x XOR %02x = %02x\n", hash0[0], hash1[0], hash0[0] ^ hash1[0]);
    
    return 0;
}
