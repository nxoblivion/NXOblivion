#!/usr/bin/env python3
"""Print the PowerPC disassembly (the generator's comments) of recompiled functions.

  python tools/renut/asm.py 8255EB18 [8255EF80 ...] [--gen nx/app/generated/default]
"""
import argparse
import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FUNC = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)')
ASM = re.compile(r'^\t// (\S.*)$')
LABEL = re.compile(r'^loc_([0-9A-F]{8}):')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('addrs', nargs='+')
    ap.add_argument('--gen', default=os.path.join(ROOT, 'nx', 'app', 'generated', 'default'))
    a = ap.parse_args()
    want = {x.upper().replace('SUB_', '').replace('0X', '') for x in a.addrs}
    out = {}
    for path in sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))):
        cur = None
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = FUNC.match(line)
                if m:
                    cur = m.group(1) if m.group(1) in want else None
                    if cur:
                        out[cur] = []
                    continue
                if cur is None:
                    continue
                m = LABEL.match(line)
                if m:
                    out[cur].append(f'{m.group(1)}:')
                    continue
                m = ASM.match(line)
                if m:
                    out[cur].append('    ' + m.group(1))
    for w in a.addrs:
        k = w.upper().replace('SUB_', '').replace('0X', '')
        print(f'sub_{k}:')
        print('\n'.join(out.get(k, ['    (not found)'])))


if __name__ == '__main__':
    main()
