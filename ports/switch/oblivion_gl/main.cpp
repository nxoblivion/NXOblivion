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
#include <atomic>
#include <cstdarg>
#include <cstring>
#include <sys/stat.h>
#include <malloc.h>
#include <unordered_map>
#include <algorithm>
#include <vector>
#include <string>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <unistd.h>

#include <switch.h>
#include <spdlog/sinks/base_sink.h>
#include <mutex>

#include <rex/ppc/context.h>
#include <rex/audio/switch/switch_audio_system.h>
#include <rex/cvar.h>
#include <rex/chrono/clock.h>
#include <rex/graphics/gl/graphics_system.h>
#include <rex/input/input_system.h>
#include <rex/input/switch/switch_input_driver.h>
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

REXCVAR_DEFINE_BOOL(switch_diag, false, "Switch",
                    "Heavy diagnostics: 20 ms main-thread sample histogram and Oblivion queue/thread memory dumps");
REXCVAR_DEFINE_BOOL(switch_clocks, true, "Switch",
                    "Set CPU/GPU/memory clocks through clkrst/pcv (re-applied every 2 s). The old "
                    "appletSetCpuBoostMode(FastLoad) gives 1785 MHz CPU but drops the GPU to 76.8 MHz");
REXCVAR_DEFINE_INT32(switch_cpu_mhz, 1785, "Switch", "CPU clock in MHz with switch_clocks (0 = leave as is)");
REXCVAR_DEFINE_INT32(switch_gpu_mhz, 768, "Switch",
                     "GPU clock in MHz with switch_clocks (0 = auto: 460.8 handheld / 768 docked; -1 = leave as is). "
                     "768 by default in handheld too: the port is GPU-bound there (more battery use and heat)");
REXCVAR_DEFINE_INT32(switch_mem_mhz, 1600, "Switch", "Memory (EMC) clock in MHz with switch_clocks (0 = leave as is)");
REXCVAR_DEFINE_BOOL(switch_fastload_boost, false, "Switch",
                    "Use appletSetCpuBoostMode(FastLoad) (CPU 1785 MHz, GPU 76.8 MHz!) when clkrst/pcv is unavailable");

namespace rex::memory {
void SwitchSetGuestPool(void* pool, size_t size);
void* SwitchInitGuestArena();
void SwitchGuestPoolStats(size_t* used, size_t* peak, size_t* total, size_t* win0, size_t* win1);
}
namespace rex::graphics::gl::perf {  // SDK src/graphics/gl/perf.cpp (GL backend diagnostics)
std::string ActivityString();
std::string MemoryString();
}

// Relative to the cwd (sdmc:/): std::filesystem::absolute() mangles "sdmc:/..." into "/sdmc:/...".
static const char* kRoot = "oblivion";

// ---- boot breadcrumbs ---------------------------------------------------------------------
// sdmc:/oblivion/boot.txt, one fopen/fprintf/fflush/fclose per line so it survives a hang. Lines logged
// before the SD card is mounted (inside __libnx_initheap) are buffered and flushed by userAppInit.
// Also called (weak symbol) from the SDK: memory arena, GL init, audio, thread core masks.
static char g_pre[8192];
static size_t g_pre_len = 0;
static volatile bool g_sd_ready = false;
static std::atomic_flag g_bl_lock = ATOMIC_FLAG_INIT;
static bool g_console_up = false;

static void bl_write_line(const char* line) {
  FILE* f = std::fopen("sdmc:/oblivion/boot.txt", "a");
  if (!f) {
    mkdir("sdmc:/oblivion", 0777);
    f = std::fopen("sdmc:/oblivion/boot.txt", "a");
  }
  if (!f) return;
  std::fprintf(f, "%s\n", line);
  std::fflush(f);
  std::fclose(f);
}

extern "C" void oblivion_bootlog(const char* fmt, ...);
// ---- USB log streaming (libnx usbComms, device side) ------------------------------------
// Every breadcrumb and spdlog line is pushed (newline-terminated text) into a bounded ring; a dedicated
// thread drains it with usbCommsWrite, which blocks until a host reads. Lines are dropped when the ring is
// full, so the game never waits on USB. Read on the PC with ports/switch/usb_log.py.
static char g_usb_ring[256 * 1024];
static size_t g_usb_head = 0, g_usb_tail = 0;  // free-running counters
static Mutex g_usb_mtx;
static std::atomic<unsigned> g_usb_dropped{0};

