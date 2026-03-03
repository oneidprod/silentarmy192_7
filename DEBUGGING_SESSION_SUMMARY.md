# Equihash 192,7 Debugging Session Summary

## Problem Statement

The silentarmy192_7 Equihash miner was experiencing critical issues:
- **clCreateBuffer (-37) errors** - OpenCL buffer allocation failures
- **Zero solutions found** - Miner runs but finds no valid solutions
- **Corrupted build state** - Previous session left files in broken state

## Root Cause Analysis Approach

### Phase 1: Build Restoration & Basic Fixes
- **Issue**: Kernel/host function signature mismatches from previous corruption
- **Solution**: Restored clean build state, fixed clCreateBuffer argument checking
- **Tools Added**: Defensive error checking in `check_clCreateBuffer()`

### Phase 2: Debug Infrastructure Implementation
- **Added**: Comprehensive extraction debug buffers (`extraction_debug_t`)
- **Added**: Per-round collision/storage counters (`round_stored[8]`, `round_collisions[8]`)
- **Purpose**: Capture kernel execution state to understand solution flow

### Phase 3: Round Count Correction (Critical Fix)
- **Discovery**: Code was using 7 rounds instead of required 8 rounds for Equihash (192,7)
- **Mathematical Rule**: Equihash uses k+1 rounds, so k=7 requires 8 rounds (0-7)
- **Changes Made**:
  - Updated input.cl to generate `kernel_round7`
  - Modified main.c to allocate and run rounds 0..PARAM_K (0..7)
  - Fixed `potential_sol()` to start from round PARAM_K
  - Updated per-round buffer sizing to `(PARAM_K + 1)` entries

### Phase 4: Crash Prevention
- **Issue**: Segmentation faults during debug dumps
- **Solution**: Added comprehensive signal handlers and bounds checking
- **Added**: SIGSEGV/SIGABRT handler with backtrace output
- **Added**: Defensive bounds checking in HT row dump code

### Phase 5: Deep Kernel Analysis
- **Observation**: Round 0 stores ~8M entries, rounds 1+ store very few or zero
- **Investigation**: Implemented detailed per-store status logging
- **Added**: `table_half`, `xi_sig`, `stored0-3` fields to `extraction_debug_t`
- **Created**: Parsing tools (`parse_dbg.py`, `compare_dbg_raw.py`) for correlation

## Key Technical Discoveries

### Memory Analysis
- **HT Size**: 536.9 MB total (2 × 256 MB hash tables)
- **GPU VRAM**: ~3.9GB and ~6.2GB available on test devices
- **Conclusion**: Memory exhaustion ruled out as root cause

### Round Logic Validation
- **Confirmed**: 8-round logic correctly implemented after fixes
- **Validation**: Deterministic kernel self-tests prove offset/endianness correct
- **Issue**: Collision candidates still not surviving round transitions

### Extraction Debug Correlation
- **Challenge**: Kernel `stored0-3` values don't match host HT dumps
- **Cause**: Timing differences - kernel logs at insert time, host dumps later
- **Approach**: Implemented per-insert snapshot buffer for real-time capture

## Code Changes Summary

### Core Files Modified

#### input.cl (OpenCL Kernel)
- Added `kernel_round7` for 8th round
- Implemented `extraction_debug_t` with `stored0-3`, `table_half`, `xi_sig`
- Added per-store status logging (xor_zero, store_overflow, store_success)
- Added deterministic self-test pattern writing
- Added insertion snapshot buffer capture

#### main.c (Host Code)
- Extended round loop: `for (round = 0; round <= PARAM_K; round++)`
- Added snapshot buffer allocation and management
- Implemented crash handlers with backtrace
- Added defensive bounds checking in HT dumps
- Fixed solution size encoding (400 bytes → `fd 90 01`)

#### param.h
- Confirmed `PARAM_K = 7` for Equihash (192,7)
- `NR_ROWS_LOG = 18` (262k rows), `NR_SLOTS = 32`

### Tool Files Created

#### tools/parse_dbg.py
- Parses `extraction_dbg` entries from solver output
- Correlates kernel debug data with host HT dumps
- Filters zero-valued stored entries

#### tools/compare_dbg_raw.py
- Compares kernel `stored0-3` values with host `rawslot` bytes
- Handles xi_offset spanning across slot boundaries
- Implements multiple matching heuristics (row%NR_ROWS, table_half variations)

#### tools/calc_ht_memory.py
- Calculates required GPU memory for given parameters
- Compares against available device VRAM
- Suggests parameter reductions if needed

## Current Status

### What's Working
- ✅ Build system restored and stable
- ✅ 8-round kernel execution (0-7)
- ✅ No segmentation faults
- ✅ Debug infrastructure captures kernel state
- ✅ Memory allocation sufficient for current parameters

### What's Still Broken
- ❌ **Zero solutions found** - Main issue persists
- ❌ **Collision candidates lost between rounds** - Round 0 stores ~8M, rounds 1+ store ~0
- ❌ **Kernel-host debug correlation** - stored0-3 don't match rawslot bytes due to timing

### Latest Investigation Status
- **Snapshot Buffer**: Started implementing per-insert 32-byte snapshot capture
- **Correlation**: 1/3781 DBG entries match host dumps (mostly timing mismatches)
- **Next**: Complete snapshot buffer to get real-time kernel write data

## Debugging Tools Chain

```
solver run → extraction_dbg → parse_dbg.py → correlation analysis
     ↓              ↓              ↓               ↓
  kernel logs → host dumps → compare_dbg_raw.py → mismatch report
```

## Next Steps Priority

1. **Complete snapshot buffer implementation** - Get real-time kernel write capture
2. **Analyze why round 1+ get zero stored entries** - Find collision filtering issue  
3. **Verify xi_offset calculations** - Ensure proper bit field extraction per round
4. **Check collision detection masks** - Verify NR_ROWS_LOG/masking logic
5. **Test with non-zero headers** - Current tests use all-zero (degenerate case)

## Files of Interest

### Critical Code Paths
- `equihash_round()` - Round execution logic
- `ht_store()` - Hash table insertion with overflow handling
- `xor_and_store()` - Collision detection and forwarding
- `potential_sol()` - Solution reconstruction and validation

### Debug Outputs
- `diag_persist_test*.txt` - Diagnostic runs with deterministic patterns
- `compare_persist_out*.txt` - Correlation analysis results
- `parsed_*.txt` - Structured debug data extracts

## Historical Context

- **Previous Model**: Left codebase in corrupted state with kernel/host mismatches
- **nheqminer Reference**: /home/mine/silentarmy192_7/nheqminer-C-192_7-zero shows similar fixes
- **Solution Format**: Fixed from invalid to proper 400-byte compact encoding
- **Round Logic**: Aligned with mathematical requirements (k+1 rounds for Equihash)

---

**Last Updated**: Current debugging session (continuing from collision detection analysis)  
**Status**: In progress - investigating why collisions don't propagate past round 0  
**Commit**: Latest changes in master branch include 8-round support and crash prevention