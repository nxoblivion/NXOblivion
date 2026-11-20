#!/usr/bin/env python3
"""reOblivion web (2026-11-18): the game shaders as WGSL for plume_webgpu, and their package.

    python nx/tools/webgpu_shaders.py [--vk out/deko3d_shaders/vk] [--out out/webgpu_shaders] [--limit N]

Input: SPIRV-Cross's Vulkan GLSL of the deko3d-flavoured SPIR-V (tools/switch_shaders.sh makes out/deko3d_shaders/vk:
XenosRecomp with BANJO_DEKO3D_SPIRV_DIR, then spirv-cross). For each shader tools/webgpu_glsl.py (bound textures,
WebGPU's bind groups), then in the reoblivion-tint container (web/tint/Dockerfile) glslang (GLSL -> SPIR-V) and tint
(SPIR-V -> WGSL). Writes <out>/glsl, <out>/wgsl, <out>/report.txt and the package app/out/web/reoblivion_wgsl.bin, which
the browser build reads at startup (plume_webgpu.cpp / renut's shader library):

    header  "WGSL", version 1, count, reserved          (16 bytes)
    entries hash u64, stage u32 (0 vertex, 4 fragment), meta offset u32, meta size u32, wgsl offset u32, wgsl size u32,
            reserved u32                                 (32 bytes each, sorted by hash)
    data    the binding lines (webgpu_glsl.py) and the WGSL texts
    all zstd-compressed (one frame) when zstd is on PATH or beside the deko3d tools.

Game data (made from the player's own disc): the build tree only, never published.
"""
import argparse
import collections
import os
import re
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import webgpu_glsl  # noqa: E402

NX = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGE = 'reoblivion-tint'
STAGE = {'vs': ('vert', 0), 'ps': ('frag', 4)}

# tint's --allow-non-uniform-derivatives puts diagnostic(off, derivative_uniformity) first: the game's pixel shaders
# sample inside branches on values the uniformity analysis cannot prove uniform (Xenos flow control on registers); D3D
# and Vulkan allow it, the derivatives being undefined there.
WGSL_PREAMBLE = ''


def docker_path(p):
    p = os.path.abspath(p)
    return p.replace('\\', '/')


