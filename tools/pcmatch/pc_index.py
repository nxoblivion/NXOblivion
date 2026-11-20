#!/usr/bin/env python3
"""Index the PC Oblivion.exe (read only; nothing from the exe is stored in the repo -- the cache goes to out/names/cache).
Produces: function starts/ends, per-function string refs / call targets / data refs, RTTI vtables with class names.
Usage: python tools/pcmatch/pc_index.py [exe]"""
import bisect, pickle, re, struct, sys, os
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_IMM, CS_OP_MEM, CS_GRP_CALL, CS_GRP_JUMP, CS_GRP_RET
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_INVALID, X86_REG_ESP, X86_REG_EBP

EXE = sys.argv[1] if len(sys.argv) > 1 else r'C:\GOG Games\Oblivion\Oblivion.exe'
OUT = os.path.join(os.path.dirname(__file__), '..', '..', 'out', 'names', 'cache', 'pc.pkl')

pe = pefile.PE(EXE)
BASE = pe.OPTIONAL_HEADER.ImageBase
secs = {}
for s in pe.sections:
    n = s.Name.rstrip(b'\0').decode()
    secs[n] = (BASE + s.VirtualAddress, s.Misc_VirtualSize, s.get_data().ljust(s.Misc_VirtualSize, b'\0'))
tx, txs, txd = secs['.text']
rd, rds, rdd = secs['.rdata']
da, das, dad = secs['.data']
TEXT = (tx, tx + txs)
def rd32(a):
    for lo, sz, d in secs.values():
        if lo <= a < lo + sz - 3:
            return struct.unpack_from('<I', d, a - lo)[0]
    return None
def insec(a, names=('.text', '.rdata', '.data')):
    for n in names:
        lo, sz, d = secs[n]
        if lo <= a < lo + sz:
            return n
def cstr(a, minlen=4, maxlen=400):
    n = insec(a, ('.rdata', '.data'))
    if not n: return None
    lo, sz, d = secs[n]
    o = a - lo
    e = d.find(b'\0', o, o + maxlen)
    if e < 0 or e - o < minlen: return None
    b = d[o:e]
    if any(c < 32 and c not in (9, 10, 13) or c > 126 for c in b): return None
    if o > 0 and d[o - 1] != 0: return None
    return b.decode('ascii')

# ---- RTTI vtables
def demangle(n):
    m = re.match(r'\.\?A[VU](.*)@@$', n)
    if not m: return n
    parts = m.group(1).split('@')
    return '::'.join(reversed(parts))
vtables = {}
for off in range(0, rds - 8, 4):
    v = struct.unpack_from('<I', rdd, off)[0]
    if not (rd <= v < rd + rds): continue
    o = v - rd
    if o + 20 > rds: continue
    sig, ofs, cdo, ptd, pchd = struct.unpack_from('<5I', rdd, o)
    if sig != 0 or insec(ptd) is None or insec(pchd) is None: continue
    tdn = cstr(ptd + 8, 4)
    if not tdn or not tdn.startswith('.?A'): continue
    vt = rd + off + 4
    ents = []
    a = vt
    while True:
        e = rd32(a)
        if e is None or not (TEXT[0] <= e < TEXT[1]): break
        ents.append(e); a += 4
    bases = []
    n = rd32(pchd + 8)
    arr = rd32(pchd + 12)
    if n and n < 64 and arr:
        for i in range(n):
            bcd = rd32(arr + 4 * i)
            if bcd:
                td = rd32(bcd)
                nm = cstr(td + 8, 3) if td else None
                if nm: bases.append(demangle(nm))
    vtables[vt] = dict(name=demangle(tdn), ofs=ofs, bases=bases, entries=ents)
print('vtables', len(vtables), file=sys.stderr)

# ---- function discovery by recursive descent
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True
strong = set()
for vt in vtables.values():
    strong.update(vt['entries'])
for lo_, sz_, d_ in (secs['.rdata'], secs['.data']):
    for off in range(0, sz_ - 3, 4):
        v = struct.unpack_from('<I', d_, off)[0]
        if TEXT[0] <= v < TEXT[1]:
            strong.add(v)
seedf = os.path.join(os.path.dirname(__file__), '..', '..', 'out', 'names', 'cache', 'obse_seeds.txt')
if os.path.exists(seedf):
    for l in open(seedf):
        w = l.split()
        if w:
            a0 = int(w[0], 16)
            if TEXT[0] <= a0 < TEXT[1]: strong.add(a0)