static void usb_push(const char* s, size_t n) {
  mutexLock(&g_usb_mtx);
  if (g_usb_head - g_usb_tail + n + 1 > sizeof g_usb_ring) {
    g_usb_dropped.fetch_add(1);
  } else {
    for (size_t i = 0; i < n; ++i) g_usb_ring[(g_usb_head + i) % sizeof g_usb_ring] = s[i];
    g_usb_head += n;
    g_usb_ring[g_usb_head++ % sizeof g_usb_ring] = '\n';
  }
  mutexUnlock(&g_usb_mtx);
}
static size_t usb_pop(char* out, size_t cap) {
  mutexLock(&g_usb_mtx);
  size_t n = g_usb_head - g_usb_tail;
  if (n > cap) n = cap;
  for (size_t i = 0; i < n; ++i) out[i] = g_usb_ring[(g_usb_tail + i) % sizeof g_usb_ring];
  g_usb_tail += n;
  mutexUnlock(&g_usb_mtx);
  return n;
}
static Thread g_usb_thread;
static void usb_thread_fn(void*) {
  Result rc = usbCommsInitialize();
  oblivion_bootlog("usb: usbCommsInitialize rc=0x%x", (unsigned)rc);
  if (R_FAILED(rc)) return;
  static char chunk[4096];
  for (;;) {
    size_t n = usb_pop(chunk, sizeof chunk);
    if (!n) {
      svcSleepThread(20000000ll);
      continue;
    }
    for (size_t off = 0; off < n;) {
      size_t w = usbCommsWrite(chunk + off, n - off);  // blocks until a host reads; this thread only
      if (!w) { svcSleepThread(100000000ll); break; }  // error: drop the chunk and retry later
      off += w;
    }
  }
}
static void usb_start() {
  if (R_SUCCEEDED(threadCreate(&g_usb_thread, usb_thread_fn, nullptr, nullptr, 0x8000, 0x30, -2)))
    threadStart(&g_usb_thread);
  oblivion_bootlog("usb: log thread started");
}

class UsbSink : public spdlog::sinks::base_sink<std::mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& m) override {
    spdlog::memory_buf_t b;
    formatter_->format(m, b);
    size_t n = b.size();
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) --n;
    usb_push(b.data(), n);
  }
  void flush_() override {}
};

extern "C" void oblivion_try_flush_pre(void);
static void try_flush_pre() { oblivion_try_flush_pre(); }
extern "C" void oblivion_bootlog(const char* fmt, ...) {
  char buf[400];
  int n = std::snprintf(buf, sizeof buf, "[%8.3f] ", (double)armGetSystemTick() / armGetSystemTickFreq());
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf + n, sizeof buf - n, fmt, ap);
  va_end(ap);
  usb_push(buf, std::strlen(buf));
  while (g_bl_lock.test_and_set(std::memory_order_acquire)) svcSleepThread(1000);
  if (!g_sd_ready) try_flush_pre();
  if (!g_sd_ready) {
    size_t l = std::strlen(buf);
    if (g_pre_len + l + 2 < sizeof g_pre) {
      std::memcpy(g_pre + g_pre_len, buf, l);
      g_pre[g_pre_len + l] = '\n';
      g_pre_len += l + 1;
    }
  } else {
    bl_write_line(buf);
  }
  g_bl_lock.clear(std::memory_order_release);
}
#define BL(...) oblivion_bootlog(__VA_ARGS__)

// ---- clocks ---------------------------------------------------------------------------------
// Up to v0.1.10 the port used appletSetCpuBoostMode(FastLoad): that performance configuration is the one
// Horizon uses for loading screens: CPU 1785 MHz, memory 1600 MHz, but GPU 76.8 MHz. Every hardware GPU number
// before v0.1.11 was measured at 76.8 MHz. Now the clocks are set directly (clkrst on 8.0+, pcv before) and
// re-applied every 2 s (apm resets them on dock/undock and after sleep).
namespace swclk {
enum { kCpu, kGpu, kMem, kCount };
bool g_ok = false, g_clkrst = false;
ClkrstSession g_sess[kCount];
const PcvModuleId kIds[kCount] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};
const PcvModule kMods[kCount] = {PcvModule_CpuBus, PcvModule_GPU, PcvModule_EMC};
const char* const kNames[kCount] = {"cpu", "gpu", "mem"};
std::atomic<uint32_t> g_gpu_mhz{0};  // last read GPU clock (overlay)

u32 Get(int m) {
  u32 hz = 0;
  if (!g_ok) return 0;
  if (g_clkrst) clkrstGetClockRate(&g_sess[m], &hz);
  else pcvGetClockRate(kMods[m], &hz);
  return hz;
}
Result Set(int m, u32 hz) { return g_clkrst ? clkrstSetClockRate(&g_sess[m], hz) : pcvSetClockRate(kMods[m], hz); }

u32 TargetHz(int m) {
  int32_t mhz = 0;
  if (m == kCpu) mhz = REXCVAR_GET(switch_cpu_mhz);
  if (m == kMem) mhz = REXCVAR_GET(switch_mem_mhz);
  if (m == kGpu) {
    mhz = REXCVAR_GET(switch_gpu_mhz);
    if (mhz < 0) return 0;
    if (mhz == 0) return appletGetOperationMode() == AppletOperationMode_Console ? 768000000u : 460800000u;
  }
  return mhz > 0 ? u32(mhz) * 1000000u : 0u;
}