def run_tint(work, stems):
    """glslang + tint over <work>/glsl/<stem>.glsl in one container run; <work>/wgsl/<stem>.wgsl or <stem>.err."""
    script = os.path.join(work, 'build.sh')
    lines = ['#!/bin/sh', 'cd /work', 'mkdir -p spv wgsl err']
    for stem, stage in stems:
        lines.append(f'if glslangValidator -V --target-env vulkan1.1 -S {stage} -o spv/{stem}.spv glsl/{stem}.glsl '
                     f'> err/{stem}.txt 2>&1; then tint --format wgsl --allow-non-uniform-derivatives true -o wgsl/{stem}.wgsl spv/{stem}.spv '
                     f'>> err/{stem}.txt 2>&1 || echo tint >> err/{stem}.txt; else echo glslang >> err/{stem}.txt; fi')
    with open(script, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    env = dict(os.environ, MSYS_NO_PATHCONV='1')
    r = subprocess.run(['docker', 'run', '--rm', '-v', f'{docker_path(work)}:/work', IMAGE, 'sh', '/work/build.sh'],
                       capture_output=True, text=True, env=env)
    if r.returncode != 0:
        sys.exit(f'webgpu_shaders: docker run failed ({r.returncode}):\n{r.stdout}{r.stderr}\n'
                 f'(build the image: docker build -t {IMAGE} nx/web/tint)')


def find_zstd():
    for c in (shutil.which('zstd'), os.path.join(NX, 'out', 'deko3d_tools', 'win', 'zstd.exe')):
        if c and os.path.isfile(c):
            return c
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--vk', default=os.path.join(NX, 'out', 'deko3d_shaders', 'vk'))
    ap.add_argument('--out', default=os.path.join(NX, 'out', 'webgpu_shaders'))
    ap.add_argument('--package', default=os.path.join(NX, 'app', 'out', 'web', 'reoblivion_wgsl.bin'))
    ap.add_argument('--limit', type=int, default=0)
    a = ap.parse_args()
    if os.path.isdir(a.out):
        shutil.rmtree(a.out)
    os.makedirs(os.path.join(a.out, 'glsl'))
    files = sorted(f for f in os.listdir(a.vk) if f.endswith('.vk.glsl'))
    if a.limit:
        files = files[:a.limit]
    fails = collections.defaultdict(list)
    metas = {}
    stems = []
    for f in files:
        stem = f[:-len('.vk.glsl')]
        stage, _ = STAGE[stem[:2]]
        try:
            text, meta = webgpu_glsl.convert(open(os.path.join(a.vk, f), encoding='utf-8').read(), stage)
        except webgpu_glsl.Unsupported as e:
            fails['convert'].append((stem, str(e)))
            continue
        with open(os.path.join(a.out, 'glsl', stem + '.glsl'), 'w', encoding='utf-8', newline='\n') as o:
            o.write(text)
        metas[stem] = meta
        stems.append((stem, stage))
    print(f'webgpu_shaders: {len(stems)} converted, glslang + tint in {IMAGE}...', flush=True)
    run_tint(a.out, stems)
    entries = []
    for stem, stage in stems:
        wgsl = os.path.join(a.out, 'wgsl', stem + '.wgsl')
        if not os.path.isfile(wgsl):
            err = open(os.path.join(a.out, 'err', stem + '.txt'), encoding='utf-8', errors='replace').read().strip()
            tool = err.split('\n')[-1] if err else '?'
            first = next((l for l in err.split('\n') if 'error' in l.lower()), err[:200])
            fails[tool].append((stem, first.strip()[:300]))
            continue
        text = WGSL_PREAMBLE + open(wgsl, encoding='utf-8').read()
        with open(wgsl, 'w', encoding='utf-8', newline='\n') as o:
            o.write(text)
        entries.append((int(stem[3:], 16), STAGE[stem[:2]][1], metas[stem].encode(), text.encode()))
    entries.sort()

    data = bytearray()
    table = bytearray()
    base = 16 + 32 * len(entries)
    for h, stage, meta, wgsl in entries:
        mo = base + len(data)
        data += meta
        wo = base + len(data)
        data += wgsl
        table += struct.pack('<QIIIIII', h, stage, mo, len(meta), wo, len(wgsl), 0)
    blob = b'WGSL' + struct.pack('<III', 1, len(entries), 0) + bytes(table) + bytes(data)
    os.makedirs(os.path.dirname(os.path.abspath(a.package)), exist_ok=True)
    raw = a.package + '.raw'
    with open(raw, 'wb') as o:
        o.write(blob)
    zstd = find_zstd()
    if zstd:
        subprocess.run([zstd, '-q', '-f', '-19', raw, '-o', a.package], check=True)
        os.remove(raw)
    else:
        os.replace(raw, a.package)

    lines = [f'{len(files)} shaders: {len(entries)} in the package, {sum(len(v) for v in fails.values())} failed',
             f'package {a.package}: {os.path.getsize(a.package) >> 10} KB ({len(blob) >> 10} KB uncompressed'
             f'{", zstd" if zstd else ""})']
    for reason, items in sorted(fails.items(), key=lambda kv: -len(kv[1])):
        lines.append(f'\n[{reason}] {len(items)}')
        by = collections.Counter(re.sub(r'_[0-9A-Fa-f]{6,}|\d+', 'N', d)[:200] for _, d in items)
        for d, n in by.most_common(15):
            ex = next(s for s, dd in items if re.sub(r'_[0-9A-Fa-f]{6,}|\d+', 'N', dd)[:200] == d)
            lines.append(f'  {n:5}  {d}   (e.g. {ex})')
    report = '\n'.join(lines)
    with open(os.path.join(a.out, 'report.txt'), 'w', encoding='utf-8') as o:
        o.write(report + '\n')
    print(report)
    return 0 if not fails else 1


if __name__ == '__main__':
    sys.exit(main())
