#!/usr/bin/env python3
"""Virtual slot names from the OBSE class declarations (headers), validated against PC RTTI vtable lengths.
Output pickle out/names/cache/vslots.pkl: {class: {slot: name}}."""
import glob, os, re, sys, pickle
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *
SRC = r'C:\GOG Games\Oblivion\src\obse\obse'
vt, pc, x = load()
primary = {}
for v, d in vt.items():
    if d['ofs'] == 0: primary.setdefault(d['name'], d)

def strip_comments(t):
    t = re.sub(r'/\*.*?\*/', '', t, flags=re.S)
    return re.sub(r'//[^\n]*', lambda m: '', t)

classes = {}   # name -> (base, [virtual names in order])
re_cls = re.compile(r'\b(?:class|struct)\s+(\w+)\s*(?::\s*public\s+(\w+(?:<[^>{]*>)?))?\s*\{')
for p in glob.glob(os.path.join(SRC, '*.h')):
    t = strip_comments(open(p, encoding='latin-1').read())
    # drop version-specific #if branches other than 416 (rare in headers) -- ignore
    for m in re_cls.finditer(t):
        name, base = m.group(1), m.group(2)
        i = m.end(); depth = 1; body = []
        j = i
        while j < len(t) and depth:
            c = t[j]
            if c == '{': depth += 1
            elif c == '}': depth -= 1
            j += 1
        body = t[i:j - 1]
        # keep depth-0 text only
        flat = []; d = 0
        for k, c in enumerate(body):
            if c == '{': d += 1
            elif c == '}': d -= 1
            elif d == 0: flat.append(c)
        flat = ''.join(flat)
        virt = []
        for stmt in flat.split(';'):
            sm = re.search(r'\bvirtual\b(.*)', stmt, flags=re.S)
            if not sm: continue
            s = sm.group(1)
            mm = re.search(r'(~?\w+)\s*\(', s)
            if mm:
                virt.append(mm.group(1))
        if name not in classes or len(virt) > len(classes[name][1]):
            classes[name] = (base, virt)
print(len(classes), 'classes parsed')

def chain(c):
    out = []
    while c in classes and c not in out:
        out.append(c); c = classes[c][0]
    return out

res = {}; ok = bad = 0
def slots_for(c, memo={}):
    """returns (names list indexed by slot) or None"""
    if c in memo: return memo[c]
    memo[c] = None
    if c not in classes or c not in primary: return None
    base, virt = classes[c]
    if base is None:
        names = []
    else:
        names = slots_for(base)
        if names is None: return None
        names = list(names)
    n_vt = len(primary[c]['entries'])
    best = None
    for variant in (0, 1):     # 0: a re-used name is an override of the inherited slot; 1: every declaration is a new slot
        new = []
        for n in virt:
            if variant == 0 and ((n in names and n != 'Destructor') or n.startswith('~') or (n == 'Destructor' and names)):
                continue
            if variant == 1 and (n.startswith('~') or (n == 'Destructor' and names)):
                continue
            new.append(n)
        cand = names + new
        if len(cand) > n_vt: continue
        score = 0
        for i, n in enumerate(cand):
            mm = re.fullmatch(r'Unk_([0-9A-Fa-f]{2})', n)
            if mm: score += 1 if int(mm.group(1), 16) == i else -1
        if best is None or score > best[0]: best = (score, cand)
    if best is None:
        memo[c] = None
        return None
    names = best[1]
    memo[c] = names
    return names
for c in classes:
    r = slots_for(c)
    if r: res[c] = {i: n for i, n in enumerate(r)}; ok += 1
    elif c in primary: bad += 1
print('validated classes', ok, 'rejected', bad)
pickle.dump(res, open(os.path.join(CACHE, 'vslots.pkl'), 'wb'))
for c in ('NiObject', 'NiAVObject', 'NiNode', 'TESForm', 'NiRenderer', 'NiDX9Renderer'):
    print(c, c in res, list(res.get(c, {}).values())[:30])
