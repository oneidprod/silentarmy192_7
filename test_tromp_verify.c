#include "equi.h"
#include <stdio.h>
#include <string.h>

// Test if Tromp's verify() accepts the solution for the real Zero header
int main() {
    // Real Zero block header from zero-cli.txt
    unsigned char header[140];
    const char *hex = "040000003dd4a53440b11dde6a279a5318b2cce23bcef20655c6567d20b416b183150000ec45bcf34a761fe160cc5ba398f5bf366b68a8304493fc56c86847914d8918b32701efcedb2bf54fef50397923516e1a6a2b5d13b395fb21840ee94a3bab2231d3aa769ebbd2a1ec0020d00000000000000000000000000000000000000000000000000fc6b1000";
    
    for (int i = 0; i < 140; i++) {
        sscanf(hex + 2*i, "%2hhx", &header[i]);
    }
    
    // Solution from eq1927
    u32 indices[128] = {
        0x7a4f, 0x1eb1541, 0x294426, 0x13d3c47, 0x1f0a41, 0x14ab265, 0x1d73266, 0x1f58162,
        0x70df63, 0x12b3a4a, 0x10044e1, 0x13bf5ac, 0xca6b43, 0x13cb6f4, 0x195535e, 0x1e1f933,
        0x306d24, 0x194f438, 0x137f91c, 0x1f2e8f9, 0x116e89c, 0x13e5bd6, 0x1cc51ce, 0x1f7824f,
        0x518202, 0x1ff263b, 0x87d065, 0x10bfe62, 0x52a8e7, 0x185f91c, 0xf826f4, 0x15b667b,
        0x18e296, 0xc2a913, 0xce669a, 0x104da42, 0x23fcda, 0x1ccc7b1, 0x198ae2c, 0x1a7ffee,
        0x21aaea, 0x67184a, 0x7ae697, 0x91c700, 0x706428, 0x10d05c8, 0x1b11a2a, 0x1bed7a9,
        0x19285e, 0x1d92a36, 0xb10875, 0x1997fb4, 0x2d6e9a, 0x1593b3a, 0x1583544, 0x1b49c6b,
        0x262d7e, 0x16350e8, 0x8bd067, 0xe5c782, 0x96b35a, 0x16c7be5, 0x16d849d, 0x1c91009,
        0x56cb3, 0x1bb6b92, 0x3767f8, 0x11231a3, 0x1fa8be, 0x1b4a883, 0xc5fac6, 0x16f9cbd,
        0x4d7cb4, 0x1a10c59, 0xfde0f0, 0x1718dfc, 0x4dd50f, 0x1056c0c, 0xbbdaf0, 0x11f603b,
        0x5f7c22, 0x6feca1, 0xada3c4, 0x16281f4, 0x6824a9, 0x76e10d, 0xcb5528, 0xd0ac52,
        0xc3bc0b, 0x1bb48eb, 0x11525fa, 0x1a65781, 0x15cd1a7, 0x18b2429, 0x16e213a, 0x1b04e0a,
        0xa18d3, 0x1691057, 0x8ed418, 0x1a37253, 0xe5457, 0xff8f06, 0x689dbf, 0xe9fd00,
        0x74501a, 0x1ad1308, 0x800a59, 0xfc3820, 0xbb9d36, 0x18637b7, 0x14cd484, 0x1a28184,
        0x1db541, 0x1aa7c2f, 0x138d926, 0x17aff5d, 0x4f3c27, 0x175b9fd, 0xa67206, 0xabe5a3,
        0x4828ff, 0xd14ba3, 0x55560d, 0xbd1130, 0xd9d830, 0x19f785b, 0xf9ddec, 0x1dbd250
    };
    
    printf("Testing Tromp's verify() with real Zero header...\n");
    int rc = verify(indices, (char *)header, 140, "ZERO_PoW");
    
    if (rc == POW_OK) {
        printf("✓ Tromp's verifier ACCEPTS the solution!\n");
        printf("This confirms eq1927 generated a valid solution.\n");
        return 0;
    } else {
        printf("✗ Tromp's verifier REJECTS: %s\n", errstr[rc]);
        return 1;
    }
}
