#!/usr/bin/env python3
"""Which of Oblivion's own Direct3D functions still run under reNut, and which of those touch the device
(docs/renut-port.md, milestone 3).

reNut's device keeps katamari-nx's layout (XDK 5632), not XDK 2417's, so a D3D library function that is not hooked
and reads or writes device fields at 2417's offsets corrupts it (or reads garbage). This walks the call graph of the
recompiled tree from the game's code into the D3D library, stops at every function the nx/ hook table registers
(BANJO_RENUT_HOOK / _RAW lines), and lists the library functions reached, each with:
  - dev: it loads VdGlobalDevice (0x820006D4) or accesses a displacement >= 0x2000 off any register (the device's
    private fields; resource headers and packets are smaller), with those displacements;
  - its game callers and, from a d3d_trace.txt (src/renut/d3d_trace.cpp), how often the game called it.

  python tools/renut/unhooked_reach.py [--hooks nx/app/src/renut_engine/gpu/d3d/hooks] [--trace <d3d_trace.txt>]
"""
import argparse
import collections
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import d3d_lib  # noqa: E402

ROOT = d3d_lib.ROOT
HOOKED = re.compile(r'^BANJO_RENUT_HOOK(?:_SHARED)?(?:_RAW)?\(sub_([0-9A-F]{8})')
# The other registration forms nx/ uses outside the BANJO_RENUT_HOOK table (Oblivion's own D3D layer).
OBLIVION_HOOKED = re.compile(r'^OBLIVION_RENUT_(?:DISPATCH_)?HOOK\w*\(sub_([0-9A-F]{8})')
VD_GLOBAL = 1748  # lis rX,-32256 (0x82000000) + 1748 = 0x820006D4


def hooked_set(hooks_dir):
    out = set()
    for path in glob.glob(os.path.join(hooks_dir, '**', '*.cpp'), recursive=True):
        for line in open(path, encoding='utf-8'):
            m = HOOKED.match(line) or OBLIVION_HOOKED.match(line)
            if m:
                out.add('sub_' + m.group(1))
    return out


def trace_counts(path):
    counts = collections.Counter()
    if not path or not os.path.exists(path):
        return counts
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('  sub_'):
            p = line.split()
            counts[p[0]] += int(p[2])
    return counts


def device_marks(f):
    big = set()
    vd = False
    for ins in f['asm']:
        m = d3d_lib.DISP.match(ins)
        if not m:
            continue
        disp = int(m.group(2))
        if disp == VD_GLOBAL and m.group(1).startswith('lwz'):
            vd = True
        elif disp >= 0x2000 and m.group(3) != 'r1':
            big.add(disp)
    return vd, big


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default=os.path.join(ROOT, 'generated', 'default'))
    ap.add_argument('--hooks', default=os.path.join(ROOT, 'nx', 'app', 'src', 'renut_engine', 'gpu', 'd3d', 'hooks'))
    ap.add_argument('--trace', default=os.path.join(ROOT, 'nx', 'app', 'out', 'build', 'win-amd64-release',
                                                    'd3d_trace.txt'))
    a = ap.parse_args()
    funcs = d3d_lib.load(a.gen)
    hooked = hooked_set(a.hooks)
    counts = trace_counts(a.trace)
    # Entry points: library functions the game's code calls.
    entries = collections.defaultdict(set)
    for name, f in funcs.items():
        if d3d_lib.in_lib(f['addr']):
            continue
        for c in f['calls']:
            if c.startswith('sub_') and c in funcs and d3d_lib.in_lib(funcs[c]['addr']):
                entries[c].add(name)
    reached = {}  # library function -> the entry it was reached from
    stack = [(e, e) for e in entries if e not in hooked]
    while stack:
        name, via = stack.pop()
        if name in reached:
            continue
        reached[name] = via
        for c in funcs[name]['calls']:
            if c.startswith('sub_') and c in funcs and d3d_lib.in_lib(funcs[c]['addr']) and c not in hooked:
                stack.append((c, via))
    print(f'hooked {len(hooked)}; library entries called by the game {len(entries)}, {len(entries) - len(set(entries) & hooked)} '
          f'not hooked; library functions reached through them {len(reached)}')
    rows = []
    for name, via in reached.items():
        vd, big = device_marks(funcs[name])
        if not (vd or big):
            continue
        n = counts.get(name, 0) + (counts.get(via, 0) if via != name else 0)
        rows.append((-(counts.get(via, 0)), name, via, vd, big))
    for _, name, via, vd, big in sorted(rows):
        callers = ','.join(sorted(entries.get(name, ()))[:4])
        marks = ('VdGlobalDevice ' if vd else '') + ' '.join(str(d) for d in sorted(big)[:12])
        print(f'{name} via {via} (traced {counts.get(via, 0)}) insns={len(funcs[name]["asm"])} dev: {marks}'
              + (f'  game callers: {callers}' if callers else ''))


if __name__ == '__main__':
    main()
