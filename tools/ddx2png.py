"""Decode Oblivion Xbox 360 .ddx (XDDO, uncompressed) textures to PNG.

Usage: ddx2png.py <in.ddx> <out.png>
Handles tiled DXT1/DXT3/DXT5 with k8in16 endianness (top mip only).
"""
import struct
import sys

from PIL import Image

FMT_DXT1, FMT_DXT3, FMT_DXT5 = 18, 19, 20


def tiled_offset_2d_outer(y, width, log2_bpp):
    macro = ((y >> 5) * (width >> 5)) << (log2_bpp + 7)
    micro = ((y & 6) << 2) << log2_bpp
    return macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2_bpp)) + ((y & 1) << 4)


def tiled_offset_2d_inner(x, y, log2_bpp, base):
    macro = (x >> 5) << (log2_bpp + 7)
    micro = (x & 7) << log2_bpp
    offset = base + (macro + ((micro & ~0xF) << 1) + (micro & 0xF))
    return (((offset & ~0x1FF) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F)
            + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6))


def rgb565(c):
    return (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)


def color_block(b, dxt1):
    c0, c1, bits = struct.unpack('<HHI', b)
    p0, p1 = rgb565(c0), rgb565(c1)
    if c0 > c1 or not dxt1:
        pal = [p0, p1, tuple((2 * a + c) // 3 for a, c in zip(p0, p1)), tuple((a + 2 * c) // 3 for a, c in zip(p0, p1))]
    else:
        pal = [p0, p1, tuple((a + c) // 2 for a, c in zip(p0, p1)), (0, 0, 0)]
    return [pal[(bits >> (2 * i)) & 3] for i in range(16)]


def decode(path):
    d = open(path, 'rb').read()
    name_len = struct.unpack('>I', d[4:8])[0]
    o = 8 + name_len + 1
    d0, d1, d2 = struct.unpack('>III', d[o + 15:o + 27])
    fmt = d1 & 0x3F
    w, h = (d2 & 0x1FFF) + 1, ((d2 >> 13) & 0x1FFF) + 1
    tiled = d0 >> 31
    data = bytearray(d[o + 39:])
    # k8in16: swap bytes within each 16-bit word
    data[0::2], data[1::2] = data[1::2], data[0::2]
    bw, bh = (w + 3) // 4, (h + 3) // 4
    pitch = (bw + 31) & ~31
    bsize = 8 if fmt == FMT_DXT1 else 16
    log2 = 3 if bsize == 8 else 4
    img = Image.new('RGBA', (w, h))
    px = img.load()
    for by in range(bh):
        outer = tiled_offset_2d_outer(by, pitch, log2) if tiled else 0
        for bx in range(bw):
            off = tiled_offset_2d_inner(bx, by, log2, outer) if tiled else (by * pitch + bx) * bsize
            blk = data[off:off + bsize]
            if len(blk) < bsize:
                continue
            if fmt == FMT_DXT1:
                cols, alphas = color_block(blk, True), [255] * 16
            elif fmt == FMT_DXT3:
                a = int.from_bytes(blk[:8], 'little')
                alphas = [((a >> (4 * i)) & 15) * 17 for i in range(16)]
                cols = color_block(blk[8:], False)
            else:
                a0, a1 = blk[0], blk[1]
                bits = int.from_bytes(blk[2:8], 'little')
                if a0 > a1:
                    tab = [a0, a1] + [((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)]
                else:
                    tab = [a0, a1] + [((4 - i) * a0 + (i + 1) * a1) // 5 for i in range(4)] + [0, 255]
                alphas = [tab[(bits >> (3 * i)) & 7] for i in range(16)]
                cols = color_block(blk[8:], False)
            for i in range(16):
                x, y = bx * 4 + (i & 3), by * 4 + (i >> 2)
                if x < w and y < h:
                    px[x, y] = cols[i] + (alphas[i],)
    return img, fmt, w, h


if __name__ == '__main__':
    img, fmt, w, h = decode(sys.argv[1])
    img.save(sys.argv[2])
    print(sys.argv[1], 'fmt', fmt, w, 'x', h)
