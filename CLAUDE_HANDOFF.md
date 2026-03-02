CLAUDE HANDOFF
===============

Purpose
-------
This file hands off the current diagnostic state for `silentarmy192_7` to a new engineer (Claude). It contains the changes made, how to reproduce the builds/runs, where diagnostic outputs live, and suggested next steps.

Author / Context
----------------
- Prepared by: GitHub Copilot (assistant)
- Model: GPT-5 mini
- Prepared on: 2026-03-02 UTC
- Branch: `diag-plan` (latest diagnostic commits pushed)
- Note: I performed kernel+host instrumentation, ran short diagnostics, produced binary dumps (`snapshots_*.bin`, `extraction_dbg_*.bin`, `row_*.bin`) and added parsing/aggregation tools in `tools/`.

Workspace & branch
-------------------
- Repository: /home/mine/silentarmy192_7
- Active branch used during diagnostics: `diag-plan` (several commits with "diag:" messages)

Key files changed
-----------------
- `input.cl`
  - Added `DEBUG_EXTRACTION` instrumentation (extraction_debug entries).
  - Standardized per-snapshot layout to 8 × uint64 (stored0..3, thread_id, table_half, marker, seq).
  - Wrote deterministic self-test that thread 0 writes a known 32-byte pattern into row 0 slot 0 and logs it into `extraction_dbg[0]` (SNAP[0] verifies host readback path).
  - Added multiple diagnostic snapshot write paths:
    - per-insert snapshot writes indexed by `extraction_dbg` index (1:1 mapping) to avoid relying on `snapshot_counter` atomics.
    - (temporarily) `DEBUG_FORCE_SNAPSHOT` block to force each work-item to emit a snapshot for testing (enabled then reverted).

- `main.c`
  - Allocates and passes `buf_snapshots`, `buf_snapshot_counter`, `buf_snapshot_seq` to kernels.
  - Resets snapshot/extraction counters before kernel execution, does `clFinish`/readbacks, prints small `SNAP[...]` console lines for the first 64 snapshots, and writes full binary dumps:
    - `snapshots_<ts>.bin` (raw snapshot buffer, entries × 8 × 8 bytes)
    - `extraction_dbg_<ts>.bin` (extraction debug entries)
  - Writes HT row dumps for top sampled rows: `row_<row>_round<r>.bin` for offline inspection.

- `tools/` (Python):
  - `aggregate_snapshots.py`: aggregate histograms, correlate snapshots and extraction debug entries.
  - `compare_dbg_to_snap.py`: parse console SNAP lines and compare with extraction debug.
  - `match_exdbg_to_row.py`, `search_stored_in_row.py`: search stored 32-byte patterns from `extraction_dbg` against HT row dumps.

Important constants/layout
-------------------------
- `EXTRACTION_DEBUG_ENTRIES` = 4096 (extraction debug buffer size)
- `SNAPSHOT_ENTRIES` (host) = 65536 (snapshot buffer allocation; effective per-run counts vary)
- Snapshot layout (8 × uint64):
  0..3: stored0..stored3 (first 32 bytes of stored slot)
  4: thread_id
  5: table_half
  6: marker (deterministic: (thread_id << 32) | idx or sidx)
  7: seq (monotonic device-side atomic when available)

How to reproduce (build + diag run)
-----------------------------------
1. Build:

```bash
make -j2
```

2. Run one-nonce diagnostic (fast):

```bash
./sa-solver --diag -i "$HEADERHEX" --nonces 1 > diag_out.txt 2>&1
grep -n "snapshot_counter =\|snapshot_seq_counter =\|extraction_dbg_counter =\|SNAP\[" diag_out.txt || true
```

3. Normal solver run (non-diagnostic) for single nonce:

```bash
./sa-solver -i "$HEADERHEX" --nonces 1 > run_one_nonce.txt 2>&1
sed -n '1,200p' run_one_nonce.txt
```

Files produced by diagnostics
-----------------------------
- `snapshots_<ts>.bin` — raw snapshot buffer dump (entries × 8 × 8 bytes)
- `extraction_dbg_<ts>.bin` — extraction debug binary dump
- `row_<row>_round<r>.bin` — HT row raw slot dumps (round 0 and 1)
- Console outputs: `diag_out_*.txt`, `run_one_nonce.txt` contain counters and `SNAP[...]` printed lines

Tools and commands used
-----------------------
- Aggregate snapshots:
  python3 tools/aggregate_snapshots.py snapshots_<ts>.bin extraction_dbg_<ts>.bin > aggregate.txt
- Search stored 32-byte patterns inside an HT row dump:
  python3 tools/search_stored_in_row.py extraction_dbg_<ts>.bin row_<row>_round<r>.bin
