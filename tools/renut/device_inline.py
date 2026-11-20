#!/usr/bin/env python3
"""The Direct3D device fields Oblivion's own code (outside the D3D library) reads and writes itself
(docs/renut-port.md, milestone 3): reNut's device must keep those at Oblivion's offsets.

A register holds the device in a function when it is passed as r3 (`mr r3,rX` before the call) to a function of the
library that takes the device first (DEVICE_API below, from tools/renut/hook_map.txt), or loaded from the global device
slot. Every D-form access on such a register (one that is assigned once in the function, so it holds the device
throughout) is a candidate. Output: offset, access kinds, number of functions, examples.

  python tools/renut/device_inline.py [--gen generated/default] [--out out/renut/device_inline.txt]
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import d3d_lib  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEVICE_API = {int(a, 16) for a in '''
8255B230 82563C20 82563CC0 82557130 825639B0 825638E8 82564118 82564480 82555908 82555C78 82555368 825554B0
825555F8 82555650 825556A8 825556F8 8255E250 8255E490 8255DE70 8255DEF8 8255E230 8255B7A8 8255BED8 825604D0
8255FBC0 8255F5E0 8255E890 8255E7F8 8255E730 8255E748 82557858 829B9610 829B9A48 829B94D8 8255D750 82563BF8
8255F470 82564858 82563D40 8255D488 8255D6D8 82564040 82563F30 82565720
'''.split()}
DEST = re.compile(r'^(\w+\.?)\s+(r\d+),')
BL = re.compile(r'^bl 0x([0-9a-f]{8})')
MR3 = re.compile(r'^mr r3,(r\d+)$')
MR = re.compile(r'^mr (r\d+),(r\d+)$')
LOAD = re.compile(r'^lwz (r\d+),(-?\d+)\(r\d+\)$')
NONVOLATILE = {f'r{i}' for i in range(14, 32)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default=os.path.join(ROOT, 'generated', 'default'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'renut', 'device_inline.txt'))
    a = ap.parse_args()
    funcs = d3d_lib.load(a.gen)
    game = {n: f for n, f in funcs.items() if not d3d_lib.in_lib(f['addr'])}

    # 1. The fields of game objects that hold the device: `lwz r3,X(rB)` with r3 then passed to a device function.
    slots = collections.Counter()
    for f in game.values():
        r3_from = None
        for ins in f['asm']:
            m = LOAD.match(ins)
            if m and m.group(1) == 'r3':
                r3_from = int(m.group(2))
                continue
            m = BL.match(ins)
            if m:
                if int(m.group(1), 16) in DEVICE_API and r3_from is not None:
                    slots[r3_from] += 1
                r3_from = None
            elif DEST.match(ins) and DEST.match(ins).group(2) == 'r3':
                r3_from = None
    slots = {d for d, n in slots.items() if n >= 3 and d >= 64}  # 4, 8: every object's first fields

    # 2. Straight-line tracking per function: a register loaded from a device slot (or copied from one that holds the
    # device, or passed as r3 to a device function) holds the device until it is assigned again.
    fields = collections.defaultdict(lambda: {'kinds': collections.Counter(), 'funcs': set()})
    for name, f in game.items():
        dev = set()
        for ins in f['asm']:
            m = d3d_lib.DISP.match(ins)
            if m and m.group(3) in dev and not ins.startswith(('lwz r3,', )) or (m and m.group(3) in dev):
                off = int(m.group(2))
                fields[off]['kinds'][m.group(1)] += 1
                fields[off]['funcs'].add(name)
            m = LOAD.match(ins)
            if m:
                if int(m.group(2)) in slots:
                    dev.add(m.group(1))
                else:
                    dev.discard(m.group(1))
                continue
            m = MR.match(ins)
            if m:
                if m.group(2) in dev:
                    dev.add(m.group(1))
                else:
                    dev.discard(m.group(1))
                continue
            m = DEST.match(ins)
            if m and not m.group(1).startswith(('st', 'cmp', 'tw', 'dcb', 'mt')):
                dev.discard(m.group(2))
            if ins.startswith(('bl ', 'bctrl')):
                dev &= NONVOLATILE
    print('device slots:', sorted(slots))
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, 'w') as out:
        for off in sorted(fields):
            fld = fields[off]
            kinds = ' '.join(f'{k}x{n}' for k, n in sorted(fld['kinds'].items()))
            ex = ','.join(sorted(fld['funcs'])[:4])
            out.write(f'+{off} (0x{off:X}) {kinds} funcs={len(fld["funcs"])} {ex}\n')
    print(f'{len(fields)} offsets -> {a.out}')


if __name__ == '__main__':
    main()
