#!/usr/bin/env python3
"""Stage 1+2: string anchors and vtable (NiRTTI) pairing between PC and 360. Pickles state to out/names/cache/m1.pkl."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *

vt, pc, x = load()
I = img()
END = B + len(I)

# ---------------------------------------------------------------- NiRTTI static initialisers (class name <-> ms_RTTI object)
def rtti_inits(F, tailok):
    out = []
    for a, f in F.items():
        tg = f['calls'] + (f['tails'] if tailok else [])
        if f['n'] <= 10 and len(f['strs']) == 1 and len(tg) == 1 and 1 <= len(f['data']) <= 3:
            out.append((a, f, tg[0]))
    c = collections.Counter(t for _, _, t in out)
    top = c.most_common(1)[0][0]
    return [(a, f) for a, f, t in out if t == top]

pc_rt, x_rt = {}, {}          # ms_RTTI object address -> class name
pc_rt_parent, x_rt_parent = {}, {}
for a, f in rtti_inits(pc, False):
    pc_rt[f['data'][-1]] = f['strs'][0][1]
    if len(f['data']) >= 2: pc_rt_parent[f['data'][-1]] = f['data'][0]
for a, f in rtti_inits(x, True):
    x_rt[f['data'][-1]] = f['strs'][0][1]
    if len(f['data']) >= 2: x_rt_parent[f['data'][-1]] = f['data'][0]

# ---------------------------------------------------------------- 360 vtables
refs = collections.defaultdict(list)
for f, v in x.items():
    for d in v['data']: refs[d].append(f)
x_vts = {}
a = B + 0x1000
hi = 0x82110000
while a < hi:
    r = []
    while a < hi and w32(a) in x:
        r.append(w32(a)); a += 4
    if len(r) >= 2:
        s = a - 4 * len(r)
        cuts = [s] + sorted(d for d in refs if s < d < a and (d - s) % 4 == 0)
        for i, c in enumerate(cuts):
            e = cuts[i + 1] if i + 1 < len(cuts) else a
            if e - c >= 8: x_vts[c] = [w32(c + 4 * k) for k in range((e - c) // 4)]
    a += 4
x_vt_name = {}
for v, ents in x_vts.items():
    f = x[ents[1]]
    for d in f['data']:
        if d in x_rt and f['n'] <= 6:
            x_vt_name[v] = x_rt[d]

# PC vtables: primary (ofs==0) by class name
pc_vt_by_name = {}
for v, d in vt.items():
    if d['ofs'] == 0: pc_vt_by_name.setdefault(d['name'], v)

# ---------------------------------------------------------------- vtable pairs by class name
vt_pairs = {}   # x vtable -> pc vtable
for v, n in x_vt_name.items():
    if n in pc_vt_by_name: vt_pairs[v] = pc_vt_by_name[n]
print('x vtables', len(x_vts), 'named', len(x_vt_name), 'paired by name', len(vt_pairs), file=sys.stderr)

# slot pairs (pc fn -> x fn) with votes
slot_votes = collections.defaultdict(collections.Counter)  # pc fn -> Counter(x fn)
for v, p in vt_pairs.items():
    xe, pe = x_vts[v], vt[p]['entries']
    for i in range(min(len(xe), len(pe))):
        slot_votes[pe[i]][xe[i]] += 1

# ---------------------------------------------------------------- string anchors
def idx(F):
    m = collections.defaultdict(set)
    for a, f in F.items():
        for _, s in f['strs']: m[s].add(a)
    return m
pm, xm = idx(pc), idx(x)
votes = collections.defaultdict(float)
for s in set(pm) & set(xm):
    np_, nx_ = len(pm[s]), len(xm[s])
    if np_ > 4 or nx_ > 4: continue
    w = 1.0 / (np_ * nx_) * min(1.0, len(s) / 12.0)
    for p in pm[s]:
        for xx in xm[s]:
            votes[(p, xx)] += w
best_p, best_x = {}, {}
for (p, xx), w in votes.items():
    if w > best_p.get(p, (0, None))[0]: best_p[p] = (w, xx)
    if w > best_x.get(xx, (0, None))[0]: best_x[xx] = (w, p)
str_pairs = {}
for (p, xx), w in votes.items():
    if best_p[p][1] == xx and best_x[xx][1] == p:
        sp = set(s for _, s in pc[p]['strs']); sx = set(s for _, s in x[xx]['strs'])
        jac = len(sp & sx) / max(1, len(sp | sx))
        str_pairs[xx] = (p, w, jac)
print('string anchor pairs', len(str_pairs), file=sys.stderr)
import pickle
pickle.dump(dict(x_vts=x_vts, x_vt_name=x_vt_name, vt_pairs=vt_pairs, str_pairs=str_pairs, pc_rt=pc_rt, x_rt=x_rt,
                 pc_rt_parent=pc_rt_parent, x_rt_parent=x_rt_parent), open(os.path.join(CACHE, 'm1.pkl'), 'wb'))

# ---------------------------------------------------------------- agreement between the two evidence kinds
agree = dis = 0
for xx, (p, w, j) in str_pairs.items():
    c = slot_votes.get(p)
    if c:
        if xx in c: agree += 1
        else: dis += 1
print('string pair vs vtable slot: agree', agree, 'disagree', dis)
ratios = [x[xx]['n'] / pc[p]['n'] for xx, (p, w, j) in str_pairs.items() if j > 0.8 and pc[p]['n'] > 20]
ratios.sort(); print('n ratio quantiles', [round(ratios[int(len(ratios) * q)], 2) for q in (0.1, 0.25, 0.5, 0.75, 0.9)])
