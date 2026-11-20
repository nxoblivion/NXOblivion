#!/usr/bin/env python3
"""Stage 3: vtable slot alignment + call-graph propagation. Reads cache from pc_index/x360_index/match1.
Writes out/names/cache/m2.pkl with the match table x -> (pc, conf, why).   --holdout F : seed with only fraction F of string
anchors and score the propagation against the remaining ones (precision estimate)."""
import sys, os, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *
from sim import sim

vt, pc, x = load()
m1 = pickle.load(open(os.path.join(CACHE, 'm1.pkl'), 'rb'))
holdout = None
if '--holdout' in sys.argv:
    holdout = float(sys.argv[sys.argv.index('--holdout') + 1])
random.seed(7)

str_pairs = dict(m1['str_pairs'])
test_pairs = {}
if holdout is not None:
    keys = sorted(str_pairs)
    random.shuffle(keys)
    cut = int(len(keys) * holdout)
    for k in keys[cut:]:
        test_pairs[k] = str_pairs.pop(k)

M = {}      # x -> [p, conf, why]
P = {}      # p -> x
def add(xx, p, conf, why):
    if xx in M or p in P: return False
    M[xx] = [p, conf, why]; P[p] = xx
    return True

def callees(f):
    out = []
    for t in f['calls'] + f['tails']:
        if t not in out: out.append(t)
    return out

def strip(f, hide):   # remove string info (holdout evaluation)
    return f

for xx, (p, w, j) in str_pairs.items():
    conf = 0.95 if (w >= 2 and j >= 0.8) else 0.8 if (w >= 0.8 and j >= 0.5) else 0.6
    add(xx, p, conf, 'strings')
print('seeds', len(M), file=sys.stderr)

def align(Lp, Lx, anchors_ok=True, gap=-0.5):
    """global alignment of PC list Lp and 360 list Lx; returns [(ip, ix, score)]"""
    n, m = len(Lp), len(Lx)
    S = [[0.0] * m for _ in range(n)]
    for i in range(n):
        fp = pc.get(Lp[i])
        for j in range(m):
            fx = x.get(Lx[j])
            if fp is None or fx is None: S[i][j] = -9; continue
            s = sim(fp, fx)
            if M.get(Lx[j], [None])[0] == Lp[i]: s += 4
            elif Lx[j] in M or Lp[i] in P: s -= 2.5
            S[i][j] = s
    D = [[0.0] * (m + 1) for _ in range(n + 1)]
    T = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(1, n + 1):
        D[i][0] = i * gap; T[i][0] = 1
    for j in range(1, m + 1):
        D[0][j] = j * gap; T[0][j] = 2
    for i in range(1, n + 1):
        for j in range(1, m + 1):
            d = D[i - 1][j - 1] + S[i - 1][j - 1]
            u = D[i - 1][j] + gap
            l = D[i][j - 1] + gap
            if d >= u and d >= l: D[i][j] = d; T[i][j] = 0
            elif u >= l: D[i][j] = u; T[i][j] = 1
            else: D[i][j] = l; T[i][j] = 2
    i, j = n, m; out = []
    while i > 0 or j > 0:
        t = T[i][j]
        if i > 0 and j > 0 and t == 0:
            out.append((i - 1, j - 1, S[i - 1][j - 1])); i -= 1; j -= 1
        elif i > 0 and (j == 0 or t == 1): i -= 1
        else: j -= 1
    return out[::-1]

x_vts = m1['x_vts']

# ---- call-graph propagation + address-order gap filling, iterated
x_sorted = sorted(x); pc_sorted = sorted(pc)
import bisect
def propagate_callees(seeds):
    queue = list(seeds); done = set(); n0 = len(M)
    while queue:
        xx = queue.pop()
        if xx in done: continue
        done.add(xx)
        p, conf, why = M[xx]
        Lp, Lx = callees(pc[p]), callees(x[xx])
        if not Lp or not Lx or len(Lp) > 150 or len(Lx) > 150: continue
        for ip, ix, s in align(Lp, Lx):
            a, b = Lx[ix], Lp[ip]
            if a in M or b in P: continue
            if s < 1.1: continue
            c = min(conf * 0.9, 0.35 + 0.15 * min(s, 3.0))
            if add(a, b, c, 'callee'): queue.append(a)
    return len(M) - n0

