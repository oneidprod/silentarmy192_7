# Claude Haiku 4.5 Action Plan - Blake2b Fix & Pool Solution

##  Mission Objective
**Get pools to accept silentarmy192_7 miner's valid solutions**

## Phase 1: Fix CPU Verifier - DEBUGGING IN PROGRESS

**Status**: ✅ Partially working (error changed: d1 → 03, meaning blake2b is running differently)

### Discoveries
- ✅ zcash_blake2b_final() is BROKEN - just dumps st->h without finalization
- ✅ test_verifier compiles with Tromp's blake2b.cpp
- ✅ Personalization setup confirmed correct:
  - "ZERO_PoW" = 5a45524f5f506f57
  - N=192 LE = c0000000
  - K=7 LE = 07000000
- ⧗ But verification still fails: "XOR byte 0 is 03 (expected 00)"

### Current Investigation
- Created debug_blake2b.c to trace state changes:
  - Initial state after blake2b_init_param
  - State after header update
  - Hash output for test indices
- Error changed type (d1 vs 03) suggests we're calling right functions, wrong somewhere else

### Next Steps
1. ✅ Personalization confirmed - not the issue
2. ⏳ Check if header is being hashed with full verification flow
3. ⏳ Verify solution index parsing is correct
4. ⏳ Compare hash output byte-by-byte with Tromp for first few rounds

### Test Case
```
Header: 140 bytes (real blockchain data)
Solution: 7a4f 1eb1541 294426 13d3c47 1f0a41 14ab265... (128 indices from eq1927)
Expected: XOR should be zero at each round
Current: FAIL at r=1, XOR byte 0 = 03
```

###Session Progress
- Phase 1: 60% complete (Blake2b initialized, but verification still broken)
- Phase 2: Blocked until Phase 1 succeeds
- Phase 3: Blocked until Phase 2 succeeds

### Files Modified
- test_verifier.c: Now uses Tromp's blake2b
- debug_blake2b.c: New tool for tracing state
- main.c: Updated include to reference Tromp blake2b

### Key Finding
The fact that error changed from "d1" to "03" is actually good news - means we switched implementations. The wrong result suggests:
- NOT: Personalization (already verified correct)
- NOT: Header hashing (seems to work)
- MAYBE: Solution index interpretation? Or XOR calculation?
- MAYBE: State not fully propagating through verification tree?

**Ready to continue debugging or need user input?**
