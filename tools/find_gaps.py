#!/usr/bin/env python3
"""List code ranges in the image not covered by any generated function (undiscovered
code). Uses tools/image.bin and generated/default/*recomp*.cpp. Prints candidate
function starts (first non-padding word after a terminator) as manifest lines."""
import re, struct, glob, sys
img = open('tools/image.bin', 'rb').read(); B = 0x82000000
cs, ce = 0x82110000, 0x82A4D2C0
def w(a): return struct.unpack_from('>I', img, a - B)[0]
cov = bytearray((ce - cs) // 4)
owner = {}
LABEL = re.compile(r'^loc_([0-9A-F]{8}):'); DEFN = re.compile(r'DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)')
INSN = re.compile(r'^\s*// [a-z]')
for fn in glob.glob('generated/default/*recomp*.cpp'):
    cur = None
    for l in open(fn, errors='replace'):
        m = DEFN.search(l) or LABEL.match(l)
        if m:
            cur = int(m.group(1), 16)
            if DEFN.search(l): fcur = cur
            continue
        if cur is not None and INSN.match(l):
            if cs <= cur < ce: cov[(cur - cs) // 4] = 1; owner[cur] = fcur
            cur += 4
gaps = []; i = 0; n = len(cov)
while i < n:
    if not cov[i]:
        j = i
        while j < n and not cov[j]: j += 1
        gaps.append((cs + i * 4, cs + j * 4)); i = j
    else: i += 1
def isptr(v): return cs <= v < ce and v % 4 == 0
keep = 0; skip = 0
for a, b in gaps:
    words = [w(x) for x in range(a, b, 4)]
    nz = [x for x in words if x not in (0, 0x60000000)]
    if not nz or a >= 0x82A3C000: continue  # import thunk area is not code to recompile
    if sum(1 for x in nz if isptr(x)) * 10 >= len(nz) * 7: continue   # jump table
    if owner.get(a - 4) is not None and owner.get(a - 4) == owner.get(b):
        skip += 1; continue                                          # inside a function
    last = nz[-1]
    if not (last in (0x4E800020, 0x4E800420) or ((last >> 26) == 18 and not last & 1)):
        skip += 1; continue
    x = a
    while w(x) in (0, 0x60000000): x += 4
    print(f'0x{x:08X} = {{}}  # undiscovered code gap {a:08X}-{b:08X}'); keep += 1
# data-referenced candidates (vtables): keep only those not covered by generated code
import os
if os.path.exists('tools/vt_cands.txt'):
    for l in open('tools/vt_cands.txt'):
        m = re.match(r'0x([0-9A-F]{8})', l)
        if m:
            v = int(m.group(1), 16)
            if not cov[(v - cs) // 4]:
                print(f'0x{v:08X} = {{}}  # referenced from data table, not covered'); keep += 1
print(keep, 'kept', skip, 'skipped', file=sys.stderr)
