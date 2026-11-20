"""Where the deko3d shader chain's host tools are (spirv-cross, uam, zstd).

DEKO3D_TOOLS names a folder that holds them (the Switch installer's kit sets it); otherwise tools/deko3d_build_tools_win.bat's
out/deko3d_tools/win on Windows, and on Linux (the devkitA64 container) tools/deko3d_build_tools.sh's build folders and
the zstd on PATH.
"""
import os
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def tool(name):
    exe = name + ('.exe' if os.name == 'nt' else '')
    if os.environ.get('DEKO3D_TOOLS'):
        return os.path.join(os.environ['DEKO3D_TOOLS'], exe)
    if os.name == 'nt':
        return os.path.join(ROOT, 'out', 'deko3d_tools', 'win', exe)
    if name == 'zstd':
        return shutil.which('zstd') or 'zstd'
    return os.path.join(ROOT, 'out', 'deko3d_tools', name + '-build', name)
