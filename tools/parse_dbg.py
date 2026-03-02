#!/usr/bin/env python3
import re
import sys
from collections import defaultdict
fn='run_non_diag_20.txt'
if len(sys.argv)>1:
    fn=sys.argv[1]
with open(fn,'r') as f:
    lines=f.readlines()

dbg_re=re.compile(r"DBG\[\d+\]: .*round=(?P<round>\d+) .* row=(?P<row>\d+) slot=(?P<slot>\d+) .* stored0=(?P<st0>[0-9a-fA-F]+) stored1=(?P<st1>[0-9a-fA-F]+) stored2=(?P<st2>[0-9a-fA-F]+) stored3=(?P<st3>[0-9a-fA-F]+)")
dump_re=re.compile(r"Dump row (?P<row>\d+) \(round (?P<round>\d+)(?: [^)]*)?\): cnt=(?P<cnt>\d+)")
slot_re=re.compile(r"\s*slot\s+\d+: i=([0-9a-fA-F]+) xi=([0-9a-fA-F]+)")

dbgs=[]
dumps=defaultdict(list)  # (round,row)-> list of xi hex

i=0
while i<len(lines):
    l=lines[i]
    m=dbg_re.search(l)
    if m:
        d={k:int(v,0) if k in ('round','row','slot') else v for k,v in m.groupdict().items()}
        d['raw']=l.rstrip('\n')
        dbgs.append(d)
    m2=dump_re.search(l)
    if m2:
        row=int(m2.group('row'))
        roundn=int(m2.group('round'))
        cnt=int(m2.group('cnt'))
        # next cnt lines contain slots
        j=i+1
        got=0
        while j<len(lines) and got<cnt:
            m3=slot_re.search(lines[j])
            if m3:
                xi=m3.group(2)
                dumps[(roundn,row)].append(xi)
                got+=1
            j+=1
        i=j
        continue
    i+=1

# analyze
matches=0
checked=0
mismatches=[]
for d in dbgs:
    r=d['round']; row=d['row']; slot=d['slot']
    # skip empty/unused debug records where stored words are all zero
    if all(d.get(f'st{i}','0') == '0000000000000000' for i in range(4)):
        continue
    key=(r,row)
    if key not in dumps:
        continue
    xi_list=dumps[key]
    # slot index may be > len(xi_list); skip
    if slot>=len(xi_list):
        continue
    xihex=xi_list[slot]
    # ensure even length
    if len(xihex)%2!=0:
        xihex='0'+xihex
    xb=bytes.fromhex(xihex)
    # break xb into 8-byte little-endian ints
    parts=[]
    for k in range(0, len(xb), 8):
        chunk=xb[k:k+8]
        # pad to 8
        if len(chunk)<8:
            chunk += b"\x00"*(8-len(chunk))
        val=int.from_bytes(chunk,'little')
        parts.append('{:016x}'.format(val))
    # compare first 4 parts to stored fields (they may be fewer)
    st0=d['st0']; st1=d['st1']; st2=d['st2']; st3=d['st3']
    checked+=1
    ok=True
    # normalize stored fields to lower no-leading
    svals=[st0.lower(),st1.lower(),st2.lower(),st3.lower()]
    for idx in range(4):
        if idx < len(parts):
            if parts[idx] != svals[idx].rjust(16,'0'):
                ok=False
        else:
            if svals[idx] != '0'*len(svals[idx]):
                ok=False
    if ok:
        matches+=1
    else:
        mismatches.append((d, xb.hex(), parts, svals))

print(f"Total DBG entries: {len(dbgs)}, checked against dumps: {checked}, matches: {matches}, mismatches: {len(mismatches)}")
print('\nDump rows present:')
for k in sorted(dumps.keys())[:50]:
    print(k)
print('\nDBG rows that match a dump row:')
for d in dbgs:
    for k in dumps.keys():
        if d['row'] == k[1]:
            print(f"DBG row {d['row']} (DBG round {d['round']}) matches dump key {k}")
            break
for idx,mm in enumerate(mismatches[:20]):
    d, xihex, parts, svals = mm
    print('---')
    print(f"DBG round={d['round']} row={d['row']} slot={d['slot']}")
    print('xi hex (from dump) =', xihex)
    print('computed parts =', parts)
    print('stored vals   =', svals)

print('\nDone')
