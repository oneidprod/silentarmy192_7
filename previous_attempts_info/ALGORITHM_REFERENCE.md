# Algorithm Reference Notes

## Important Documentation Location

**Equihash 200,9 Algorithm Description**: `/home/griffithm/builds/equihash-xenon/notes/`

This folder contains the original xenoncat algorithm documentation that explains:
- Why specific STATE_DEST values are chosen (memory layout strategy)
- Memory overlap/reuse patterns between stages
- Bucket and partition logic
- Pairs compression scheme
- Expected memory calculations

**Key files:**
- `algorithm description.txt` - Full text documentation
- `algorithm description.pdf` - Original PDF
- `memory.csv` - Memory layout calculations
- `buckets.csv` - Bucket distribution details
- `bits.csv` - Bit allocation per stage

## Memory Layout Strategy (200,9)

From the documentation, **14 units of memory** are needed for Pairs and Xorwork in Equihash 200,9:
- One unit = 2965504 items × 4 bytes = 11,862,016 bytes
- Total: 15 × 11,862,016 = ~178MB (includes basemap)

**Key insight from documentation:**
> "Pairs from each stage need to be preserved. Xorwork from previous stages can be overwritten."

This explains the STATE_DEST overlap pattern in working 200,9:
```
STATE_DEST values: 8,2,9,4,9,6,10,8,10 * BLOCKUNIT
```

Notice STATE2_DEST = STATE4_DEST = 9*BLOCKUNIT - this is intentional memory reuse because:
- Stage 2 writes Xorwork2 to position 9
- Stage 4 can reuse position 9 because Xorwork2 is no longer needed
- Pairs arrays are preserved separately at different positions

## State Size Table (200,9)

| State | Bits in Xorwork | Bytes (rounded) | Bytes (aligned to 4) | Memory Units |
|-------|----------------|-----------------|---------------------|--------------|
| 0     | 192            | 24             | 24                  | 6            |
| 1     | 172            | 22             | 24                  | 6            |
| 2     | 152            | 19             | 20                  | 5            |
| 3     | 132            | 17             | 20                  | 5            |
| 4     | 112            | 14             | 16                  | 4            |
| 5     | 92             | 12             | 12                  | 3            |
| 6     | 72             | 9              | 12                  | 3            |
| 7     | 52             | 7              | 8                   | 2            |
| 8     | 32             | 4              | 4                   | 1            |

Memory units calculated as: `ceil(aligned_bytes / 4)` since one unit = one Pairs array worth of space

## Applying to 192,7

For **Equihash 192,7**, we have **8 stages** (Blake2b + 7 collision stages):

| State | Bits in Xorwork | Bytes (rounded) | Bytes (aligned to 4) | Expected Memory Units |
|-------|----------------|-----------------|---------------------|----------------------|
| 0     | 192            | 24             | 24                  | 6                    |
| 1     | 168            | 21             | 24                  | 6                    |
| 2     | 144            | 18             | 20                  | 5                    |
| 3     | 120            | 15             | 16                  | 4                    |
| 4     | 96             | 12             | 12                  | 3                    |
| 5     | 72             | 9              | 12                  | 3                    |
| 6     | 48             | 6              | 8                   | 2                    |
| 7     | 24             | 3              | 4                   | 1                    |

**Key differences from 200,9:**
- 24 bits removed per stage (192/8 = 24) vs 20 bits (200/10 = 20)
- 8 stages instead of 9
- Estimated total: ~12 units for Pairs/Xorwork + 1 for basemap = ~13 units
- Context size: 13 × 11,890,688 = ~147MB ✅ (matches our xenoncat.cpp setting)

## Deriving STATE_DEST Pattern for 192,7

Based on the memory reuse strategy documented for 200,9, the STATE_DEST values should:
1. Preserve all Pairs arrays (one per stage)
2. Allow Xorwork memory to be reused once consumed
3. Fit within allocated `.pairs` (8 units) and `.buf` (5 units) struct space

The 200,9 pattern `8,2,9,4,9,6,10,8,10` suggests stages are **not written sequentially** to enable maximum memory reuse.

### Working Solution for 192,7 ✅

```nasm
STATE0_DEST = 7 * BLOCKUNIT   ; .pairs + 7 units (6 unit Xorwork overlaps into .buf)
STATE1_DEST = 1 * BLOCKUNIT   ; .pairs + 1 units
STATE2_DEST = 8 * BLOCKUNIT   ; .buf + 0 units (reusable)
STATE3_DEST = 3 * BLOCKUNIT   ; .pairs + 3 units
STATE4_DEST = 8 * BLOCKUNIT   ; .buf + 0 units (REUSE STATE2 space!)
STATE5_DEST = 5 * BLOCKUNIT   ; .pairs + 5 units
STATE6_DEST = 9 * BLOCKUNIT   ; .buf + 1 units
STATE7_DEST = 7 * BLOCKUNIT   ; .pairs + 7 units (REUSE STATE0 space!)
```

**Key Insights:**
- Positions 0-7: within `.pairs` struct (preserved)
- Positions 8+: within `.buf` struct (reusable Xorwork space)
- STATE2_DEST = STATE4_DEST = 8 (memory reuse after STATE2 consumed)
- STATE0_DEST = STATE7_DEST = 7 (reuse after long delay)
- Maximum .buf usage: units 0-4 (5 units needed)

**Performance Results:**
- **xenoncat AVX2 (192,7)**: 7.3 Sols/s
- **cpu_tromp (192,7)**: 2.35 Sols/s
- **Speedup**: ~3x faster with optimized xenoncat solver

**Status**: ✅ **WORKING** - No crashes, proper solution validation