// Applies the targets that differ from the current rate by more than 1 MHz. Returns true if anything changed.
bool Apply(bool log) {
  if (!g_ok) return false;
  bool changed = false;
  for (int m = 0; m < kCount; ++m) {
    u32 want = TargetHz(m);
    if (!want) continue;
    u32 cur = Get(m);
    if (cur + 1000000u >= want && cur <= want + 1000000u) continue;
    Result rc = Set(m, want);
    u32 now = Get(m);
    changed = true;
    if (log) BL("clocks: %s %u -> %u Hz (want %u) rc=0x%x", kNames[m], cur, now, want, (unsigned)rc);
  }
  return changed;
}

void Init() {
  if (!REXCVAR_GET(switch_clocks)) {
    BL("clocks: switch_clocks=false, not touching clocks");
  } else if (hosversionAtLeast(8, 0, 0)) {
    Result rc = clkrstInitialize();
    if (R_SUCCEEDED(rc)) {
      g_ok = g_clkrst = true;
      for (int m = 0; m < kCount; ++m) {
        Result r2 = clkrstOpenSession(&g_sess[m], kIds[m], 3);
        if (R_FAILED(r2)) {
          BL("clocks: clkrstOpenSession(%s) rc=0x%x", kNames[m], (unsigned)r2);
          g_ok = false;
        }
      }
    } else {
      BL("clocks: clkrstInitialize rc=0x%x", (unsigned)rc);
    }
  } else {
    Result rc = pcvInitialize();
    g_ok = R_SUCCEEDED(rc);
    if (!g_ok) BL("clocks: pcvInitialize rc=0x%x", (unsigned)rc);
  }
  if (g_ok) {
    BL("clocks: before cpu=%u gpu=%u mem=%u Hz (mode %s)", Get(kCpu), Get(kGpu), Get(kMem),
       appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld");
    Apply(true);
    BL("clocks: after cpu=%u gpu=%u mem=%u Hz", Get(kCpu), Get(kGpu), Get(kMem));
    g_gpu_mhz.store(Get(kGpu) / 1000000u, std::memory_order_relaxed);
    static Thread t;
    if (R_SUCCEEDED(threadCreate(&t, [](void*) {
          int logged = 0;
          for (;;) {
            svcSleepThread(2000000000ll);
            if (Apply(logged < 20)) ++logged;
            g_gpu_mhz.store(Get(kGpu) / 1000000u, std::memory_order_relaxed);
          }
        }, nullptr, nullptr, 0x4000, 0x2C, -2)))
      threadStart(&t);
  } else if (REXCVAR_GET(switch_fastload_boost)) {
    Result brc = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    BL("clocks: fallback appletSetCpuBoostMode(FastLoad) rc=0x%x (GPU runs at 76.8 MHz!)", (unsigned)brc);
  }
}

void Restore() {
  if (g_ok) {
    // Hand the clocks back to apm: re-selecting the normal boost mode makes it re-apply its configuration.
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
  } else {
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
  }
}
}  // namespace swclk

// Read by the SDK perf overlay (weak reference there).
extern "C" uint32_t rex_switch_gpu_clock_mhz() { return swclk::g_gpu_mhz.load(std::memory_order_relaxed); }

// Static constructors: first (priority 101) and last (default priority, end of this TU).
struct CtorMarker {
  explicit CtorMarker(const char* m) { oblivion_bootlog("static ctors: %s", m); }
};
static CtorMarker g_ctor_first __attribute__((init_priority(101))) ("first marker (init_priority 101)");

