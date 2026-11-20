#!/usr/bin/env python3
"""Census of Oblivion's statically linked Direct3D library in the recompiled tree (docs/renut-port.md, milestone 1).

For every function in the D3D address ranges: size, callers inside / outside the library, callees, kernel imports and
the r3-relative (device) displacements it loads and stores. "API" = called directly from outside the library.

  python tools/renut/d3d_lib.py [--gen generated/default] [--out out/renut/d3d_lib.txt] [--api out/renut/d3d_api.txt]
"""
import argparse
import collections
import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# The XDK 2.0.2417 Direct3D library and its neighbours that are D3D themselves (HLE_RENDERER_FEASIBILITY.md section 2).
RANGES = [(0x82550000, 0x82570000), (0x829B9000, 0x829BA000)]
FUNC = re.compile(r'^DEFINE_REX_FUNC\((sub_([0-9A-F]{8}))\)')
CALL = re.compile(r'\b(sub_[0-9A-F]{8}|__imp__\w+)\(ctx, base\)')
ASM = re.compile(r'^\t// (\S.*)$')
DISP = re.compile(r'^(l[bhwd]z?u?x?|lha|lwa|st[bhwd]u?|lfs|lfd|stfs|stfd|lvx\d*|stvx\d*)\s+\w+,(-?\d+)\((r\d+)\)')


def in_lib(a):
    return any(lo <= a < hi for lo, hi in RANGES)


def load(gen):
    funcs = {}
    for path in sorted(glob.glob(os.path.join(gen, '*_recomp.*.cpp'))):
        cur = None
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = FUNC.match(line)
                if m:
                    cur = funcs.setdefault(m.group(1), {'addr': int(m.group(2), 16), 'calls': collections.Counter(),
                                                        'asm': []})
                    continue
                if cur is None:
                    continue
                for c in CALL.findall(line):
                    cur['calls'][c] += 1
                a = ASM.match(line)
                if a:
                    cur['asm'].append(a.group(1))
    return funcs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default=os.path.join(ROOT, 'generated', 'default'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'renut', 'd3d_lib.txt'))
    ap.add_argument('--api', default=os.path.join(ROOT, 'out', 'renut', 'd3d_api.txt'))
    a = ap.parse_args()
    funcs = load(a.gen)
    callers = collections.defaultdict(collections.Counter)
    for name, f in funcs.items():
        for c, n in f['calls'].items():
            callers[c][name] += n
    lib = sorted((f['addr'], n) for n, f in funcs.items() if in_lib(f['addr']))
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    api = []
    with open(a.out, 'w') as out:
        for addr, name in lib:
            f = funcs[name]
            ext = {c: n for c, n in callers[name].items() if not in_lib(funcs[c]['addr'])} if name in callers else {}
            internal = {c: n for c, n in callers[name].items() if c not in ext}
            imports = sorted(c[6:] for c in f['calls'] if c.startswith('__imp__') and not c.startswith('__imp__sub_'))
            callees = sorted(c for c in f['calls'] if c.startswith('sub_'))
            r3 = collections.Counter()
            for ins in f['asm']:
                m = DISP.match(ins)
                if m and m.group(3) in ('r3', 'r31', 'r30') and int(m.group(2)) >= 0:
                    r3[(m.group(1)[0], int(m.group(2)))] += 1
            fields = ' '.join(f'{k}{o}' for (k, o) in sorted(r3, key=lambda x: (x[1], x[0])))
            out.write(f'{name} insns={len(f["asm"])} ext={sum(ext.values())}/{len(ext)} int={sum(internal.values())} '
                      f'callees={",".join(callees)} imports={",".join(imports)}\n    fields: {fields}\n')
            if ext:
                api.append((name, len(f['asm']), sum(ext.values()), sorted(ext, key=lambda c: -ext[c])))
    with open(a.api, 'w') as out:
        for name, n, sites, who in api:
            out.write(f'{name} insns={n} sites={sites} callers={",".join(who[:8])}{" ..." if len(who) > 8 else ""}\n')
    print(f'{len(lib)} library functions, {len(api)} called from outside; {a.out}, {a.api}')


if __name__ == '__main__':
    main()
