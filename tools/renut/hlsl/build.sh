#!/bin/bash
# Builds oblivion_hlsl (the translator, HLSL only) into out/renut/bin with LLVM clang, from a Renut XenosRecomp tree
# (default: katamari-nx's in out/ref; later nx/app/thirdparty/reblue-XenosRecomp).
#   tools/renut/hlsl/build.sh [<reblue-XenosRecomp folder>]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
XR=${1:-$ROOT/out/ref/katamari-nx/app/thirdparty/reblue-XenosRecomp}
OUT=$ROOT/out/renut/bin
mkdir -p "$OUT"
CXX=${CXX:-clang++}
"$CXX" -std=c++20 -O2 -w -fms-extensions -DRENUT_RECOMP -DFMT_HEADER_ONLY -DXXH_INLINE_ALL -D_CRT_SECURE_NO_WARNINGS \
  -include "$HERE/pch_min.h" -I"$HERE" -I"$XR/XenosRecomp" -I"$XR/thirdparty/fmt/include" -I"$XR/thirdparty/xxHash" \
  "$HERE/oblivion_hlsl.cpp" "$XR/XenosRecomp/shader_recompiler.cpp" -o "$OUT/oblivion_hlsl.exe"
echo "$OUT/oblivion_hlsl.exe"
