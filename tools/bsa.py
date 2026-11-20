"""Minimal Oblivion (v103) BSA lister/extractor. Usage: bsa.py <bsa> list|extract <substr> <outdir>"""
import struct, sys, zlib, os
BS = chr(92)

def read(path):
    f = open(path, 'rb')
    magic, ver, off, aflags, nfold, nfile, lfold, lfile, fflags = struct.unpack('<4sIIIIIIII', f.read(36))
    assert magic == b'BSA\0', magic
    compressed_default = bool(aflags & 4)
    f.seek(off)
    folders = [struct.unpack('<QII', f.read(16)) for _ in range(nfold)]
    entries = []
    for _, count, _ in folders:
        n = f.read(1)[0]
        name = f.read(n)[:-1].decode('latin1')
        for _ in range(count):
            h, size, foff = struct.unpack('<QII', f.read(16))
            entries.append([name, size, foff])
    names = f.read(lfile).split(b'\0')
    for e, n in zip(entries, names):
        e.insert(1, n.decode('latin1'))
    return f, entries, compressed_default

def data(f, e, comp_default):
    folder, name, size, off = e
    comp = comp_default ^ bool(size & 0x40000000)
    size &= 0x3FFFFFFF
    f.seek(off); raw = f.read(size)
    return zlib.decompress(raw[4:]) if comp else raw

if __name__ == '__main__':
    f, ents, cd = read(sys.argv[1])
    if sys.argv[2] == 'list':
        for e in ents: print(e[0] + BS + e[1], e[2] & 0x3FFFFFFF)
    else:
        sub = sys.argv[3].lower(); out = sys.argv[4]
        for e in ents:
            p = e[0] + BS + e[1]
            if sub in p.lower():
                d = os.path.join(out, *p.split(BS)); os.makedirs(os.path.dirname(d), exist_ok=True)
                open(d, 'wb').write(data(f, e, cd)); print(p)
