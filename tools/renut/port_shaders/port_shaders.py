#!/usr/bin/env python3
"""Writes Renut's port-written shaders where the translator takes a shader's HLSL from (peggle2-nx's
tools/renut_port_shaders.py, via feedingfrenzy-nx, with Oblivion's shaders).

Oblivion compiles three shaders at boot (sub_8211C3A8: the Bink movie vertex shader and two movie pixel shaders), so no
game file holds them and no tool can translate them from the player's files. tools/renut/port_shaders/ has
port-written versions of them (no game shader code). This tool puts in front of each body what Renut's translator puts
in front of every shader it writes (its shader_common.h and the prologue of constant and descriptor-index macros, by
the same pattern) and writes <vs|ps>_<container XXH3>.hlsl into the directory of BANJO_HLSL_OVERRIDE.

    python tools/renut/port_shaders/port_shaders.py --out <override dir> [--common <shader_common.h>] [--table <json>]
"""
import argparse
import os

PORT = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(PORT)))
COMMON = os.path.join(ROOT, 'nx', 'app', 'thirdparty', 'reblue-XenosRecomp', 'XenosRecomp', 'shader_common.h')

# The containers' XXH3 (the names the translator gives their HLSL), the body and its defines.
SHADERS = [
    ('vs_66497D2FC8FCCFAC', 'oblivion_renut_movie_vs.hlsl', []),
    ('ps_819818509CD1E1ED', 'oblivion_renut_movie_ps.hlsl', []),
    ('ps_7A0CE24F6A7DED83', 'oblivion_renut_movie_ps.hlsl', ['RENUT_MOVIE_ALPHA']),
]

# oblivion nx (2026-11-04): what the shader table (shader_cache.h ShaderCacheEntry) says about each port shader, for a
# table made without the game's containers (the Switch installer, nx/tools/renut_shader_table.py). Facts of the port
# shaders' interface: the movie vertex shader reads POSITION0 and TEXCOORD0 from the bound declaration (usage 0 / 5,
# fetch 95, format 0: the declaration's), the pixel shaders sample s0-s2 (Y, Cr, Cb) and s3 (A) and read c0-c3.
# nx/tools/build_kit.sh checks them against the translator's table.
PORT_TABLE = {
    'vs_66497D2FC8FCCFAC': {'textureFetchMask': 0x0, 'floatConstants': [0] * 8,
                            'vertexInputs': [[0, 0, 95, 0, 0, 0, 0], [5, 0, 95, 0, 0, 0, 0]]},
    'ps_819818509CD1E1ED': {'textureFetchMask': 0x7, 'floatConstants': [0xF] + [0] * 7, 'vertexInputs': []},
    'ps_7A0CE24F6A7DED83': {'textureFetchMask': 0xF, 'floatConstants': [0xF] + [0] * 7, 'vertexInputs': []},
}

SAMPLER_COUNT = 32  # shader_recompiler.cpp
TEXTURE_DIMENSIONS = ['2D', '3D', 'Cube']
SWIZZLES = 'xyzw'


def prologue(pixel):
    """ShaderRecompiler's text between shader_common.h and the shader's constant names, for a shader whose samplers
    have no names (s0 to s31)."""
    name = 'Pixel' if pixel else 'Vertex'
    out = ['#ifdef __spirv__', '']
    out.append(f'#define g_C(INDEX) select((uint)(INDEX) < 256u, g_Ubo{name}.v[min((uint)(INDEX), 255u)], 0.0)')
    for r in range(SAMPLER_COUNT):
        for j, dim in enumerate(TEXTURE_DIMENSIONS):
            out.append(f'#define s{r}_Texture{dim}DescriptorIndex RENUT_SHARED_UINT({j * SAMPLER_COUNT * 4 + r * 4})')
        out.append(f'#define s{r}_SamplerDescriptorIndex '
                   f'RENUT_SHARED_UINT({len(TEXTURE_DIMENSIONS) * SAMPLER_COUNT * 4 + r * 4})')
    out += ['', '#else', '']
    out.append(f'cbuffer {name}ShaderConstants : register(b{1 if pixel else 0}, space4)')
    out += ['{', '\tfloat4 g_CArr[256] : packoffset(c0);', '};', '',
            '#define g_C(INDEX) select((uint)(INDEX) < 256u, g_CArr[min((uint)(INDEX), 255u)], 0.0)', '',
            'cbuffer SharedConstants : register(b2, space4)', '{']
    for r in range(SAMPLER_COUNT):
        for j, dim in enumerate(TEXTURE_DIMENSIONS):
            out.append(f'\tuint s{r}_Texture{dim}DescriptorIndex : '
                       f'packoffset(c{j * (SAMPLER_COUNT // 4) + r // 4}.{SWIZZLES[r % 4]});')
        out.append(f'\tuint s{r}_SamplerDescriptorIndex : '
                   f'packoffset(c{len(TEXTURE_DIMENSIONS) * (SAMPLER_COUNT // 4) + r // 4}.{SWIZZLES[r % 4]});')
    out += ['\tDEFINE_SHARED_CONSTANTS();', '};', '', '#endif', '', '']
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--common', default=COMMON)
    ap.add_argument('--table', default=None, help='also write PORT_TABLE as JSON to this file')
    a = ap.parse_args()
    if a.table:
        import json
        with open(a.table, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(PORT_TABLE, f, indent=1)
    common = open(a.common, encoding='utf-8').read()
    os.makedirs(a.out, exist_ok=True)
    for stem, body_name, defines in SHADERS:
        pixel = stem.startswith('ps_')
        body = open(os.path.join(PORT, body_name), encoding='utf-8').read()
        text = (f'// oblivion port shader {stem} (tools/renut/port_shaders/port_shaders.py, {body_name})\n'
                + common + '\n' + prologue(pixel)
                + ''.join(f'#define {d} 1\n' for d in defines) + body)
        path = os.path.join(a.out, stem + '.hlsl')
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    print(f'port_shaders: {len(SHADERS)} port shaders -> {a.out}')


if __name__ == '__main__':
    main()
