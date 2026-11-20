# source nx/web/env.sh: the repo's Emscripten (out/emsdk) on PATH.
E="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../out/emsdk" && pwd)"
export EMSDK="$E" EM_CONFIG="$E/.emscripten"
export PATH="$E/upstream/emscripten:$E/node/24.19.0_64bit/bin:$E/python/3.13.3_64bit:$PATH"
