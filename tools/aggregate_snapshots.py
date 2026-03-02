#!/usr/bin/env python3
"""Aggregate extraction_dbg and snapshot binary dumps.

Usage: python3 tools/aggregate_snapshots.py extraction_dbg.bin snapshots.bin > aggregate.txt
"""
import sys, struct, collections

if len(sys.argv) < 3:
    print(__doc__)
    sys.exit(2)

exf = sys.argv[1]
snapf = sys.argv[2]

# extraction_debug_t layout (host-side): 4I, 8Q, I, I, Q, I -> 104 bytes
ex_fmt = '<IIIIQQQQQQQQIIQI'
EX_SZ = struct.calcsize(ex_fmt)

sn_fmt = '<' + 'Q'*8
SN_SZ = struct.calcsize(sn_fmt)

def read_ex(path):
    data = open(path, 'rb').read()
    n = len(data)//EX_SZ
    out = []
    for i in range(n):
        off = i*EX_SZ
        tup = struct.unpack_from(ex_fmt, data, off)
        # fields: round, thread_id, row, slot, xi0..xi3, stored0..stored3, status, table_half, xi_sig, _pad
        rec = {
            'round': tup[0], 'thread': tup[1], 'row': tup[2], 'slot': tup[3],
            'xi0': tup[4], 'xi1': tup[5], 'xi2': tup[6], 'xi3': tup[7],
            'stored0': tup[8], 'stored1': tup[9], 'stored2': tup[10], 'stored3': tup[11],
            'status': tup[12], 'half': tup[13], 'xi_sig': tup[14]
        }
        out.append(rec)
    return out

def read_sn(path):
    data = open(path, 'rb').read()
    n = len(data)//SN_SZ
    out = []
    for i in range(n):
        off = i*SN_SZ
        tup = struct.unpack_from(sn_fmt, data, off)
        rec = {
            'words': [tup[0], tup[1], tup[2], tup[3]],
            'tid': tup[4], 'half': tup[5], 'marker': tup[6], 'seq': tup[7]
        }
        out.append(rec)
    return out

ex = read_ex(exf)
sn = read_sn(snapf)

print('Extraction entries:', len(ex))
print('Snapshot entries:', len(sn))

by_row = collections.Counter()
by_thread = collections.Counter()
by_half = collections.Counter()
marker_map = {}
for i, s in enumerate(sn):
    marker_map[s['marker']] = i
    by_thread[s['tid']] += 1
    by_half[s['half']] += 1

for e in ex:
    by_row[e['row']] += 1
    by_thread[e['thread']] += 1
    by_half[e['half']] += 1

print('\nTop rows in extraction_dbg:')
for r,c in by_row.most_common(10):
    print(' row', r, 'count', c)

print('\nTop threads (ex+sn):')
for t,c in by_thread.most_common(10):
    print(' thread', t, 'count', c)

print('\nSnapshots by half:')
for h,c in by_half.items():
    print(' half', h, 'count', c)

# match stored words from ex against snapshots by finding marker matches
matched = 0
found_elsewhere = 0
for idx, e in enumerate(ex):
    # attempt to find snapshot whose marker encodes thread<<32|sidx matching extraction index
    # marker stored as (thread<<32)|sidx or other
    # try to find by searching marker_map for any marker containing thread in high 32 bits
    found = False
    for m, sidx in marker_map.items():
        if (m >> 32) == e['thread']:
            # compare stored words
            s = sn[sidx]
            if [e['stored0'], e['stored1'], e['stored2'], e['stored3']] == s['words']:
                matched += 1
                found = True
                break
    if not found:
        # best-effort: search any snapshot words equal
        for s in sn:
            if [e['stored0'], e['stored1'], e['stored2'], e['stored3']] == s['words']:
                found_elsewhere += 1
                found = True
                break

print('\nMatched extraction_dbg -> snapshots (by thread+marker):', matched)
print('Found extraction stored words elsewhere in snapshots:', found_elsewhere)

print('\nSample unmatched extraction entries (first 10):')
u = 0
for e in ex:
    # naive check if exists in any snapshot
    exists = any([e['stored0']==s['words'][0] and e['stored1']==s['words'][1] and e['stored2']==s['words'][2] and e['stored3']==s['words'][3] for s in sn])
    if not exists:
        print(' row', e['row'], 'tid', e['thread'], 'stored0', hex(e['stored0']))
        u += 1
        if u >= 10: break
