#!/usr/bin/env python3
"""Extract first N DBG entries for a given row/slot into persist_list.txt

Usage: python3 tools/extract_persist_list.py diag_persist_test3.txt > persist_list.txt
"""
import re, sys

if len(sys.argv) < 2:
    print('Usage: extract_persist_list.py diag_file [row] [slot] [N]')
    sys.exit(2)

path = sys.argv[1]
row_match = int(sys.argv[2]) if len(sys.argv) > 2 else 2
slot_match = int(sys.argv[3]) if len(sys.argv) > 3 else 0
N = int(sys.argv[4]) if len(sys.argv) > 4 else 8

with open(path, 'r') as f:
    data = f.read()

# capture DBG blocks similar to comparator
blocks = re.findall(r'(DBG\[\d+\]:.*?)(?=\nDBG\[|\Z)', data, flags=re.S)
count = 0
for b in blocks:
    rr = re.search(r'\brow=(\d+)', b)
    ss = re.search(r'\bslot=(\d+)', b)
    if not rr or not ss:
        continue
    row = int(rr.group(1)); slot = int(ss.group(1))
    if row != row_match or slot != slot_match:
        continue
    s0 = re.search(r'\bstored0=([0-9a-fA-F]+)', b)
    s1 = re.search(r'\bstored1=([0-9a-fA-F]+)', b)
    s2 = re.search(r'\bstored2=([0-9a-fA-F]+)', b)
    s3 = re.search(r'\bstored3=([0-9a-fA-F]+)', b)
    if not (s0 and s1 and s2 and s3):
        continue
    s0h = s0.group(1).lower().rjust(16,'0')
    s1h = s1.group(1).lower().rjust(16,'0')
    s2h = s2.group(1).lower().rjust(16,'0')
    s3h = s3.group(1).lower().rjust(16,'0')
    print(f"{row} {slot} 0 {s0h} {s1h} {s2h} {s3h}")
    count += 1
    if count >= N:
        break
