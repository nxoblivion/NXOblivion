#!/usr/bin/env python3
"""Find function entry points referenced from data (vtables, callback tables)
that the codegen analyzer misses (Oblivion has no RTTI so the SDK's RTTI-based
vtable scanner finds nothing). Needs tools/image.bin (see OBLIVION_DUMP_IMAGE in
src/oblivion_app.h). Prints manifest [entrypoint.functions] lines."""
import re, struct, sys
img = open('tools/image.bin', 'rb').read()
init = open('generated/default/oblivion_init.cpp').read()
funcs = set(int(m, 16) for m in re.findall(r'\{ 0x([0-9A-F]{8}), sub_', init))
manifest = open('oblivion_manifest.toml').read()
funcs |= set(int(m, 16) for m in re.findall(r'^0x([0-9A-Fa-f]{8}) = \{', manifest, re.M))
B = 0x82000000; cs, ce = 0x82110000, 0x82A4D2C0
minrun = int(sys.argv[1]) if len(sys.argv) > 1 else 2
def w(a): return struct.unpack_from('>I', img, a - B)[0]
def term(prev):
    return prev in (0x4E800020, 0x4E800420, 0x60000000, 0) or (prev >> 26) == 18 and not (prev & 1) or (prev >> 26) == 2 and False
def isptr(v): return cs <= v < ce and v % 4 == 0
new = {}
a = ce
end = len(img) + B
regions = [(B, cs), (ce, end - 4)]
for lo, hi in regions:
    a = lo
    while a < hi:
        run = []
        while a < hi and isptr(w(a)):
            run.append(w(a)); a += 4
        if len(run) >= minrun:
            for v in run:
                if v not in funcs and term(w(v - 4)):
                    new.setdefault(v, a)
        a += 4
for v in sorted(new): print(f'0x{v:08X} = {{}}')
print(len(new), 'new', file=sys.stderr)
