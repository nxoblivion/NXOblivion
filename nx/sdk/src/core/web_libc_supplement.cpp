/**
 * @file        rex/core/web_libc_supplement.cpp
 * @brief       What Emscripten's musl declares or the POSIX layer expects but the browser does not provide
 *              (reOblivion web, 2026-11-17)
 *
 * - pthread_setname_np: thread names are only for logs and profilers; the browser's workers keep theirs.
 * - pthread_{set,get}affinity_np: Web Workers run wherever the browser puts them; every thread reports all cores.
 */

#include <rex/platform.h>

#if REX_PLATFORM_WEB

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>

extern "C" {

int pthread_setname_np(pthread_t thread, const char* name) {
  (void)thread;
  (void)name;
  return 0;
}

int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize, const cpu_set_t* cpuset) {
  (void)thread;
  (void)cpusetsize;
  (void)cpuset;
  return 0;
}

int pthread_getaffinity_np(pthread_t thread, size_t cpusetsize, cpu_set_t* cpuset) {
  (void)thread;
  if (!cpuset) return EINVAL;
  memset(cpuset, 0xFF, cpusetsize);
  return 0;
}

}  // extern "C"

#endif  // REX_PLATFORM_WEB
