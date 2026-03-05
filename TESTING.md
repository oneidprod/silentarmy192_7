# Test Programs for Equihash 192,7 GPU Implementation

## Overview
These test programs validate the GPU implementation of Tromp's Equihash 192,7 algorithm at various stages.

---

## CPU Reference

### `cpu_tromp_baseline`
**Purpose**: Complete CPU implementation of Tromp algorithm (reference for GPU)

**Usage**:
```bash
./cpu_tromp_baseline [nonce_count]
```

**Default**: 10,000 nonces

**Example**:
```bash
./cpu_tromp_baseline 10000
# Output: Stage 1: 16 collisions, Stage 2+, etc.
```

**Source**: cpu_tromp_baseline.c (734 lines)

---

## GPU Stage Tests

### `test_stage1`
**Purpose**: Quick validation with synthetic collision pairs

**Usage**:
```bash
./test_stage1
```

**Test**: 100 hashes with 2 manually crafted collision pairs

**Expected**: "✅ TEST PASSED: Collision count matches!"

**Source**: test_stage1.c (231 lines)

---

### `test_stage1_real`
**Purpose**: Stage 1 testing with real Blake2b hashes

**Usage**:
```bash
./test_stage1_real [nonce_count]
```

**Default**: 1,000 nonces (2,000 hashes)

**Examples**:
```bash
./test_stage1_real 1000   # Quick test (~0 collisions expected)
./test_stage1_real 10000  # Standard test (~16-17 collisions)
./test_stage1_real 50000  # Extended test (~80-90 collisions)
```

**Source**: test_stage1_real.c (254 lines)

**Memory**: ~2.7GB GPU

---

### `test_all_stages`
**Purpose**: Complete 7-stage collision detection pipeline

**Usage**:
```bash
./test_all_stages [nonce_count]
```

**Default**: 100 nonces (200 hashes)

**Examples**:
```bash
./test_all_stages 100     # Quick validation (0 collisions expected)
./test_all_stages 10000   # Standard test (~16 Stage 1 collisions)
./test_all_stages 50000   # Extended test (collision cascade visible)
```

**Source**: test_all_stages.c (14KB)

**Memory**: ~17.5GB GPU (may fail on 8GB iGPU)

**Output**:
```
Stage 1: X collisions
Stage 2: Y collisions
...
Stage 7: Z solution candidates
```

---

### `test_solution_extraction`
**Purpose**: Full pipeline + solution extraction and validation

**Usage**:
```bash
./test_solution_extraction [nonce_count]
```

**Default**: 50,000 nonces (100,000 hashes)

**Examples**:
```bash
./test_solution_extraction 50000   # Default test
./test_solution_extraction 100000  # Larger test (more likely to find solutions)
```

**Source**: test_solution_extraction.c (14KB)

**Memory**: ~17.5GB GPU

**What it tests**:
1. All 7 collision detection stages
2. Solution candidate extraction from Stage 7
3. Recursive tree traversal to get 128 indices
4. Duplicate validation
5. (Future) Full Equihash verification

---

## Test Compilation

All tests should already be compiled. If you need to rebuild:

```bash
# Stage 1 tests
gcc -o test_stage1 test_stage1.c blake.c sha256.c -lOpenCL -lm
gcc -o test_stage1_real test_stage1_real.c blake.c sha256.c -lOpenCL -lm

# Full pipeline tests
gcc -o test_all_stages test_all_stages.c blake.c sha256.c -lOpenCL -lm
gcc -o test_solution_extraction test_solution_extraction.c solution_extraction.c blake.c sha256.c -lOpenCL -lm

# CPU baseline
gcc -o cpu_tromp_baseline cpu_tromp_baseline.c blake.c sha256.c -std=c99 -O3 -march=native
```

---

## Expected Results

### Small Tests (100-1K nonces)
- **Stage 1**: 0-1 collisions (random variation)
- **Stage 2+**: 0 collisions (cascade dies out)
- **Memory**: Manageable

### Medium Tests (10K nonces)
- **Stage 1**: 14-20 collisions (birthday paradox)
- **Stage 2**: 0-2 collisions (most die out)
- **Stage 3+**: 0 collisions (very rare)
- **Memory**: ~2.7GB (Stage 1), ~17.5GB (full pipeline)

### Large Tests (100K-1M nonces)
- **Stage 1**: 150-3000 collisions
- **Stage 2+**: Cascade continues
- **Stage 7**: 0-10 solution candidates (depends on luck)
- **Memory**: ~17.5GB GPU required

### Solution Probability
- Zero-nheqminer: Finds ~1 solution per 19s with full 33M hash dataset
- Equihash 192,7: Approximately 1 solution per 1-2M nonces
- Our implementation: **Needs 1M+ nonces for reasonable solution rate**

---

## Known Issues

### Memory Constraints
- Full pipeline requires ~17.5GB GPU memory
- Intel iGPU (8GB) shows "No space left on device" errors
- **Solution**: Optimize NSLOTS (96→32) or sequential processing

### Collision Cascade
- With too few nonces, collisions die out after Stage 1-2
- Need large nonce counts to propagate through all 7 stages
- **Solution**: Test with 100K-1M nonces (requires memory optimization)

### GPU Driver Issues
- Beignet driver (Intel) shows occasional errors
- "Exec event error, type is 4592, error status is -5"
- Tests still complete successfully despite warnings

---

## Troubleshooting

### Test fails with "No space left on device"
- **Cause**: GPU memory exhausted (need <17.5GB available)
- **Fix**: Use smaller nonce counts or optimize NSLOTS in input.cl

### "0 collisions found" with large nonce count
- **Cause**: Kernel logic error or hash generation mismatch
- **Debug**: Compare with `cpu_tromp_baseline` using same header/nonce

### OpenCL compilation errors
- **Cause**: _kernel.h regeneration needed
- **Fix**: `make clean && make _kernel.h && make`

### Test hangs or crashes
- **Cause**: GPU driver instability or memory corruption
- **Fix**: Reset GPU driver or reboot system

---

## Next Steps

After validating all tests pass:
1. **Optimize memory**: Reduce NSLOTS or implement sequential processing
2. **Integrate**: Connect kernels to main.c mining loop
3. **Test large scale**: Run with 1M+ nonces to find actual solutions
4. **Pool test**: Submit solutions and verify acceptance

---

**Status**: All test programs functional, ready for optimization and integration
