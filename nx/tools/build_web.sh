#!/bin/sh
# Builds reOblivion for the browser (app/out/web: oblivion.html/.js/.wasm), WebAssembly wasm64 + pthreads with the
# repo's Emscripten (out/emsdk, nx/web/env.sh). Needs app/generated (the PC build's codegen).
#   sh nx/tools/build_web.sh [extra cmake --build args]
# Environment: BUILD_DIR (default app/out/web, relative to nx/), JOBS (default 24), CMAKE_EXTRA, CONFIG (Release).
set -e
cd "$(dirname "$0")/.."
. web/env.sh
BUILD_DIR="${BUILD_DIR:-app/out/web}"
JOBS="${JOBS:-24}"
CONFIG="${CONFIG:-Release}"
# Every object: 64-bit memory, threads (shared memory + atomics), wasm SIMD (simde maps SSE onto it), native wasm
# exceptions and setjmp/longjmp (the generated code's guest setjmp).
WEB_FLAGS="-sMEMORY64=1 -pthread -msimd128 -fwasm-exceptions -sSUPPORT_LONGJMP=wasm"
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
  emcmake cmake -S app -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG" \
    -DREXSDK_DIR="$PWD/sdk" -DOBLIVION_RENUT_HOOKS=ON -DREXGLUE_ENABLE_TRACY=OFF \
    -DCMAKE_C_FLAGS="$WEB_FLAGS" -DCMAKE_CXX_FLAGS="$WEB_FLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="$WEB_FLAGS" $CMAKE_EXTRA
fi
# The generated code's pch from an older codegen gets the web mirror fold (idempotent).
python tools/web_patch_pch.py
cmake --build "$BUILD_DIR" -j "$JOBS" "$@"
