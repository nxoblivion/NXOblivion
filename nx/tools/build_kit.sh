#!/bin/bash
# The Switch installer of the reNut build (docs/renut-port.md), the GL port's "bring your own copy" model
# (ports/switch/installer): nx/out/reoblivion-installer/ with
#   OblivionNX-Installer.exe   the same GUI as the GL port's (its kit.json says what to build)
#   kit/                       clang + lld, the SDK fork's codegen, the precompiled reNut build (SDK, renderer, plume,
#                              deko3d, libnx, newlib/libstdc++) without the game code and without any game shader,
#                              the headers the game code includes, the shader stage (XenosRecomp + DXC, spirv-cross,
#                              uam, zstd, the deko3d scripts, a Python runtime, the Bink port shaders, pipeline lists)
# On the player's PC the installer translates default.xex and the game's shaders from their own copy, compiles the
# game code and the shader table with clang, links with lld and writes the NRO plus deko3d_glsl.bin and
# deko3d_shaders.bin next to it. The kit holds NO game data and NO game-derived code or shader.
#
# Prerequisites (all from earlier steps of this tree): the Switch build (tools/build_switch.sh: its objects, the SDK's
# archives), the PC build (tools/build_pc.bat: XenosRecomp) and its codegen (sdk/out/win-amd64/rexglue.exe),
# tools/deko3d_build_tools_win.bat, the shader captures in out/renut_shader_dump (only the three Bink containers are
# read, for their hashes: the port shaders replace them), LLVM for Windows, a Windows CPython 3 (python on PATH),
# Docker on the first run (the devkitA64 sysroot), dotnet SDK 10. The game copy (app/assets) is read for the reference
# compile, the expected file list and the default.xex hash.
# Usage: nx/tools/build_kit.sh [out-dir]
set -e
cd "$(dirname "$0")/.." || exit 1
NX=$(pwd)
# The installer is assembled beside its place and moved there at the very end: a build that fails half way (the
# antivirus taking XenosRecomp.exe, 2026-11-11) used to leave the place emptied of the previous installer.
FINAL=${1:-out/reoblivion-installer}
DIST=$FINAL.new
KIT=$DIST/kit
STAGE=out/renut-kit
B=app/out/switch
L=sdk/out/switch-arm64
LLVM="${LLVM_DIR:-C:/Program Files/LLVM}"
CLANG_VER=$(ls "$LLVM/lib/clang" | sort -V | tail -1)
CODEGEN=sdk/out/win-amd64
XR=app/out/build/win-amd64-release/xenos_recomp/build/XenosRecomp
TOOLS=out/deko3d_tools/win
PORT=../tools/renut/port_shaders
VERSION=$(grep -o 'VERSION "[0-9.]*"' app/CMakeLists.txt | grep -o '[0-9][0-9.]*')
LISTS="app/out/build/win-amd64-release/pipeline_list.bin ${SWITCH_PIPELINE_LIST:-../tools/eden-renut/user/sdmc/switch/reoblivion/pipeline_list.bin}"
export PATH="$LLVM/bin:$PATH"
for f in $L/librexruntime.a $B/plume/libplume.a $B/plume/uam_local.o $B/build.ninja app/assets/default.xex \
         "$LLVM/bin/clang.exe" $CODEGEN/rexglue.exe $XR/XenosRecomp.exe $TOOLS/uam.exe $TOOLS/spirv-cross.exe \
         $TOOLS/zstd.exe $LISTS; do
  [ -f "$f" ] || { echo "missing $f" >&2; exit 1; }
done
# Windows Defender has taken the locally built XenosRecomp.exe as Trojan:Win32/Bearfoos.B!ml (a machine-learning
# verdict, docs/renut-port.md) more than once, also in the middle of this script: read it now, before anything is built.
head -c 64 "$XR/XenosRecomp.exe" > /dev/null 2>&1 || {
  echo "cannot read $XR/XenosRecomp.exe: an antivirus holds or has removed it (Windows Security, Protection history)." >&2
  echo "Nothing was changed. The kit needs that tool (the player's shader stage runs it)." >&2
  exit 1; }
