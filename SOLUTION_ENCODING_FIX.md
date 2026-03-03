# Solution Encoding Fix - Error 20/21 Resolution

## Problem Summary

Pool rejection (error 20/21 "invalid solution") was caused by **solution encoding mismatch** between our `store_encoded_sol()` and what the pool's equihashverify expects.

### Root Cause

The pool receives solutions in this format:
```
fd9001 <1600 hex chars> = 806 chars total
```

Processing:
1. Pool strips fd9001 prefix (first 6 hex chars)
2. Converts remaining 800 hex chars to 400 raw bytes
3. **Calls ev.verify(headerBuffer, 400_bytes, personalization, N, K)**
4. ev.verify calls `GetIndicesFromMinimal(400_bytes, 24)` to decompress
5. GetIndicesFromMinimal uses `ExpandArray()` with **Big-Endian bit-packing**

**Our old store_encoded_sol() used a DIFFERENT algorithm** that didn't match this format!

### The Fix

Replaced simple bit-shifting with the reference `CompressArray()` algorithm from equihashverify:

**Old approach (WRONG):**
```c
// Simple right-shifting - doesn't match CompressArray
if (bits_left >= 8) {
    b = inputs[i] >> (bits_left -= 8);
}
```

**New approach (CORRECT):**
```c
// Matches equihashverify CompressArray exactly
// 1. Convert indices to Big-Endian
uint32_t bei = htobe32(inputs[i]); 

// 2. Use complex masking and left-shifts
acc_value = acc_value << bit_len;  // 25-bit shift
for (size_t x = byte_pad; x < in_width; x++) {
    uint32_t mask = (bit_len_mask >> (8*(in_width-x-1))) & 0xFF;
    acc_value = acc_value | ((byte_val & mask) << (8*(in_width-x-1)));
}
```

## Technical Details

### Algorithm Parameters (192,7)

| Parameter | Value | Notes |
|-----------|-------|-------|
| N | 192 | Equihash N parameter |
| K | 7 | Equihash K parameter |
| CollisionBitLength (cBitLen) | 24 | N/(K+1) = 192/8 |
| Compressed bits (bit_len) | 25 | cBitLen + 1 |
| Byte padding | 0 | sizeof(uint32_t) - 4 |
| Input width | 4 bytes | 32-bit indices |
| Output length | 400 bytes | 128 * 25 / 8 |

### CompressArray Algorithm

1. **Big-Endian conversion**: Each index is converted to Big-Endian using `htobe32()`
2. **Accumulator-based bit extraction**: 
   - Maintain 32-bit accumulator with bit counter
   - When accumulator has < 8 bits, shift left 25 bits and read next index
   - Extract 8 bits at a time from right side of accumulator
3. **Complex masking**: Each byte is masked according to bit boundaries

### Why This Matters

The reference implementation in equihashverify uses Big-Endian byte-ordering because:
- **Lexicographic comparison is equivalent to integer comparison** with Big-Endian
- This ensures pool-side verification matches solver-side generation
- Wrong endianness = wrong bytes = verification failure

## Verification

### Local Testing

The fixed encoding should match what equihashverify expects. To verify:

1. Generate a solution (encoder has 128 25-bit indices)
2. Encode using new `store_encoded_sol()`
3. Send 400 bytes to ev.verify() 
4. ExpandArray() should correctly decompress to 128 indices

### Expected Outcome

After re-submitting to the pool, solutions should be ACCEPTED instead of rejected with error 20.

```
Before: Pool returns error [20, "invalid solution"]
After:  Pool accepts share: stratum.error = None
```

## Pool Configuration Verification

Ensure pool knows about your Zero coin parameters:

```javascript
// In stratum-lib/lib/jobManager.js line 248-254
let parameters = options.coin.parameters
if (!parameters) {
    parameters = {
        N: 200,      // DEFAULT - wrong for Zero!
        K: 9,        // DEFAULT - wrong for Zero!
        personalization: 'ZcashPoW'  // DEFAULT - wrong for Zero!
    }
}

// Pool config should have:
{
    "coin": {
        "name": "Zero",
        "algorithm": "equihash",
        "parameters": {
            "N": 192,
            "K": 7,
            "personalization": "ZERO_PoW"  // EXACTLY "ZERO_PoW"
        }
    }
}
```

## File Changes

- **main.c lines 596-650**: Replaced `store_encoded_sol()` with CompressArray-compatible version
- No changes to:
  - Solution format (still fd9001 + 800 hex)
  - Pool protocol (still stratum)
  - Blob header format (still 140 bytes)

## Next Steps

1. **Build**: `make clean && make -j4`
2. **Test locally**: Run solver and verify output format
3. **Re-submit**: Send solutions to zeropool.io
4. **Monitor**: Check for error 20 rejections (should disappear)
5. **Verify**: Pool should accept shares and report difficulty

## References

- Source: equihashverify/crypto/equihash.cpp
- Functions: `CompressArray()`, `GetMinimalFromIndices()`, `ExpandArray()`
- Import: `#include <endian.h>` for htobe32()

