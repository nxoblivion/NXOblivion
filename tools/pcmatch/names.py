#!/usr/bin/env python3
"""Final stage: derive PC names (OBSE, RTTI vtable slots, ctors) and transfer them to matched 360 functions.
Also names 360 virtual functions from the 360 vtables themselves (NiRTTI class names) and adds string hints.
Writes out/names/oblivion360_names.txt   (82XXXXXX <name> <high|med|low> <evidence>)
        out/names/oblivion360_matches.txt (360 addr <-> PC addr for every match, named or not)"""
import sys, os, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *

vt, pc, x = load()
m1 = pickle.load(open(os.path.join(CACHE, 'm1.pkl'), 'rb'))
m2 = pickle.load(open(os.path.join(CACHE, 'm2.pkl'), 'rb'))
M, vt_pairs = m2['M'], m2['vt_pairs']
vslots = pickle.load(open(os.path.join(CACHE, 'vslots.pkl'), 'rb'))
OUTDIR = os.path.join(ROOT, 'out', 'names')

def pretty(n):
    if '?$' not in n: return n
    parts = n.split('::')
    t = [q for q in parts if q.startswith('?$')]
    args = [re.sub(r'^[VUPA]+(?=[A-Z])', '', q) for q in reversed([q for q in parts if q and not q.startswith('?$')])]
    base = t[-1][2:]
    if len(t) == 1 and args and all(re.fullmatch(r'\w+', q) for q in args) and len(args) <= 3:
        return f'{base}<{",".join(args)}>'
    return base + '<>'

primary = {}
for v, d in vt.items():
    if d['ofs'] == 0: primary.setdefault(d['name'], v)

# ---------------------------------------------------------------- PC names
pc_name = {}      # pc fn -> (name, kind)   kind: obse | vslot | ctor | vfunc
junk = re.compile(r'^(ctr|patch)$|_Hook|_Retn|Hook$|Retn$')
for l in open(os.path.join(CACHE, 'obse_names.txt')):
    a, n, k, f = l.split()
    a = int(a, 16)
    if a in pc and not junk.search(n) and len(n) > 3:
        pc_name.setdefault(a, (n, 'obse'))

def parent_of(cls):
    d = vt[primary[cls]]
    return d['bases'][1] if len(d['bases']) > 1 else None
def slot_owner(cls, i, f):
    """highest ancestor whose primary vtable has f at slot i"""
    owner = cls; c = parent_of(cls) if cls in primary else None
    guard = 0
    while c and c in primary and guard < 30:
        e = vt[primary[c]]['entries']
        if i < len(e) and e[i] == f: owner = c
        else: break
        c = parent_of(c); guard += 1
    return owner
fn_owners = collections.defaultdict(set)
slot_info = {}
for cls, v in primary.items():
    for i, f in enumerate(vt[v]['entries']):
        o = slot_owner(cls, i, f)
        fn_owners[f].add((o, i))
for f, owners in fn_owners.items():
    # lineage check: all owners the same class -> unambiguous
    if len(owners) == 1 and pc[f]['n'] > 3:
        o, i = next(iter(owners))
        sn = vslots.get(o, {}).get(i)
        if sn and not sn.startswith('Unk_'):
            pc_name.setdefault(f, (f'{pretty(o)}::{sn}', 'vslot'))
        else:
            pc_name.setdefault(f, (f'{pretty(o)}::vfunc{i:02X}', 'vfunc'))
# constructors / destructors: functions that store a primary vtable. Destructors = those reached from a slot-0 function
# (scalar deleting dtor) through calls, the rest are constructors.
pc_vt_refs = {}
for f, v in pc.items():
    refs = [d for d in v['data'] if d in vt and vt[d]['ofs'] == 0]
    if refs: pc_vt_refs[f] = refs
