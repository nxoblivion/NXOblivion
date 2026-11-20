import math
def jac(a, b):
    if not a or not b: return None
    a, b = set(a), set(b)
    return len(a & b) / len(a | b)
def sim(fp, fx):
    """similarity of a PC function record and a 360 function record, 0..~3"""
    s = 0.0
    cj = jac(fp['consts'], fx['consts'])
    if cj is not None: s += 2.0 * cj
    sp = set(t for _, t in fp['strs']); sx = set(t for _, t in fx['strs'])
    if sp or sx:
        s += 2.0 * len(sp & sx) / len(sp | sx) - (0.5 if not (sp & sx) else 0)
    r = (fx['n'] + 2) / (fp['n'] + 2)
    s -= min(1.0, abs(math.log(r / 1.1)) * 0.8)
    nc1, nc2 = len(fp['calls']) + len(fp['tails']), len(fx['calls']) + len(fx['tails'])
    s -= min(0.8, abs(nc1 - nc2) / (1.0 + max(nc1, nc2)))
    return s
