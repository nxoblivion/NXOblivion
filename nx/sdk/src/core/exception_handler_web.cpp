/**
 * @file        rex/core/exception_handler_web.cpp
 * @brief       Host exception handler on WebAssembly (reOblivion web, 2026-11-17)
 *
 * A wasm trap cannot be caught and resumed: an out-of-bounds access or an unreachable kills the instance. And the
 * guest window never faults (memory_web.cpp: protection is bookkeeping), so there is nothing for a handler to do.
 * Install and Uninstall keep the list, for symmetry, and nothing calls it.
 */

#include <rex/platform.h>

#if REX_PLATFORM_WEB

#include <rex/exception_handler.h>

namespace rex::arch {

void ExceptionHandler::Install(Handler fn, void* data) {
  (void)fn;
  (void)data;
}

void ExceptionHandler::Uninstall(Handler fn, void* data) {
  (void)fn;
  (void)data;
}

}  // namespace rex::arch

#endif  // REX_PLATFORM_WEB
