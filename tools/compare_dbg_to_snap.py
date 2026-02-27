#!/usr/bin/env python3
"""Compare extraction_dbg stored words to SNAP[...] entries in diag output.

Usage: python3 tools/compare_dbg_to_snap.py diag_persist_snap.txt
"""
import re, sys

if len(sys.argv) < 2:
    print('Usage: compare_dbg_to_snap.py diag_out.txt')
    sys.exit(2)

path = sys.argv[1]
with open(path, 'r') as f:
    data = f.read()

# extract SNAP lines
snap_re = re.compile(r"SNAP\[(?P<idx>\d+)\]:\s*(?P<s0>[0-9a-fA-F]+)\s+(?P<s1>[0-9a-fA-F]+)\s+(?P<s2>[0-9a-fA-F]+)\s+(?P<s3>[0-9a-fA-F]+)(?:\s+tid=(?P<tid>\d+)\s+half=(?P<half>\d+)\s+(?:seq|marker)=(?P<marker>\d+))?")
snaps = []
for m in snap_re.finditer(data):
    s0 = m.group('s0').lower().rjust(16, '0')
    s1 = m.group('s1').lower().rjust(16, '0')
    s2 = m.group('s2').lower().rjust(16, '0')
    s3 = m.group('s3').lower().rjust(16, '0')
    tid = m.group('tid')
    half = m.group('half')
    marker = m.group('marker')
    snaps.append({'words':[s0, s1, s2, s3], 'tid': int(tid) if tid else None, 'half': int(half) if half else None, 'marker': int(marker) if marker else None})

# extract DBG blocks
dbg_block_re = re.compile(r"(DBG\[\d+\]:.*?)(?=\nDBG\[|\Z)", re.S)
stored_list = []
for m in dbg_block_re.finditer(data):
    block = m.group(1)
    def find_hex(field):
        mm = re.search(r"\b" + field + r"=([0-9a-fA-F]+)", block)
        return mm.group(1).lower() if mm else None
    s0 = find_hex('stored0') or '0'
    s1 = find_hex('stored1') or '0'
    s2 = find_hex('stored2') or '0'
    s3 = find_hex('stored3') or '0'
        stored_list.append([s0.rjust(16,'0'), s1.rjust(16,'0'), s2.rjust(16,'0'), s3.rjust(16,'0')])

# Compare by index
N = min(len(snaps), len(stored_list))
matches = 0
mismatches = 0
for i in range(N):
    snap_words = snaps[i]['words'] if isinstance(snaps[i], dict) else snaps[i]
    if snap_words == stored_list[i]:
        matches += 1
    else:
        mismatches += 1

# Also attempt to match stored entries anywhere in snaps (best-effort)
snap_map = {tuple(snap['words'] if isinstance(snap, dict) else snap): idx for idx, snap in enumerate(snaps)}
unmatched = []
found_elsewhere = 0
for i, stored in enumerate(stored_list):
    if tuple(stored) in snap_map:
        found_elsewhere += 1
    else:
        unmatched.append((i, stored))

print('SNAP entries:', len(snaps))
print('DBG entries:', len(stored_list))
print('Index matches (snap[i] == dbg[i]):', matches)
print('Index mismatches:', mismatches)
print('Stored entries found elsewhere in snapshots:', found_elsewhere)
print('Unmatched stored entries:', len(unmatched))
if unmatched:
    print('\nSample unmatched stored entries (first 10):')
    for i, s in unmatched[:10]:
        print(i, s)
