#!/usr/bin/env python3
"""Extract named PC addresses for Oblivion 1.2.0.416 from the OBSE sources (v1.2.416 preprocessor branches only).
Writes out/names/cache/obse_names.txt: `00XXXXXX Name kind`, and (for the PC indexer) obse_seeds.txt.
Usage: python tools/pcmatch/obse_names.py [obse_src_dir]"""
import glob, os, re, sys
SRC = sys.argv[1] if len(sys.argv) > 1 else r'C:\GOG Games\Oblivion\src\obse\obse'
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'out', 'names', 'cache')
TARGET = 'OBLIVION_VERSION_1_2_416'
re_addr = re.compile(r'\b0x00([4-9A-Fa-f][0-9A-Fa-f]{5})\b')
re_def = re.compile(r'^[A-Za-z_][\w:<>\*&, ]*?\b((?:\w+::)+~?\w+|\w+)\s*\([^;]*$')

def clean_const(n):
    n = re.sub(r'^(s_|k|g_|_)', '', n)
    n = re.sub(r'(Addr|_Addr|Address)$', '', n)
    return n

def process(path):
    out = []
    stack = []   # entries: [active, taken, isver]
    cur_fn = None
    for raw in open(path, encoding='latin-1'):
        line = raw.rstrip('\n')
        st = line.strip()
        if st.startswith('#'):
            d = st[1:].strip()
            if d.startswith('if'):
                if 'OBLIVION_VERSION' in d and '==' in d:
                    v = d.split('==')[-1].strip().split()[0]
                    act = (v == TARGET)
                    stack.append([act, act, True])
                elif re.match(r'if\s+0\b', d):
                    stack.append([False, False, False])
                else:
                    stack.append([True, True, False])
            elif d.startswith('elif') and stack:
                if 'OBLIVION_VERSION' in d and '==' in d:
                    v = d.split('==')[-1].strip().split()[0]
                    act = (v == TARGET) and not stack[-1][1]
                    stack[-1][0] = act; stack[-1][1] = stack[-1][1] or act
                else:
                    stack[-1][0] = not stack[-1][1]
            elif d.startswith('else') and stack:
                stack[-1][0] = not stack[-1][1]; stack[-1][1] = True
            elif d.startswith('endif') and stack:
                stack.pop()
            continue
        if stack and not all(s[0] for s in stack):
            continue
        if line and not line[0].isspace() and not st.startswith(('//', '{', '}')):
            m = re_def.match(line)
            if m and '::' in m.group(1): cur_fn = m.group(1)
            elif m and not st.startswith(('typedef', 'class', 'struct', 'static const', 'const', 'extern')): cur_fn = m.group(1)
        for m in re_addr.finditer(line):
            a = int(m.group(1), 16) | 0x400000 if False else int('00' + m.group(1), 16)
            name = None; kind = None
            s2 = line.split('//')[0]
            mm = re.search(r'(\w+)\s*(?:\[\])?\s*=\s*(?:\([^)]*\)\s*)?0x00' + m.group(1), s2)
            if mm:
                name = clean_const(mm.group(1)); kind = 'const'
            elif re.search(r'(Call|Jump|StdCall|ThisCall|CdeclCall|FastCall)\w*\s*\(\s*0x00' + m.group(1), s2) and cur_fn:
                name = cur_fn; kind = 'call'
            if name:
                out.append((a, name, kind, os.path.basename(path)))
    return out

res = []
for p in sorted(glob.glob(os.path.join(SRC, '*.cpp')) + glob.glob(os.path.join(SRC, '*.h'))):
    if os.path.basename(p).startswith('Hooks_'): continue
    res.extend(process(p))
os.makedirs(OUT, exist_ok=True)
seen = {}
for a, n, k, f in res:
    seen.setdefault(a, []).append((n, k, f))
with open(os.path.join(OUT, 'obse_names.txt'), 'w') as f:
    for a in sorted(seen):
        f.write('%08X %s %s %s\n' % (a, seen[a][0][0], seen[a][0][1], seen[a][0][2]))
with open(os.path.join(OUT, 'obse_seeds.txt'), 'w') as f:
    for a in sorted(seen): f.write('%08X\n' % a)
print(len(seen), 'addresses')
