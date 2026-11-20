#!/bin/sh
# Build oblivion_gl.nro (OpenGL Xenos backend). Run INSIDE the devkitA64 container from /work:
#   ports/switch/oblivion_gl/docker.sh gl 'ports/switch/oblivion_gl/build.sh'
# Prerequisites: the switch-gl SDK worktree built (out/sdk-gl/out/switch-arm64/*.a, incl. librexgpu-gl.a,
# libspirv-cross-*.a) and the generated objects compiled (ports/switch/compile-generated.sh -c -> out/switch-gen/*.o).
set -e
SDK=/work/out/sdk-gl
OUT=/work/out/switch-gen
GEN=$OUT/app_gl
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec"
mkdir -p $GEN
aarch64-none-elf-g++ -c /work/ports/switch/oblivion_gl/main.cpp -o $GEN/main.o \
  -DREX_PLATFORM_SWITCH=1 -D_GNU_SOURCE -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL -DSPDLOG_NO_TZ_OFFSET \
  -D__SWITCH__ -I$SDK/include -I$SDK/out/build/switch-release/include -I$SDK/thirdparty/fmt/include \
  -I$SDK/thirdparty/spdlog/include -isystem $SDK/thirdparty/tomlplusplus/include -I$SDK/thirdparty/simde \
  -I$DEVKITPRO/libnx/include $ARCH -O2 -DNDEBUG -std=c++23 -fPIE -g -ffunction-sections -w
aarch64-none-elf-g++ -c /work/ports/switch/oblivion_gl/hostprof.cpp -o $GEN/hostprof.o   -DREX_PLATFORM_SWITCH=1 -D_GNU_SOURCE -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL -DSPDLOG_NO_TZ_OFFSET   -D__SWITCH__ -I$SDK/include -I$SDK/out/build/switch-release/include -I$SDK/thirdparty/fmt/include   -I$SDK/thirdparty/spdlog/include -isystem $SDK/thirdparty/tomlplusplus/include -I$SDK/thirdparty/simde   -I$DEVKITPRO/libnx/include $ARCH -O2 -DNDEBUG -std=c++23 -fPIE -g -ffunction-sections -w
L=$SDK/out/switch-arm64
LIBS="$L/librexgpu-gl.a $L/librexruntime.a $L/libspdlog.a $L/libfmt.a $L/liblibavcodec.a $L/liblibavutil.a $L/libmspack.a \
 $L/libaes128.a $L/libtiny-aes.a $L/libo1heap.a $L/libxxhash.a $L/libdisasm.a"
# SPIRV-Cross / glslang SPIRV (spv::Builder) from the build tree
for x in libspirv-cross-glsl.a libspirv-cross-core.a libSPIRV.a libglslang.a libMachineIndependent.a libGenericCodeGen.a libOGLCompiler.a libOSDependent.a; do [ -f $L/$x ] && LIBS="$LIBS $L/$x"; done
# shellcheck disable=SC2086
aarch64-none-elf-g++ -specs=$DEVKITPRO/libnx/switch.specs $ARCH -fPIE -g -Wl,--gc-sections -Wl,--wrap=nvc0_program_translate -Wl,--undefined=__wrap_nvc0_program_translate -Wl,--wrap=fsFsCreateFile,--wrap=fsFsDeleteFile,--wrap=fsFsCreateDirectory,--wrap=fsFsDeleteDirectory,--wrap=fsFsDeleteDirectoryRecursively,--wrap=fsFsRenameFile,--wrap=fsFsRenameDirectory \
  -Wl,-Map,$OUT/oblivion_gl.map -o $OUT/oblivion_gl.elf $GEN/main.o $GEN/hostprof.o $OUT/oblivion_*.o \
  -Wl,--start-group $LIBS -Wl,--end-group -L$DEVKITPRO/portlibs/switch/lib -L$DEVKITPRO/libnx/lib \
  -lz -lzstd -llz4 -llzma -lglad -lEGL -lglapi -ldrm_nouveau -lnx -lm
nacptool --create "Oblivion" "ReXGlue port" "0.1.18" $OUT/oblivion_gl.nacp
elf2nro $OUT/oblivion_gl.elf $OUT/oblivion_gl.nro --nacp=$OUT/oblivion_gl.nacp --icon=/work/ports/switch/oblivion_gl/icon.jpg
# Built into out/switch-gen first: an Eden instance running the previous NRO keeps the file open (elf2nro failed).
cp -f $OUT/oblivion_gl.nro /work/ports/switch/oblivion_gl/oblivion_gl.nro || echo "WARNING: ports/switch/oblivion_gl/oblivion_gl.nro is in use; the new NRO is out/switch-gen/oblivion_gl.nro"
ls -la $OUT/oblivion_gl.nro /work/ports/switch/oblivion_gl/oblivion_gl.nro
