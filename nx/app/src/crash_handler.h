// Developer aid: logs the faulting address and a host backtrace on a fatal
// exception. Windows only; no-op elsewhere (macOS/Linux have native crash tools).
#pragma once

#ifdef _WIN32
#include <windows.h>
#include <cstdio>

inline LONG WINAPI OblivionCrashFilter(EXCEPTION_POINTERS* ep) {
  FILE* f = std::fopen("oblivion_crash.txt", "w");
  if (!f) f = stderr;
  std::fprintf(f, "exception 0x%08lX at %p\n", ep->ExceptionRecord->ExceptionCode,
               ep->ExceptionRecord->ExceptionAddress);
  void* frames[48];
  USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
  for (USHORT i = 0; i < n; ++i) {
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)frames[i], &mod);
    char name[MAX_PATH] = "?";
    if (mod) GetModuleFileNameA(mod, name, MAX_PATH);
    std::fprintf(f, "  #%u %p %s+0x%llx\n", i, frames[i], name,
                 (unsigned long long)((char*)frames[i] - (char*)mod));
  }
  if (f != stderr) std::fclose(f);
  return EXCEPTION_CONTINUE_SEARCH;
}
inline void InstallOblivionCrashHandler() { SetUnhandledExceptionFilter(OblivionCrashFilter); }
#else
inline void InstallOblivionCrashHandler() {}
#endif
