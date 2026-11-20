#!/usr/bin/env python3
"""The Python runtime of the Switch installer's kit (nx/tools/build_kit.sh): the shader stage's scripts
(tools/deko3d_*.py, renut_switch_shader_cache.py) run on the player's PC, who installs nothing.

Copies the running CPython for Windows (python.org's build: PSF license, redistributable) into <out>: python.exe and
its DLLs, the extension modules of DLLs/ (no Tcl/Tk), the standard library zipped as python<XY>.zip (no tests, IDLE,
Tkinter, ensurepip, venv or site-packages), and a python<XY>._pth that isolates it (no site, no user or environment
paths). The scripts need the standard library only.

    python nx/tools/kit_python.py <out>
"""
import os
import shutil
import sys
import zipfile

SKIP_DIRS = {'test', 'tests', 'idlelib', 'tkinter', 'turtledemo', 'ensurepip', 'venv', 'site-packages', 'lib2to3',
             'pydoc_data', '__pycache__', 'unittest'}
SKIP_DLLS = ('tcl', 'tk', '_tkinter')


def main():
    out = sys.argv[1]
    base = sys.base_prefix
    if os.name != 'nt':
        sys.exit('kit_python: the kit runs on Windows; run this with a Windows CPython')
    tag = f'python{sys.version_info.major}{sys.version_info.minor}'
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    for name in ('python.exe', f'{tag}.dll', 'python3.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'LICENSE.txt'):
        if os.path.isfile(os.path.join(base, name)):
            shutil.copy2(os.path.join(base, name), out)
    for name in sorted(os.listdir(os.path.join(base, 'DLLs'))):
        if name.lower().endswith(('.pyd', '.dll')) and not name.lower().startswith(SKIP_DLLS):
            shutil.copy2(os.path.join(base, 'DLLs', name), out)
    lib = os.path.join(base, 'Lib')
    files = 0
    with zipfile.ZipFile(os.path.join(out, tag + '.zip'), 'w', zipfile.ZIP_DEFLATED) as z:
        for root, dirs, names in os.walk(lib):
            dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
            for name in sorted(names):
                if name.endswith('.py'):
                    path = os.path.join(root, name)
                    z.write(path, os.path.relpath(path, lib).replace(os.sep, '/'))
                    files += 1
    with open(os.path.join(out, tag + '._pth'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(f'{tag}.zip\n.\n')
    size = sum(os.path.getsize(os.path.join(out, n)) for n in os.listdir(out))
    print(f'kit_python: Python {sys.version.split()[0]}, {files} library modules, {size >> 20} MB -> {out}')


if __name__ == '__main__':
    main()