def gap_fill():
    n0 = len(M)
    anchors = sorted(M)    # by x address
    for i in range(len(anchors) - 1):
        xa, xb = anchors[i], anchors[i + 1]
        pa, pb = M[xa][0], M[xb][0]
        if not (pa < pb and pb - pa < 8 * (xb - xa) + 0x400): continue
        ia, ib = bisect.bisect_right(x_sorted, xa), bisect.bisect_left(x_sorted, xb)
        jx, jp = x_sorted[ia:ib], pc_sorted[bisect.bisect_right(pc_sorted, pa):bisect.bisect_left(pc_sorted, pb)]
        jx = [a for a in jx if a not in M]; jp = [b for b in jp if b not in P]
        if not jx or not jp or len(jx) > 60 or len(jp) > 80: continue
        base = min(M[xa][1], M[xb][1])
        for ip, ix, s in align(jp, jx):
            a, b = jx[ix], jp[ip]
            thr = 0.3 if (len(jx) == 1 and len(jp) == 1) else 1.0
            if s >= thr and a not in M and b not in P:
                add(a, b, min(base * 0.85, 0.3 + 0.15 * min(s, 3.0)), 'gap')
    return len(M) - n0

# vtable references (ctors / dtors): function -> vtables it stores
pc_vt_refs = collections.defaultdict(set)   # pc vtable -> funcs
for f, v in pc.items():
    for d in v['data']:
        if d in vt: pc_vt_refs[d].add(f)
x_vt_refs = collections.defaultdict(set)
for f, v in x.items():
    for d in v['data']:
        if d in x_vts: x_vt_refs[d].add(f)
pc_slot_of = collections.defaultdict(list)    # pc fn -> [(vtable, slot)]
for v, d in vt.items():
    for i, e in enumerate(d['entries']): pc_slot_of[e].append((v, i))
vt_pairs = dict(m1['vt_pairs'])

def extend_vtables():
    """pair more 360 vtables with PC vtables by voting with matched slot functions"""
    n0 = len(vt_pairs)
    used_p = set(vt_pairs.values())
    cand = {}
    for v, ents in x_vts.items():
        if v in vt_pairs: continue
        votes = collections.Counter()
        for i, e in enumerate(ents):
            if e in M and x[e]['n'] > 4:
                for pv, pi in pc_slot_of.get(M[e][0], ()):
                    if vt[pv]['ofs'] == 0 and abs(pi - i) <= 4 and pv not in used_p: votes[pv] += 1
        if votes:
            pv, c = votes.most_common(1)[0]
            if c >= 2 or (c >= 1 and len(ents) <= 6):
                if len(votes) == 1 or votes.most_common(2)[1][1] < c:
                    cand[v] = (pv, c)
    # mutual best
    byp = collections.defaultdict(list)
    for v, (pv, c) in cand.items(): byp[pv].append((c, v))
    for pv, l in byp.items():
        l.sort(reverse=True)
        if len(l) > 1 and l[0][0] == l[1][0]: continue
        vt_pairs[l[0][1]] = pv
    return len(vt_pairs) - n0

aligned = set()
def align_vtables():
    n0 = len(M)
    for v, p in vt_pairs.items():
        if v in aligned: continue
        aligned.add(v)
        xe, pe = x_vts[v], vt[p]['entries']
        for ip, ix, s in align(pe, xe):
            if s >= 0.6 or (s >= 0.0 and ip == ix and abs(len(pe) - len(xe)) < 3):
                add(xe[ix], pe[ip], 0.7 if s >= 1.2 else 0.5, 'vtable-slot')
        # constructors / destructors: functions that store the vtable
        rx = sorted(x_vt_refs.get(v, ())); rp = sorted(pc_vt_refs.get(p, ()))
        rx = [a for a in rx if a not in M]; rp = [b for b in rp if b not in P]
        if rx and rp and len(rx) <= 8 and len(rp) <= 8:
            for ip, ix, s in align(rp, rx):
                if s >= 0.4 or (len(rx) == 1 and len(rp) == 1 and s >= -0.5):
                    add(rx[ix], rp[ip], 0.6, 'vtable-ref')
    return len(M) - n0

for rnd in range(8):
    n0 = extend_vtables()
    n1 = align_vtables()
    n2 = propagate_callees(list(M))
    n3 = gap_fill()
    print('round', rnd, 'vtpairs+', n0, 'vt', n1, 'callee', n2, 'gap', n3, 'total', len(M), file=sys.stderr)
    if n0 + n1 + n2 + n3 == 0: break
pickle.dump(dict(M=M, vt_pairs=vt_pairs), open(os.path.join(CACHE, 'm2.pkl' if holdout is None else 'm2_holdout.pkl'), 'wb'))

if holdout is not None:
    ok = bad = miss = 0
    for xx, (p, w, j) in test_pairs.items():
        if xx in M:
            if M[xx][0] == p: ok += 1
            else: bad += 1
        else: miss += 1
    print('holdout: right', ok, 'wrong', bad, 'not found', miss)
    byconf = collections.defaultdict(lambda: [0, 0])
    for xx, (p, w, j) in test_pairs.items():
        if xx in M and M[xx][2] != 'strings':
            byconf[round(M[xx][1], 1)][M[xx][0] == p] += 1
    print(sorted(byconf.items()))
