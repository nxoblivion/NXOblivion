#!/bin/bash
# Convert every shot_*.ppm in the given out dir (default out/gl-run) to PNG using python3 from the devkitPro image.
cd "$(dirname "$0")/../../.." || exit 1
D=${1:-out/gl-run}
for f in $D/shot_*.ppm; do
  MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/work" -w /work devkitpro/devkita64:latest python3 ports/switch/oblivion_gl/ppm2png.py "$f" "${f%.ppm}.png" 2>&1
done