dtors = set()
work = [vt[v]['entries'][0] for v in primary.values() if vt[v]['entries'] and vt[v]['entries'][0] in pc]
seen = set()
while work:
    f = work.pop()
    if f in seen: continue
    seen.add(f)
    for t in pc[f]['calls']:
        if t in pc_vt_refs and t not in dtors:
            dtors.add(t); work.append(t)
for f, refs in pc_vt_refs.items():
    cls = pretty(vt[refs[-1]]['name'])
    if f in pc_name or f in fn_owners or pc[f]['n'] > 200 or len(set(refs)) > 2: continue
    pc_name[f] = (f'{cls}::~{cls.split("::")[-1]}', 'dtor') if f in dtors else (f'{cls}::ctor', 'ctor')
print('pc names', len(pc_name), collections.Counter(k for _, k in pc_name.values()), file=sys.stderr)

# ---------------------------------------------------------------- 360 side
names = {}   # x -> (name, conf, evidence, rank)
def setname(a, n, conf, ev, rank):
    cur = names.get(a)
    if cur is None or rank > cur[3]:
        names[a] = (n, conf, ev, rank)
def cname(c): return 'high' if c >= 0.85 else 'med' if c >= 0.6 else 'low' if c >= 0.4 else None

# (a) transfer from matched PC functions
for xx, (p, c, why) in M.items():
    pn = pc_name.get(p)
    cn = cname(c)
    if pn and cn:
        kind = pn[1]
        rank = (70 if kind in ('obse', 'vslot') else 60 if kind in ('ctor', 'dtor') else 30)
        cn2 = cn
        setname(xx, pn[0], cn2, f'{why} pc={p:08X} ({kind})', rank + int(c * 10))

# (b) 360 vtables with a known class: NiRTTI names (high) or the PC class of the paired vtable (med)
x_vts, x_vt_name = m1['x_vts'], m1['x_vt_name']
x_class = {}     # x vtable -> (class, source)
for v, cls in x_vt_name.items(): x_class[v] = (cls, 'NiRTTI')
for v, pv in vt_pairs.items():
    if v not in x_class: x_class[v] = (pretty(vt[pv]['name']), 'paired')
x_by_name = {}
for v, (cls, src) in x_class.items():
    if cls not in x_by_name or src == 'NiRTTI': x_by_name[cls] = v
rt_parent_name = {}
for obj, par in m1['x_rt_parent'].items():
    if obj in m1['x_rt'] and par in m1['x_rt']: rt_parent_name[m1['x_rt'][obj]] = m1['x_rt'][par]
pc_par = {pretty(n): (pretty(parent_of(n)) if parent_of(n) else None) for n in primary}
def alias(c):
    # the 360 renderer classes are NiXenon* where the PC has NiDX9*
    return 'NiDX9' + c[7:] if c.startswith('NiXenon') and 'NiDX9' + c[7:] in primary else None
for _c in {c for c, _s in x_class.values()}:
    if _c not in pc_par and alias(_c): pc_par[_c] = pc_par.get(alias(_c)) if alias(_c) in pc_par else None
    if _c in rt_parent_name and _c not in pc_par: pass
def slot_name(cls, i):
    """OBSE name of virtual slot i of cls, looked up in cls and its ancestors; ancestors must have the same vtable length on both platforms"""
    c = cls; guard = 0
    while c and guard < 30:
        if c not in vslots and alias(c) in vslots:
            pa = parent_of(alias(c)); lim = len(vt[primary[pa]]['entries']) if pa in primary else 0
            if i >= lim: return None
            c = alias(c)
        if c in vslots and i in vslots[c]:
            sn = vslots[c][i]
            ok = True
            if c in primary and c in x_by_name:
                ok = len(vt[primary[c]]['entries']) == len(x_vts[x_by_name[c]])
            if ok and not sn.startswith('Unk_'): return sn
            return None
        c = rt_parent_name.get(c) if rt_parent_name.get(c) in vslots else (pc_par.get(c) or rt_parent_name.get(c)); guard += 1
    return None

