#!/usr/bin/env python3
"""Index the recompiled 360 functions from generated/default/oblivion_recomp.*.cpp (PPC instruction comments) and
tools/image.bin. Per function: string refs (lis+addi/ori pairs, lis+lwz data refs), ordered bl targets, tail b targets.
Cache: out/names/cache/x360.pkl.   Usage: python tools/pcmatch/x360_index.py"""
import glob, os, pickle, re, struct, sys
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
IMG = open(os.path.join(ROOT, 'tools', 'image.bin'), 'rb').read()
B = 0x82000000
OUT = os.path.join(ROOT, 'out', 'names', 'cache', 'x360.pkl')

def cstr(a, minlen=4, maxlen=400):
    o = a - B
    if o < 0 or o >= len(IMG): return None
    e = IMG.find(b'\0', o, o + maxlen)
    if e < 0 or e - o < minlen: return None
    b = IMG[o:e]
    if any(c < 32 and c not in (9, 10, 13) or c > 126 for c in b): return None
    if o > 0 and IMG[o - 1] != 0: return None
    return b.decode('ascii')

def s16(v): return v - 65536 if v >= 32768 else v
def num(t):
    t = t.strip()
    return int(t, 16) if t.lower().startswith(('0x', '-0x')) else int(t)

re_def = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)')
re_cm = re.compile(r'^\t// (\S+)(?:\s+(.*))?$')
NODEST = ('st', 'cmp', 'b', 'mt', 'tw', 'td', 'dcb', 'sync', 'isync', 'eieio', 'nop', 'stv', 'li_')
funcs = {}
for path in sorted(glob.glob(os.path.join(ROOT, 'generated', 'default', 'oblivion_recomp.*.cpp'))):
    cur = None
    hi = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re_def.match(line)
        if m:
            a = int(m.group(1), 16)
            cur = funcs[a] = dict(strs=[], calls=[], tails=[], data=[], n=0, consts=set())
            hi = {}
            continue
        if cur is None: continue
        m = re_cm.match(line)
        if not m: continue
        op, args = m.group(1), (m.group(2) or '')
        cur['n'] += 1
        ar = [x.strip() for x in args.split(',')]
        if op == 'lis' and len(ar) == 2:
            try: hi[ar[0]] = (num(ar[1]) << 16) & 0xffffffff
            except ValueError: hi.pop(ar[0], None)
            continue
        val = None
        if op in ('addi', 'ori') and len(ar) == 3 and ar[1] in hi:
            try:
                imm = num(ar[2])
                val = (hi[ar[1]] + (imm if op == 'addi' else (imm & 0xffff))) & 0xffffffff
            except ValueError: pass
        elif op in ('lwz', 'lfs', 'lfd', 'lbz', 'lhz', 'lha', 'stw', 'stb', 'sth', 'stfs', 'stfd', 'ld', 'std') and len(ar) == 2:
            mm = re.match(r'(-?\w+)\((r\d+)\)', ar[1])
            if mm and mm.group(2) in hi:
                try: val = (hi[mm.group(2)] + num(mm.group(1))) & 0xffffffff
                except ValueError: pass
        if op in ('li', 'cmpwi', 'cmplwi', 'andi.', 'ori', 'xori', 'mulli', 'subfic') and ar:
            try:
                v = num(ar[-1])
                if abs(v) >= 0x40: cur['consts'].add(abs(v))
            except ValueError: pass
        elif op == 'addi' and len(ar) == 3 and ar[1] != 'r1' and ar[1] not in hi:
            try:
                v = num(ar[2])
                if abs(v) >= 0x40: cur['consts'].add(abs(v))
            except ValueError: pass
        elif len(ar) == 2 and op[0] in 'ls' and op not in ('lis',):
            mm = re.match(r'(-?\w+)\((r\d+)\)', ar[1])
            if mm and mm.group(2) not in ('r1', 'r13') and mm.group(2) not in hi:
                try:
                    v = num(mm.group(1))
                    if 0x40 <= v < 0x100000: cur['consts'].add(v)
                except ValueError: pass
        if val is not None and not (B <= val < B + len(IMG)) and val >= 0x40:
            cur['consts'].add(val)
        if val is not None and B <= val < B + len(IMG):
            st = cstr(val)
            if st is not None: cur['strs'].append((val, st))
            else: cur['data'].append(val)
        if op == 'bl' and ar[0].startswith('0x'):
            cur['calls'].append(int(ar[0], 16))
        elif op == 'b' and ar[0].startswith('0x'):
            cur['tails'].append(int(ar[0], 16))
        # invalidate destination register
        if ar and re.match(r'r\d+$', ar[0]) and not op.startswith(('st', 'cmp', 'b', 'mt', 'tw', 'td', 'dcb')) and op != 'lis':
            hi.pop(ar[0], None)
    print(os.path.basename(path), len(funcs), file=sys.stderr)
# function ranges: sorted starts
starts = sorted(funcs)
for i, a in enumerate(starts):
    funcs[a]['end'] = starts[i + 1] if i + 1 < len(starts) else a + 4 * funcs[a]['n']
# filter tails to those that are real func starts and outside own body
for a, f in funcs.items():
    f['tails'] = [t for t in f['tails'] if t in funcs and t != a]
    f['consts'] = sorted(f['consts'])
pickle.dump(funcs, open(OUT, 'wb'))
print('funcs', len(funcs), 'with strings', sum(1 for f in funcs.values() if f['strs']), file=sys.stderr)
