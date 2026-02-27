# SilentArmy (192,7) Diagnostic & Fix Plan

Status: work in progress — instrumentation added to kernel and host; snapshots now 8 words.

## Summary of what we learned
- Xi offset mapping validated by deterministic kernel test.
- Kernel extraction debug (`extraction_dbg`) implemented and read back by host.
- Per-insert `SNAP` buffer added (4 stored words + `thread_id`, `table_half`, `seq`, reserved).
- Many `extraction_dbg` entries still do not correlate with host HT raw-slot dumps; hotspot rows observed (e.g. row 2 round 0 slot 0).

## Objectives
1. Produce deterministic correlation between kernel `extraction_dbg` entries and host HT bytes.
2. Identify root cause of disappearing/invalid candidates (half-selection, timing race, or storage bug).
3. Fix root cause so miner can produce valid Equihash 192,7 solutions reliably and submit them.

## Action plan (ordered)
1. Run a large diagnostic (configurable; default 1000 nonces) and collect `diag_out_1k.txt`.
2. Host: dump the full `buf_snapshots` buffer to a binary file per run (prevent print truncation).
3. Kernel: write an unequivocal per-snapshot marker (thread_id<<32 | idx) into snapshot reserved word.
4. Update `tools/compare_dbg_to_snap.py` and `tools/compare_dbg_raw.py` to parse 8-word snapshots and the marker.
5. Run targeted HT dumps for top-N hotspot rows (both halves) and save raw rows.
6. Aggregate stats: per-row / per-thread / per-half histograms and match rates; timeline of snapshot seq/marker.
7. Diagnose root cause from aggregated data and propose one of: (A) fix xi offset/endianness, (B) fix half-selection in kernel, (C) mitigate timing/race by stronger persistence/readback.
8. Implement fix and validate with repeated diagnostics and comparator runs.
9. Validate miner end-to-end (self-test producing valid `sol:` lines) and, if desired, pool submission test mode.
10. Automate the diagnostic runbook and document reproducible steps.

## Deliverables I will produce
- `DIAGNOSTIC_PLAN.md` (this file)
- Kernel patch: deterministic per-snapshot marker (in `input.cl`).
- Host patch: full snapshot buffer dump (`main.c`) and small summary print.
- Tools patch: `tools/*` updated for 8-word snapshot layout.
- Diagnostic outputs: `diag_out_1k.txt`, `snapshots_*.bin`, comparator summaries.
- Final short report with root-cause analysis and recommended fix.

## Git commit strategy (how to rollback)
- New branch: `diag-plan` (work branch). Commits will be small and focused:
  - `commit: add DIAGNOSTIC_PLAN.md`
  - `commit: host: dump full snapshots` 
  - `commit: kernel: write deterministic snapshot marker`
  - `commit: tools: parse 8-word snapshots`
  - `commit: diag: run and add logs` 
- To rollback: `git checkout master` or `git checkout <commit>` or `git revert <commit>`.

## How I will run diagnostics (examples)
```
./sa-solver --diag --nonces 1000 > diag_out_1k.txt 2>&1
grep '^SNAP' diag_out_1k.txt | sed -n 's/.*marker=\([0-9a-fA-Fx]*\).*/\1/p' | sort | uniq -c
python3 tools/compare_dbg_to_snap.py diag_out_1k.txt > compare_snap_1k.txt
```

## Next step (I will execute if you approve)
- Implement steps 2–4 (host full-dump, kernel marker, tools updates) and run a 1k diagnostic, then produce a short report.

If you want immediate action, reply "go" and I'll start commit-by-commit on branch `diag-plan`.
