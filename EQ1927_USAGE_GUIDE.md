# eq1927 & test_verifier Usage Guide

## Overview

`eq1927` is Tromp's reference Equihash 192,7 solver for Zero coin. `test_verifier` is our CPU-based solution verifier using Tromp's blake2b implementation.

## Key Parameters

### Equihash 192,7 Specifics
- **N = 192**: Digit size in bits
- **K = 7**: Number of rounds/digits
- **Personalization**: "ZERO_PoW" (8 bytes) + N in LE (4 bytes) + K in LE (4 bytes) = 16 bytes total
- **Solution size**: 128 indices (2^7), each 25 bits → stored as 32-bit values
- **Header format**: 140 bytes (HEADERNONCELEN)
- **Nonce location**: Bytes 108-111 (u32 index [27]) in little-endian

## Using eq1927 Solver

### 1. Generate Solutions for Empty Header
```bash
./equihash_tromp/eq1927 -s -p "ZERO_PoW" -n 0
```

**Flags:**
- `-s`: Show solutions (required to see output)
- `-p "ZERO_PoW"`: Personalization string (for Zero coin)
- `-n 0`: Nonce value (default 0)
- `-h ""`: Header string (default empty, creates 140-byte zero header)

**Output format:**
```
Solution <128 hex indices separated by spaces>
```

### 2. Generate Solutions for Custom Header
```bash
./equihash_tromp/eq1927 -s -x "040000003dd4a..." -p "ZERO_PoW" -n 0
```

**Important flags:**
- `-x "hexstring"`: Pass header as hex string (must be exactly 280 hex chars = 140 bytes)
- When using `-x`, nonce still goes at bytes 108-111, **which gets overwritten**!

**Critical Issue**: If your header already has a non-zero nonce and you generate with `-n 0`, eq1927 will overwrite it:
```
Original header bytes 108-111: c0020d00  (some nonce value)
eq1927 with -n 0:             00000000  (nonce gets set to 0)
```

### 3. Nonce Handling

The nonce parameter `-n N` sets bytes 108-111 of the 140-byte header:

```c
// From eq1927 source:
((u32 *)headernonce)[27] = htole32(nonce+r);  // Index 27 = bytes 108-111
```

**When generating solutions:**
1. If using `-h ""` (empty header): Nonce is set at bytes 108-111
2. If using `-x "hex_header"`: Nonce **overwrites** bytes 108-111 regardless of input
3. Solutions are only valid for the exact nonce used during generation

### 4. Multiple Solutions
eq1927 finds **all valid solutions** for the given header+nonce combination:

```bash
./equihash_tromp/eq1927 -s -p "ZERO_PoW" -n 0 2>&1 | grep "^Solution"
```

Output shows: `N solutions` + `N total solutions`

**Example output:**
```
2 solutions
2 total solutions
```

Solutions appear in order as `Solution <indices1>` and `Solution <indices2>`.

## Verifying Solutions

### 1. Prepare Solution File

Extract solution indices from eq1927 output into a file:

```bash
./equihash_tromp/eq1927 -s -p "ZERO_PoW" -n 0 2>&1 | \
  grep "^Solution" | head -1 | sed 's/Solution //' > solution.txt
```

This creates a file with 128 hex indices separated by spaces.

### 2. Run test_verifier

```bash
./test_verifier "<140-byte header in hex>" solution.txt
```

**Requirements:**
- Header must be exactly 280 hex characters (140 bytes)
- Solution file must contain 128 hex indices separated by whitespace
- Header nonce (bytes 108-111) must match the nonce used when generating the solution

### 3. Verification Output

**Success:**
```
✓ VERIFICATION PASSED - Our verifier accepts the reference solution!
This means our verification code is CORRECT.
The problem is in GPU candidate extraction.
```

**Failure:**
```
✗ VERIFICATION FAILED - Our verifier rejects the reference solution!
This means our verification code has a BUG.
```

If verification fails, check:
1. Header is exactly 280 hex chars
2. Nonce at bytes 108-111 matches generation nonce
3. All 128 indices present in solution file

## End-to-End Workflow

### Workflow 1: Empty Header (Easiest)
```bash
# Generate solution for empty header with nonce=0
./equihash_tromp/eq1927 -s -p "ZERO_PoW" -n 0 2>&1 | \
  grep "^Solution" | head -1 | sed 's/Solution //' > /tmp/sol.txt

# Verify it
./test_verifier "$(printf '%280s' | tr ' ' '0')" /tmp/sol.txt
```