- Match extraction debug entries to host row bytes with different heuristics:
  python3 tools/match_exdbg_to_row.py <args>

What was observed
-----------------
- Deterministic self-test (`SNAP[0]`) consistently present — confirms host readback and offsets for that test case.
- In earlier runs snapshot_counter often stayed at 1 and most `SNAP[...]` were zero (only deterministic entry present).
- Added `DEBUG_FORCE_SNAPSHOT` to force per-work-item snapshots; this showed atomics and snapshot writes can work on this system but the forced-flag was reverted.
- Added per-insert snapshot writes at extraction_dbg index so each stored-event has a 1:1 snapshot entry (less reliant on `snapshot_counter`). After this change, `snapshots_*.bin` and `extraction_dbg_*.bin` showed many matching stored entries and `tools/search_stored_in_row.py` found matches in `row_0_round0.bin` and `row_0_round1.bin` for many extraction entries.
- Current normal solver run (single nonce) produced 0 solutions (as expected for a single nonce test), but extraction debug + snapshots + row dumps are present for correlation.

Known open issues / hypotheses
----------------------------
- `snapshot_counter` often reads as `1` in normal runs (meaning only deterministic snapshot used the `snapshot_counter` atomic). Hypotheses:
  - Driver/OpenCL atomic semantics might not support the `atomic_inc` pattern used for `snapshot_counter` in some kernels/rounds.
  - Kernel arguments ordering mismatch in earlier iterations (was fixed for final round), ensure all kernels get the same arg ordering.
  - Race/overflow of counters if `atomic_inc` wraps (we saw large `extraction_dbg_counter` values in some runs).
- Implemented mitigation: write per-insert snapshots at `extraction_dbg` index so stored events map 1:1 to snapshot entries.

Recent commits and branch
-------------------------
- Branch: `diag-plan`
- Recent diag commits (examples):
  - "diag: enable DEBUG_FORCE_SNAPSHOT (force per-work-item snapshots)"
  - "diag: disable DEBUG_FORCE_SNAPSHOT (restore normal snapshot behaviour)"
  - "diag: record per-insert snapshots for stored events (write snapshot_buf at extraction_dbg idx)"

Suggested next steps for Claude
------------------------------
1. Inspect the commits on `diag-plan` (`git log --oneline diag-plan -n 20`) and review `input.cl` and `main.c` diffs.
2. Reproduce the latest diagnostic run and confirm the presence of both `snapshots_*.bin` and `extraction_dbg_*.bin` and `row_*.bin`.
3. Run `python3 tools/aggregate_snapshots.py` and `python3 tools/search_stored_in_row.py` to validate correlations; focus on top rows (row 0, row 2) shown by the aggregator.
4. If you want to test `snapshot_counter` atomics directly, enable the `DEBUG_FORCE_SNAPSHOT` block in `input.cl` (comment/uncomment the `#define DEBUG_FORCE_SNAPSHOT` line near top) and run a one-nonce diagnostic — it will force per-work-item snapshot writes so you can verify device atomics/readback behavior isolated from store-path logic.
5. If the per-insert snapshot entries match HT row bytes consistently, the next step is to search for why candidate solutions later become invalid: investigate xi offsets, endianness, or how colliding pairs are combined. Use the extracted `stored0..3` from `extraction_dbg` as ground truth to compare to solver's constructed candidates.

Quick useful commands
---------------------
- Build:
  `make -j2`
- Diagnostic run (1 nonce, fast):
  `./sa-solver --diag -i "$HEADERHEX" --nonces 1 > diag_out.txt 2>&1`
- Normal run (1 nonce):
  `./sa-solver -i "$HEADERHEX" --nonces 1 > run_one_nonce.txt 2>&1`
- Aggregate:
  `python3 tools/aggregate_snapshots.py snapshots_<ts>.bin extraction_dbg_<ts>.bin > aggregate.txt`
- Search stored bytes in row dump:
  `python3 tools/search_stored_in_row.py extraction_dbg_<ts>.bin row_0_round0.bin`

Where to look for outputs
-------------------------
- Snapshot dumps: `snapshots_*.bin`
- Extraction debug dumps: `extraction_dbg_*.bin`
- HT row dumps: `row_*.bin`
- Runner outputs: `diag_out_*.txt`, `run_one_nonce.txt`, `aggregate_*.txt`

Contact notes & expectations
----------------------------
The current state provides a repeatable diagnostic runpath and tools to correlate kernel-side stored bytes with host HT dumps. The per-insert snapshot mitigation should allow Claude to continue correlation work without being blocked by `snapshot_counter` atomics. Recommended immediate actions are reproduction, aggregation, and deeper inspection of the top hotspot rows (row 0 and row 2).

--- end
