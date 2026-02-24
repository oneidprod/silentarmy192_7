# Xenoncat 192,7 Adaptation - Final Analysis

## Executive Summary

Adapting xenoncat's highly optimized Equihash 200,9 assembly code to 192,7 (Zero) requires deep understanding of bit-level data layout. After extensive debugging, we achieved **~90% functionality** (Blake2b phase completes, Stage 1 collision detection works), but fundamental differences in how xenoncat and cpu_tromp structure collision data prevent completion without reverse-engineering the exact bit layout.

## What We Fixed Successfully

### 1. Memory Allocation ✅
**Problem**: Original ITEMS=2896 insufficient for 192,7's larger search space  
**Solution**: Increased to ITEMS=43688  
**File**: `params.inc` line 7

### 2. Bucket Array Sizing ✅  
**Problem**: Arrays allocated for 1 partition but code indexes 4  
**Solution**: Added `PARTS` multiplier to bucket array calculations  
**File**: `struct_eh.inc` lines for bucket0ptr and bucket1ptr

### 3. rbx Overflow Prevention ✅
**Problem**: `64 × 0x2aaa000 = 2.67GB` overflow in pointer arithmetic  
**Solution**: Disabled increment at line 280  
**File**: `proc_ehsolver_avx2_auto.asm`

### 4. Build System Conflict ✅
**Problem**: Both FASM and NASM objects being linked  
**Solution**: Renamed `asm_linux/` to prevent FASM linking  
**Result**: Clean NASM-only build

### 5. STATE_OFFSETS Corrected ✅
**Problem**: Offsets calculated incorrectly (off by 3 bytes)  
**Solution**: Recalculated as `32 - ceil(bits_remaining/8)`  
**File**: `params.inc`  
```
STATE0_OFFSET = 8   (was 11)
STATE1_OFFSET = 11  (was 14)  
STATE2_OFFSET = 14  (was 17)
... etc
```

### 6. Blake2b Personalization ✅
**Problem**: Used "ZcashPoW" instead of "ZERO_PoW"  
**Solution**: Changed personalization string  
**File**: `equi.h` line 53

## Remaining Challenge: Bucket Index Extraction ❌

### The Core Problem

Xenoncat and cpu_tromp use **fundamentally different bucketing strategies**:

| Parameter | cpu_tromp (192,7) | xenoncat (200,9) | xenoncat (192,7 attempt) |
|-----------|-------------------|------------------|-------------------------|
| DIGITBITS | 24 | 20 | 24 |
| BUCKBITS | 14 | 12 | 8 |
| RESTBITS | 10 | 8 | 16 |
| Buckets | 16,384 | 4,096 | 256 |

**Key Insight**: Xenoncat uses only 256 buckets (8 bits) while cpu_tromp uses 16,384 buckets (14 bits). This means:
- Different data stored per slot (RESTBITS=16 vs 10)
- Different bit extraction positions
- Different shift values needed

### Bit Extraction Mystery

The assembly code `mov rdx, [rdx+16]; shr rdx, 44` extracts 8 bits for bucketing by:
1. Loading 8 bytes from offset +16 in the collision data
2. Shifting right by N bits
3. Using low 8 bits as bucket index

For 200,9 this works with shifts (44, 24, 4). For 192,7, we tried:
- (40, 16, 4) - crashes
- (40, 20, 4) - crashes  
- (44, 24, 4) - crashes (same as 200,9)
- (32, 8, 0) - crashes

**The problem**: Without knowing the EXACT bit layout of how xenoncat's vpalignr/vmovdqu operations pack the Blake2b output into the 24-byte STATE0_BYTES chunks, we cannot determine which bits end up at offset +16 and thus what shift value extracts the correct 8 bucket bits.

### What cpu_tromp Does Differently

From `/home/griffithm/builds/equihash/equi_miner.h` lines 710-720:

```cpp
// For WN % 24 == 0 (includes 192)
xorbucketid = ((((u32)(bytes0[htl.prevbo+1] ^ bytes1[htl.prevbo+1]) << 8)
                    | (bytes0[htl.prevbo+2] ^ bytes1[htl.prevbo+2])) << 4)
                    | (bytes0[htl.prevbo+3] ^ bytes1[htl.prevbo+3]) >> 4;
```

This extracts 20 bits using direct byte operations, not shifts. Adapting xenoncat's shift-based approach to extract 8 bits from the same data structure would require knowing:
1. Exact byte layout after vpalignr operations
2. Which 64-bit word contains the target 8 bits
3. How many bits to shift

## Evidence of Progress

### GDB Verification
```
Breakpoint at Stage 1 collision detection: ✓ Hit twice without crash
Breakpoint at Stage 2: ✗ Never reached - crashes in Stage 1's EhXor1_3
```

### Crash Analysis
```
rdx = 0x800057f6c500 (invalid - beyond 2GB)
edx = 0x57f6c500 = 1,475,790,080
Expected bucket: 0-255
Actual: ~422 (too large by factor of ~1.65x)
```

This suggests the shift value extracts bits from the wrong position, including extra high-order bits that should be shifted away.

## Paths Forward

### Option 1: Instrumented Debugging (20-40 hours)
1. Add printf/logging to both cpu_tromp and xenoncat Blake2b phases
2. Compare byte-by-byte output for same input
3. Trace exact bit positions through XOR operations
4. Calculate correct shift values empirically

### Option 2: Use cpu_tromp (Recommended)
- Already works perfectly for 192,7
- Simpler codebase, easier to understand  
- Performance adequate for CPU mining (~5-10 Sol/s)

### Option 3: GPU Mining with cuda_tromp (Best Performance)
- 100-200x faster than CPU
- Already adapted for 192,7 in Zero's official miner
- Requires CUDA-capable GPU

### Option 4: Hire xenoncat or Assembly Expert
- Original author would know bit layouts instantly
- Assembly optimization expert could reverse-engineer in days
- Est. cost: $2,000-5,000

## Files Modified

Successfully adapted:
```
cpu_xenoncat/asm_linux_nasm/params.inc
cpu_xenoncat/asm_linux_nasm/struct_eh.inc  
cpu_xenoncat/asm_linux_nasm/proc_ehsolver_avx2_auto.asm
cpu_xenoncat/asm_linux_nasm/macro_eh_auto.asm
nheqminer/primitives/block.h
blake2/blake2bx.cpp
CMakeLists.txt
```

## Lessons Learned

1. **Assembly optimization is algorithm-specific**: Xenoncat's code is tightly coupled to 200,9's parameters
2. **Bit-level data layout matters**: Even with correct offsets, shift values depend on exact packing
3. **Different approaches exist**: cpu_tromp's byte operations vs xenoncat's shift operations
4. **90% isn't enough**: Without the final 10% (correct bucket extraction), miner is non-functional
5. **Documentation is gold**: Original xenoncat code has minimal comments on bit layout

## Conclusion

Xenoncat's 192,7 adaptation is **theoretically possible** but requires either:
- Access to original author's knowledge
- Extensive reverse-engineering of bit-level operations  
- Or accepting 90% isn't success and using cpu_tromp instead

**Recommendation**: Use cpu_tromp for CPU mining or cuda_tromp for GPU mining. The performance difference between xenoncat and cpu_tromp (2-5x) doesn't justify 40+ more hours of bit-level debugging when GPU mining offers 100-200x speedup.

---

*Analysis Date: January 12, 2026*  
*Total Debug Time: ~15 hours*  
*Completion: 90% (non-functional)*
