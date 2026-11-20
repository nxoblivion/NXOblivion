// Oblivion (Xbox 360 recompilation) - Nintendo Switch homebrew entry point (headless).
//
// Replaces src/main.cpp + OblivionApp (rex::ReXApp is a windowed SDL app and cannot be used
// here). It does what ReXApp::ConstructRuntime/LaunchModule do, minus window/ImGui:
// Runtime::Setup with kernel_init (xboxkrnl/xam/xbdm), the "null Xenos" GPU
// (rex::graphics::NullGraphicsSystem: consumes the ring buffer, 60 Hz vblank, no rendering), a
// paced null audio driver, no input; load game:\default.xex, start the main guest thread and wait.
//
// SD card layout (see ports/switch/README.md):
//   sdmc:/oblivion/assets/default.xex, Oblivion.esm, *.bsa ...   (the repo's assets/ folder)
//   sdmc:/oblivion/user, sdmc:/oblivion/cache                     (created on demand)
//   sdmc:/oblivion/log.txt                                        (spdlog output)
//   sdmc:/oblivion/oblivion.toml                                  (optional cvars)
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <unistd.h>

#include <switch.h>

#include <rex/ppc/context.h>
#include <rex/audio/nop/nop_audio_system.h>
#include <rex/cvar.h>
#include <rex/graphics/null_graphics_system.h>
#include <rex/image_info.h>
#include <rex/kernel/init.h>
#include <rex/kernel/crt/heap.h>
#include <rex/logging.h>
#include <spdlog/spdlog.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>
#include <rex/system/thread_state.h>

extern const rex::PPCImageInfo PPCImageConfig;

namespace rex::memory {
void SwitchSetGuestPool(void* pool, size_t size);
}

// Relative to the cwd (sdmc:/): std::filesystem::absolute() mangles "sdmc:/..." into "/sdmc:/...".
static const char* kRoot = "oblivion";

// ---- libnx hooks -----------------------------------------------------------------------
// The default libnx heap grabs nearly all free memory, which would starve the shared-memory
// segments that back the guest arena (memory_switch.cpp). Bound it instead.
extern "C" {
extern char* fake_heap_start;
extern char* fake_heap_end;

void __libnx_initheap(void) {
  // One heap block = [newlib malloc part][guest pool]. The pool backs committed guest memory
  // (svcMapMemory moves 64 KiB units of it into the guest windows, see memory_switch.cpp).
  u64 total = 0, used = 0;
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  u64 free_mem = total > used ? total - used : 0;
  u64 malloc_size = (640ull << 20), pool_size = (1280ull << 20);
  if (free_mem < malloc_size + pool_size + (256ull << 20)) {  // small budget (applet mode)
    malloc_size = (free_mem / 4) & ~0xFFFFFull;
    pool_size = (free_mem / 2) & ~0xFFFFull;
  }
  void* addr = nullptr;
  svcSetHeapSize(&addr, malloc_size + pool_size);
  fake_heap_start = (char*)addr;
  fake_heap_end = (char*)addr + malloc_size;
  rex::memory::SwitchSetGuestPool((char*)addr + malloc_size, pool_size);
}

void userAppInit(void) {
  fsdevMountSdmc();
}
}

static void status(const char* msg) {
  std::printf("%s\n", msg);
  consoleUpdate(nullptr);
}

static rex::Runtime* g_rt = nullptr;

// Debug aid: guest call stacks (saved-LR chain) of every guest thread, written to log.txt.
static void dump_guest_threads() {
  if (!g_rt) return;
  auto* m = g_rt->memory();
  auto ld = [&](uint32_t a) { return __builtin_bswap32(*(uint32_t*)m->TranslateVirtual(a)); };
  for (uint32_t id = 1; id < 40; ++id) {
    auto t = g_rt->kernel_state()->GetThreadByID(id);
    if (!t || !t->thread_state()) continue;
    auto* c = t->thread_state()->context();
    if (!c->r1.u32) continue;
    std::string st;
    uint32_t sp = c->r1.u32;
    for (int d = 0; d < 12 && sp > 0x70000000 && sp < 0x80000000; ++d) {
      uint32_t back = ld(sp);
      if (back <= sp || back >= 0x80000000) break;
      st += fmt::format(" {:08X}", ld(back - 8));
      sp = back;
    }
    std::string obj;
    if ((c->r3.u32 & 0xFF000000) == 0xF8000000) {
      auto o = g_rt->kernel_state()->object_table()->LookupObject<rex::system::XObject>(c->r3.u32);
      if (o) obj = fmt::format(" [wait obj type {}]", (int)o->type());
    }
    REXLOG_INFO("threads: t{} lr={:08X} r3={:08X}{} stack:{}", id, (uint32_t)c->lr, c->r3.u32, obj, st);
  }
}

