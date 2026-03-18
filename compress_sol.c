/*
 * compress_sol.c — Equihash 192,7 solution compression for Stratum pool submission
 *
 * C port of GetMinimalFromIndices + CompressArray from:
 *   nheqminer/libstratum/ZcashStratum.cpp (Jack Grigg, MIT license)
 *
 * For Equihash 192,7:
 *   cBitLen = 24, bit_len = cBitLen+1 = 25
 *   bytePad  = sizeof(uint32_t) - ((25+7)/8) = 4 - 4 = 0
 *   input    = 128 indices × 4 bytes = 512 bytes (big-endian uint32)
 *   output   = 25 × 512 / (8×4) = 400 bytes
 */

#include "compress_sol.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>

#ifdef __linux__
#include <endian.h>
#define htobe32_local htobe32
#else
/* fallback big-endian conversion */
static uint32_t htobe32_local(uint32_t x) {
    return ((x & 0xFF) << 24) | (((x >> 8) & 0xFF) << 16) |
           (((x >> 16) & 0xFF) << 8) | (x >> 24);
}
#endif

/* Write a uint32_t index as 4-byte big-endian (port of EhIndexToArray). */
static void eh_index_to_array(uint32_t idx, uint8_t *out) {
    uint32_t bei = htobe32_local(idx);
    memcpy(out, &bei, 4);
}

/*
 * compress_array — bit-pack an array of fixed-width integers.
 *
 * Each input element is in_width = (bit_len+7)/8 + byte_pad bytes (big-endian),
 * with the leading byte_pad bytes skipped (zero / don't-care).
 * The output is the concatenated bit_len-bit values, packed MSB-first.
 *
 * For Equihash 192,7: bit_len=25, byte_pad=0, in_width=4.
 */
void compress_array(const uint8_t *in, size_t in_len,
                    uint8_t *out, size_t out_len,
                    size_t bit_len, size_t byte_pad)
{
    assert(bit_len >= 8);
    assert(8 * sizeof(uint32_t) >= 7 + bit_len);

    size_t in_width = (bit_len + 7) / 8 + byte_pad;
    assert(out_len == bit_len * in_len / (8 * in_width));

    uint32_t bit_len_mask = ((uint32_t)1 << bit_len) - 1;

    size_t acc_bits = 0;
    uint32_t acc_value = 0;

    size_t j = 0;
    for (size_t i = 0; i < out_len; i++) {
        if (acc_bits < 8) {
            acc_value = acc_value << bit_len;
            for (size_t x = byte_pad; x < in_width; x++) {
                acc_value = acc_value |
                    ((in[j + x] & ((bit_len_mask >> (8 * (in_width - x - 1))) & 0xFF))
                     << (8 * (in_width - x - 1)));
            }
            j += in_width;
            acc_bits += bit_len;
        }
        acc_bits -= 8;
        out[i] = (acc_value >> acc_bits) & 0xFF;
    }
}

/*
 * get_minimal_from_indices — compress 128 Equihash 192,7 solution indices.
 *
 * Returns 0 on success, -1 if arguments are wrong.
 */
int get_minimal_from_indices(const uint32_t *indices, int num_indices,
                             uint8_t *out, size_t out_len)
{
    if (num_indices != COMPRESS_PROOFSIZE || out_len < COMPRESSED_SOL_SIZE)
        return -1;

    const size_t cBitLen  = COMPRESS_CBITLEN;          /* 24 */
    const size_t bit_len  = cBitLen + 1;               /* 25 */
    const size_t len_indices = (size_t)num_indices * 4; /* 512 bytes */
    const size_t byte_pad = sizeof(uint32_t) - ((bit_len + 7) / 8); /* 0 */
    const size_t min_len  = bit_len * len_indices / (8 * sizeof(uint32_t)); /* 400 */

    /* Expand indices to big-endian byte array */
    uint8_t array[COMPRESS_PROOFSIZE * 4]; /* 512 bytes */
    for (int i = 0; i < num_indices; i++)
        eh_index_to_array(indices[i], array + i * 4);

    compress_array(array, len_indices, out, min_len, bit_len, byte_pad);
    return 0;
}
