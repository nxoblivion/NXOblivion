#!/bin/sh
# Build oblivion.nro. Run INSIDE the devkitA64 container from /work (use sdk-docker.sh):
#   ports/switch/sdk-docker.sh 'ports/switch/oblivion/build.sh'
# Prerequisites: SDK built (thirdparty/rexglue-sdk/out/switch-arm64/*.a) and generated objects
# compiled: ports/switch/compile-generated.sh -c   (-> out/switch-gen/*.o)
set -e
SDK=/work/thirdparty/rexglue-sdk
OUT=/work/out/switch-gen
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec"
mkdir -p $OUT/app
aarch64-none-elf-g++ -c /work/ports/switch/oblivion/main.cpp -o $OUT/app/main.o \
  -DREX_PLATFORM_SWITCH=1 -D_GNU_SOURCE -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL -DSPDLOG_NO_TZ_OFFSET \
  -D__SWITCH__ -I$SDK/include -I$SDK/out/build/switch-release/include -I$SDK/thirdparty/fmt/include \
  -I$SDK/thirdparty/spdlog/include -isystem $SDK/thirdparty/tomlplusplus/include -I$SDK/thirdparty/simde \
  -I$DEVKITPRO/libnx/include $ARCH -O2 -DNDEBUG -std=c++23 -fPIE -g -ffunction-sections -w
L=$SDK/out/switch-arm64
LIBS="$L/librexruntime.a $L/libspdlog.a $L/libfmt.a $L/liblibavcodec.a $L/liblibavutil.a $L/libmspack.a \
 $L/libaes128.a $L/libtiny-aes.a $L/libo1heap.a $L/libxxhash.a $L/libdisasm.a"
# shellcheck disable=SC2086
aarch64-none-elf-g++ -specs=$DEVKITPRO/libnx/switch.specs $ARCH -fPIE -g -Wl,--gc-sections \
  -Wl,-Map,$OUT/oblivion.map -o $OUT/oblivion.elf $OUT/app/main.o $OUT/oblivion_*.o \
  -Wl,--start-group $LIBS -Wl,--end-group -L$DEVKITPRO/portlibs/switch/lib -L$DEVKITPRO/libnx/lib \
  -lz -lzstd -llz4 -llzma -lnx -lm
nacptool --create "Oblivion (recomp)" "ReXGlue port" "0.1" $OUT/oblivion.nacp
elf2nro $OUT/oblivion.elf /work/ports/switch/oblivion/oblivion.nro --nacp=$OUT/oblivion.nacp
ls -la /work/ports/switch/oblivion/oblivion.nro
