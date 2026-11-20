#!/usr/bin/env python3
"""reOblivion web (2026-11-17): Renut's shader cache for the browser build.

The Switch's copy (tools/renut_switch_shader_cache.py: app/out/renut_switch/shader_cache_switch.cpp, the PC build's
cache without DXIL, and spirv_cache.zst) embeds the SPIR-V with the assembler's .incbin, which WebAssembly's assembler
does not have. This writes app/out/renut_web/shader_cache_web.cpp with the same tables and the blob through C23 #embed
(clang takes it in C++ too), next to a copy of the blob.

    python nx/tools/renut_web_shader_cache.py
"""
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    src_dir = os.path.join(ROOT, 'app', 'out', 'renut_switch')
    src = os.path.join(src_dir, 'shader_cache_switch.cpp')
    blob = os.path.join(src_dir, 'spirv_cache.zst')
    if not os.path.exists(src) or not os.path.exists(blob):
        sys.exit(f'renut_web_shader_cache: no {src} / {blob}: run tools/renut_switch_shader_cache.py first')
    text = open(src, encoding='utf-8').read()
    m = re.search(r"// banjo-nx: the compressed SPIR-V, embedded by the assembler.*?.previous[^;]*;", text, re.S)
    if not m:
        sys.exit('renut_web_shader_cache: no .incbin block in ' + src)
    out_dir = os.path.join(ROOT, 'app', 'out', 'renut_web')
    os.makedirs(out_dir, exist_ok=True)
    shutil.copyfile(blob, os.path.join(out_dir, 'spirv_cache.zst'))
    embed = ('// reOblivion web: the compressed SPIR-V through #embed (spirv_cache.zst beside this file).\n'
             '#pragma clang diagnostic push\n'
             '#pragma clang diagnostic ignored "-Wc23-extensions"\n'
             'extern const uint8_t g_compressedSpirvCache[];\n'
             'alignas(16) const uint8_t g_compressedSpirvCache[] = {\n'
             '#embed "spirv_cache.zst"\n'
             '};\n'
             '#pragma clang diagnostic pop')
    text = text[:m.start()] + embed + text[m.end():]
    out = os.path.join(out_dir, 'shader_cache_web.cpp')
    open(out + '.tmp', 'w', encoding='utf-8').write(text)
    os.replace(out + '.tmp', out)
    print(f'renut_web_shader_cache: {out} (+ spirv_cache.zst, {os.path.getsize(blob) >> 10} KB)')


if __name__ == '__main__':
    main()
