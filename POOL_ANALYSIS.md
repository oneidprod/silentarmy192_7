# Pool Compatibility Analysis - silentarmy192_7
**Date**: 2026-03-03  
**Status**: Error 20 "invalid solution" - Under Investigation

## Problem Statement
Solver produces valid Equihash 192,7 solutions locally, but zeropool.io rejects all submissions with:
```
Stratum server returned an error: [20, 'invalid solution']
```

## Findings So Far

### ✅ Verified Correct
1. **Solution Encoding**: 400 bytes (128 indices × 25 bits)
   - Format: fd9001 (compact size) + 800 hex chars
   - Encoding function `store_encoded_sol()` uses correct 25-bit packing
   - Total stratum submission: 806 characters

2. **Algorithm**: Blake2b with "ZERO_PoW" personalization
   - Confirmed from Zero coin source: `/home/mine/Zero/src/crypto/equihash.cpp` line 39
   - Parameters: N=192, K=7 (verified in chainparams.cpp)

3. **Stratum Protocol**: 
   - Submission format: `[worker, job_id, ntime, nonce, solution]`
   - All parameters correctly extracted and submitted
   - No race conditions (error 25 fixed previously)

4. **Local Solver**: Produces multiple valid solutions per nonce
   - Example from test: Found 2000 potential solutions, many passed local validation
   - Solutions follow Equihash 192,7 structure correctly

### ❓  Potential Issues

#### 1. Pool Personalization String Configuration (MOST LIKELY)  
**Evidence**:
- `algoProperties.js` (stratum library) defaults to "ZcashPoW" for equihash
- Zero coin uses "ZERO_PoW" (8 bytes) confirmed from source
- Pool must be explicitly configured with correct personalization

**Test Location**: `/home/mine/stratum-lib/lib/algoProperties.js` lines 28-53

**Pool Config Required**:
```javascript
parameters: {
    N: 192,
    K: 7,
    personalization: 'ZERO_PoW'  // NOT 'ZcashPoW'
}
```

#### 2. Solution Prefix Ambiguity
**Evidence**:
- Some pools expect solution WITHfd9001 prefix (our current format)
- Other pools expect solution WITHOUT prefix (pool adds it)
- Tested removing prefix - inconclusive (connection issues)

**Current Format**: `fd900100002c...` (806 chars total)  
**Alternative Format**: `00002c...` (800 chars, no prefix)

#### 3. Block Header Construction
**Requires Verification**:
- Zero uses 140-byte header (standard)
- Nonce: 32 bytes (uint256) - correct for Zero
- CEquihashInput excludes nonce and solution from Blake2b hash
- Our miner constructs header correctly (needs cross-verification)

### 🔍 Next Steps (In Order)

1. **Verify Solution Locally Against Zero Node** (CRITICAL)
   - Build Zero coin locally if not already available
   - Use Zero's `CheckEquihashSolution()` to validate our solutions
   - If this passes → problem is pool configuration
   - If this fails → problem is our solution generation

2. **Test Different Pools**
   - Try another Zero coin pool (if available)
   - Compare error messages across pools
   - Document which pools work/don't work

3. **Contact Pool Operator**
   - If local verification passes, contact zeropool.io
   - Verify their N/K/personalization configuration
   - Check if they support Equihash 192,7

4. **Test Prefix Variations** (If Above Fails)
   - Create test branch with prefix variations
   - Test both fd9001+solution and raw solution formats
   - Document pool responses for each

### 📊 Test Log Analysis

**File**: `zeropool_current_test.log`  
**Timestamp**: 2026-03-03 11:30

Key Observations:
- Connection established successfully ✓
- Target received: `00a0000000...` ✓  
- Job received: `"f8f5"` ✓
- 6 solutions submitted, ALL rejected with error 20 ✗

Sample submission:
```
DEBUG: job_id=f8f5, ntime=7c1aa769, nonce=0a3156f2a1493306d45e2b1c11690282000000000000000000000000
DEBUG: sol length=806, sol prefix=fd900100002c...
Result: Stratum server returned an error: [20, 'invalid solution']
```

### 🛠️ Code Locations

**Solution Encoding**:
- `main.c:606` - `store_encoded_sol()` function
- `main.c:735` - Blake2b solution output in `print_solver_line_blake2b()`
- `param.h:63` - ZCASH_SOLSIZE_HEX = "fd9001"
- `param.h:65` - ZCASH_SOL_LEN calculation

**Stratum Submission**:
- `silentarmy:368` - mining.submit call
- `silentarmy:81` - Solution regex parser
- `silentarmy:484` - Stratum message construction

**Zero Verification (Reference)**:
- `~/Zero/src/pow.cpp:CheckEquihashSolution()` - Entry point
- `~/Zero/src/crypto/equihash.h:#define EhIsValidSolution` - Macro
- `~/Zero/src/crypto/equihash.cpp:GetMinimalFromIndices()` - Format reference

### 💡 Working Theory

**Most Probable Cause**: Pool misconfiguration

The pool (zeropool.io) is likely:
1. Using default "ZcashPoW" personalization instead of "ZERO_PoW"
2. Configured for Equihash 200,9 instead of 192,7
3. Not properly initialized for Zero coin parameters

**Evidence Supporting This**:
- Our solutions are correctly formatted (806 chars, proper structure)
- Local solver finds many valid solutions consistently
- Zero coin source confirms our algorithm choices are correct
- Generic stratum library defaults don't match Zero's requirements

**Next Critical Test**: Validate solutions locally using Zero coin's verification code to prove they are mathematically valid. If they pass local verification, the issue is definitively on the pool side.

---

**Time Spent**: ~35 minutes (Phase 1-3 analysis)  
**Commits**: 0 (pending completion of local verification test)