// ---- libnx hooks -----------------------------------------------------------------------
// The default libnx heap grabs nearly all free memory, which would starve the shared-memory
// segments that back the guest arena (memory_switch.cpp). Bound it instead.
extern "C" {
extern char* fake_heap_start;
extern char* fake_heap_end;

// Tiny static fallback heap so early mallocs never hit a null heap if svcSetHeapSize fails.
alignas(0x1000) static char g_fallback_heap[2 << 20];
static Result g_heap_rc = 0;
static u64 g_hs_total, g_hs_used, g_hs_malloc, g_hs_pool;
static void* g_hs_addr;

void __libnx_initheap(void) {
  // One heap block = [newlib malloc part][guest pool]. The pool backs committed guest memory
  // (svcMapMemory moves 64 KiB units of it into the guest windows, see memory_switch.cpp).
  u64 total = 0, used = 0;
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  g_hs_total = total; g_hs_used = used;
  // Launched by nx-hbloader (hbmenu, real hardware): the loader already owns the process heap and
  // hands it over; svcSetHeapSize fails with 0xD401 then. Split the handed-over heap instead.
  // (Eden's direct -g NRO launch has no override and takes the svcSetHeapSize path below.)
  if (envHasHeapOverride()) {
    char* base = (char*)envGetHeapOverrideAddr();
    u64 size = envGetHeapOverrideSize();
    u64 pool_size = std::min<u64>(1536ull << 20, size / 2) & ~0xFFFFull;
    u64 malloc_size = (size - pool_size) & ~0xFFFFull;
    g_hs_malloc = malloc_size; g_hs_pool = pool_size; g_hs_addr = base;
    fake_heap_start = base;
    fake_heap_end = base + malloc_size;
    rex::memory::SwitchSetGuestPool(base + malloc_size, pool_size);
    return;
  }
  u64 free_mem = total > used ? total - used : 0;
  u64 malloc_size = (1100ull << 20), pool_size = (900ull << 20);
  if (free_mem < malloc_size + pool_size + (256ull << 20)) {  // small budget (applet mode)
    // svcSetHeapSize needs a multiple of 2 MiB; keep both parts 2 MiB aligned (pool also 64 KiB units).
    malloc_size = (free_mem / 4) & ~0x1FFFFFull;
    pool_size = (free_mem / 2) & ~0x1FFFFFull;
  }
  void* addr = nullptr;
  Result rc = svcSetHeapSize(&addr, malloc_size + pool_size);
  g_hs_total = total; g_hs_used = used; g_hs_malloc = malloc_size; g_hs_pool = pool_size;
  if (R_FAILED(rc) || !addr) {
    g_heap_rc = rc ? rc : 1;
    g_hs_addr = nullptr;
    fake_heap_start = g_fallback_heap;
    fake_heap_end = g_fallback_heap + sizeof g_fallback_heap;
    rex::memory::SwitchSetGuestPool(nullptr, 0);
    return;
  }
  g_hs_addr = addr;
  fake_heap_start = (char*)addr;
  fake_heap_end = (char*)addr + malloc_size;
  rex::memory::SwitchSetGuestPool((char*)addr + malloc_size, pool_size);
}

void oblivion_try_flush_pre(void) {
  if (g_sd_ready) return;
  FILE* f = std::fopen("sdmc:/oblivion/boot.txt", "w");  // truncate: one run per file
  if (!f) {
    // userAppInit is not necessarily called by this libnx: mount the SD card ourselves (once).
    static bool mount_tried = false;
    if (!mount_tried) {
      mount_tried = true;
      fsInitialize();
      Result mrc = fsdevMountSdmc();
      std::snprintf(g_pre + g_pre_len, sizeof g_pre - g_pre_len, "fsdevMountSdmc (late) rc=0x%x\n", (unsigned)mrc);
      g_pre_len += std::strlen(g_pre + g_pre_len);
    }
    mkdir("sdmc:/oblivion", 0777);
    f = std::fopen("sdmc:/oblivion/boot.txt", "w");
  }
  if (!f) return;
  std::fprintf(f, "oblivion-switch boot log (package v0.1.18 resolve-snapshots)\n");
  std::fprintf(f, "env: heap override=%d addr=%p size=0x%llx nso=%d\n", (int)envHasHeapOverride(),
               envGetHeapOverrideAddr(), (unsigned long long)envGetHeapOverrideSize(), (int)envIsNso());
  std::fprintf(f, "initheap: total=0x%llx used=0x%llx malloc=0x%llx pool=0x%llx svcSetHeapSize rc=0x%x addr=%p%s\n",
               (unsigned long long)g_hs_total, (unsigned long long)g_hs_used, (unsigned long long)g_hs_malloc,
               (unsigned long long)g_hs_pool, (unsigned)g_heap_rc, g_hs_addr,
               g_heap_rc ? " (FAILED, using 2 MiB fallback heap)" : "");
  if (g_pre_len) std::fwrite(g_pre, 1, g_pre_len, f);
  std::fflush(f);
  std::fclose(f);
  g_sd_ready = true;
  // Mesa / libdrm_nouveau report problems on stderr (e.g. NOUVEAU_ERR); keep them.
  if (std::freopen("sdmc:/oblivion/stderr.txt", "w", stderr)) setvbuf(stderr, nullptr, _IONBF, 0);
}

void userAppInit(void) {
  Result rc = fsdevMountSdmc();
  if (R_FAILED(rc)) return;  // nothing can be logged
  mkdir("sdmc:/oblivion", 0777);
  while (g_bl_lock.test_and_set(std::memory_order_acquire)) svcSleepThread(1000);
  oblivion_try_flush_pre();
  g_bl_lock.clear(std::memory_order_release);
  oblivion_bootlog("userAppInit: sdmc mounted rc=0x%x", (unsigned)rc);
}

// ---- crash handler ------------------------------------------------------------------------
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

static char g_crash_buf[8192];
void oblivion_crash_report(ThreadExceptionDump* ctx) {
  size_t n = 0;
  auto add = [&](const char* fmt, auto... a) {
    if (n < sizeof g_crash_buf - 200) n += std::snprintf(g_crash_buf + n, sizeof g_crash_buf - n, fmt, a...);
  };
  MemoryInfo mi{};
  u32 pi = 0;
  u64 modbase = 0;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)&oblivion_crash_report))) modbase = mi.addr;
  add("EXCEPTION desc=%u pc=0x%llx (pc-codebase=0x%llx) lr=0x%llx sp=0x%llx fp=0x%llx far=0x%llx esr=0x%x\n",
      ctx->error_desc, (unsigned long long)ctx->pc.x, (unsigned long long)(ctx->pc.x - modbase),
      (unsigned long long)ctx->lr.x, (unsigned long long)ctx->sp.x, (unsigned long long)ctx->fp.x,
      (unsigned long long)ctx->far.x, (unsigned)ctx->esr);
  add("code block base=0x%llx lr-base=0x%llx\n", (unsigned long long)modbase, (unsigned long long)(ctx->lr.x - modbase));
  for (int i = 0; i < 29; i += 2)
    add("x%d=%llx x%d=%llx\n", i, (unsigned long long)ctx->cpu_gprs[i].x, i + 1,
        i + 1 < 29 ? (unsigned long long)ctx->cpu_gprs[i + 1].x : 0ull);
  u64* fp = (u64*)ctx->fp.x;
  for (int d = 0; d < 16 && fp && ((u64)fp & 7) == 0; d++) {
    MemoryInfo m2{};
    if (R_FAILED(svcQueryMemory(&m2, &pi, (u64)fp)) || !(m2.perm & Perm_R)) break;
    add("bt%d=%llx (rel %llx)\n", d, (unsigned long long)fp[1], (unsigned long long)(fp[1] - modbase));
    if (fp[0] <= (u64)fp) break;
    fp = (u64*)fp[0];
  }
  MemoryInfo ms{};
  if (R_SUCCEEDED(svcQueryMemory(&ms, &pi, ctx->sp.x)) && (ms.perm & Perm_R)) {
    u64 lim = ms.addr + ms.size;
    u64 a = ctx->sp.x & ~7ull;
    int shown = 0;
    for (; a + 8 <= lim && shown < 96; a += 8) {
      u64 v = *(u64*)a;
      if (v >= modbase && v < modbase + 0x4000000) {  // looks like a code address
        add("stk+%llx: %llx (rel %llx)\n", (unsigned long long)(a - ctx->sp.x), (unsigned long long)v,
            (unsigned long long)(v - modbase));
        ++shown;
      }
      if (a - ctx->sp.x > 0x2000) break;
    }
  }
  if (!g_sd_ready) return;
  FILE* f = std::fopen("sdmc:/oblivion/crash.txt", "a");
  if (f) {
    std::fputs(g_crash_buf, f);
    std::fflush(f);
    std::fclose(f);
  }
  if (g_console_up) {
    std::printf("\n*** CRASH ***\n%.1400s\n", g_crash_buf);
    consoleUpdate(nullptr);
    svcSleepThread(30000000000ll);
  }
}
}

