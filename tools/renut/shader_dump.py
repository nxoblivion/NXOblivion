#!/usr/bin/env python3
"""The shader containers reNut's translator builds its shader cache from, for Oblivion (docs/renut-port.md, milestone 5).

reNut looks a shader up by the XXH3-64 of its container as D3DDevice_Create*Shader receives it (header + physical
block) and expects <out>/{vs,ps}_<xxh3>.bin. Oblivion's containers are XDK 2.0.2417's version 0x0E (magic 0x102A0E0x,
low bit 1 = vertex shader), the version Feeding Frenzy's and Zuma's XenosRecomp (container_0e.h) converts. Sources:
  - Data/Shaders/shaderpackage.sdp: 531 entries {char name[256]; u32 size; container}, big endian, after a 12-byte header
    {u32 100; u32 count; u32 payload size};
  - containers stored whole in the other files of Data/Shaders (Gamebryo's fragment cache
    Fragments/Cached/DefaultCache.psc: 22, 19 of them created at run time) and in the executable image (tools/image.bin:
    none), found by their header;
  - folders of containers a run dumped (reNut's shader_capture/, or the trace build's), copied as they are.
The three shaders the game compiles at boot from source text in the executable (Bink's YUV -> RGB: sub_8211C3A8) are in
no file; their hashes have port shaders of their own.

  python tools/renut/shader_dump.py [--game assets] [--image tools/image.bin] [--dumps DIR ...] [--out out/renut/shader_dump]
"""
import argparse
import os
import re
import struct

import xxhash

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAGIC = re.compile(rb'\x10\x2a[\x0e\x11][\x00\x01]')


def container_size(data, o):
    """Total size of the container at o (header + physical block), or 0 if the header does not hold together."""
    if o + 0x18 > len(data):
        return 0
    virt, phys = struct.unpack_from('>II', data, o + 4)
    if not (0x18 <= virt < 0x10000 and 0 < phys < 0x100000 and o + virt + phys <= len(data)):
        return 0
    return virt + phys


def save(out, blob, seen):
    kind = 'vs' if blob[3] & 1 else 'ps'
    name = f'{kind}_{xxhash.xxh3_64_hexdigest(blob)}.bin'
    if name in seen:
        return False
    seen.add(name)
    path = os.path.join(out, name)
    if not os.path.exists(path):
        with open(path, 'wb') as f:
            f.write(blob)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=os.path.join(ROOT, 'assets'))
    ap.add_argument('--image', default=os.path.join(ROOT, 'tools', 'image.bin'))
    ap.add_argument('--dumps', nargs='*', default=[])
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'renut', 'shader_dump'))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    seen = set()
    counts = {}

    sdp = open(os.path.join(a.game, 'data', 'Shaders', 'shaderpackage.sdp'), 'rb').read()
    version, count, _ = struct.unpack_from('>3I', sdp, 0)
    assert version == 100, version
    p, n = 12, 0
    for _ in range(count):
        size = struct.unpack_from('>I', sdp, p + 256)[0]
        blob = sdp[p + 260:p + 260 + size]
        p += 260 + size
        assert container_size(blob, 0) == len(blob), 'sdp entry is not one whole container'
        n += save(a.out, blob, seen)
    counts['shaderpackage.sdp'] = (count, n)

    def scan(label, data):
        found = n = 0
        for m in MAGIC.finditer(data):
            o = m.start()
            size = container_size(data, o)
            if size:
                found += 1
                n += save(a.out, data[o:o + size], seen)
        counts[label] = (found, n)

    shaders = os.path.join(a.game, 'data', 'Shaders')
    for root, _, files in sorted(os.walk(shaders)):
        for name in sorted(files):
            path = os.path.join(root, name)
            if name.lower() != 'shaderpackage.sdp':
                scan(os.path.relpath(path, shaders), open(path, 'rb').read())
    scan('image', open(a.image, 'rb').read())

    for folder in a.dumps:
        found = n = 0
        for name in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
            if re.fullmatch(r'(vs|ps)_[0-9a-f]{16}\.bin', name):
                found += 1
                n += save(a.out, open(os.path.join(folder, name), 'rb').read(), seen)
        counts[folder] = (found, n)

    for src, (found, new) in counts.items():
        print(f'{src}: {found} containers, {new} new')
    print(f'{len(seen)} unique containers in {a.out}')


if __name__ == '__main__':
    main()
