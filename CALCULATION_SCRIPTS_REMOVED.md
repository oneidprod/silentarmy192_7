# CALCULATION SCRIPTS REMOVED - IMPORTANT WARNING

## Files Removed (February 19, 2026)
- `calculate_192_7_params.py` ❌ WRONG: Used 27-bit reduction (`N // K = 192 // 7 = 27`)
- `calculate_192_7_bits.py` ❌ WRONG: Also based on incorrect mathematical assumptions

## Why These Were Dangerous
These scripts contained **INCORRECT ALGORITHMIC ASSUMPTIONS** that led development down the wrong path:

### The Problem:
```python
bits_remaining = N - stage * (N // K)  # 192 // 7 = 27 ❌WRONG
```

### The Correct Algorithm (from xenoncat):
- **24-bit reduction per stage** (NOT 27-bit)
- Progression: 192→168→144→120→96→72→48→24
- Source: `../previous_attempts_info/ALGORITHM_REFERENCE.md`

## Source of Truth
**NEVER create calculation scripts again!** 

Use only: `../previous_attempts_info/ALGORITHM_REFERENCE.md`
This contains the original xenoncat documentation with verified correct values.

## Git History
These scripts were removed after discovery they caused major algorithmic errors.
The project was saved by reverting to documented xenoncat values (24-bit reduction).

## Warning for Future Developers
**DO NOT RE-CREATE CALCULATION SCRIPTS**
- They introduce errors
- Original documentation is authoritative
- Any "calculated" values should be cross-referenced with ALGORITHM_REFERENCE.md

The algorithm works when using the original documented values.
Trust the documentation, not derived calculations.
