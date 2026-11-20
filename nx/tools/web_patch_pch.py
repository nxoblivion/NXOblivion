#!/usr/bin/env python3
"""reOblivion web (2026-11-17): give an already generated oblivion_pch.h the browser's mirror fold.

sdk/resources/templates/codegen/pch_h.inja has it (REX_PHYS_HOST_OFFSET under REX_PLATFORM_WEB, and the D-form
accesses through it), so a new codegen run writes it; this applies the same two edits to app/generated, which an
older codegen wrote, so the web build needs no codegen run. Idempotent; other platforms compile the header unchanged.

    python nx/tools/web_patch_pch.py [app/generated/default/oblivion_pch.h]
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FOLD = """#if REX_PLATFORM_WEB
// reOblivion web (2026-11-17): WebAssembly cannot map one memory at several addresses, so the 360's physical mirrors
// are folded here instead: 0xC0000000-0xDFFFFFFF and 0xE0000000-0xFFFFFFFF land on the 0xA0000000 window (and
// 0xE0000000 four kilobytes further, as on the PC), where xmemory.cpp keeps physical memory (physical_membase =
// virtual_membase + 0xA0000000). A signed offset, so every macro below keeps its base + (u32)(x) + offset form.
#define REX_PHYS_HOST_OFFSET(addr) \
  ((u32)(addr) >= 0xC0000000u ? ((u32)(addr) >= 0xE0000000u ? -0x3FFFF000ll : -0x20000000ll) : 0ll)
#elif REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
#define REX_PHYS_HOST_OFFSET(addr) (((u32)(addr) >= 0xE0000000u) ? 0x1000u : 0u)"""
OLD_FOLD = """#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
#define REX_PHYS_HOST_OFFSET(addr) (((u32)(addr) >= 0xE0000000u) ? 0x1000u : 0u)"""
OLD_DFORM = "#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)\n#define REX_LOAD_D_U8"
NEW_DFORM = "#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64) || REX_PLATFORM_WEB\n#define REX_LOAD_D_U8"


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'app', 'generated', 'default', 'oblivion_pch.h')
    text = open(path, encoding='utf-8', newline='').read()
    if 'REX_PLATFORM_WEB' in text:
        print(f'web_patch_pch: {path} already has the web fold')
        return
    if text.count(OLD_FOLD) != 1 or text.count(OLD_DFORM) != 1:
        sys.exit(f'web_patch_pch: {path} does not look like the template this script knows')
    text = text.replace(OLD_FOLD, FOLD).replace(OLD_DFORM, NEW_DFORM)
    open(path, 'w', encoding='utf-8', newline='').write(text)
    print(f'web_patch_pch: {path} patched')


if __name__ == '__main__':
    main()
