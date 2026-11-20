#!/bin/bash
# Builds out/switch-package/ (+ oblivion-switch.zip) from the GL NRO. No game assets are included.
# Prerequisite: ports/switch/oblivion_gl/oblivion_gl.nro (ports/switch/oblivion_gl/rebuild.sh).
set -e
cd "$(dirname "$0")/../.." || exit 1
NRO=ports/switch/oblivion_gl/oblivion_gl.nro
[ -f "$NRO" ] || { echo "missing $NRO - run ports/switch/oblivion_gl/rebuild.sh first" >&2; exit 1; }
PKG=${1:-out/switch-package}   # optional arg: output dir (e.g. out/switch-package-v0.3)
rm -rf "$PKG"
mkdir -p "$PKG/switch/oblivion" "$PKG/oblivion"
cp "$NRO" "$PKG/switch/oblivion/oblivion.nro"
cp ports/switch/package/oblivion.toml "$PKG/oblivion/oblivion.toml"
cp ports/switch/package/INSTALL.md "$PKG/INSTALL.md"
cp ports/switch/copy-assets.ps1 "$PKG/copy-assets.ps1"
cp ports/switch/usb_log.py "$PKG/usb_log.py"
if [ -f ports/switch/memprobe/memprobe.nro ]; then mkdir -p "$PKG/switch"; cp ports/switch/memprobe/memprobe.nro "$PKG/switch/memprobe.nro"; fi
python - "$PKG" <<'PYEOF'
import os, sys, zipfile
pkg = sys.argv[1]
zp = os.path.join(pkg, 'oblivion-switch.zip')
with zipfile.ZipFile(zp, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    for root, _, files in os.walk(pkg):
        for f in sorted(files):
            full = os.path.join(root, f)
            if full == zp: continue
            z.write(full, os.path.relpath(full, pkg).replace(os.sep, '/'))
print('wrote', zp, os.path.getsize(zp) // (1024 * 1024), 'MiB')
PYEOF
find "$PKG" -type f | sort
