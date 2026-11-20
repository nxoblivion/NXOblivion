#!/bin/bash
# Host-side: rebuild the switch-gl SDK GL library + oblivion_gl.nro (needs Docker). Usage: ports/switch/oblivion_gl/rebuild.sh [tag]
cd "$(dirname "$0")/../../.." || exit 1
TAG=${1:-r}
ports/switch/oblivion_gl/docker.sh "$TAG" 'cd out/sdk-gl && cmake --build out/build/switch-release --target rexgpu-gl rexruntime -j24 2>&1 | grep -E "error|FAILED|warning: unused|Linking CXX static library .*(rexgpu|rexruntime)" ; cd /work && ports/switch/oblivion_gl/build.sh 2>&1 | tail -5'
