import os, pickle, struct, re, collections
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
CACHE = os.path.join(ROOT, 'out', 'names', 'cache')
B = 0x82000000
def load():
    d = pickle.load(open(os.path.join(CACHE, 'pc.pkl'), 'rb'))
    x = pickle.load(open(os.path.join(CACHE, 'x360.pkl'), 'rb'))
    pc = d['funcs']
    for F in (pc, x):
        for f in F.values():
            f['calls'] = [t for t in f['calls'] if t in F]
            f['tails'] = [t for t in f['tails'] if t in F]
    return d['vtables'], pc, x
_img = None
def img():
    global _img
    if _img is None: _img = open(os.path.join(ROOT, 'tools', 'image.bin'), 'rb').read()
    return _img
def w32(a):
    o = a - B
    return struct.unpack_from('>I', img(), o)[0]
