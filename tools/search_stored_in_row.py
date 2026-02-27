#!/usr/bin/env python3
"""Search extraction_dbg stored 32-byte patterns inside an HT row binary.

Usage: python3 tools/search_stored_in_row.py extraction_dbg.bin row_X_roundY.bin
"""
import sys, struct

if len(sys.argv) < 3:
    print(__doc__)
    sys.exit(2)

exf = sys.argv[1]
rowf = sys.argv[2]

ex_fmt = '<IIIIQQQQQQQQIIQI'
EX_SZ = struct.calcsize(ex_fmt)

data = open(exf, 'rb').read()
n = len(data)//EX_SZ
ex = []
for i in range(n):
    tup = struct.unpack_from(ex_fmt, data, i*EX_SZ)
    # pack stored0..stored3 as 32-byte little-endian sequence
    packed = struct.pack('<QQQQ', tup[8], tup[9], tup[10], tup[11])
    ex.append((i, tup[1], tup[2], packed))

rowdata = open(rowf, 'rb').read()

print('Loaded', n, 'extraction entries, searching in', rowf, 'size', len(rowdata))

for idx, tid, row, packed in ex:
    pos = rowdata.find(packed)
    if pos != -1:
        slot = pos // 32
        print('Found extraction idx', idx, 'tid', tid, 'row', row, 'at offset', pos, 'slot', slot)
