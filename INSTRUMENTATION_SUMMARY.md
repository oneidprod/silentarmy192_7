# SilentArmy 192,7 Instrumentation & Diagnostic Summary

Date: 2026-02-25

This document captures the instrumentation work, observations, and next actions taken to debug and adapt the SilentArmy solver for Equihash 192,7 (Tromp-compatible layout). It summarizes the kernel- and host-side changes, runtime diagnostics added, and the current findings.

## 1. Goals
- Port/adapt the SilentArmy solver for Equihash 192,7 producing Tromp-compatible, pool-submittable solutions.
- Add kernel-level diagnostics to record per-insert events (round/thread/row/slot/Xi) and per-round counters to determine where candidate pairs are lost during reduction rounds.

## 2. What was added

- Kernel-side diagnostics (in `input.cl`):
  - `#define DEBUG_EXTRACTION` enabled.
  - `extraction_debug_t` struct (fields: `round`, `thread_id`, `row`, `slot`, `xi0..xi3`, `status`).
  - `EXTRACTION_DEBUG_ENTRIES` (circular sample buffer; default 4096).
  - Writes to `extraction_dbg` at key points in `xor_and_store` and `ht_store` with `status` values:
    - `1` = xor_nonzero (pair XOR produced a non-zero Xi)
    - `2` = stored (successful store into destination HT)
    - `3` = overflow (row overflow prevented store)
  - Macros to pass debug and per-round counter buffers to hash-table helpers: `HT_DBG_ARGS/HT_DBG_PASS`, `ROUND_CNT_ARGS/ROUND_CNT_PASS`.
  - Atomic per-round sentinel increment in `equihash_round` to verify kernel execution and help detect ABI/arg-binding issues.

- Host-side additions (in `main.c`):
  - Matching `extraction_debug_t` typedef and allocation of device buffers:
    - `buf_extraction_dbg`, `buf_extraction_dbg_counter`
    - `buf_round_collisions`, `buf_round_stored`, `buf_potential_cnt`
  - Fixed `clSetKernelArg` ordering to match the generated kernel signatures (critical: host arg order is brittle).
  - Readback and printing of:
    - `extraction_dbg_counter`
    - per-round `round_collisions[]` and `round_stored[]`
    - samples from `extraction_dbg[]` including `status` and Xi words
  - Row reconstruction checks in host to validate xi->row mapping for sampled entries.

## 3. Key implementation details

- Hash table layout and reductions follow Equihash 192,7 layout (round-specific Xi lengths, slot offsets, NR_ROWS_LOG dependent encoding).
- `ht_store` performs the 16-bit rotation across ulong words before writing Xi bytes into the slot at `p + xi_offset_for_round(round)`; it uses packed `rowCounters` with atomic_add and bitfield extraction to detect and increment per-slot counts.
- `xor_and_store` reads Xi values from previous-round table, XORs the appropriate bytes for the round, and calls `ht_store` to insert the resulting Xi and encoded input pair.

## 4. Runtime observations (after instrumentation)

- Build / runtime fixes
  - Initial runs failed because host `clSetKernelArg` did not match generated kernel signatures in `_kernel.h`. After fixing the ordering, kernel sentinel increments and debug writes became visible.
  - A stray/incorrectly placed debug block introduced a kernel compile error; it was removed and braces fixed.

- Diagnostics produced
  - `extraction_dbg_counter` values observed non-zero (examples: 16,777,216; 98,533,262; 8,388,608 depending on run).
  - Per-round counters show a strong concentration of stored events in round 0, while rounds 1..6 show stored ≈ 0 in many runs.
  - Sampled `extraction_dbg` entries are mostly `round=0`, `status=2` (stored) and contain plausible Xi hex words. Host-side row reconstruction matched recorded rows for sampled round-0 entries via the