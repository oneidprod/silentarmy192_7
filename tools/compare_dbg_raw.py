#!/usr/bin/env python3
"""Compare extraction_dbg stored words to Dump row rawslot chunks.

Usage: python3 tools/compare_dbg_raw.py /path/to/diag_out.txt
"""
import re
import sys

dump_re = re.compile(r"Dump row (?P<row>\d+) \(round (?P<round>\d+)(?: [^)]*)?\): cnt=(?P<cnt>\d+)")
slot_re = re.compile(r" slot (?P<idx>\d+): .*\n  rawslot=(?P<raw>[0-9a-fA-F]+)")
# capture DBG blocks (may span multiple visual lines); parse fields inside each block
dbg_block_re = re.compile(r"(DBG\[\d+\]:.*?)(?=\nDBG\[|\Z)", re.S)

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

    for m in dbg_block_re.finditer(data):
        block = m.group(1)
        # extract fields individually to avoid ordering/newline issues
        def find_int(field):
            mm = re.search(r"\b" + field + r"=(\d+)", block)
            return int(mm.group(1)) if mm else None
        def find_hex(field):
            mm = re.search(r"\b" + field + r"=([0-9a-fA-F]+)", block)
            return mm.group(1).lower() if mm else None

        roundn = find_int('round')
        row = find_int('row')
        slot = find_int('slot')
        half = find_int('table_half')
        # default half to 0 if not present
        if half is None:
            half = 0
        s0 = find_hex('stored0') or '0'
        s1 = find_hex('stored1') or '0'
        s2 = find_hex('stored2') or '0'
        s3 = find_hex('stored3') or '0'
        s = [s0.rjust(16, '0'), s1.rjust(16, '0'), s2.rjust(16, '0'), s3.rjust(16, '0')]
        # if round/row/slot not found, skip
        if roundn is None or row is None or slot is None:
            continue
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
            xi_offset = 8 + (crnd * 3)
            def compute_from_raw(rawhex, slot_idx, xi_offset):
                if not rawhex:
                    return None
                if len(rawhex) < 64:
                    rawhex = rawhex.ljust(64, '0')
                slot_bytes = bytes.fromhex(rawhex)
                needed_end = xi_offset + 32
                if needed_end > len(slot_bytes):
                    next_raw = slots.get(slot_idx + 1)
                    next_bytes = bytes.fromhex(next_raw) if next_raw else b''
                    slot_bytes = slot_bytes + next_bytes
                if len(slot_bytes) < xi_offset + 32:
                    slot_bytes = slot_bytes.ljust(xi_offset + 32, b'\x00')
                computed = []
                for i in range(4):
                    start = xi_offset + i * 8
                    chunk = slot_bytes[start:start+8]
                    if len(chunk) < 8:
                        chunk = chunk.ljust(8, b'\x00')
                    val = int.from_bytes(chunk, 'little')
                    computed.append('{:016x}'.format(val))
                return computed

            if raw:
                computed = compute_from_raw(raw, slot, xi_offset)
            else:
                computed = None

            if computed == s:
                matched_ok += 1
            else:
                # try any slot in this row as a fallback
                found_in_other = None
                for sidx, sraw in slots.items():
                    comp = compute_from_raw(sraw, sidx, xi_offset)
                    if comp == s:
                        found_in_other = sidx
                        break
                if found_in_other is not None:
                    mismatches.append((row, roundn, slot, half, s, comp, f'found_in_slot_{found_in_other}', dkey))
                    matched_ok += 1
                else:
                    mismatches.append((row, roundn, slot, half, s, computed, 'mismatch' if computed is not None else 'no_slot', dkey))
                    matched_bad += 1
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