x_owner = collections.defaultdict(set)
for v, (cls, src) in x_class.items():
    ents = x_vts[v]
    par = rt_parent_name.get(cls) or pc_par.get(cls) or (pc_par.get(alias(cls)) if alias(cls) else None)
    pv = x_by_name.get(par)
    for i, f in enumerate(ents):
        if pv is not None and i < len(x_vts[pv]) and x_vts[pv][i] == f:
            continue    # inherited: named under the parent
        x_owner[f].add((cls, i, src))
for f, owners in x_owner.items():
    if len({(c, i) for c, i, s_ in owners}) == 1 and x[f]['n'] > 3:
        cls, i, src = next(iter(owners))
        conf = 'high' if src == 'NiRTTI' else 'med'
        sn = slot_name(cls, i)
        if sn:
            if cls not in primary: conf = 'med' if conf == 'high' else conf
            setname(f, f'{cls}::{sn}', conf, f'360 vtable {cls} slot {i} (OBSE slot name, {src})', 80 if conf == 'high' else 45)
        else:
            setname(f, f'{cls}::vfunc{i:02X}', conf, f'360 vtable {cls} slot {i} ({src})', 40 if conf == 'high' else 35)

# (c) string hints: Class::Method inside referenced strings (only when exactly one distinct candidate)
rx = re.compile(r'\b([A-Z][A-Za-z0-9_]+::~?[A-Za-z_][A-Za-z0-9_]*)')
for a, f in x.items():
    if a in names: continue
    cands = set()
    for _, s in f['strs']:
        if len(s) < 120:
            for m in rx.finditer(s): cands.add(m.group(1))
    if len(cands) == 1:
        setname(a, next(iter(cands)), 'low', 'string mentions it', 10)

# (d) manual annotations (hot list review): addr name conf evidence
man = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'manual_names.txt')
if os.path.exists(man):
    for l in open(man):
        l = l.strip()
        if not l or l[0] == '#': continue
        a, n, c, ev = l.split(None, 3)
        setname(int(a, 16), n, c, 'manual: ' + ev, 200)

# (e) context tier: unnamed functions whose callers (1..3) are all named in the same class become `Class::Caller.callee`
callers = collections.defaultdict(set)
for a, f in x.items():
    for t in set(f['calls'] + f['tails']):
        if t != a: callers[t].add(a)
def cls_of(n): return n.rsplit('::', 1)[0] if '::' in n else None
for rnd in range(2):
    new = {}
    for a in x:
        if a in names or a in new: continue
        cs = callers.get(a)
        if not cs or len(cs) > 3: continue
        if not all(c in names and names[c][1] in ('high', 'med') or (rnd and c in names) for c in cs): continue
        cl = {cls_of(names[c][0]) for c in cs}
        if len(cl) != 1 or None in cl: continue
        c0 = min(cs)
        new[a] = (f'{names[c0][0].split(".callee")[0]}.callee', 'low', f'sole caller(s) {", ".join("%08X" % c for c in sorted(cs))}', 1)
    names.update(new)
    print('context names round', rnd, len(new), file=sys.stderr)

os.makedirs(OUTDIR, exist_ok=True)
with open(os.path.join(OUTDIR, 'oblivion360_names.txt'), 'w') as f:
    for a in sorted(names):
        n, c, ev, _ = names[a]
        f.write(f'{a:08X} {n} {c} {ev}\n')
with open(os.path.join(OUTDIR, 'oblivion360_matches.txt'), 'w') as f:
    for a in sorted(M):
        p, c, why = M[a]
        f.write(f'{a:08X} pc={p:08X} conf={c:.2f} {why}\n')
cnt = collections.Counter(v[1] for v in names.values())
print('360 names', len(names), dict(cnt), 'matched', len(M), file=sys.stderr)
print('PC functions known', len(pc), 'with RTTI vtables', len(vt), file=sys.stderr)
