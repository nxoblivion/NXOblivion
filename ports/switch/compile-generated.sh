#!/bin/sh
# Syntax-check (or compile) the generated recompiled game sources for aarch64-none-elf.
# Run INSIDE the devkitA64 container (see ports/switch/sdk-docker.sh), from /work.
# Usage: compile-generated.sh [-fsyntax-only|-c] [file...]   (default: all generated/default/*.cpp)
MODE="${1:--fsyntax-only}"; shift
SDK=/work/thirdparty/rexglue-sdk
FILES="${*:-$(ls /work/generated/default/*.cpp)}"
mkdir -p /work/out/switch-gen/inc
# The generated pch uses GCC-incompatible alias attributes (see below). Until the generated files are
# regenerated from the patched SDK template, shadow the headers with a patched copy (sources are fed on
# stdin, so the quote-include resolves via -I, shadow dir first).
cp /work/generated/default/*.h /work/out/switch-gen/inc/
# GCC (unlike clang) drops the weak alias when attributes precede extern "C": rewrite the
# non-Apple DEFINE_REX_FUNC (alias line replaced, the weak/noinline line deleted).
sed -i -e 's@^  __attribute__((alias("__imp__" #name))) *\\$@  extern "C" __attribute__((alias("__imp__" #name), weak, noinline)) REX_FUNC(name); \\@' \
  -e '/^  __attribute__((weak, noinline)) extern "C" REX_FUNC(name); *\\$/d' /work/out/switch-gen/inc/oblivion_pch.h
# Switch has no aliased views of physical memory (memory_switch.cpp): fold the C0000000 / E0000000 physical
# aliases (D3D writes microcode through one alias and reads it through another) onto the A0000000 window.
# The common case (below 0xC0000000) is one shift + compare that falls through; written as two selects, GCC put
# a taken branch and re-materialised constants on every guest load / store.
sed -i 's@^#define REX_PHYS_HOST_OFFSET(addr) 0u$@#define REX_PHYS_HOST_OFFSET(addr) (__builtin_expect(((u32)(addr) >> 30) == 3u, 0) ? (((u32)(addr) >= 0xE0000000u) ? (long long)(0x1000 - 0x40000000) : (long long)(-0x20000000)) : 0ll)@' /work/out/switch-gen/inc/oblivion_pch.h
grep -q "__builtin_expect(((u32)(addr) >> 30) == 3u" /work/out/switch-gen/inc/oblivion_pch.h || echo "WARNING: REX_PHYS_HOST_OFFSET patch did not apply"
# Guest "interrupts off" regions (mfmsr / mtmsrd r13 ... mtmsrd rX, e.g. around every lwarx/stwcx. refcount
# update) took the global recursive mutex three times: mfmsr locked it only to read the nesting count, which
# only the holder can ever see non-zero. Keep the count per thread and read it without the lock (enter / leave
# still take the mutex, so native code that holds it stays exclusive). ~10% of the main thread was mutex code.
cat >> /work/out/switch-gen/inc/oblivion_pch.h <<'EOF'

// ---- Switch (compile-generated.sh): lock-free mfmsr, per-thread nesting count ----
inline thread_local i32 ppc_global_lock_depth_ = 0;
#undef REX_CHECK_GLOBAL_LOCK
#undef REX_ENTER_GLOBAL_LOCK
#undef REX_LEAVE_GLOBAL_LOCK
#define REX_CHECK_GLOBAL_LOCK() (u64(ppc_global_lock_depth_ ? 0 : 0x8000))
#define REX_ENTER_GLOBAL_LOCK()                          \
  do {                                                   \
    rex::thread::global_critical_region::mutex().lock(); \
    ++ppc_global_lock_depth_;                            \
  } while (0)
#define REX_LEAVE_GLOBAL_LOCK()                            \
  do {                                                     \
    --ppc_global_lock_depth_;                              \
    rex::thread::global_critical_region::mutex().unlock(); \
  } while (0)
EOF
grep -q "alias(\"__imp__\" #name), weak" /work/out/switch-gen/inc/oblivion_pch.h || echo "WARNING: pch alias patch did not apply"
echo "$FILES" | tr ' ' '\n' | xargs -P "$(nproc)" -I{} sh -c '
  f={}; b=$(basename $f .cpp)
  if [ "'"$MODE"'" = "-c" ]; then OUT="-c -o /work/out/switch-gen/$b.o"; else OUT="-fsyntax-only"; fi
  aarch64-none-elf-g++ -DREXGLUE_BUILD_CONFIG=\"Release\" -DREX_PLATFORM_SWITCH=1 -D_GNU_SOURCE \
    -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL -DSPDLOG_NO_TZ_OFFSET \
    -I'$SDK'/include -I'$SDK'/out/build/switch-release/include \
    -I'$SDK'/thirdparty/simde -I'$SDK'/thirdparty/simde/simde \
    -I'$SDK'/thirdparty/fmt/include -I'$SDK'/thirdparty/spdlog/include \
    -isystem '$SDK'/thirdparty/tomlplusplus/include \
    -I/work/out/switch-gen/inc -I/work -I/work/src -I/work/generated/default \
    -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec \
    -ffunction-sections -fdata-sections -D__SWITCH__ -O2 -DNDEBUG -std=c++23 -fPIC \
    -ffp-contract=off -fno-strict-aliasing -fno-char8_t -g0 -w $OUT -x c++ - < $f 2>&1 \
    && echo "OK $b" || echo "FAIL $b"
'
