#!/bin/bash
# Builds the user-facing installer folder out/oblivion-installer/:
#   OblivionNX-Installer.exe   the GUI (self-contained .NET, nothing to install)
#   kit/                       clang + lld (LLVM), ReXGlue codegen, the precompiled port (SDK runtime, GL backend,
#                              main.o, libnx, Mesa, newlib/libstdc++), headers, kit.json
# The folder holds NO game data and NO game-derived code: the installer recompiles the game code on the user's PC
# from their own default.xex. assets/ (the developer's own copy) is only used here to find which headers the
# generated code includes and to record the expected file list / default.xex hash.
#
# Prerequisites: LLVM for Windows (clang 22, "C:/Program Files/LLVM"), the switch-gl SDK libs and app objects
# (ports/switch/oblivion_gl/rebuild.sh), Docker (first run only, to copy the devkitA64 sysroot), dotnet SDK 10.
# Usage: ports/switch/installer/build-kit.sh [out-dir]
set -e
cd "$(dirname "$0")/../../.." || exit 1
ROOT=$(pwd)
DIST=${1:-out/oblivion-installer}
KIT=$DIST/kit
STAGE=out/switch-kit                 # scratch: sysroot copy + reference build
SDK=out/sdk-gl                       # switch-gl SDK worktree (headers + libs must come from the same tree)
L=$SDK/out/switch-arm64
APP=out/switch-gen/app_gl            # main.o / hostprof.o from ports/switch/oblivion_gl/build.sh
LLVM="${LLVM_DIR:-C:/Program Files/LLVM}"
CLANG_VER=$(ls "$LLVM/lib/clang" | sort -V | tail -1)
REXGLUE=thirdparty/rexglue-sdk/out/install/win-amd64/bin
VERSION=$(grep -o 'nacptool --create "Oblivion" "ReXGlue port" "[^"]*"' ports/switch/oblivion_gl/build.sh | grep -o '"[0-9][^"]*"' | tr -d '"')
export PATH="$LLVM/bin:$PATH"
for f in "$L/librexruntime.a" "$L/librexgpu-gl.a" "$APP/main.o" "$APP/hostprof.o" assets/default.xex "$LLVM/bin/clang.exe" "$REXGLUE/rexglue.exe"; do
  [ -f "$f" ] || { echo "missing $f" >&2; exit 1; }
done
echo "kit v$VERSION, clang $CLANG_VER"

