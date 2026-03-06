# Zero Attr Bug Analysis

**Author**: Claude Opus 4.6 (GitHub Copilot)  
**Date**: March 6, 2026  
**Context**: Second opinion review of CLAUDE_SONNET_4.5.md (2,852 lines)

---

## Project Status Assessment

### Journey Summary
The project has gone through 3 major phases:
1. **Session 1**: Discovered silentarmy's 1-byte collision detection is architecturally incompatible with 192,7's 24-bit requirement
2. **Session 2**: Rewrote GPU stages using Tromp's algorithm - reached Stage 7 with 518 candidates, but all had duplicate indices
3. **Session 3**: Fixed attr encoding stage-by-stage (bucket+slot triplets), updated solution_extraction.c

### The Current Blocker: Zero Attr Bug
The latest diagnostic plan correctly identifies that most collision attrs are `0x00000000`. But the hypotheses (H1-H4) in CLAUDE_SONNET_4.5.md are looking in the wrong place. **The actual root cause is a macro redefinition conflict.**

---

## Critical Bug: Macro Redefinition Conflict

In `sa-tromp.c` (around line 30-44), the include order is:

```c
#define NBUCKETS (1<<BUCKBITS)  // 16,384 (16K)
#define NSLOTS 512
#define SLOTBITS 9

// ... then later ...
#include "solution_extraction.c"
```

But `solution_extraction.c` (lines 15-17) unconditionally redefines:

```c
#define NBUCKETS (1<<20)   // 1,048,576 (1M) ← OVERWRITES 16K!
#define NSLOTS 32           // ← OVERWRITES 512!
#define SLOTBITS 5          // ← OVERWRITES 9!
```

### Impact

After the include, ALL host-side code uses the WRONG constants:

| Constant | GPU Kernel (input.cl) | Host After Include | Mismatch |
|----------|----------------------|-------------------|----------|
| NBUCKETS | 16,384 | 1,048,576 | **64x wrong** |
| NSLOTS | 512 | 32 | **16x wrong** |
| SLOTBITS | 9 | 5 | Wrong shift |

### How This Causes the Zero Attr Symptom

When the debug code reads tree buffer slots:
```c
clEnqueueReadBuffer(queue, buf_tree1, CL_TRUE, 
    i * NSLOTS * sizeof(stage1_slot_t), ...)
    //    ↑ Uses NSLOTS=32 (host), but GPU wrote with stride of 512!
```

- GPU writes collision at buffer offset: `bucket * 512 * 25 bytes`
- Host reads from offset: `bucket * 32 * 26 bytes`
- **These addresses don't align** → host reads uninitialized memory → zero attrs

The one valid attr (`0x03422db5`) is just a lucky alignment where the strides happened to overlap.

**Solution extraction is also broken** for the same reason: `trees->trees0_stage2[parent0]` uses parent0 as a flat index, but the struct sizeof differs between host and kernel compilation contexts.

### Also: Hash Size Mismatch in Debug Struct

The debug code defines `hash[22]` (26-byte struct) while the kernel writes `hash[21]` (25-byte struct). This compounds the addressing error.

---

## Recommended Fix

1. **Remove the conflicting `#define`s from solution_extraction.c** - it should use the same constants as sa-tromp.c
2. **Use `#ifndef` guards** or pass constants from the including file
3. **Fix the debug struct** to use `hash[21]` matching the kernel

This is a ~10-line fix that could resolve the entire zero-attr problem and potentially the duplicate indices issue in one shot. The GPU kernels themselves appear correct - it's the host-side reading that's wrong.

---

## Overall Assessment

The algorithmic work is solid. The Tromp-based GPU rewrite (Phases 0-6a) is well-structured and the cascade reaches all 7 stages correctly. But the macro conflict has been silently corrupting all host-side buffer reads, making it impossible to get valid solution extraction. Fix the constants, and valid solutions may finally appear.
