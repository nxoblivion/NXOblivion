/**
 * @file        rex/core/fiber_web.cpp
 * @brief       Host fibers on WebAssembly (reOblivion web, 2026-11-17)
 *
 * WebAssembly has no way to switch stacks short of Asyncify (or the stack-switching proposal, not yet shipped),
 * and Asyncify would instrument the whole recompiled game. Every guest thread converts itself to a fiber
 * (xthread.cpp), which costs nothing here; only the CRT's guest fibers (rexcrt_SwitchToFiber) would need a real
 * switch, and Oblivion is not known to use them. Switching to a created fiber is reported and aborts.
 */

#include <rex/platform.h>

#if REX_PLATFORM_WEB

#include <cstdio>
#include <cstdlib>

#include <rex/thread/fiber.h>

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  (void)stack_size;
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  return f;
}

void Fiber::SwitchTo(Fiber* target) {
  if (target == tls_current_) return;
  std::fprintf(stderr, "[web] Fiber::SwitchTo: guest fibers are not supported in the browser build\n");
  std::abort();
}

void Fiber::Destroy() {
  if (is_thread_fiber_ && tls_current_ == this) tls_current_ = nullptr;
  delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_WEB
