#include "equi.h"
#include <stdio.h>

int main() {
    printf("Compiled with:\n");
    printf("WN = %d (should be 192)\n", WN);
    printf("WK = %d (should be 7)\n", WK);
    printf("PROOFSIZE = %d (should be 128)\n", PROOFSIZE);
    printf("DIGITBITS = %d (should be 24)\n", DIGITBITS);
    printf("HASHESPERBLAKE = %d (should be 2)\n", HASHESPERBLAKE);
    printf("HASHOUT = %d (should be 48)\n", HASHOUT);
    return 0;
}
