#!/usr/bin/env bash
# The host tools of the deko3d shader chain, for Linux (run in the devkitA64 container: tools/switch_docker.sh), into
# out/deko3d_tools: SPIRV-Cross (the repository SDK's copy, thirdparty/rexglue-sdk/thirdparty/SPIRV-Cross) and uam
# (app/thirdparty/uam, SwitchWakerHD's patched 1.1.0). After katamari-nx's tools/deko3d_build_tools.sh (MSYS2 clang).
#   sh tools/switch_docker.sh "bash tools/deko3d_build_tools.sh"
set -euo pipefail
NX="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SPIRV_CROSS_SRC="${1:-$NX/../thirdparty/rexglue-sdk/thirdparty/SPIRV-Cross}"
OUT="$NX/out/deko3d_tools"
mkdir -p "$OUT/spirv-cross-build" "$OUT/uam-build"
cmake -S "$SPIRV_CROSS_SRC" -B "$OUT/spirv-cross-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_ENABLE_C_API=OFF
ninja -C "$OUT/spirv-cross-build" spirv-cross
cmake -S "$NX/app/thirdparty/uam" -B "$OUT/uam-build" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$OUT/uam-build" uam_tool
ls -la "$OUT/spirv-cross-build/spirv-cross" "$OUT/uam-build/uam"
