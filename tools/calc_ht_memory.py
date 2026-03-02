#!/usr/bin/env python3
"""Calculate hash-table memory from param.h and compare to device VRAM.

Usage: python3 tools/calc_ht_memory.py [--safety 0.9]
"""
import re
import subprocess
import sys
import os

ROOT = os.path.join(os.path.dirname(__file__), '..')
PARAM = os.path.join(ROOT, 'param.h')

def parse_param(path):
    data = open(path, 'r').read()
    def get_int(name):
        m = re.search(r'#define\s+' + re.escape(name) + r'\s+(\S+)', data)
        if not m:
            return None
        v = m.group(1)
        # try to evaluate simple expressions
        try:
            return int(eval(v, {}))
        except Exception:
            # strip suffixes like ULL
            v2 = re.sub(r'[uUlL]', '', v)
            try:
                return int(v2, 0)
            except Exception:
                return None
    vals = {}
    vals['PARAM_N'] = get_int('PARAM_N')
    vals['PARAM_K'] = get_int('PARAM_K')
    vals['NR_ROWS_LOG'] = get_int('NR_ROWS_LOG')
    vals['NR_SLOTS'] = get_int('NR_SLOTS')
    vals['SLOT_LEN'] = get_int('SLOT_LEN')
    return vals

def bytes_to_mb(b):
    return b / 1024.0 / 1024.0

def get_device_mem_sizes():
    try:
        out = subprocess.check_output(['clinfo'], stderr=subprocess.DEVNULL, text=True)
    except Exception:
        return None
    sizes = []
    for line in out.splitlines():
        if 'Global memory size' in line:
            m = re.search(r'Global memory size\s+(\d+) \(([^)]+)\)', line)
            if m:
                sizes.append(int(m.group(1)))
    return sizes

def suggest_reductions(nr_rows_log, nr_slots, slot_len, device_bytes, safety=0.9):
    # compute current sizes
    NR_ROWS = 1 << nr_rows_log
    per_table = NR_ROWS * nr_slots * slot_len
    total2 = per_table * 2
    cap = int(device_bytes * safety)
    suggestions = []
    if total2 <= cap:
        return suggestions
    # try lowering NR_ROWS_LOG until it fits
    for new_log in range(nr_rows_log-1, -1, -1):
        new_rows = 1 << new_log
        new_total2 = new_rows * nr_slots * slot_len * 2
        if new_total2 <= cap:
            suggestions.append(('NR_ROWS_LOG', new_log, new_total2))
            break
    # also suggest reducing NR_SLOTS
    for new_slots in (16, 8, 4):
        new_total2 = (1 << nr_rows_log) * new_slots * slot_len * 2
        if new_total2 <= cap:
            suggestions.append(('NR_SLOTS', new_slots, new_total2))
            break
    return suggestions

def main():
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument('--safety', type=float, default=0.92)
    args = p.parse_args()

    vals = parse_param(PARAM)
    if not vals['NR_ROWS_LOG'] or not vals['NR_SLOTS'] or not vals['SLOT_LEN']:
        print('Failed to parse param.h')
        sys.exit(2)
    NR_ROWS = 1 << vals['NR_ROWS_LOG']
    per_table = NR_ROWS * vals['NR_SLOTS'] * vals['SLOT_LEN']
    total2 = per_table * 2
    print('Params from param.h: NR_ROWS_LOG={}, NR_SLOTS={}, SLOT_LEN={}'.format(vals['NR_ROWS_LOG'], vals['NR_SLOTS'], vals['SLOT_LEN']))
    print('Per-table HT size: {:.2f} MB'.format(bytes_to_mb(per_table)))
    print('Two tables total:   {:.2f} MB'.format(bytes_to_mb(total2)))

    devs = get_device_mem_sizes()
    if not devs:
        print('\nclinfo not available or no device info found. Pass device memory manually.')
        return
    print('\nDetected device Global memory sizes (bytes):')
    for i, d in enumerate(devs):
        print('  Device {}: {} bytes ({:.2f} MB)'.format(i, d, bytes_to_mb(d)))
    # suggest per-device
    for i, d in enumerate(devs):
        cap = int(d * args.safety)
        ok = total2 <= cap
        print('\nDevice {}: capacity with safety {:.0f}% = {:.2f} MB -> fits: {}'.format(i, args.safety*100, bytes_to_mb(cap), ok))
        if not ok:
            print(' Suggestions to fit this device:')
            s = suggest_reductions(vals['NR_ROWS_LOG'], vals['NR_SLOTS'], vals['SLOT_LEN'], d, safety=args.safety)
            if not s:
                print('  No simple suggestion found (would need more drastic changes)')
            for key, val, new_total in s:
                print('  Set {} = {} -> total {:.2f} MB'.format(key, val, bytes_to_mb(new_total)))

if __name__ == '__main__':
    main()
