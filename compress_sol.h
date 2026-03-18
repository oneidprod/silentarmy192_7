#ifndef COMPRESS_SOL_H
#define COMPRESS_SOL_H

#include <stdint.h>
#include <stddef.h>

/* Equihash 192,7 parameters */
#define COMPRESS_PROOFSIZE      128     /* 2^K = 2^7 */
#define COMPRESS_CBITLEN        24      /* N/(K+1) = 192/8 */
#define COMPRESSED_SOL_SIZE     400     /* (cBitLen+1) * PROOFSIZE / 8 = 25*128/8 */

/*
 * Compress 128 Equihash 192,7 solution indices into the minimal 400-byte
 * representation used by Stratum pool protocol.
 *
 * indices: 128 uint32_t values in canonical (sorted) order
 * out:     must point to COMPRESSED_SOL_SIZE (400) bytes
 * Returns 0 on success, -1 on error.
 */
int get_minimal_from_indices(const uint32_t *indices, int num_indices,
                             uint8_t *out, size_t out_len);

/* Low-level bit-packing (exposed for testing). */
void compress_array(const uint8_t *in, size_t in_len,
                    uint8_t *out, size_t out_len,
                    size_t bit_len, size_t byte_pad);

#endif /* COMPRESS_SOL_H */