### Workflow 2: Custom Header
```bash
# 1. Generate solution for specific header
HEADER="040000003dd4a53440b11dde6a279a5318b2cc..."  # 280 hex chars
./equihash_tromp/eq1927 -s -x "$HEADER" -p "ZERO_PoW" -n 0 2>&1 | \
  grep "^Solution" | head -1 | sed 's/Solution //' > /tmp/sol.txt

# 2. Fix nonce in header to match generation (-n 0 means bytes 108-111 = 00000000)
HEADER_NONCE_ZERO="${HEADER:0:216}00000000${HEADER:224}"

# 3. Verify with corrected header
./test_verifier "$HEADER_NONCE_ZERO" /tmp/sol.txt
```

### Workflow 3: Real Blockchain Header
```bash
# 1. Get real header from blockchain (bytes 0-139 of block)
REAL_HEADER="040000003dd4a53440b11dde6a279a53..."  # Real nonce might be non-zero

# 2. Generate solution (eq1927 will overwrite nonce to match -n parameter)
./equihash_tromp/eq1927 -s -x "$REAL_HEADER" -p "ZERO_PoW" -n 0 2>&1 | \
  grep "^Solution" | head -1 | sed 's/Solution //' > /tmp/sol.txt

# 3. Fix header nonce to match generation (-n 0)
HEADER_FOR_VERIFY="${REAL_HEADER:0:216}00000000${REAL_HEADER:224}"

# 4. Verify
./test_verifier "$HEADER_FOR_VERIFY" /tmp/sol.txt
```

## Compilation

### test_verifier
```bash
gcc -o test_verifier test_verifier.c \
  equihash_tromp/blake/blake2b.cpp \
  -DWN=192 -DWK=7 \
  -I. -I equihash_tromp -I equihash_tromp/blake \
  -lstdc++ -lm
```

**Critical flags:**
- `-DWN=192 -DWK=7`: **MUST** set correct parameters for Zero coin (not default 200,9 for Zcash)
- `-lstdc++`: Link C++ standard library (for blake2b.cpp)
- `-lm`: Math library

### eq1927 (Pre-compiled)
Usually comes pre-built at `./equihash_tromp/eq1927`. If rebuilding:
```bash
cd equihash_tromp
make eq1927 WN=192 WK=7
```

## Troubleshooting

### Verification fails with XOR error
```
FAIL: XOR byte 0 is 03 (expected 00)
```

**Cause**: Header nonce doesn't match solution generation nonce

**Solution**:
1. Check what nonce was used with eq1927 (usually -n 0)
2. Set those bytes in the header before verification
3. For nonce=0: Replace bytes 108-111 with `00000000`

### eq1927 says "2 total solutions" but none show
```
2 solutions
```

**Cause**: Missing `-s` flag to show solutions

**Solution**: Add `-s` flag to display solution indices

### "can't find eq1927" or compilation errors
```
./equihash_tromp/eq1927: command not found
```

**Solution**:
```bash
cd equihash_tromp
make clean
make eq1927 WN=192 WK=7
cd ..
```

### Header format errors
```
Header must be 280 hex chars (got 278)
```

**Cause**: Header not exactly 140 bytes

**Solution**: Verify header is 280 hex characters:
```bash
echo -n "yourheader" | wc -c  # Should be 280
```

## Performance Notes

- **Single-threaded**: eq1927 default is slow (-t 1)
- **Multi-threaded**: Add `-t N` for N threads (faster but uses more memory)
- **Empty vs Real headers**: Take similar time to solve
- **test_verifier**: Instant verification (<100ms)

## Index Format

Solutions are stored as **128 hex values** (one per index), each representing a 25-bit index:

```
Index 1:  7a4f   (0x7a4f = 31311)
Index 2:  1eb1541 (0x1eb1541 = 32183617)
...
Index 128: 1dbd250 (0x1dbd250 = 31324752)
```

Maximum valid index: 2^25 - 1 = 33554431 = 0x1FFFFFF

## Summary Table

| Operation | Command | Output | Notes |
|-----------|---------|--------|-------|
| Generate (empty) | `eq1927 -s -p "ZERO_PoW" -n 0` | Solution indices | Very fast (seconds) |
| Generate (real) | `eq1927 -s -x "header_hex" -p "ZERO_PoW" -n 0` | Solution indices | Nonce gets overwritten |
| Verify | `test_verifier "header_hex" solution.txt` | PASSED/FAILED | Must compile with WN=192 WK=7 |
| Fix nonce | Bash string ops | Fixed header | Bytes 108-111 to match nonce |

## References

- **Tromp's Equihash**: https://github.com/tromp/equihash
- **Zero Coin**: Uses Equihash 192,7 with personalization "ZERO_PoW"
- **Blake2b**: Used for initial hash, then Wagner tree algorithm