static int g_step = 0;
// Early-phase progress: breadcrumb + on-screen line (until the console is torn down before EGL).
static void step(const char* msg) {
  ++g_step;
  BL("step %d: %s", g_step, msg);
  if (g_console_up) {
    std::printf("Oblivion: starting... step %d: %s\n", g_step, msg);
    consoleUpdate(nullptr);
  }
}

static void status(const char* msg) {
  BL("status: %s", msg);
  if (g_console_up) std::printf("%s\n", msg);
}

static void console_up() {
  if (!g_console_up) {
    consoleInit(nullptr);
    g_console_up = true;
  }
}
static void console_down() {
  if (g_console_up) {
    consoleExit(nullptr);
    g_console_up = false;
  }
}

// Pre-flight screen (before EGL exists, so the libnx text console is usable): print a message and
// wait for Plus. Returns when the user presses Plus or the applet is asked to close.
static void show_message_and_wait(PadState* pad, const char* text) {
  BL("message screen: %.200s", text);
  console_up();
  std::printf("\n%s\n\nPress + to exit.\n", text);
  consoleUpdate(nullptr);
  while (appletMainLoop()) {
    if (pad) padUpdate(pad);
    if (pad && (padGetButtonsDown(pad) & HidNpadButton_Plus)) break;
    consoleUpdate(nullptr);
  }
  console_down();
}


