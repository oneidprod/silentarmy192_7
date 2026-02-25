#!/usr/bin/env python3
"""Compare extraction_dbg stored words to Dump row rawslot chunks.

Usage: python3 tools/compare_dbg_raw.py /path/to/diag_out.txt
"""
import re
import sys

dump_re = re.compile(r"Dump row (?P<row>\d+) \(round (?P<round>\d+)(?: [^)]*)?\): cnt=(?P<cnt>\d+)")
slot_re = re.compile(r" slot (?P<idx>\d+): .*\n  rawslot=(?P<raw>[0-9a-fA-F]+)")
dbg_re = re.compile(r"DBG\[\d+\]: round=(?P<round>\d+) .* row=(?P<row>\d+) slot=(?P<slot>\d+) .* table_half=(?P<half>\d+) .* stored0=(?P<s0>[0-9a-fA-F]+) stored1=(?P<s1>[0-9a-fA-F]+) stored2=(?P<s2>[0-9a-fA-F]+) stored3=(?P<s3>[0-9a-fA-F]+)")

def le64_from_rawslot(rawhex, idx, byte_offset=0):
    # rawhex is hex string length 64 (32 bytes). read 8-byte chunk starting at
    # byte_offset + idx*8 and interpret as little-endian 64-bit
    b = bytes.fromhex(rawhex)
    start = byte_offset + idx * 8
    chunk = b[start:start+8]
    # little-endian -> interpret accordingly
    val = int.from_bytes(chunk, 'little')
    return '{:016x}'.format(val)

def main():
    if len(sys.argv) < 2:
        print('Usage: compare_dbg_raw.py diag_out.txt')
        return
    path = sys.argv[1]
    with open(path, 'r') as f:
        data = f.read()

    # parse dumps
    dumps = {}  # (row, round, half) -> {slot_idx: rawhex}
    for m in dump_re.finditer(data):
        row = int(m.group('row'))
        roundn = int(m.group('round'))
        # find the dump block starting at m.end()
        start = m.end()
        # extract until next 'Dump row' or end
        end = len(data)
        nxt = dump_re.search(data, pos=start)
        if nxt:
            end = nxt.start()
        block = data[start:end]
        # find half in the original header
        header = data[m.start():m.end()]
        half = 0
        hh = re.search(r'half=(\d+)', data[m.start():m.end()+50])
        if hh:
            half = int(hh.group(1))
        slots = {}
        for sm in slot_re.finditer(block):
            idx = int(sm.group('idx'))
            raw = sm.group('raw')
            slots[idx] = raw
        dumps[(row, roundn, half)] = slots

    # parse DBG lines and try multiple matching heuristics when locating the dump row
    mismatches = []
    total_checks = 0
    total_with_dump = 0
    matched_ok = 0
    matched_bad = 0
    # try to get NR_ROWS from param.h if available for modular matching
    NR_ROWS = None
    try:
        with open('../param.h', 'r') as ph:
            for line in ph:
                if 'NR_ROWS_LOG' in line and '=' in line:
                    import re as _re
                    mm = _re.search(r'NR_ROWS_LOG\s*(\d+)', line)
                    if mm:
                        NR_ROWS = 1 << int(mm.group(1))
                        break
    except Exception:
        NR_ROWS = None

    for m in dbg_re.finditer(data):
        roundn = int(m.group('round'))
        row = int(m.group('row'))
        slot = int(m.group('slot'))
        half = int(m.group('half'))
        s = [m.group('s0').lower().rjust(16, '0'), m.group('s1').lower().rjust(16, '0'), m.group('s2').lower().rjust(16, '0'), m.group('s3').lower().rjust(16, '0')]
        # skip all-zero stored entries
        if all(x == '0000000000000000' for x in s):
            continue
        total_checks += 1

        # candidate keys to try, ordered by preference
        candidates = []
        candidates.append((row, roundn, half))
        # ignore half
        candidates.append((row, roundn, None))
        # try table_half = round&1
        candidates.append((row, roundn, roundn & 1))
        # if NR_ROWS known, try row % NR_ROWS
        if NR_ROWS:
            candidates.append((row % NR_ROWS, roundn, half))
            candidates.append((row % NR_ROWS, roundn, None))
            candidates.append((row % NR_ROWS, roundn, roundn & 1))

        found = False
        for c in candidates:
            crow, crnd, chalf = c
            # search dumps: if chalf is None, match any half for same (row,round)
            if chalf is None:
                # look for any (crow, crnd, any_half)
                keys = [k for k in dumps.keys() if k[0] == crow and k[1] == crnd]
                if not keys:
                    continue
                # pick first matching key (there should be at most a few)
                dkey = keys[0]
            else:
                dkey = (crow, crnd, chalf)
                if dkey not in dumps:
                    continue
            slots = dumps[dkey]
            total_with_dump += 1
            raw = slots.get(slot)
            if not raw:
                mismatches.append((row, roundn, slot, half, s, None, 'no_slot', dkey))
                matched_bad += 1
                found = True
                break
            if len(raw) < 64:
                raw = raw.ljust(64, '0')
            # compute xi byte offset per kernel macro: 8 + round*3
            xi_offset = 8 + (crnd * 3)
            # build a byte buffer that covers xi_offset..xi_offset+32 bytes
            slot_bytes = bytes.fromhex(raw)
            # if requested range spills past this slot, try to read next slot's bytes
            needed_end = xi_offset + 32
            if needed_end > len(slot_bytes):
                # attempt to append next slot bytes (slot+1) if available
                next_raw = slots.get(slot + 1)
                if next_raw:
                    next_bytes = bytes.fromhex(next_raw)
                else:
                    next_bytes = b''
                slot_bytes = slot_bytes + next_bytes
            # ensure we have enough bytes; pad with zeros if not
            if len(slot_bytes) < xi_offset + 32:
                slot_bytes = slot_bytes.ljust(xi_offset + 32, b'\x00')
            # extract four little-endian 8-byte words starting at xi_offset
            computed = []
            for i in range(4):
                start = xi_offset + i * 8
                chunk = slot_bytes[start:start+8]
                if len(chunk) < 8:
                    chunk = chunk.ljust(8, b'\x00')
                val = int.from_bytes(chunk, 'little')
                computed.append('{:016x}'.format(val))
            if computed != s:
                mismatches.append((row, roundn, slot, half, s, computed, 'mismatch', dkey))
                matched_bad += 1
            else:
                matched_ok += 1
            found = True
            break
        # if none of the candidate keys matched, continue
        if not found:
            continue

    # report
    print('Total DBG non-zero entries checked:', total_checks)
    print('DBG entries with a matching Dump row:', total_with_dump)
    print('Matches:', matched_ok, 'Mismatches:', matched_bad)
    print('Mismatches / issues found (details):', len(mismatches))
    for item in mismatches[:200]:
        # item can be (row, roundn, slot, half, stored, computed, kind)
        # or (row, roundn, slot, half, stored, computed, kind, dkey)
        if len(item) == 7:
            row, roundn, slot, half, stored, computed, kind = item
            dkey = (row, roundn, half)
        else:
            row, roundn, slot, half, stored, computed, kind, dkey = item
        print('\nROW', row, 'ROUND', roundn, 'SLOT', slot, 'HALF', half, 'KIND', kind)
        print(' stored :', stored)
        if computed is None:
            print(' computed: (no dump data)')
        else:
            print(' computed:', computed)
            # show raw slot if available
            raw_show = dumps.get(dkey, {}).get(slot)
            print(' rawslot :', raw_show)

if __name__ == '__main__':
    main()