# ---- 1. devkitA64 sysroot (headers, newlib/libstdc++/libgcc, libnx, Mesa portlibs) from the official image ----
S=$STAGE/sysroot
if [ ! -f $S/libnx/switch.ld ] || [ ! -d $S/licenses ]; then
  echo "copying the devkitA64 sysroot out of devkitpro/devkita64"
  MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/work" -w /work devkitpro/devkita64:latest bash -c '
    set -e; D=$DEVKITPRO; S=/work/'$S'; rm -rf $S; mkdir -p $S/a64/lib/pic $S/gcc/pic $S/libnx $S/portlibs/lib
    G=$(ls -d $D/devkitA64/lib/gcc/aarch64-none-elf/*)
    cp -r $D/devkitA64/aarch64-none-elf/include $S/a64/
    cp $D/devkitA64/aarch64-none-elf/lib/pic/*.a $S/a64/lib/pic/
    cp -r $G/include $S/gcc/; cp $G/pic/*.o $G/pic/libgcc.a $S/gcc/pic/
    cp -r $D/libnx/include $D/libnx/lib $D/libnx/switch.ld $D/libnx/switch.specs $S/libnx/
    cp -r $D/portlibs/switch/include $S/portlibs/
    for l in z zstd lz4 lzma glad EGL glapi drm_nouveau; do cp $D/portlibs/switch/lib/lib$l.a $S/portlibs/lib/; done
    cp -r $D/licenses $S/licenses 2>/dev/null || mkdir -p $S/licenses
    ls $D/portlibs/switch/share/licenses 2>/dev/null && cp -r $D/portlibs/switch/share/licenses $S/licenses/portlibs || true'
fi
CXXV=$(ls $S/a64/include/c++ | sort -V | tail -1)

# ---- 2. Include roots: kit name -> source dir (the generated code may include from any of them) ----
declare -A INC=(
  [cxx]=$S/a64/include/c++/$CXXV
  [newlib]=$S/a64/include
  [sdk]=$SDK/include
  [sdkbuild]=$SDK/out/build/switch-release/include
  [simde]=$SDK/thirdparty/simde
  [fmt]=$SDK/thirdparty/fmt/include
  [spdlog]=$SDK/thirdparty/spdlog/include
  [toml]=$SDK/thirdparty/tomlplusplus/include
  [clang]="$LLVM/lib/clang/$CLANG_VER/include"
)
# Flags shared by the reference build (real paths) and kit.json ({kit} paths). Same options as compile-generated.sh.
COMMON='--target=aarch64-none-elf -march=armv8-a+crc+crypto -mtune=cortex-a57 -fPIC -ffunction-sections -fdata-sections
-O2 -DNDEBUG -std=c++23 -ffp-contract=off -fno-strict-aliasing -fno-char8_t -g0 -w
-DREXGLUE_BUILD_CONFIG=\"Release\" -DREX_PLATFORM_SWITCH=1 -D_GNU_SOURCE -D__SWITCH__
-DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL -DSPDLOG_NO_TZ_OFFSET -nostdlibinc'
inc_flags() { # $1 = function mapping a root name to a path
  echo "-isystem $($1 cxx) -isystem $($1 cxx)/aarch64-none-elf -isystem $($1 cxx)/backward -idirafter $($1 newlib)
-I$($1 sdk) -I$($1 sdkbuild) -I$($1 simde) -I$($1 simde)/simde -I$($1 fmt) -I$($1 spdlog) -isystem $($1 toml)"
}
real_root() { echo "${INC[$1]}"; }
kit_root() { echo "{kit}/include/$1"; }

# ---- 3. pch patches (same as compile-generated.sh, but TLS-free for clang: see pch-append.h) ----
mkdir -p $STAGE
PHYS_FIND='#define REX_PHYS_HOST_OFFSET(addr) 0u'
PHYS_REPL='#define REX_PHYS_HOST_OFFSET(addr) (__builtin_expect(((u32)(addr) >> 30) == 3u, 0) ? (((u32)(addr) >= 0xE0000000u) ? (long long)(0x1000 - 0x40000000) : (long long)(-0x20000000)) : 0ll)'
cat > $STAGE/pch-append.h <<'EOF'
// ---- Switch (installer kit, clang): lock-free mfmsr, per-thread nesting count ----
// Switch has no aliased views of physical memory: REX_PHYS_HOST_OFFSET folds the C0000000 / E0000000 aliases onto
// A0000000 (kit.json pchPatches). Guest "interrupts off" regions keep their nesting count per thread and read it
// without the global lock. The counter is a TLS variable of the precompiled kit (kit_tls.o). devkitPro's
// -mtp=soft thread pointer is *(tpidrro_el0 + 0x1F8) (libnx __aarch64_read_tp); clang has no such mode, so it is
// read by hand and the local-exec offset added.
extern "C" i32 ppc_global_lock_depth_tls;
__attribute__((always_inline)) inline i32* ppc_global_lock_depth_ptr_() {
  u64 p;
  __asm__("mrs %0, tpidrro_el0\n\t"
          "ldr %0, [%0, #0x1F8]\n\t"
          "add %0, %0, #:tprel_hi12:ppc_global_lock_depth_tls, lsl #12\n\t"
          "add %0, %0, #:tprel_lo12_nc:ppc_global_lock_depth_tls"
          : "=r"(p));
  return reinterpret_cast<i32*>(p);
}
#undef REX_CHECK_GLOBAL_LOCK
#undef REX_ENTER_GLOBAL_LOCK
#undef REX_LEAVE_GLOBAL_LOCK
#define REX_CHECK_GLOBAL_LOCK() (u64(*ppc_global_lock_depth_ptr_() ? 0 : 0x8000))
#define REX_ENTER_GLOBAL_LOCK()                          \
  do {                                                   \
    rex::thread::global_critical_region::mutex().lock(); \
    ++*ppc_global_lock_depth_ptr_();                     \
  } while (0)
#define REX_LEAVE_GLOBAL_LOCK()                            \
  do {                                                     \
    --*ppc_global_lock_depth_ptr_();                       \
    rex::thread::global_critical_region::mutex().unlock(); \
  } while (0)
EOF

# ---- 4. Reference build: codegen + clang -MD to learn exactly which headers the game code needs ----
REF=$STAGE/ref
rm -rf $REF; mkdir -p $REF/assets $REF/obj
cp assets/default.xex $REF/assets/
cp oblivion_manifest.toml $REF/
(cd $REF && "$ROOT/$REXGLUE/rexglue.exe" codegen oblivion_manifest.toml > codegen.log 2>&1) || { tail -20 $REF/codegen.log; exit 1; }
GEN=$REF/generated/default
python -I - "$GEN/oblivion_pch.h" "$PHYS_FIND" "$PHYS_REPL" $STAGE/pch-append.h <<'PYEOF'
import sys
p, find, repl, app = sys.argv[1:5]
t = open(p, encoding='utf-8').read()
assert find in t, 'REX_PHYS_HOST_OFFSET patch did not apply'
open(p, 'w', encoding='utf-8', newline='\n').write(t.replace(find, repl) + '\n' + open(app, encoding='utf-8').read())
PYEOF
REAL_FLAGS="$COMMON $(inc_flags real_root) -I$GEN"
echo "$REAL_FLAGS" | tr '\n' ' ' > $STAGE/ref-flags.txt
NSRC=$(ls $GEN/*.cpp | wc -l)
echo "reference compile: $NSRC files"
T0=$(date +%s)
ls $GEN/*.cpp | xargs -P "$(nproc)" -I{} sh -c 'b=$(basename {} .cpp); clang++ @'$STAGE'/ref-flags.txt -MD -MF '$REF'/obj/$b.d -c {} -o '$REF'/obj/$b.o || echo "FAIL $b"' | tee $REF/compile.log
grep -q FAIL $REF/compile.log && exit 1
echo "reference compile took $(( $(date +%s)-T0 )) s"

# ---- 5. Assemble the kit ----
rm -rf "$DIST"; mkdir -p $KIT/toolchain/bin "$KIT/toolchain/lib/clang/$CLANG_VER" $KIT/codegen $KIT/include $KIT/lib $KIT/payload $KIT/licenses
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

# Libraries and objects (debug info stripped: the SDK archives are ~570 MB with it).
for a in $(cat <<EOF
$L/librexgpu-gl.a $L/librexruntime.a $L/libspdlog.a $L/libfmt.a $L/liblibavcodec.a $L/liblibavutil.a $L/libmspack.a
$L/libaes128.a $L/libtiny-aes.a $L/libo1heap.a $L/libxxhash.a $L/libdisasm.a
EOF
); do llvm-strip --strip-debug "$a" -o $KIT/lib/$(basename $a); done
SPIRV_LIBS=""
for x in libspirv-cross-glsl.a libspirv-cross-core.a libSPIRV.a libglslang.a libMachineIndependent.a libGenericCodeGen.a libOGLCompiler.a libOSDependent.a; do
  [ -f $L/$x ] && { llvm-strip --strip-debug $L/$x -o $KIT/lib/$x; SPIRV_LIBS="$SPIRV_LIBS $x"; }
done
for a in $S/portlibs/lib/*.a $S/libnx/lib/libnx.a $S/a64/lib/pic/{libstdc++,libm,libg,libc,libsysbase}.a $S/gcc/pic/libgcc.a; do
  llvm-strip --strip-debug "$a" -o $KIT/lib/$(basename $a)
done
cp $S/gcc/pic/{crti,crtbegin,crtend,crtn}.o $KIT/lib/
llvm-strip --strip-debug $APP/main.o -o $KIT/lib/main.o
llvm-strip --strip-debug $APP/hostprof.o -o $KIT/lib/hostprof.o
cat > $STAGE/kit_tls.cpp <<'EOF'
// Port-kit glue for the clang-built game code (not game-derived).
extern "C" {
// mfmsr/mtmsrd nesting count read by the generated code through tpidrro_el0 (see pch-append.h).
__attribute__((tls_model("local-exec"))) thread_local int ppc_global_lock_depth_tls = 0;
}
// devkitPro's GNU ld fills .nx-module-name from --nx-module-name; lld has no such option.
__asm__(".section .nx-module-name, \"a\"\n\t.word 0\n\t.word 11\n\t.ascii \"oblivion_gl\\0\"\n\t.previous");
EOF
clang++ --target=aarch64-none-elf -march=armv8-a+crc+crypto -fPIC -O2 -c $STAGE/kit_tls.cpp -o $KIT/lib/kit_tls.o
# lld variant of libnx's switch.ld: PT_TLS header (lld requires one for TLS symbols), .relr.dyn not swallowing lld's
# .relr.auth.dyn, and the main thread's TLS reservation as NOBITS (otherwise .bss is written out as zeros).
sed -e 's@^\tdyn    PT_DYNAMIC;@\tdyn    PT_DYNAMIC;\n\ttls    PT_TLS;@' \
    -e 's@\.relr\.dyn : { \*(\.relr\.\*) } :rodata@.relr.dyn : { *(.relr.dyn) } :rodata@' \
    -e 's@^\t\.main\.tls ALIGN(MAX(ALIGNOF(.tdata),ALIGNOF(.tbss))) :@\t.main.tls ALIGN(MAX(ALIGNOF(.tdata),ALIGNOF(.tbss))) (NOLOAD) :@' \
    $S/libnx/switch.ld | perl -0pe 's/(\.tdata :\s*\{[^}]*\}) :data/$1 :data :tls/; s/(\.tbss :\s*\{[^}]*\}) :data/$1 :data :tls/' > $KIT/lib/switch-lld.ld
grep -q "tls    PT_TLS" $KIT/lib/switch-lld.ld && grep -q "(NOLOAD)" $KIT/lib/switch-lld.ld && [ "$(grep -c ':data :tls' $KIT/lib/switch-lld.ld)" = 2 ] \
  || { echo "switch-lld.ld patch did not apply" >&2; exit 1; }

# Codegen tool (Windows) + its runtime DLLs (MSVC runtime deployed app-locally).
cp $REXGLUE/rexglue.exe $REXGLUE/rexruntime.dll $REXGLUE/TracyClient.dll $KIT/codegen/
for d in vcruntime140.dll vcruntime140_1.dll msvcp140.dll; do cp "/c/Windows/System32/$d" $KIT/codegen/; done
cp oblivion_manifest.toml $KIT/codegen/oblivion_manifest.toml
cp $STAGE/pch-append.h $KIT/pch-append.h
cp ports/switch/package/oblivion.toml $KIT/payload/oblivion.toml

# Licenses of everything shipped in the kit.
cp thirdparty/rexglue-sdk/LICENSE $KIT/licenses/rexglue-sdk.txt
for t in fmt spdlog simde tomlplusplus FFmpeg libmspack SPIRV-Cross glslang o1heap xxHash tiny-aes-c; do
  f=$(ls $SDK/thirdparty/$t/LICENSE* $SDK/thirdparty/$t/COPYING* 2>/dev/null | head -1); [ -n "$f" ] && cp "$f" $KIT/licenses/$t.txt
done
cp -r $S/licenses $KIT/licenses/devkitpro 2>/dev/null || true

# ---- 6. kit.json ----
LINK_LIBS="librexgpu-gl.a librexruntime.a libspdlog.a libfmt.a liblibavcodec.a liblibavutil.a libmspack.a libaes128.a libtiny-aes.a libo1heap.a libxxhash.a libdisasm.a$SPIRV_LIBS"
python -I - "$KIT/kit.json" "$VERSION" assets "$NSRC" "$PHYS_FIND" "$PHYS_REPL" "$COMMON $(inc_flags kit_root) -I{gen}" "$LINK_LIBS" <<'PYEOF'
import sys, os, json, hashlib, shlex
out, version, assets, nsrc, find, repl, cflags, libs = sys.argv[1:9]
files = []
for root, _, names in os.walk(assets):
    for n in names:
        p = os.path.join(root, n)
        files.append({'path': os.path.relpath(p, assets).replace(os.sep, '/'), 'size': os.path.getsize(p)})
files.sort(key=lambda f: f['path'].lower())
xex = hashlib.sha256(open(os.path.join(assets, 'default.xex'), 'rb').read()).hexdigest()
wrap = ['nvc0_program_translate', 'fsFsCreateFile', 'fsFsDeleteFile', 'fsFsCreateDirectory', 'fsFsDeleteDirectory',
        'fsFsDeleteDirectoryRecursively', 'fsFsRenameFile', 'fsFsRenameDirectory']
k = '{kit}/lib/'
link = (['-EL', '-X', '-m', 'aarch64elf', '-T', k + 'switch-lld.ld', '-pie', '--no-dynamic-linker', '--gc-sections',
         '-z', 'text', '-z', 'now', '-z', 'nodynamic-undefined-weak', '-z', 'pack-relative-relocs', '--build-id=sha1']
        + ['--wrap=' + w for w in wrap] + ['--undefined=__wrap_nvc0_program_translate']
        + [k + 'crti.o', k + 'crtbegin.o', '-u', 'main', k + 'main.o', k + 'hostprof.o', '{objects}', k + 'kit_tls.o']
        + ['--start-group'] + [k + l for l in libs.split()] + ['--end-group', '-L{kit}/lib']
        + ['-l' + l for l in 'z zstd lz4 lzma glad EGL glapi drm_nouveau nx stdc++ m'.split()]
        + ['--start-group', '-lgcc', '-lg', '-lc', '-lsysbase', '--end-group', k + 'crtend.o', k + 'crtn.o'])
kit = {
    'version': version, 'title': 'Oblivion', 'author': 'ReXGlue port',
    'gameName': 'The Elder Scrolls IV: Oblivion (Xbox 360, title 425307D1)',
    'xexPath': 'default.xex', 'xexSha256': xex, 'files': files,
    'manifest': 'codegen/oblivion_manifest.toml', 'codegen': 'codegen/rexglue.exe',
    'clang': 'toolchain/bin/clang.exe', 'lld': 'toolchain/bin/ld.lld.exe',
    'compileArgs': shlex.split(cflags, posix=True),
    'pchName': 'oblivion_pch.h', 'pchPatches': [{'find': find, 'replace': repl}], 'pchAppend': 'pch-append.h',
    'linkArgs': link, 'expectedSources': int(nsrc),
}
json.dump(kit, open(out, 'w'), indent=1)
print(f"kit.json: {len(files)} game files ({sum(f['size'] for f in files)/1e9:.2f} GB), xex {xex[:16]}..., {nsrc} sources")
PYEOF

# ---- 7. The GUI ----
dotnet publish ports/switch/installer/OblivionInstaller/OblivionInstaller.csproj -c Release -o $STAGE/publish --nologo -v q
cp $STAGE/publish/OblivionNX-Installer.exe "$DIST/"
cp ports/switch/installer/README.txt "$DIST/README.txt"
du -sh "$DIST" $KIT/* | sed 's/^/  /'
echo "done: $DIST"
