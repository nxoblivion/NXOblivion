/* FFmpeg configuration for the browser (reOblivion web, 2026-11-17: WebAssembly wasm64 under Emscripten).
 *
 * Linux/aarch64's config with every CPU-specific path off: WebAssembly has no inline assembly and FFmpeg has no
 * wasm SIMD code, so the C versions are used (the SDK's CMake adds no arch sources: Emscripten's processor is
 * neither x86_64 nor arm64). musl and Emscripten provide the rest (pthreads, the math functions). */
#pragma once
#include "config_linux_aarch64.h"

#undef ARCH_AARCH64
#define ARCH_AARCH64 0
#undef HAVE_ARMV8
#define HAVE_ARMV8 0
#undef HAVE_NEON
#define HAVE_NEON 0
#undef HAVE_VFP
#define HAVE_VFP 0
#undef HAVE_ARMV8_EXTERNAL
#define HAVE_ARMV8_EXTERNAL 0
#undef HAVE_NEON_EXTERNAL
#define HAVE_NEON_EXTERNAL 0
#undef HAVE_VFP_EXTERNAL
#define HAVE_VFP_EXTERNAL 0
#undef HAVE_ARMV8_INLINE
#define HAVE_ARMV8_INLINE 0
#undef HAVE_NEON_INLINE
#define HAVE_NEON_INLINE 0
#undef HAVE_VFP_INLINE
#define HAVE_VFP_INLINE 0
#undef HAVE_INLINE_ASM
#define HAVE_INLINE_ASM 0
#undef HAVE_INTRINSICS_NEON
#define HAVE_INTRINSICS_NEON 0
#undef HAVE_INLINE_ASM_LABELS
#define HAVE_INLINE_ASM_LABELS 0
#undef HAVE_INLINE_ASM_NONLOCAL_LABELS
#define HAVE_INLINE_ASM_NONLOCAL_LABELS 0
#undef HAVE_SYMVER_ASM_LABEL
#define HAVE_SYMVER_ASM_LABEL 0
#undef HAVE_SYMVER_GNU_ASM
#define HAVE_SYMVER_GNU_ASM 0
#undef HAVE_ASM_TYPES_H
#define HAVE_ASM_TYPES_H 0
#undef HAVE_FAST_CLZ
#define HAVE_FAST_CLZ 0
#undef HAVE_SCHED_GETAFFINITY
#define HAVE_SCHED_GETAFFINITY 0
#undef HAVE_SYSCTL
#define HAVE_SYSCTL 0
#undef HAVE_ARC4RANDOM
#define HAVE_ARC4RANDOM 0
