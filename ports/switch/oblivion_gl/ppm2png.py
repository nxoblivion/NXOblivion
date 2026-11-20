#!/usr/bin/env python3
# Minimal PPM(P6) -> PNG converter (stdlib only). Usage: ppm2png.py in.ppm [out.png]
import sys, zlib, struct
src = sys.argv[1]; dst = sys.argv[2] if len(sys.argv) > 2 else src.rsplit('.', 1)[0] + '.png'
d = open(src, 'rb').read()
tok, i, cur = [], 0, b''
while len(tok) < 4:
    c = d[i:i+1]; i += 1
    if c.isspace():
        if cur: tok.append(cur); cur = b''
    else: cur += c
w, h = int(tok[1]), int(tok[2]); px = d[i:i+w*h*3]
raw = b''.join(b'\x00' + px[y*w*3:(y+1)*w*3] for y in range(h))
def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(dst, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