echo "reNut kit v$VERSION, clang $CLANG_VER"
mkdir -p $STAGE

# ---- 1. devkitA64 sysroot (headers, newlib/libstdc++/libgcc, libnx + deko3d, portlibs) from the official image ----
S=$STAGE/sysroot
if [ ! -f $S/libnx/switch.ld ] || [ ! -f $S/libnx/lib/libdeko3d.a ]; then
  echo "copying the devkitA64 sysroot out of the container"
  sh tools/switch_docker.sh 'set -e; D=$DEVKITPRO; S=/work/nx/'$S'; rm -rf $S; mkdir -p $S/a64/lib/pic $S/gcc/pic $S/libnx $S/portlibs/lib
    G=$(ls -d $D/devkitA64/lib/gcc/aarch64-none-elf/*)
    cp -r $D/devkitA64/aarch64-none-elf/include $S/a64/
    cp $D/devkitA64/aarch64-none-elf/lib/pic/*.a $S/a64/lib/pic/
    cp -r $G/include $S/gcc/; cp $G/pic/*.o $G/pic/libgcc.a $S/gcc/pic/
    cp -r $D/libnx/include $D/libnx/lib $D/libnx/switch.ld $D/libnx/switch.specs $S/libnx/
    cp -r $D/portlibs/switch/include $S/portlibs/
    for l in z zstd; do cp $D/portlibs/switch/lib/lib$l.a $S/portlibs/lib/; done
    mkdir -p $S/licenses; cp -r $D/licenses/. $S/licenses/ 2>/dev/null || true
    cp -r $D/portlibs/switch/share/licenses $S/licenses/portlibs 2>/dev/null || true'
fi
CXXV=$(ls $S/a64/include/c++ | sort -V | tail -1)

# ---- 2. Include roots: kit name -> source dir (the generated code may include from any of them) ----
declare -A INC=(
  [cxx]=$S/a64/include/c++/$CXXV
  [newlib]=$S/a64/include
  [libnx]=$S/libnx/include
  [portlibs]=$S/portlibs/include
  [sdk]=sdk/include
  [sdkbuild]=$B/rexglue-sdk/include
  [simde]=sdk/thirdparty/simde
  [fmt]=sdk/thirdparty/fmt/include
  [spdlog]=sdk/thirdparty/spdlog/include
  [toml]=sdk/thirdparty/tomlplusplus/include
  [vulkan]=sdk/thirdparty/vulkan-headers/include
  [clang]="$LLVM/lib/clang/$CLANG_VER/include"
)
# The Switch build's options for the game code (build.ninja, oblivion_recomp) in clang's spelling; -O2 as the GL kit.
COMMON='--target=aarch64-none-elf -march=armv8-a+crc+crypto -mtune=cortex-a57 -fPIC -ffunction-sections -fdata-sections
-O2 -DNDEBUG -std=gnu++23 -ffp-contract=off -fno-strict-aliasing -fno-math-errno -g0 -w
-D__SWITCH__ -DNX -D_GNU_SOURCE -DSPDLOG_NO_TZ_OFFSET -DREX_HAS_VULKAN=1 -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL
-nostdlibinc'
inc_flags() { # $1 = function mapping a root name to a path
  echo "-isystem $($1 cxx) -isystem $($1 cxx)/aarch64-none-elf -isystem $($1 cxx)/backward -idirafter $($1 newlib)
-isystem $($1 libnx) -isystem $($1 portlibs) -I$($1 sdk) -I$($1 sdkbuild) -I$($1 simde) -I$($1 simde)/simde -I$($1 fmt)
-I$($1 spdlog) -I$($1 vulkan) -isystem $($1 toml)"
}
real_root() { echo "${INC[$1]}"; }
kit_root() { echo "{kit}/include/$1"; }

# ---- 3. Reference build: codegen + clang -MD, to learn which headers the game code needs ----
REF=$STAGE/ref
rm -rf $REF; mkdir -p $REF/assets $REF/obj
cp app/assets/default.xex $REF/assets/
cp app/oblivion_manifest.toml $REF/
(cd $REF && REX_VFS_INDEX=false "$NX/$CODEGEN/rexglue.exe" codegen oblivion_manifest.toml > codegen.log 2>&1) \
  || { tail -20 $REF/codegen.log; exit 1; }
GEN=$REF/generated/default
echo "$COMMON $(inc_flags real_root) -I$GEN" | tr '\n' ' ' > $STAGE/ref-flags.txt
NSRC=$(ls $GEN/*.cpp | wc -l)
echo "reference compile: $NSRC files"
T0=$(date +%s)
ls $GEN/*.cpp | xargs -P "$(nproc)" -I{} sh -c 'b=$(basename {} .cpp); clang++ @'$STAGE'/ref-flags.txt -MD -MF '$REF'/obj/$b.d -c {} -o '$REF'/obj/$b.o || echo "FAIL $b"' | tee $REF/compile.log
grep -q FAIL $REF/compile.log && exit 1
echo "reference compile took $(( $(date +%s)-T0 )) s"
# clang has no -mtp=soft (devkitPro's thread pointer): the game code must not use thread-local storage.
llvm-objdump -r $REF/obj/*.o > $STAGE/ref-relocs.txt
grep -q R_AARCH64 $STAGE/ref-relocs.txt || { echo "no relocations listed: the TLS check cannot run" >&2; exit 1; }
TLS=$(grep -c "TLS" $STAGE/ref-relocs.txt || true)
[ "$TLS" = 0 ] || { echo "the game code uses thread-local storage ($TLS relocations): clang cannot reach it" >&2; exit 1; }

# ---- 4. The Bink port shaders (no game shader code): their HLSL through the translator's override, then the GLSL
# chain. The three captured containers only give XenosRecomp the hashes; their own code is not used. ----
P=$STAGE/port
rm -rf $P; mkdir -p $P/dump
for h in ps_7a0ce24f6a7ded83 ps_819818509cd1e1ed vs_66497d2fc8fccfac; do cp out/renut_shader_dump/$h.bin $P/dump/; done
python $PORT/port_shaders.py --out $P/hlsl --table $P/port_table.json
BANJO_HLSL_OVERRIDE="$(cygpath -w $P/hlsl)" BANJO_DEKO3D_SPIRV_DIR="$(cygpath -w $P/spirv)" $XR/XenosRecomp.exe \
  "$(cygpath -w $P/dump)" "$(cygpath -w $P/port_cache.cpp)" \
  "$(cygpath -w app/thirdparty/reblue-XenosRecomp/XenosRecomp/shader_common.h)" > $P/xenos_recomp.log
grep -q "^banjo-nx: 0 rewritten shaders (BANJO_HLSL_OVERRIDE) did not compile" $P/xenos_recomp.log \
  || { echo "a port shader did not compile (XenosRecomp kept the game's text)" >&2; exit 1; }
python tools/deko3d_shaders.py --spirv $P/spirv --out $P > /dev/null
python tools/deko3d_glsl_opt.py --in $P/glsl --out $P/glsl_opt > /dev/null
[ "$(ls $P/glsl_opt/*.glsl | wc -l)" = 3 ] || { echo "the port shaders' GLSL is missing" >&2; exit 1; }
# The port table must say what the translator says about the same interface.
python tools/renut_switch_shader_cache.py --installer $P/port_cache.cpp --out-dir $P/table_translator > /dev/null
python - $P/port_table.json $P/table_translator/shader_cache_switch.cpp <<'PYEOF'
import json, re, sys
port = json.load(open(sys.argv[1]))
text = open(sys.argv[2]).read()
nums = lambda s: [int(v, 0) for v in re.findall(r'0x[0-9A-Fa-f]+|\d+', s)]
block = lambda n: re.search(n + r'\[\]\s*=\s*\{\n(.*?)\n\};', text, re.S).group(1).split('\n')
entries = {e[0]: e for e in map(nums, block('ShaderCacheEntry g_shaderCacheEntries'))}
inputs = [nums(l) for l in block('ShaderVertexInput g_shaderVertexInputs')]
for stem, p in port.items():
    e = entries[int(stem[3:], 16)]
    got = {'textureFetchMask': e[7], 'floatConstants': e[16:24], 'vertexInputs': inputs[e[24]:e[24] + e[25]],
           'literals': e[27], 'specConstants': e[5], 'memExport': e[8:16], 'flags': e[28]}
    want = {'textureFetchMask': p['textureFetchMask'], 'floatConstants': p['floatConstants'],
            'vertexInputs': p['vertexInputs'], 'literals': 0, 'specConstants': 0, 'memExport': [0] * 8, 'flags': 0}
    if got != want:
        sys.exit(f'port table entry {stem} differs from the translator: {got} != {want}')
print('port table: the translator agrees on', len(port), 'shaders')
PYEOF

# ---- 5. Assemble the kit ----
rm -rf "$DIST"
mkdir -p $KIT/toolchain/bin "$KIT/toolchain/lib/clang/$CLANG_VER" $KIT/codegen $KIT/include/renut $KIT/lib/obj \
         $KIT/licenses $KIT/shaders/bin $KIT/shaders/scripts $KIT/shaders/port/glsl $KIT/shaders/lists
cp "$LLVM/bin/clang.exe" $KIT/toolchain/bin/
cp "$LLVM/bin/lld.exe" $KIT/toolchain/bin/ld.lld.exe          # lld picks its ELF flavour from the exe name
cp "$LLVM/LICENSE.TXT" $KIT/licenses/LLVM.txt 2>/dev/null || cp "$LLVM"/*LICENSE* $KIT/licenses/ 2>/dev/null || true

# Headers: only those the reference build read, copied under their kit root.
python -I - "$KIT" "$REF/obj" "$GEN" "$CLANG_VER" "${!INC[@]}" -- "${INC[@]}" <<'PYEOF'
import sys, os, shutil, glob
kit, objdir, gen, clangv = sys.argv[1:5]
rest = sys.argv[5:]; sep = rest.index('--'); names, dirs = rest[:sep], rest[sep+1:]
norm = lambda p: os.path.normcase(os.path.abspath(p))
roots = sorted(((norm(d), n) for n, d in zip(names, dirs)), key=lambda x: -len(x[0]))
deps = set()
for d in glob.glob(os.path.join(objdir, '*.d')):
    txt = open(d, encoding='utf-8').read().replace('\\\n', ' ')
    body = txt[txt.index('.o:') + 3:]  # after the target; paths themselves may contain "C:/"
    for tok in body.replace('\\ ', '\0').split():  # "\ " is an escaped space inside a path
        deps.add(tok.replace('\0', ' '))
copied = {}; skipped_gen = 0; unknown = []
g = norm(gen)
for dep in deps:
    p = norm(dep)
    if p.startswith(g) or p.endswith('.cpp'): skipped_gen += 1; continue
    for r, n in roots:
        if p.startswith(r + os.sep):
            rel = p[len(r) + 1:]
            dst = os.path.join(kit, 'toolchain', 'lib', 'clang', clangv, 'include', rel) if n == 'clang' else os.path.join(kit, 'include', n, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(dep, dst)
            copied[n] = copied.get(n, 0) + 1
            break
    else:
        unknown.append(dep)
if unknown:
    print('headers outside the known include roots:', *unknown[:10], sep='\n  '); sys.exit(1)
print('headers copied:', dict(sorted(copied.items())), '(generated:', skipped_gen, ')')
PYEOF
# The shader table's header (it includes <cstddef> and <cstdint> only, which the game code reads too).
cp app/src/renut_engine/gpu/shaders/shader_cache.h $KIT/include/renut/

# The precompiled build: the Switch build's own objects (not the game code's, not the two shader sources of the build
# tree), the SDK's and the renderer's archives, then devkitPro's. Debug info stripped.
python -I - $B/build.ninja $KIT/lib/obj > $STAGE/objects.txt <<'PYEOF'
import os, re, subprocess, sys
ninja, out = sys.argv[1:3]
line = next(l for l in open(ninja, encoding='utf-8') if l.startswith('build oblivion: '))
inputs = line.split(':', 1)[1].split(' | ')[0].split()[1:]  # after the rule name
kept = [i for i in inputs if '/generated/default/' not in i and '/renut_switch/' not in i]
for n, obj in enumerate(kept):
    name = f'{n:03d}_' + re.sub(r'\.(cpp|c)\.obj$', '.o', os.path.basename(obj))
    subprocess.run(['llvm-strip', '--strip-debug', os.path.join(os.path.dirname(ninja), obj), '-o', os.path.join(out, name)], check=True)
    print(name)
dropped = [i for i in inputs if i not in kept and '/generated/default/' not in i]
print(f'{len(kept)} objects of the build kept; dropped (game-derived): {[os.path.basename(d) for d in dropped]}', file=sys.stderr)
PYEOF
SDK_LIBS="librexruntime.a librexgpu-xenos.a libSPIRV.a libglslang.a libMachineIndependent.a libGenericCodeGen.a
libOSDependent.a libOGLCompiler.a libaes128.a libmspack.a liblibavcodec.a liblibavutil.a libspdlog.a libfmt.a libxxhash.a
libo1heap.a"
for a in $SDK_LIBS; do llvm-strip --strip-debug $L/$a -o $KIT/lib/$a; done
for a in $B/plume/libplume.a $B/librenut_zstd.a $B/librenut_smolv.a $B/librenut_xxhash.a $S/libnx/lib/libnx.a \
         $S/libnx/lib/libdeko3d.a $S/portlibs/lib/libz.a $S/portlibs/lib/libzstd.a \
         $S/a64/lib/pic/{libstdc++,libm,libc,libsysbase}.a $S/gcc/pic/libgcc.a; do
  llvm-strip --strip-debug "$a" -o $KIT/lib/$(basename $a)
done
llvm-strip --strip-debug $B/plume/uam_local.o -o $KIT/lib/uam_local.o
cp $S/gcc/pic/{crti,crtbegin,crtend,crtn}.o $KIT/lib/
# devkitPro's GNU ld fills .nx-module-name from --nx-module-name; lld has no such option.
cat > $STAGE/kit_glue.cpp <<'EOF'
// Port-kit glue for the lld link (not game-derived): the module name devkitPro's ld writes.
__asm__(".section .nx-module-name, \"a\"\n\t.word 0\n\t.word 8\n\t.ascii \"oblivion\\0\"\n\t.previous");
EOF
clang++ --target=aarch64-none-elf -march=armv8-a+crc+crypto -fPIC -O2 -c $STAGE/kit_glue.cpp -o $KIT/lib/kit_glue.o
# lld variant of libnx's switch.ld (as the GL kit): PT_TLS header, .relr.dyn not swallowing lld's .relr.auth.dyn, the
# main thread's TLS reservation as NOBITS.
sed -e 's@^\tdyn    PT_DYNAMIC;@\tdyn    PT_DYNAMIC;\n\ttls    PT_TLS;@' \
    -e 's@\.relr\.dyn : { \*(\.relr\.\*) } :rodata@.relr.dyn : { *(.relr.dyn) } :rodata@' \
    -e 's@^\t\.main\.tls ALIGN(MAX(ALIGNOF(.tdata),ALIGNOF(.tbss))) :@\t.main.tls ALIGN(MAX(ALIGNOF(.tdata),ALIGNOF(.tbss))) (NOLOAD) :@' \
    $S/libnx/switch.ld | perl -0pe 's/(\.tdata :\s*\{[^}]*\}) :data/$1 :data :tls/; s/(\.tbss :\s*\{[^}]*\}) :data/$1 :data :tls/' > $KIT/lib/switch-lld.ld
grep -q "tls    PT_TLS" $KIT/lib/switch-lld.ld && grep -q "(NOLOAD)" $KIT/lib/switch-lld.ld && [ "$(grep -c ':data :tls' $KIT/lib/switch-lld.ld)" = 2 ] \
  || { echo "switch-lld.ld patch did not apply" >&2; exit 1; }
echo "// The reNut build needs no additions to the generated header (kit.json pchAppend)." > $KIT/pch-append.h

# Codegen tool (Windows) + the MSVC runtime, app-local.
MSVC_DLLS="vcruntime140.dll vcruntime140_1.dll msvcp140.dll msvcp140_atomic_wait.dll"
cp $CODEGEN/rexglue.exe $CODEGEN/rexruntime.dll $KIT/codegen/
for d in $MSVC_DLLS; do cp "/c/Windows/System32/$d" $KIT/codegen/; done
cp app/oblivion_manifest.toml $KIT/codegen/oblivion_manifest.toml

# The shader stage: XenosRecomp + DXC, the deko3d tools and scripts, a Python runtime, the port shaders, the lists.
cp $XR/XenosRecomp.exe $XR/dxcompiler.dll $XR/dxil.dll $KIT/shaders/
for d in $MSVC_DLLS; do cp "/c/Windows/System32/$d" $KIT/shaders/; done
cp app/thirdparty/reblue-XenosRecomp/XenosRecomp/shader_common.h $KIT/shaders/
cp $TOOLS/spirv-cross.exe $TOOLS/uam.exe $TOOLS/zstd.exe $KIT/shaders/bin/
for s in deko3d_shaders.py deko3d_glsl.py deko3d_glsl_opt.py deko3d_shader_package.py deko3d_variant_cache.py \
         deko3d_tools.py renut_switch_shader_cache.py; do cp tools/$s $KIT/shaders/scripts/; done
cp $P/glsl_opt/*.glsl $KIT/shaders/port/glsl/
cp $P/port_table.json $KIT/shaders/port/
n=0; for l in $LISTS; do cp "$l" $KIT/shaders/lists/list$n.bin; n=$((n+1)); done
python tools/kit_python.py $KIT/python

# Licenses of everything shipped in the kit.
cp sdk/LICENSE $KIT/licenses/rexglue-sdk.txt 2>/dev/null || cp ../thirdparty/rexglue-sdk/LICENSE $KIT/licenses/rexglue-sdk.txt
for t in fmt spdlog simde tomlplusplus FFmpeg libmspack glslang o1heap xxHash tiny-aes-c vulkan-headers; do
  f=$(ls sdk/thirdparty/$t/LICENSE* sdk/thirdparty/$t/COPYING* 2>/dev/null | head -1); [ -n "$f" ] && cp "$f" $KIT/licenses/$t.txt
done
cp ../thirdparty/rexglue-sdk/thirdparty/SPIRV-Cross/LICENSE $KIT/licenses/SPIRV-Cross.txt
cp app/thirdparty/plume/LICENSE $KIT/licenses/plume.txt
cp app/thirdparty/uam/LICENSE $KIT/licenses/uam.txt
cp app/thirdparty/reblue-XenosRecomp/LICENSE.md $KIT/licenses/XenosRecomp.txt
f=$(ls app/thirdparty/reblue-XenosRecomp/thirdparty/zstd/LICENSE 2>/dev/null) && cp $f $KIT/licenses/zstd.txt
cp LICENSE $KIT/licenses/reoblivion-GPL-3.0.txt
cp -r $S/licenses $KIT/licenses/devkitpro 2>/dev/null || true

# ---- 6. kit.json ----
LIBS="$SDK_LIBS libplume.a librenut_zstd.a librenut_smolv.a librenut_xxhash.a libdeko3d.a libnx.a libzstd.a libz.a
libstdc++.a libm.a libgcc.a libc.a libsysbase.a"
# NXOblivion 0.1.23: the player's paths stay out of the NRO. Without the maps, __FILE__ in the game code and in the kit's
# headers wrote the installer's folder (the player's Windows user folder) into the program; clang matches the maps
# whatever the slashes.
PATH_MAPS='-ffile-prefix-map={work}=work -ffile-prefix-map={kit}=kit'
python -I - "$KIT/kit.json" "$VERSION" app/assets "$NSRC" "$COMMON $(inc_flags kit_root) -I{gen} $PATH_MAPS" "$(echo $LIBS)" \
  "$(cat $STAGE/objects.txt | tr '\n' ' ')" <<'PYEOF'
import sys, os, json, hashlib, shlex
out, version, assets, nsrc, cflags, libs, objs = sys.argv[1:8]
files = []
for root, _, names in os.walk(assets):
    for n in names:
        p = os.path.join(root, n)
        files.append({'path': os.path.relpath(p, assets).replace(os.sep, '/'), 'size': os.path.getsize(p)})
files.sort(key=lambda f: f['path'].lower())
xex = hashlib.sha256(open(os.path.join(assets, 'default.xex'), 'rb').read()).hexdigest()
# The Switch build's link (collect2 of tools/build_switch.sh's link), in lld's terms.
wrap = ['threadCreate', 'threadClose', 'nvFenceWait', 'nvGpuChannelKickoff', 'nvMapCreate', 'nvAddressSpaceAllocFixed',
        'nvioctlNvhostAsGpu_MapBufferEx', 'armDCacheClean', 'nwindowQueueBuffer', 'bqDequeueBuffer', 'svcSleepThread',
        'svcSetThreadCoreMask']
k = '{kit}/lib/'
link = (['-EL', '-X', '-m', 'aarch64elf', '-T', k + 'switch-lld.ld', '-pie', '--no-dynamic-linker', '--gc-sections',
         '-z', 'text', '-z', 'now', '-z', 'nodynamic-undefined-weak', '-z', 'pack-relative-relocs', '--build-id=sha1',
         '--allow-multiple-definition', k + 'crti.o', k + 'crtbegin.o', '-u', 'main',
         '--whole-archive', k + 'librexruntime.a', k + 'librexgpu-xenos.a', '--no-whole-archive']
        + ['--wrap=' + w for w in wrap]
        + ['{objects}'] + [k + 'obj/' + o for o in objs.split()] + [k + 'uam_local.o']
        + ['--start-group'] + [k + l for l in libs.split()] + ['--end-group']
        + [k + 'crtend.o', k + 'crtn.o', k + 'kit_glue.o'])
w = '{work}/shaders/'
py = '{kit}/python/python.exe'
sc = '{kit}/shaders/scripts/'
tools = {'DEKO3D_TOOLS': '{kit}/shaders/bin'}
lists = sorted(os.listdir(os.path.join(os.path.dirname(out), 'shaders', 'lists')))
steps = [
    {'label': 'XenosRecomp', 'exe': '{kit}/shaders/XenosRecomp.exe',
     'args': [w + 'dump', w + 'shader_cache_deko3d.cpp', '{kit}/shaders/shader_common.h'],
     'env': {'BANJO_DEKO3D_SPIRV_DIR': w + 'spirv'}},
    {'label': 'GLSL', 'exe': py, 'args': [sc + 'deko3d_shaders.py', '--spirv', w + 'spirv', '--out', w[:-1]], 'env': tools},
    {'label': 'GLSL passes', 'exe': py, 'args': [sc + 'deko3d_glsl_opt.py', '--in', w + 'glsl', '--out', w + 'glsl_opt']},
    {'label': 'port shaders', 'exe': py,
     'args': ['-c', 'import shutil, sys; shutil.copytree(sys.argv[1], sys.argv[2], dirs_exist_ok=True)',
              '{kit}/shaders/port/glsl', w + 'glsl_opt']},
    {'label': 'GLSL package', 'exe': py, 'args': [sc + 'deko3d_shader_package.py', '--glsl', w + 'glsl_opt', '--out', w + 'pkg'],
     'env': tools},
    {'label': 'shader table', 'exe': py,
     'args': [sc + 'renut_switch_shader_cache.py', '--installer', w + 'shader_cache_deko3d.cpp',
              '--port', '{kit}/shaders/port/port_table.json', '--out-dir', w + 'table']},
    {'label': 'DKSH cache', 'exe': py,
     'args': [sc + 'deko3d_variant_cache.py', '--generic'] + sum((['--list', '{kit}/shaders/lists/' + l] for l in lists), [])
             + ['--glsl', w + 'glsl_opt', '--out', w + 'deko3d_shaders.bin', '--jobs', '{jobs}'], 'env': tools},
]
kit = {
    'version': version, 'title': 'NXOblivion',
    'author': 'nxoblivion, after reNut (masterspike52) and katamari-nx (ChanseyIsTheBest)',
    'gameName': 'The Elder Scrolls IV: Oblivion (Xbox 360, title 425307D1)',
    'xexPath': 'default.xex', 'xexSha256': xex, 'files': files,
    'manifest': 'codegen/oblivion_manifest.toml', 'codegen': 'codegen/rexglue.exe',
    'clang': 'toolchain/bin/clang.exe', 'lld': 'toolchain/bin/ld.lld.exe',
    'compileArgs': shlex.split(cflags, posix=True),
    'pchName': 'oblivion_pch.h', 'pchPatches': [], 'pchAppend': 'pch-append.h',
    'linkArgs': link, 'expectedSources': int(nsrc),
    'nroPath': 'switch/reoblivion/reoblivion.nro',
    'shaders': {
        'gameShaderDir': 'Data/Shaders', 'dumpDir': 'shaders/dump', 'steps': steps,
        'sources': [w + 'table/shader_cache_switch.cpp'], 'sourceArgs': ['-I{kit}/include/renut'],
        'outputs': [{'from': 'shaders/pkg/deko3d_glsl.bin', 'to': 'deko3d_glsl.bin'},
                    {'from': 'shaders/deko3d_shaders.bin', 'to': 'deko3d_shaders.bin'}],
    },
}
json.dump(kit, open(out, 'w'), indent=1)
print(f"kit.json: {len(files)} game files ({sum(f['size'] for f in files)/1e9:.2f} GB), xex {xex[:16]}..., {nsrc} sources, "
      f"{len(objs.split())} build objects")
PYEOF

# ---- 7. The GUI ----
dotnet publish ../ports/switch/installer/OblivionInstaller/OblivionInstaller.csproj -c Release -o $STAGE/publish --nologo -v q
cp $STAGE/publish/OblivionNX-Installer.exe "$DIST/"
cp installer/README.txt "$DIST/README.txt"
du -sh "$DIST" $KIT/* | sed 's/^/  /'

# ---- 7b. This PC's user folder out of the kit (NXOblivion 0.1.23). The tools built here (rexglue.exe, rexruntime.dll,
# zstd.exe) hold their sources' full paths (__FILE__ in log lines); the user folder's name becomes "nxbuild" (as long as
# the name: padded with x or cut), so no offset in the files moves. Fails if any trace is left. ----
python -I - "$DIST" "$(cygpath -m "${USERPROFILE:-$HOME}")" <<'PYEOF'
import os, sys
dist, home = sys.argv[1], sys.argv[2].rstrip('/')
name = home.rsplit('/', 1)[1]
new_name = ('nxbuild' + 'x' * len(name))[:len(name)]
pairs = []
for sep in ('/', '\\', '\\\\'):
    old = home.replace('/', sep)
    pairs.append((old.encode(), old[:-len(name)].encode() + new_name.encode()))
changed = 0
for root, _, files in os.walk(dist):
    if os.path.relpath(root, dist).split(os.sep)[0] == 'work':
        continue
    for f in files:
        p = os.path.join(root, f)
        d = open(p, 'rb').read()
        e = d
        for old, new in pairs:
            e = e.replace(old, new)
        if e != d:
            open(p, 'wb').write(e)
            changed += 1
            print(f"  build paths renamed in {os.path.relpath(p, dist)}")
        low = e.lower()
        for old, _ in pairs:
            if old.lower() in low:
                sys.exit(f"{p} still holds {old.decode()}")
print(f"user folder: {changed} file(s) changed, none left")
PYEOF

# ---- 8. Into its place: only a complete installer replaces the previous one (whose work folder, the builds and the
# translated shaders of earlier runs, is kept) ----
[ -f $KIT/shaders/XenosRecomp.exe ] || {
  echo "XenosRecomp.exe is gone from the new kit (an antivirus?): $FINAL is left as it was; the new one is $DIST" >&2
  exit 1; }
if [ -d "$FINAL/work" ] && [ ! -e "$DIST/work" ]; then mv "$FINAL/work" "$DIST/work"; fi
rm -rf "$FINAL"
mv "$DIST" "$FINAL"
echo "done: $FINAL"