static int real_main();
int main(int, char**) {
  try {
    return real_main();
  } catch (const std::exception& e) {
    REXLOG_ERROR("uncaught exception: {}", e.what());
  } catch (...) {
    REXLOG_ERROR("uncaught non-std exception");
  }
  return 1;
}

static int real_main() {
  chdir("sdmc:/");
  consoleInit(nullptr);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  std::error_code ec;
  std::filesystem::path root = kRoot;
  std::filesystem::create_directories(root / "user", ec);
  std::filesystem::create_directories(root / "cache", ec);

  u64 total = 0, used = 0;
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  std::printf("Oblivion (Switch recomp) - memory %llu/%llu MiB used\n",
              (unsigned long long)(used >> 20), (unsigned long long)(total >> 20));

  // Logging first, so every later failure is on the SD card.
  std::string log_path = (root / "log.txt").string();
  std::string cfg_path = (root / "oblivion.toml").string();
  if (std::filesystem::exists(cfg_path, ec)) rex::cvar::LoadConfig(cfg_path);
  auto log_config = rex::BuildLogConfig(log_path.c_str(), "debug", {});
  log_config.flush_level = spdlog::level::trace;  // flush every line: a crash must not lose the tail
  rex::InitLogging(log_config);
  std::set_terminate([] {
    REXLOG_ERROR("std::terminate called (uncaught exception)");
    svcOutputDebugString("std::terminate", 14);
    std::abort();
  });
  std::atexit([] { REXLOG_INFO("atexit: process is exiting"); });
  std::signal(SIGABRT, [](int) {
    REXLOG_ERROR("SIGABRT raised (abort/assert/REX_FATAL)");
    dump_guest_threads();
  });
  REXLOG_INFO("oblivion-switch starting; memory {} / {} MiB used", used >> 20, total >> 20);

  std::filesystem::path game = root / "assets";
  if (!std::filesystem::exists(game / "default.xex", ec)) {
    status("ERROR: sdmc:/oblivion/assets/default.xex not found");
    REXLOG_ERROR("sdmc:/oblivion/assets/default.xex not found");
  } else {
    auto runtime = std::make_unique<rex::Runtime>(game, root / "user", std::filesystem::path{},
                                                  root / "cache", std::filesystem::path{});
    rex::RuntimeConfig config;  // null Xenos GPU: consumes the ring buffer, no rendering
    config.graphics = std::make_unique<rex::graphics::NullGraphicsSystem>();
    config.kernel_init = rex::kernel::InitializeKernel;  // xboxkrnl/xam/xbdm modules + kernel variables
    config.audio_factory = REX_AUDIO_BACKEND(rex::audio::nop::NopAudioSystem);
    status("Setting up runtime...");
    auto st = runtime->Setup(PPCImageConfig, std::move(config));
    if (XFAILED(st)) {
      REXLOG_ERROR("Runtime setup failed: {:08X}", st);
      status("Runtime setup failed (see log.txt)");
    } else {
      if (PPCImageConfig.register_modules) PPCImageConfig.register_modules(runtime->kernel_state());
      st = runtime->LoadXexImage("game:\\default.xex");
      if (XFAILED(st)) {
        REXLOG_ERROR("Failed to load XEX: {:08X}", st);
        status("XEX load failed (see log.txt)");
      } else if (PPCImageConfig.rexcrt_heap &&
                 !rex::kernel::crt::InitHeap(REXCVAR_GET(rexcrt_heap_size_mb), runtime->memory())) {
        REXLOG_ERROR("Failed to initialize rexcrt heap");
      } else {
        status("Launching module...");
        auto main_thread = runtime->PrepareModuleLaunch();
        if (!main_thread) {
          REXLOG_ERROR("Failed to launch module");
        } else {
          g_rt = runtime.get();
          static Thread hb_thread;  // periodic progress marker + guest thread stacks in log.txt
          threadCreate(&hb_thread, [](void*) {
            u64 t0 = armGetSystemTick();
            for (int beat = 1;; ++beat) {
              svcSleepThread(10000000000ll);  // 10 s
              u64 total = 0, used = 0;
              svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
              svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
              REXLOG_INFO("app heartbeat: uptime {:.0f}s, host memory {}/{} MiB",
                          (double)(armGetSystemTick() - t0) / armGetSystemTickFreq(), used >> 20,
                          total >> 20);
              if (beat <= 6 || beat % 6 == 0) dump_guest_threads();
            }
          }, nullptr, nullptr, 0x10000, 0x2C, -2);
          threadStart(&hb_thread);
          main_thread->Resume();
          status("Guest main thread running (log: sdmc:/oblivion/log.txt)");
          main_thread->Wait(0, 0, 0, nullptr);
          REXLOG_INFO("Execution complete");
        }
      }
    }
    runtime.reset();
  }

  std::printf("\nPress + to exit.\n");
  consoleUpdate(nullptr);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(nullptr);
  }
  consoleExit(nullptr);
  return 0;
}
