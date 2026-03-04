# Claude Haiku 4.5 Action Plan - Blake2b Fix & Pool Solution

## Mission Objective
**Get pools to accept silentarmy192_7 miner's valid solutions**

## Root Cause IDENTIFIED (March 3-4, 2026)
- ✅ **Tromp tools verified working**: eq1927 generates valid solutions, verify1927 accepts them
- ✅ **Our verifier FAILS**: Rejects Tromp's valid solutions with "XOR byte 0 is d1 instead of 00" at r=1
- 🎯 **Bug identified**: `eh_genhash()` in main.c (line 891-909) uses wrong Blake2b implementation
- 🎯 **Impact**: Both CPU verification AND GPU solver affected (same bug)
- 📍 **Test case ready**: "zero" header with Tromp-generated solution for validation

## 3-Phase Execution Plan

### Phase 1: Fix CPU Verifier (PRIMARY - TODAY)
**Objective**: Make CPU verification accept Tromp's valid solutions  
**Time Target**: 1-2 hours  
**Blocker for**: Phase 2 (GPU fix)

**The Bug (main.c line 906)**:
```c
// BROKEN: zcash_blake2b_update expects pre-formatted 128-byte blocks
zcash_blake2b_update(&st, block, 4, 1);  // Manual padding doesn't match blake2b buffering
```

**The Fix**:
```c
// CORRECT: Use Tromp's blake2b with internal buffering
blake2b_update(&ctx, (const uint8_t *)&g, sizeof(g));  // Just 4 bytes
blake2b_final(&ctx, hash, sizeof(hash));
```

**Tasks**:
1. Replace `eh_genhash()` in main.c (line 891-909)
   - Remove: zcash_blake2b_update() GPU-centric calls
   - Add: Tromp's blake2b_update() + blake2b_final() from equihash_tromp/blake/blake2b.h
   - Update includes to access Tromp blake2b

2. Keep eh_verifyrec() and verify_equihash_full() unchanged (logic is correct)

3. Recompile test_verifier

4. **Validate**:
   ```
   ./test_verifier "zero" /tmp/tromp_indices.txt
   Expected: ✓ VERIFICATION PASSED
   Current: FAIL at r=1: XOR byte 0 is d1
   ```

**Success Criteria**: 
- ✅ test_verifier accepts Tromp's "zero" header solution
- ✅ No XOR errors in verification output

---

### Phase 2: Fix GPU Kernel (IF Phase 1 succeeds)
**Objective**: Make GPU solver generate valid solutions  
**Time Target**: 1-2 hours  

**Tasks**:
1. Find GPU Blake2b hash generation in input.cl
2. Fix to match Phase 1 CPU fix
3. Recompile and test

---

### Phase 3: End-to-End Pool Testing (IF Phase 1+2 succeed)
**Objective**: Submit valid solutions to pool  

---

## Why "zero" Header Test Works

Blake2b bug is systematic - affects ALL headers equally.
Fix for "zero" = fix for all headers.

---

## Status

**Date**: March 4, 2026  
**Ready**: Phase 1 implementation  

**Ready to fix eh_genhash()?**
