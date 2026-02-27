#!/usr/bin/env python3
"""Match extraction_dbg stored words against an HT row dump.

Usage: python3 tools/match_exdbg_to_row.py extraction_dbg.bin row_X_roundY.bin
"""
import sys, struct

if len(sys.argv) < 3:
    print(__doc__)
    sys.exit(2)

exf = sys.argv[1]
rowf = sys.argv[2]

# extraction_debug_t layout (host-side): 4I, 8Q, I, I, Q, I -> 104 bytes
ex_fmt = '<IIIIQQQQQQQQIIQI'
EX_SZ = struct.calcsize(ex_fmt)

data = open(exf, 'rb').read()
n = len(data)//EX_SZ
ex = []
for i in range(n):
    tup = struct.unpack_from(ex_fmt, data, i*EX_SZ)
    rec = {
        'idx': i,
        'round': tup[0], 'thread': tup[1], 'row': tup[2], 'slot': tup[3],
        'stored': (tup[8], tup[9], tup[10], tup[11])
    }
    ex.append(rec)

rowdata = open(rowf, 'rb').read()
SLOT_LEN = 32
slots = len(rowdata)//SLOT_LEN

print('Loaded', n, 'extraction entries, row file has', slots, 'slots')

for s in range(slots):
    off = s * SLOT_LEN
    w0, w1, w2, w3 = struct.unpack_from('<QQQQ', rowdata, off)
    for e in ex:
        if e['stored'] == (w0, w1, w2, w3):
            print('Match: rowfile', rowf, 'slot', s, 'matches extraction idx', e['idx'], 'tid', e['thread'], 'row', e['row'])