static rex::Runtime* g_rt = nullptr;
namespace rex::graphics {
extern std::atomic<uint64_t> g_switch_gpu_packets;
extern std::atomic<uint32_t> g_switch_gpu_opcode, g_switch_gpu_wait_info, g_switch_gpu_wait_addr,
    g_switch_gpu_wait_ref, g_switch_gpu_wait_mask, g_switch_gpu_wait_value, g_switch_gpu_waiting,
    g_switch_cp_state, g_switch_vdswap_count;
}  // namespace rex::graphics

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
namespace rex::graphics {
extern std::atomic<bool> g_switch_shaders_ready;  // SDK GL command processor
}
void HostProfilerStart();  // hostprof.cpp (switch_hostprof_hz)

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
  // Reserve the guest windows in libnx virtmem BEFORE any other thread exists (logging, EGL ...): a
  // thread-stack mirror that lands inside a window makes a later commit fail with svcMapMemory 0xD401.
  BL("real_main entered");
  console_up();
  step("real_main entered");
  if (g_heap_rc) {
    char msg[300];
    std::snprintf(msg, sizeof msg,
                  "ERROR: svcSetHeapSize failed (rc 0x%x) for 0x%llx bytes.\nNot enough memory: launch with title takeover (hold R).",
                  (unsigned)g_heap_rc, (unsigned long long)(g_hs_malloc + g_hs_pool));
    show_message_and_wait(nullptr, msg);
    return 1;
  }
  step("guest arena (virtmem reservations)");
  void* arena = rex::memory::SwitchInitGuestArena();
  BL("SwitchInitGuestArena returned %p", arena);
  usb_start();  // after the arena reservations: a thread stack must not land in the guest windows
  step("chdir sdmc:/");
  chdir("sdmc:/");
  step("padConfigureInput");
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
  BL("pad initialised");

  step("create_directories");
  std::error_code ec;
  std::filesystem::path root = kRoot;
  std::filesystem::create_directories(root / "user", ec);
  BL("create_directories user ec=%d", ec.value());
  std::filesystem::create_directories(root / "cache", ec);
  BL("create_directories cache ec=%d", ec.value());

  u64 total = 0, used = 0;
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  // Applet mode (launched from the album / hbmenu without title takeover) only gets ~400 MiB.
  AppletType at = appletGetAppletType();
  BL("appletGetAppletType = %d, total=%llu MiB used=%llu MiB", (int)at, (unsigned long long)(total >> 20),
     (unsigned long long)(used >> 20));
  bool applet_mode = (at != AppletType_Application && at != AppletType_SystemApplication) ||
                     total < (2000ull << 20);
  if (applet_mode) {
    show_message_and_wait(&pad,
        "Oblivion cannot run in applet mode.\n\n"
        "This port needs the full application memory (about 3 GiB). Start it with title takeover:\n"
        "  1. Hold R while launching any installed game (e.g. from the home screen).\n"
        "  2. Keep holding R until hbmenu appears, then launch Oblivion from there.\n\n"
        "Do not open hbmenu through the Album applet.");
    return 0;
  }
  std::printf("Oblivion (Switch recomp) - memory %llu/%llu MiB used\n",
              (unsigned long long)(used >> 20), (unsigned long long)(total >> 20));
  consoleUpdate(nullptr);

  // Logging first, so every later failure is on the SD card.
  std::string log_path = (root / "log.txt").string();
  std::string cfg_path = (root / "oblivion.toml").string();
  // Keep the previous session's log next to the new one.
  std::filesystem::remove(root / "log.prev.txt", ec);
  std::filesystem::rename(log_path, root / "log.prev.txt", ec);
  step("LoadConfig");
  if (std::filesystem::exists(cfg_path, ec)) rex::cvar::LoadConfig(cfg_path);
  step("InitLogging");
  auto log_config = rex::BuildLogConfig(log_path.c_str(), "info", {});
  // Flushing every line to the SD card is very slow; flush on warn+ and every 2 s from a helper thread
  // (crash.txt / the USB stream cover the last moments of a crash).
  log_config.flush_level = spdlog::level::warn;
  log_config.extra_sinks.push_back(std::make_shared<UsbSink>());
  rex::InitLogging(log_config);
  std::set_terminate([] {
    try { if (auto e = std::current_exception()) std::rethrow_exception(e); } catch (const std::exception& ex) { REXLOG_ERROR("terminate: exception what()={}", ex.what()); } catch (...) { REXLOG_ERROR("terminate: non-std exception"); }
    REXLOG_ERROR("std::terminate called (uncaught exception)");
    svcOutputDebugString("std::terminate", 14);
    std::abort();
  });
  std::atexit([] { REXLOG_INFO("atexit: process is exiting"); });
  std::signal(SIGABRT, [](int) {
    REXLOG_ERROR("SIGABRT raised (abort/assert/REX_FATAL)");
    dump_guest_threads();
  });
  BL("InitLogging done");
  {
    static Thread flush_thread;
    if (R_SUCCEEDED(threadCreate(&flush_thread, [](void*) {
          for (;;) {
            svcSleepThread(2000000000ll);
            rex::FlushLogging();
          }
        }, nullptr, nullptr, 0x4000, 0x2C, -2)))
      threadStart(&flush_thread);
    // CPU 1785 / GPU 460.8 (handheld) or 768 (docked) / memory 1600 MHz for the whole run (restored on exit).
    swclk::Init();
    std::atexit([] { swclk::Restore(); });
  }
  REXLOG_INFO("oblivion-switch starting; memory {} / {} MiB used", used >> 20, total >> 20);

  std::filesystem::path game = root / "assets";
  if (!std::filesystem::exists(game / "default.xex", ec)) {
    REXLOG_ERROR("sdmc:/oblivion/assets/default.xex not found");
    show_message_and_wait(&pad,
        "ERROR: sdmc:/oblivion/assets/default.xex not found.\n\n"
        "Copy your own game files (default.xex, Oblivion.ini, Data, Data2) to\n"
        "  sdmc:/oblivion/assets/\n"
        "See INSTALL.md (copy-assets.ps1 does this from a PC).");
    return 1;
  } else {
    auto runtime = std::make_unique<rex::Runtime>(game, root / "user", std::filesystem::path{},
                                                  root / "cache", std::filesystem::path{});
    rex::RuntimeConfig config;  // OpenGL Xenos backend (EGL/Mesa nouveau)
    config.graphics = std::make_unique<rex::graphics::gl::GlGraphicsSystem>();
    config.kernel_init = rex::kernel::InitializeKernel;  // xboxkrnl/xam/xbdm modules + kernel variables
    config.audio_factory = REX_AUDIO_BACKEND(rex::audio::switch_hw::SwitchAudioSystem);
    config.input_factory = REX_INPUT_BACKEND(rex::input::CreateDefaultInputSystem);  // libnx HID driver
    step("runtime Setup (screen goes to GL; see boot.txt for gl:/audio: lines)");
    console_down();  // EGL takes over the display inside Setup
    BL("consoleExit done, calling Runtime::Setup");
    auto st = runtime->Setup(PPCImageConfig, std::move(config));
    BL("Runtime::Setup returned 0x%08X", (unsigned)st);
    if (XFAILED(st)) {
      REXLOG_ERROR("Runtime setup failed: {:08X}", st);
      status("Runtime setup failed (see log.txt)");
    } else {
      if (PPCImageConfig.register_modules) PPCImageConfig.register_modules(runtime->kernel_state());
      BL("loading XEX");
      st = runtime->LoadXexImage("game:\\default.xex");
      BL("LoadXexImage returned 0x%08X", (unsigned)st);
      if (XFAILED(st)) {
        REXLOG_ERROR("Failed to load XEX: {:08X}", st);
        status("XEX load failed (see log.txt)");
      } else if (PPCImageConfig.rexcrt_heap &&
                 !rex::kernel::crt::InitHeap(REXCVAR_GET(rexcrt_heap_size_mb), runtime->memory())) {
        REXLOG_ERROR("Failed to initialize rexcrt heap");
      } else {
        status("Launching module...");
        BL("PrepareModuleLaunch");
        auto main_thread = runtime->PrepareModuleLaunch();
        BL("PrepareModuleLaunch returned %p", (void*)main_thread.get());
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
              {
                struct mallinfo mi = mallinfo();
                // The malloc region is [fake_heap_start, fake_heap_end); the arena grows inside it (sbrk), so the
                // real headroom is the unused tail of the region plus the free chunks inside the arena.
                const char* brk = static_cast<const char*>(sbrk(0));
                const long long tail = (brk && brk <= fake_heap_end) ? (long long)(fake_heap_end - brk) : -1;
                REXLOG_INFO("malloc: region {} MiB, arena {} MiB, in use {} MiB, free in arena {} MiB, headroom {} MiB",
                            (long long)(fake_heap_end - fake_heap_start) >> 20, mi.arena >> 20, mi.uordblks >> 20,
                            mi.fordblks >> 20, tail < 0 ? -1 : (tail + (long long)mi.fordblks) >> 20);
                size_t pu = 0, pp = 0, pt = 0, w0 = 0, w1 = 0;
                rex::memory::SwitchGuestPoolStats(&pu, &pp, &pt, &w0, &w1);
                REXLOG_INFO("guest pool: committed {} MiB (peak {} MiB) of {} MiB | virtual 0x40000000 window {} MiB, physical 0xA0000000 window {} MiB",
                            pu >> 20, pp >> 20, pt >> 20, w0 >> 20, w1 >> 20);
                REXLOG_INFO("{}", rex::graphics::gl::perf::MemoryString());
                REXLOG_INFO("gpu {}", rex::graphics::gl::perf::ActivityString());
              }
              using namespace rex::graphics;
              static const char* const kCpState[] = {"executing", "ring-empty", "wait-reg-mem", "in-swap"};
              const uint32_t cp_state = g_switch_cp_state.load();
              REXLOG_INFO("gpu diag: cp={} guest vdswaps={} | clocks cpu={} gpu={} mem={} MHz", cp_state < 4 ? kCpState[cp_state] : "?",
                          g_switch_vdswap_count.load(), swclk::Get(swclk::kCpu) / 1000000u,
                          swclk::Get(swclk::kGpu) / 1000000u, swclk::Get(swclk::kMem) / 1000000u);
              REXLOG_INFO("gpu diag: swaps={} packets={} last_opcode={:02X} waiting={} wait info={:08X} addr={:08X} ref={:08X} mask={:08X} value={:08X}",
                          rex::input::switchpad::g_swap_counter.load(), g_switch_gpu_packets.load(),
                          g_switch_gpu_opcode.load(), g_switch_gpu_waiting.load(), g_switch_gpu_wait_info.load(),
                          g_switch_gpu_wait_addr.load(), g_switch_gpu_wait_ref.load(), g_switch_gpu_wait_mask.load(),
                          g_switch_gpu_wait_value.load());
              if (REXCVAR_GET(switch_diag) && beat % 3 == 0) {
                auto* mm = g_rt->memory();
                auto rd = [&](uint32_t a) { return __builtin_bswap32(*(uint32_t*)mm->TranslateVirtual(a)); };
                auto dump = [&](const char* nm, uint32_t a, int words) {
                  std::string h;
                  for (int i = 0; i < words; ++i) h += fmt::format(" {:08X}", rd(a + i * 4));
                  REXLOG_INFO("memdump {} @{:08X}:{}", nm, a, h);
                };
                uint32_t q = rd(0x82B05240);
                if (q > 0x40000000u && q < 0x80000000u) dump("q", q, 24);
                uint32_t item = (q > 0x40000000u && q < 0x80000000u) ? rd(rd(q + 12) + 8) : 0;
                if (item > 0x40000000u && item < 0x80000000u) dump("item", item, 80);
                uint32_t g = rd(0x82A8B824 + 4);
                dump("list", 0x82A8B824, 8);
                (void)g;
                for (uint32_t id : {7u, 8u, 11u, 13u, 9u}) {
                  auto t = g_rt->kernel_state()->GetThreadByID(id);
                  if (!t || !t->thread_state()) continue;
                  auto* c = t->thread_state()->context();
                  REXLOG_INFO("tregs t{}: r3={:08X} r26={:08X} r27={:08X} r28={:08X} r29={:08X} r30={:08X} r31={:08X}", id,
                              c->r3.u32, c->r26.u32, c->r27.u32, c->r28.u32, c->r29.u32, c->r30.u32, c->r31.u32);
                  uint32_t o = c->r29.u32;
                  if (o > 0x40000000u && o < 0x80000000u && id != 9) dump(fmt::format("t{}.r29", id).c_str(), o, 72);
                }
              }
              if (beat <= 6 || beat % 6 == 0) dump_guest_threads();
            }
          }, nullptr, nullptr, 0x10000, 0x2C, -2);
          threadStart(&hb_thread);
          static Thread prof_thread;  // main-thread (t6) sample histogram: lr + 3 saved-LR frames, top entries every 30 s
          threadCreate(&prof_thread, [](void*) {
            std::unordered_map<std::string, int> hist;
            auto* m = g_rt->memory();
            auto ld = [&](uint32_t a) { return __builtin_bswap32(*(uint32_t*)m->TranslateVirtual(a)); };
            for (int n = 1;; ++n) {
              svcSleepThread(20000000ll);
              auto t = g_rt->kernel_state()->GetThreadByID(6);
              if (t && t->thread_state()) {
                auto* c = t->thread_state()->context();
                uint32_t sp = c->r1.u32;
                std::string k = fmt::format("lr={:08X}", (uint32_t)c->lr);
                for (int d = 0; d < 4 && sp > 0x70000000 && sp < 0x80000000; ++d) {
                  uint32_t back = ld(sp);
                  if (back <= sp || back >= 0x80000000) break;
                  k += fmt::format(" {:08X}", ld(back - 8));
                  sp = back;
                }
                hist[k]++;
              }
              if (n % 1500 == 0) {
                std::vector<std::pair<int, std::string>> v;
                for (auto& kv : hist) v.push_back({kv.second, kv.first});
                std::sort(v.rbegin(), v.rend());
                for (size_t i = 0; i < v.size() && i < 10; ++i) REXLOG_INFO("prof t6 {}: {}", v[i].first, v[i].second);
                hist.clear();
              }
            }
          }, nullptr, nullptr, 0x10000, 0x2C, -2);
          if (REXCVAR_GET(switch_diag)) threadStart(&prof_thread);
          {
            // The GPU thread shows the "recompiling shaders" screen while the program cache is loaded / compiled
            // (gl_precompile_screen); the game, and with it the intro, starts only after that.
            BL("waiting for the GPU thread (start-up shader pre-compilation)");
            while (!rex::graphics::g_switch_shaders_ready.load()) svcSleepThread(50'000'000);
            BL("GPU thread ready");
          }
          HostProfilerStart();
          BL("starting guest main thread");
          main_thread->Resume();
          BL("guest main thread resumed");
          status("Guest main thread running (log: sdmc:/oblivion/log.txt)");
          main_thread->Wait(0, 0, 0, nullptr);
          REXLOG_INFO("Execution complete");
        }
      }
    }
    runtime.reset();
  }

  swclk::Restore();
  std::printf("\nPress + to exit.\n");

  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;

  }
  return 0;
}

static CtorMarker g_ctor_last("last marker (end of main.cpp TU)");
