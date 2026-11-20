#!/bin/sh
# Builds the Switch NRO (app/out/switch/oblivion.nro) in devkitPro's devkitA64 container (after katamari-nx's
# tools/build_switch.bat, which uses a Windows devkitPro). Needs, from earlier steps: app/generated (the PC build's
# codegen), tools/deko3d_build_tools.sh's host tools and tools/switch_shaders.sh's app/out/renut_switch.
#   sh nx/tools/build_switch.sh [extra cmake --build args]
# Environment: BUILD_DIR (default app/out/switch, relative to nx/), JOBS (default 24), CMAKE_EXTRA (configure options
# for a new folder). Never two at once: they share the SDK's archives in sdk/out/switch-arm64.
BUILD_DIR="${BUILD_DIR:-app/out/switch}"
JOBS="${JOBS:-24}"
# plume checks that the NVK driver's libvulkan.a exists even when it is not linked (REXGLUE_SWITCH_NVK OFF, the
# default here: reNut on deko3d only); an empty archive stands in for it.
exec sh "$(dirname "$0")/switch_docker.sh" "set -e
mkdir -p out/nvk_none/lib && [ -f out/nvk_none/lib/libvulkan.a ] || aarch64-none-elf-ar rc out/nvk_none/lib/libvulkan.a
if [ ! -f $BUILD_DIR/CMakeCache.txt ]; then
  cmake -S app -B $BUILD_DIR -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=/work/nx/tools/switch/cmake/switch-devkitA64.cmake \
    -DREXSDK_DIR=/work/nx/sdk -DREXGLUE_SWITCH_NVK_SDK=/work/nx/out/nvk_none -DOBLIVION_RENUT_HOOKS=ON \
    -DPYTHON_EXECUTABLE=/usr/bin/python3 -DPython3_EXECUTABLE=/usr/bin/python3 $CMAKE_EXTRA
fi
cmake --build $BUILD_DIR -j $JOBS $*"
