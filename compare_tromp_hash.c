#include "equi.h"
#include <stdio.h>
#include <string.h>

int main() {
    char header[140];
    memcpy(header, "zero", 4);
    memset(header + 4, 0, 136);

    blake2b_state ctx;
    setheader(&ctx, header, "ZERO_PoW");

    uchar hash[24];  // WN/8 = 192/8 = 24
    genhash(&ctx, 0x6cc1, hash);
    
    printf("Tromp hash for idx=0x6cc1: ");
    for (int i = 0; i < 24; i++) printf("%02x", hash[i]);
    printf("\n");
    
    return 0;
}