strong.add(BASE + pe.OPTIONAL_HEADER.AddressOfEntryPoint)
MAXSPAN = 0x8000
funcs = {}
work = sorted(strong, reverse=True)
known = set(work)
def jt_targets(disp, lo, hi):
    out = []
    a = disp
    while True:
        v = rd32(a)
        if v is None or not (lo <= v < hi): break
        out.append(v); a += 4
        if len(out) > 512: break
    return out
while work:
    s = work.pop()
    if s in funcs: continue
    seen = {}
    todo = [s]
    lo, hi = s, s + MAXSPAN
    calls = {}; strs = {}; dat = {}; tails = set(); consts = set()
    while todo:
        a = todo.pop()
        while a not in seen and TEXT[0] <= a < TEXT[1]:
            if a != s and a in known:
                tails.add(a); break
            code = txd[a - tx:a - tx + 16]
            ins = next(md.disasm(code, a), None)
            if ins is None:
                break
            seen[a] = ins
            gr = ins.groups
            nxt = a + ins.size
            if CS_GRP_RET in gr:
                break
            if CS_GRP_JUMP in gr:
                op = ins.operands[0] if ins.operands else None
                if op is not None and op.type == X86_OP_IMM:
                    t = op.imm & 0xffffffff
                    if lo <= t < hi and t not in known:
                        todo.append(t)
                    else:
                        tails.add(t)
                    if ins.mnemonic == 'jmp':
                        break
                elif op is not None and op.type == X86_OP_MEM and ins.mnemonic == 'jmp':
                    if op.mem.base == X86_REG_INVALID and op.mem.index != X86_REG_INVALID and op.mem.scale == 4:
                        todo.extend(jt_targets(op.mem.disp & 0xffffffff, lo, hi))
                    break
                elif ins.mnemonic == 'jmp':
                    break
            a = nxt
    for a, ins in seen.items():
        gr = ins.groups
        if CS_GRP_CALL in gr:
            for op in ins.operands:
                if op.type == X86_OP_IMM:
                    t = op.imm & 0xffffffff
                    if TEXT[0] <= t < TEXT[1]:
                        calls[a] = t
                        if t not in known:
                            known.add(t); work.append(t)
        vals = []
        for op in ins.operands:
            if op.type == X86_OP_IMM and CS_GRP_CALL not in gr and CS_GRP_JUMP not in gr:
                v = op.imm & 0xffffffff
                if not (0x400000 <= v < 0xB80000) and abs(op.imm) >= 0x40: consts.add(v if v < 0x80000000 else (-op.imm if op.imm < 0 else v))
            elif op.type == X86_OP_MEM and op.mem.base not in (X86_REG_ESP, X86_REG_EBP) and op.mem.base != X86_REG_INVALID and 0x40 <= op.mem.disp < 0x100000:
                consts.add(op.mem.disp)
            if op.type == X86_OP_IMM and CS_GRP_CALL not in gr and CS_GRP_JUMP not in gr:
                vals.append(op.imm & 0xffffffff)
            elif op.type == X86_OP_MEM and op.mem.disp and (op.mem.base == X86_REG_INVALID or op.mem.index == X86_REG_INVALID):
                vals.append(op.mem.disp & 0xffffffff)
        for v in vals:
            if insec(v, ('.rdata', '.data')):
                st = cstr(v)
                if st is not None: strs[a] = (v, st)
                else: dat[a] = v
    for t in tails:
        if TEXT[0] <= t < TEXT[1] and t not in known:
            known.add(t); work.append(t)
    end = max(a + i.size for a, i in seen.items()) if seen else s
    funcs[s] = dict(end=end, strs=[strs[k] for k in sorted(strs)], calls=[calls[k] for k in sorted(calls)],
                    tails=sorted(tails), consts=sorted(consts), data=[dat[k] for k in sorted(dat)], n=len(seen), strong=s in strong)
    if len(funcs) % 5000 == 0: print(len(funcs), len(work), file=sys.stderr)
print('funcs', len(funcs), file=sys.stderr)
pickle.dump(dict(base=BASE, vtables=vtables, funcs=funcs), open(OUT, 'wb'))
