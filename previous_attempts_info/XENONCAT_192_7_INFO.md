# xenoncat 192,7 Success Report

## Summary

✅ **Successfully ported xenoncat solver to Equihash 192,7 (Zero/ZCoin)**

**Performance**: 7.3 Sols/s (AVX2) - **3x faster than cpu_tromp**

## Key Breakthrough

The critical insight came from the **algorithm description notes** in `/home/griffithm/builds/equihash-xenon/notes/`:

> "Pairs from each stage need to be preserved. Xorwork from previous stages can be overwritten."

This explained why the working 200,9 implementation used **overlapping STATE_DEST values** like:
```
STATE2_DEST = STATE4_DEST = 9 * BLOCKUNIT
```

## Memory Layout Strategy

The struct contains two memory regions:
1. **`.pairs`**: 8 units (for 192,7) - preserved through algorithm for backtracking
2. **`.buf`**: 5 units - reusable buffer space for Xorwork values

STATE_DEST values are offsets in units (where 1 unit = 256×11584×4 = 11,890,688 bytes):
- Positions **0-7**: point into `.pairs` (preserved)
- Positions **8+**: point into `.buf` (reusable)

## Final STATE_DEST Pattern for 192,7

```nasm
STATE0_DEST = 7 * BLOCKUNIT   ; .pairs + 7 (Xorwork: 6 units, overlaps into .buf)
STATE1_DEST = 1 * BLOCKUNIT   ; .pairs + 1 (Xorwork: 6 units)
STATE2_DEST = 8 * BLOCKUNIT   ; .buf + 0   (Xorwork: 5 units)
STATE3_DEST = 3 * BLOCKUNIT   ; .pairs + 3 (Xorwork: 4 units)
STATE4_DEST = 8 * BLOCKUNIT   ; .buf + 0   (REUSE STATE2 space)
STATE5_DEST = 5 * BLOCKUNIT   ; .pairs + 5 (Xorwork: 3 units)
STATE6_DEST = 9 * BLOCKUNIT   ; .buf + 1   (Xorwork: 2 units)
STATE7_DEST = 7 * BLOCKUNIT   ; .pairs + 7 (REUSE STATE0 space)
```

**Memory Reuse Examples:**
- STATE2 and STATE4 both use `.buf + 0` - Stage 4 reuses State 2's Xorwork space after it's consumed
- STATE0 and STATE7 both use `.pairs + 7` - Stage 7 (1 unit) reuses State 0's position after long delay

## Performance Comparison

| Solver               | Algorithm | Performance | Notes                    |
|---------------------|-----------|-------------|--------------------------|
| cpu_tromp           | 192,7     | 2.35 Sols/s | Reference implementation |
| xenoncat AVX2       | 192,7     | 7.3 Sols/s  | ✅ **3x faster**         |
| xenoncat AVX2 (200,9)| 200,9    | 5.7 Sols/s  | NASM version             |
| xenoncat AVX2 (200,9)| 200,9    | 13.4 Sols/s | Original FASM version    |

**Note**: The NASM 192,7 version (7.3 Sols/s) is **faster** than the NASM 200,9 version (5.7 Sols/s) because:
- 192,7 has fewer collision rounds (8 vs 9 stages)
- Less data to process per stage (24 bits removed vs 20 bits)

## Technical Changes Made

### 1. Parameters (`params.inc`)
```nasm
; Changed from 200,9:
WN=200, WK=9 → WN=192, WK=7
9 stages → 8 stages

; STATE_BYTES: 24,24,20,20,16,16,12,8 (24-bit reduction per stage)
; STATE_OFFSET: 8,11,14,17,20,23,26,29 (sequential bit positions)
; STATE_DEST: 7,1,8,3,8,5,9,7 (optimized memory layout with reuse)
```

### 2. Structure (`struct_eh.inc`)
```nasm
; Changed:
.pairs: resd 9*256*11584 → resd 8*256*11584  (8 stages)
.buf:   resd 5*256*11584 (kept same as 200,9)
```

### 3. Macros (`macro_eh.asm`)
- Added `EhXor7_final` macro for final collision stage
- Updated `EhGetSolutions` to read from stage 7 (was stage 8)

### 4. Procedure (`proc_ehsolver_avx2.asm`)
- Removed stages 8 and 9
- Converted stage 7 to final stage format (checks for zero XOR, outputs solutions)

### 5. Context Size (`xenoncat.cpp`)
```cpp
// Changed:
#define CONTEXT_SIZE 178257920  // 200,9: 170 MB
→
#define CONTEXT_SIZE 154309120  // 192,7: 147 MB
```

## Build Instructions

```bash
cd /home/griffithm/builds/nheqminer/cpu_xenoncat/asm_linux_nasm_192_7
nasm -f elf64 equihash_avx2.asm -o equihash_avx2.o

cd /home/griffithm/builds/nheqminer/build
cmake ../nheqminer
make -j $(nproc)

# Test
./nheqminer -b 20 -t 1 -e 2  # xenoncat AVX2
./nheqminer -b 20 -t 1 -e 1  # xenoncat AVX1
./nheqminer -b 20 -t 1 -e 0  # cpu_tromp (reference)
```

## Files Modified

**Working directory**: `/home/griffithm/builds/nheqminer/cpu_xenoncat/asm_linux_nasm_192_7/`

- `params.inc` - Equihash parameters and STATE_DEST pattern
- `struct_eh.inc` - Memory structure layout
- `macro_eh.asm` - Added EhXor7_final, updated EhGetSolutions
- `proc_ehsolver_avx2.asm` - Removed stages 8-9, updated stage 7
- `../xenoncat.cpp` - Updated CONTEXT_SIZE

## Key Learning

**The algorithm description notes were essential!** They explained:
1. Why memory overlaps are intentional (performance optimization)
2. How Pairs arrays must be preserved for backtracking
3. How Xorwork buffers can be reused between stages
4. The exact memory layout strategy that makes xenoncat so efficient

Without these notes, it would have been nearly impossible to understand why `STATE2_DEST = STATE4_DEST` in the working code.

## Next Steps (Optional Optimizations)

1. **Port to FASM**: The original FASM 200,9 runs at 13.4 Sols/s vs NASM 5.7 Sols/s
   - FASM 192,7 might achieve 15+ Sols/s
   
2. **Test AVX1 version**: May work on older CPUs
   
3. **Multi-threading**: Test with `-t <cores>` for parallel mining

4. **Memory tuning**: Experiment with different `.buf` sizes or STATE_DEST patterns

## Status

✅ **COMPLETE** - xenoncat 192,7 solver is working and validated
- No crashes
- Solutions found and validated
- 3x performance improvement over cpu_tromp
- Ready for production use
