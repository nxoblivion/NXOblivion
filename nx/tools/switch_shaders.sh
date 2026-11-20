#!/bin/sh
# The game shaders for the Switch build (after katamari-nx's tools/switch_shaders.ps1): the containers of
# out/renut_shader_dump through the PC build's XenosRecomp in its deko3d mode (SPIR-V with -DBANJO_DEKO3D), then in the
# devkitA64 container SPIRV-Cross, the GLSL conversion and a uam compile check (tools/deko3d_shaders.py), the GLSL text
# passes (deko3d_glsl_opt.py) and the package (deko3d_shader_package.py: app/out/renut_switch/deko3d_glsl.bin and
# deko3d_glsl_switch.cpp), and the SPIR-V shader table (renut_switch_shader_cache.py). Run from Git Bash on Windows
# after tools/build_pc.bat and tools/deko3d_build_tools.sh. No early alpha pass (the PC build has none either).
#   sh nx/tools/switch_shaders.sh
set -e
NX="$(cd "$(dirname "$0")/.." && pwd)"
EXE="$NX/app/out/build/win-amd64-release/xenos_recomp/build/XenosRecomp/XenosRecomp.exe"
WORK="$NX/out/deko3d_shaders"
[ -x "$EXE" ] || { echo "no $EXE: build the PC tree (tools/build_pc.bat) first"; exit 1; }
# A fresh work folder each run (the SPIR-V of shaders since removed would stay otherwise).
[ -d "$WORK" ] && mv "$WORK" "$WORK.old.$$"
mkdir -p "$WORK/spirv"
BANJO_DEKO3D_SPIRV_DIR="$(cygpath -w "$WORK/spirv")" "$EXE" "$(cygpath -w "$NX/out/renut_shader_dump")" \
    "$(cygpath -w "$WORK/shader_cache_deko3d.cpp")" \
    "$(cygpath -w "$NX/app/thirdparty/reblue-XenosRecomp/XenosRecomp/shader_common.h")" > "$WORK/xenos_recomp.log"
echo "SPIR-V: $(ls "$WORK/spirv" | grep -c '\.spv$') shaders, $(ls "$WORK/spirv" | grep -c '\.fail$') failed"
sh "$NX/tools/switch_docker.sh" "(command -v zstd >/dev/null || (apt-get update -qq && apt-get install -y -qq zstd >/dev/null)) &&
    python3 tools/deko3d_shaders.py --spirv out/deko3d_shaders/spirv --out out/deko3d_shaders &&
    python3 tools/deko3d_glsl_opt.py --in out/deko3d_shaders/glsl --out out/deko3d_shaders/glsl_opt &&
    python3 tools/deko3d_shader_package.py --glsl out/deko3d_shaders/glsl_opt --out app/out/renut_switch &&
    python3 tools/renut_switch_shader_cache.py --pc app/out/build/win-amd64-release"
ls -la "$NX/app/out/renut_switch"
