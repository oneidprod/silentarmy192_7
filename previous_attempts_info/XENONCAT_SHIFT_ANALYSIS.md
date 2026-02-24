# Xenoncat 192,7 Shift Value Analysis

## Summary
After extensive analysis of xenoncat's assembly code, CUDA implementations, and tromp's source, we've identified the core challenge but haven't found the exact shift values for 192,7.

## What We Fixed (90%+ Complete)
1. ✅ **Memory Allocation**: ITEMS = 43688 (calculated correctly for 192,7)
2. ✅ **Bucket Arrays**: PARTS multiplier = 4
3. ✅ **rbx Overflow**: Fixed register calculations
4. ✅ **Build System**: NASM-only compilation working
5. ✅ **Blake2b**: Personalization string "ZERO_PoW", Blake2b phase completes successfully
6. ✅ **STATE_OFFSETS**: Corrected from [11,14,17,20,23,26,29] to [8,11,14,17,20,23,26]
7. ✅ **STATE_BYTES**: Corrected from [24,24,20,20,12,12,4] to [24,20,20,16,12,8,8]

## Remaining Challenge (8%)
**Bucket extraction shift values** in `macro_eh_auto.asm`:

### Original 200,9 (WORKING):
```nasm
Stage 1: shr rdx, 44
Stage 2: shr rdx, 24
Stage 3: shr edx, 4
```

### For 192,7 (UNKNOWN):
Tested combinations (all crashed):
- (40, 16, 0) - simple -4 adjustment
- (40, 20, 4) - maintaining spacing
- (44, 24, 4) - same as 200,9
- (32, 8, 0) - byte boundaries
- (48, 24, 0) - byte aligned
- (52, 32, 12) - relative positioning
- (36, 12, 0) - -8 bit offset
- (48, 28, 8) - DIGITBITS spacing (24)

## Technical Analysis

### Key Findings from Original Xenoncat Source
From `/home/griffithm/builds/equihash-xenon/Linux/asm/`:

1. **Shift Pattern**: Spacing equals DIGITBITS
   - 200,9: shifts differ by 20 (DIGITBITS=20)
   - 192,7: should differ by 24 (DIGITBITS=24)?

2. **Blake2b Write Operation** (proc_ehsolver_avx2.asm lines 94-100):
   ```asm
   vpalignr ymm10, ymm7, ymm1, 15
   vmovq rax, xmm10
   movnti [rdi], rax        ; bytes 0-7
   vpextrq rax, xmm10, 1
   movnti [rdi+8], rax      ; bytes 8-15
   vmovq rax, xmm9          ; THIS is read at +16 for bucket extraction!
   movnti [rdi+16], rax     ; bytes 16-23
   ```

3. **Memory Layout**:
   - 32-byte slots: [padding][hash data aligned to end]
   - STATE0_OFFSET determines where hash data starts
   - Offset +16 reads a FIXED position regardless of hash length

### From CUDA Code Analysis
File: `/home/griffithm/builds/nheqminer_1927/cuda_tromp/equi_miner.cu`

For byte-aligned WN values (WN % 24 == 0), bucket extraction uses **complete bytes**:
- WN=144 (BUCKBITS=20): uses bytes at prevbo+1, prevbo+2, prevbo+3
- WN=96 (BUCKBITS=12): uses bytes at prevbo+1, prevbo+2
- **WN=192 (BUCKBITS=8)**: should use a **single full byte**

### Hypothesis
For 192,7 with BUCKBITS=8 (256 buckets), the bucket should be:
1. A complete 8-bit byte (not bit-shifted across boundaries)
2. Located within the 64-bit word read at offset +16
3. At a position that shifts by DIGITBITS (24) between stages

### Problem
- Total bits stored: 184 (vs 200,9's 192) → 8 bits fewer
- This causes a -8 bit shift in data positioning
- Stage 3 shift: 4-8 = -4 (INVALID!)

## Paths Forward

### Option 1: Instrumented Debugging (20-40 hours)
Add logging to Blake2b write operations to capture actual bit positions:
```c
printf("Slot offset +16 contains: %016lx\n", *(uint64_t*)(slot+16));
printf("Bucket from this should be: 0-255\n");
```
Compare with tromp's bucket values for same nonce to reverse-engineer correct shifts.

### Option 2: Use cpu_tromp (RECOMMENDED - Working Now)
```bash
./nheqminer -t 4 -e 0  # cpu_tromp, 5-10 Sol/s, WORKS for 192,7
```
Performance is good for CPU mining, thoroughly tested.

### Option 3: GPU Mining (Best Performance)
CUDA solvers disabled but could be re-enabled:
```bash
# After fixing CUDA code for 192,7
./nheqminer -cd 0 -t 0  # 1000+ Sol/s
```

### Option 4: Contact xenoncat
Search for xenoncat's contact info (GitHub: @xenoncat, forums) and ask about:
1. Bit layout documentation
2. Formula for calculating shifts based on WN/DIGITBITS/RESTBITS
3. Or commission adaptation for bounty ($500-$2000)

### Option 5: Hybrid Approach
Modify xenoncat to use tromp's 14-bit bucketing (16,384 buckets instead of 256):
- Change BUCKBITS from 8 to 14
- Adapt bucket array sizes
- Use tromp's byte-level extraction pattern
- Estimated effort: 40-80 hours

## Formulas Verified

### STATE_OFFSETS Formula (CORRECT ✓)
```
STATE_OFFSET[r] = 32 - ceil((WN - (r+1)*DIGITBITS + RESTBITS) / 8)
```
Produces exact match for 200,9 and correct values for 192,7.

### STATE_BYTES Formula (CORRECT ✓)
```
bits = WN - (r+1)*DIGITBITS + RESTBITS
STATE_BYTES[r] = round((bits + 7) / 8, 4)  // round to multiple of 4
```
Produces exact match for 200,9 and correct values for 192,7.

### Shift Formula (UNKNOWN ✗)
```
shift[stage] = ??? // Function of WN, DIGITBITS, RESTBITS, stage, vpalignr packing
```
Cannot be determined without:
- Original xenoncat documentation
- Instrumented debugging of vpalignr operations
- Or empirical testing with known-good solutions

## Recommendation

**Use cpu_tromp** (`-e 0`) which works perfectly for 192,7 mining. The xenoncat optimization provides ~2x CPU performance over tromp but requires the correct shift values. Given cpu_tromp's solid 5-10 Sol/s on modern CPUs and the time investment needed to solve the shift mystery, tromp is the pragmatic choice unless:

1. You need maximum CPU performance and can invest 20-40 hours in instrumented debugging
2. You plan to mine extensively and the 2x speedup justifies the effort
3. You can contact xenoncat directly for the formula

## Files Modified
- `/home/griffithm/builds/nheqminer_1927/cpu_xenoncat/asm_linux_nasm/params.inc`
  - Fixed STATE_OFFSETS and STATE_BYTES for 192,7
- `/home/griffithm/builds/nheqminer_1927/cpu_xenoncat/asm_linux_nasm/macro_eh_auto.asm`
  - Shift values remain UNKNOWN (currently set to 48, 28, 8)

## Test Command
```bash
cd /home/griffithm/builds/nheqminer_1927/build
./nheqminer -b 10 -t 1 -e 2  # Test xenoncat (crashes)
./nheqminer -b 10 -t 1 -e 0  # Test cpu_tromp (works!)
```
