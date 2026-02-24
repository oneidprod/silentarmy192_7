# Xenoncat 192,7 Adaptation - Next Steps

## Current Status: MEMORY CORRUPTION BUG

The solver is crashing due to writing past the end of allocated memory. The crash occurs during the Blake2b hashing loop at offset 20.4 * BLOCKUNIT (242MB) when only 147MB is allocated.

## Root Cause Analysis

### What We've Fixed So Far
1. ✅ Fixed 20+ 32-bit LEA offset overflows using MOV+ADD pattern
2. ✅ Calculated correct bit allocations for 192,7:
   - Collision extraction byte offsets: [22, 20, 18, 16, 14, 12, 10]
   - STATE_BYTES: [24, 24, 20, 20, 16, 16, 12, 12]
3. ✅ Updated macro_eh_auto.asm with correct byte offsets and shifts
4. ✅ Updated params.inc with correct STATE_BYTES
5. ✅ Calculated correct CONTEXT_SIZE = 154,309,120 bytes (147.16 MB)

### Current Problem: **ASM Files Not Being Reassembled**

The changes to `params.inc` and `macro_eh_auto.asm` are NOT being picked up by the build system!

**Evidence:**
- Crash at offset 242MB suggests ASM is using old STATE_BYTES values
- No `.o` files found in source tree for cpu_xenoncat
- ASM symbols (EhPrepareAVX2, EhSolverAVX2) ARE present in final binary
- CMakeLists.txt for cpu_xenoncat doesn't include ASM compilation

**Hypothesis:** Pre-compiled ASM object files are being linked from somewhere hidden (cached build artifacts?), or there's a missing CMakeLists configuration.

## Required Actions

### Immediate: Find and Rebuild ASM
1. Search for where equihash_avx2.o or similar `.o` files are stored
   - Check: /home/griffithm/builds/nheqminer_1927/build/CMakeFiles/
   - Check: Any .a archive files that might contain ASM objects
   - Check: If there's a separate NASM/FASM build step
   
2. Force complete rebuild of ASM:
   ```bash
   cd /home/griffithm/builds/nheqminer_1927
   find . -name "*.o" -o -name "*.a" | xargs rm -f
   cd build
   rm -rf CMakeCache.txt CMakeFiles/
   cmake ../nheqminer
   make clean
   make VERBOSE=1 2>&1 | tee build.log
   ```

3. Verify params.inc values are being used:
   - Check build.log for NASM/FASM commands
   - Verify STATE1_BYTES=24 (not 20) is compiled in

### Alternative: Manual ASM Assembly
If CMake isn't set up to assemble, manually compile:
```bash
cd /home/griffithm/builds/nheqminer_1927/cpu_xenoncat/asm_linux_nasm

# Use NASM to assemble (if NASM is installed)
nasm -f elf64 -o equihash_avx2.o -l equihash_avx2.lst equihash_avx2.asm

# Or use FASM (Fast Assembler) if that's what xenoncat uses
# Check equihash-xenon reference for assembly commands
```

### Verification Steps
After successful rebuild:
1. Check crash offset is now within 147MB context
2. Verify Blake2b loop completes without segfault
3. Check that collision XOR stages execute
4. Validate solution output (if any)

## Technical Reference

### Correct Parameters for 192,7

**Bit Allocations** (each stage consumes 16 bits: 8 bucket + 8 collision):
```
Stage 0: [191:184] bucket, [183:176] collision → byte offset 22
Stage 1: [175:168] bucket, [167:160] collision → byte offset 20
Stage 2: [159:152] bucket, [151:144] collision → byte offset 18
Stage 3: [143:136] bucket, [135:128] collision → byte offset 16
Stage 4: [127:120] bucket, [119:112] collision → byte offset 14
Stage 5: [111:104] bucket, [103:96] collision → byte offset 12
Stage 6: [95:88] bucket, [87:80] collision → byte offset 10
```

**STATE_BYTES** (from params.inc):
```nasm
%define STATE0_BYTES 24
%define STATE1_BYTES 24  ; CHANGED from 20
%define STATE2_BYTES 20
%define STATE3_BYTES 20  ; CHANGED from 16
%define STATE4_BYTES 16  ; CHANGED from 12
%define STATE5_BYTES 16  ; CHANGED from 8
%define STATE6_BYTES 12  ; CHANGED from 8
```

**CONTEXT_SIZE** = 154,309,120 bytes (147.16 MB):
- Based on EH structure from equihash-xenon/Linux/asm/struct_eh.inc
- Formula: hashtab + bucket_ptrs + workingpairs + padding + debug + mids 
           + basemap(1*BLOCKUNIT) + pairs(7*BLOCKUNIT) + buf(5*BLOCKUNIT)
- Where BLOCKUNIT = 4 * ITEMS * BUCKETS * 4 = 11,862,016 bytes

### Comparison with 200,9

| Parameter | 200,9 | 192,7 |
|-----------|-------|-------|
| Total bits | 200 | 192 |
| Stages | 9 | 7 |
| Bucket bits | 8 | 8 |
| Collision bits | 12 | 8 |
| Bits/stage | ~20 | 16 |
| STATE1_BYTES | 24 | 24 |
| STATE6_BYTES | 12 | 12 |
| STATE8_BYTES | 4 | N/A |
| CONTEXT_SIZE | 178 MB | 147 MB |

### Files Modified

1. `/home/griffithm/builds/nheqminer_1927/cpu_xenoncat/asm_linux_nasm/params.inc`
   - Lines 13-18: Updated STATE_BYTES

2. `/home/griffithm/builds/nheqminer_1927/cpu_xenoncat/asm_linux_nasm/macro_eh_auto.asm`
   - Lines 48-68: EhXor1_3 - updated shifts for stages 1-3 (shr by 48, 32, 16)
   - Line 133: EhXor4 - updated byte offset to 14
   - Line 193: EhXor5 - updated byte offset to 12
   - Line 263: EhXor6_7 - updated extraction to vpextrd edx, xmm0, 2 + shr 16

3. `/home/griffithm/builds/nheqminer_1927/cpu_xenoncat/xenoncat.cpp`
   - Line 17: Updated CONTEXT_SIZE to 154309120UL

4. `/home/griffithm/builds/nheqminer_1927/cpu_xenoncat/asm_linux_nasm/proc_ehsolver_avx2_auto.asm`
   - Multiple lines: Fixed LEA 32-bit overflows (completed earlier)

## Debug Info from Last Crash
```
Context base:  0x7fffe6cd6000
Crash address: 0x7ffff5399300
Offset:        0xe6c3300 = 241,971,968 bytes (230.76 MB)
Context size:  154,309,120 bytes (147.16 MB)
CRASH: Writing 87,662,848 bytes past end!
Offset / BLOCKUNIT = 20.40
```

This indicates the ASM is calculating memory offsets using old STATE_BYTES values, proving the params.inc changes aren't being compiled.

## Next Session Actions
1. **PRIORITY**: Find how ASM is assembled and force rebuild
2. Verify correct params.inc values are compiled
3. Re-test with 3-5 benchmark iterations
4. If successful, scale up ITEMS from 2896 to full 43688
5. Validate solutions against Zero blockchain specs
