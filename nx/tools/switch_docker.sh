#!/bin/sh
# Runs a command in devkitPro's devkitA64 container (Debian: devkitA64, libnx, deko3d, host cmake / ninja / python3 /
# g++) with the repository mounted at /work, from the nx/ folder. The Switch build and the deko3d shader chain run
# here; DEVKITPRO is /opt/devkitpro.
#   sh nx/tools/switch_docker.sh "<shell command>"
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/../.." || exit 1
if pwd -W >/dev/null 2>&1; then ROOT="$(pwd -W)"; else ROOT="$(pwd)"; fi
exec docker run --rm -v "$ROOT:/work" -w /work/nx devkitpro/devkita64:latest \
    bash -c "export PATH=\$DEVKITPRO/devkitA64/bin:\$DEVKITPRO/tools/bin:\$PATH; $1"
