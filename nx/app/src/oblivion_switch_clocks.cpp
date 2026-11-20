// oblivion (nx): the console's clocks while the game runs (Switch only; the keys exist on every platform so one
// oblivion.toml works everywhere).
//
// Why: the game thread is the port's limit (docs/renut-port.md, "The game thread"), and Horizon runs a homebrew
// application at its stock clocks: CPU 1020 MHz, and in handheld mode GPU 307.2 MHz and memory 1331.2 MHz. The values
// below are operating points Nintendo's own performance configurations use (CPU 1785 MHz: the "boost mode" of loading
// screens; GPU 460.8 MHz: the handheld profile of demanding games; GPU 768 MHz and memory 1600 MHz: docked), none
// above them. No performance configuration offers CPU 1785 together with a working GPU clock (appletSetCpuBoostMode's
// drops the GPU to 76.8 MHz), so the clocks are set one by one through clkrst (pcv before 8.0.0), as the emulated-GPU
// port did (ports/switch/oblivion_gl/main.cpp, measured on the console: v0.1.11). apm puts its own values back at a
// dock or undock and after sleep: a thread applies them again every 2 s when they differ.
//
// katamari-nx asks for the handheld GPU profile through apm (banjo_gpu_boost, sdk/src/ui/switch_apm.cpp); this replaces
// it here, because the CPU has to go up as well and the two would fight over the GPU and memory clocks.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

REXCVAR_DEFINE_BOOL(switch_clocks, true, "Oblivion/Switch",
                    "Switch: set the CPU, GPU and memory clocks while the game runs (switch_cpu_mhz, switch_gpu_mhz, "
                    "switch_mem_mhz; applied again every 2 s, the system resets them at a dock, undock or sleep). "
                    "false: the system's own clocks (CPU 1020 MHz, handheld GPU 307.2 MHz)");
REXCVAR_DEFINE_INT32(switch_cpu_mhz, 1785, "Oblivion/Switch",
                     "Switch (switch_clocks): the CPU clock in MHz; 1785 is the system's boost-mode speed. 0: as it is");
REXCVAR_DEFINE_INT32(switch_gpu_mhz, 0, "Oblivion/Switch",
                     "Switch (switch_clocks): the GPU clock in MHz. 0: 460.8 in handheld mode and 768 docked (the "
                     "system's highest for each); -1: as it is. Above 460.8 in handheld mode is outside what the "
                     "system allows on battery");
REXCVAR_DEFINE_INT32(switch_mem_mhz, 1600, "Oblivion/Switch",
                     "Switch (switch_clocks): the memory clock in MHz (1600 is the docked speed). 0: as it is");

#if REX_PLATFORM_SWITCH

#include <atomic>

#include <switch.h>

namespace oblivion::clocks {
namespace {

enum { kCpu, kGpu, kMem, kCount };
bool g_ok = false, g_clkrst = false;
ClkrstSession g_sessions[kCount];
const PcvModuleId kIds[kCount] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};
const PcvModule kModules[kCount] = {PcvModule_CpuBus, PcvModule_GPU, PcvModule_EMC};
const char* const kNames[kCount] = {"CPU", "GPU", "memory"};
Thread g_thread;
std::atomic<bool> g_stop{false};

u32 Get(int m) {
  u32 hz = 0;
  if (g_clkrst)
    clkrstGetClockRate(&g_sessions[m], &hz);
  else
    pcvGetClockRate(kModules[m], &hz);
  return hz;
}

Result Set(int m, u32 hz) {
  return g_clkrst ? clkrstSetClockRate(&g_sessions[m], hz) : pcvSetClockRate(kModules[m], hz);
}

u32 TargetHz(int m) {
  int32_t mhz = 0;
  if (m == kCpu) mhz = REXCVAR_GET(switch_cpu_mhz);
  if (m == kMem) mhz = REXCVAR_GET(switch_mem_mhz);
  if (m == kGpu) {
    mhz = REXCVAR_GET(switch_gpu_mhz);
    if (mhz < 0) return 0;
    if (mhz == 0) return appletGetOperationMode() == AppletOperationMode_Console ? 768000000u : 460800000u;
    if (mhz == 460) return 460800000u;
  }
  return mhz > 0 ? u32(mhz) * 1000000u : 0u;
}

// Sets the clocks that are more than 1 MHz from their targets. True when one was set.
bool Apply(bool log) {
  bool changed = false;
  for (int m = 0; m < kCount; ++m) {
    const u32 want = TargetHz(m);
    if (!want) continue;
    const u32 before = Get(m);
    if (before + 1000000u >= want && before <= want + 1000000u) continue;
    const Result rc = Set(m, want);
    changed = true;
    if (log) {
      REXLOG_INFO("[clocks] {} {:.1f} -> {:.1f} MHz (asked {:.1f}, result 0x{:X})", kNames[m], before / 1e6,
                  Get(m) / 1e6, want / 1e6, unsigned(rc));
    }
  }
  return changed;
}

void Watch(void*) {
  int logged = 0;
  while (!g_stop.load(std::memory_order_relaxed)) {
    svcSleepThread(2'000'000'000ll);
    if (g_stop.load(std::memory_order_relaxed)) break;
    if (Apply(logged < 20)) ++logged;
  }
}

}  // namespace

void Start() {
  if (!REXCVAR_GET(switch_clocks)) {
    REXLOG_INFO("[clocks] switch_clocks = false: the system's clocks");
    return;
  }
  if (hosversionAtLeast(8, 0, 0)) {
    const Result rc = clkrstInitialize();
    g_ok = g_clkrst = R_SUCCEEDED(rc);
    for (int m = 0; g_ok && m < kCount; ++m) {
      const Result opened = clkrstOpenSession(&g_sessions[m], kIds[m], 3);
      if (R_FAILED(opened)) {
        REXLOG_WARN("[clocks] clkrstOpenSession({}) failed: 0x{:X}", kNames[m], unsigned(opened));
        g_ok = false;
      }
    }
    if (R_FAILED(rc)) REXLOG_WARN("[clocks] clkrstInitialize failed: 0x{:X}", unsigned(rc));
  } else {
    const Result rc = pcvInitialize();
    g_ok = R_SUCCEEDED(rc);
    if (!g_ok) REXLOG_WARN("[clocks] pcvInitialize failed: 0x{:X}", unsigned(rc));
  }
  if (!g_ok) {
    REXLOG_WARN("[clocks] no access to the clocks: the system's stay (CPU 1020 MHz)");
    return;
  }
  const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
  REXLOG_INFO("[clocks] before: CPU {:.1f}, GPU {:.1f}, memory {:.1f} MHz ({})", Get(kCpu) / 1e6, Get(kGpu) / 1e6,
              Get(kMem) / 1e6, docked ? "docked" : "handheld");
  Apply(true);
  REXLOG_INFO("[clocks] now: CPU {:.1f}, GPU {:.1f}, memory {:.1f} MHz", Get(kCpu) / 1e6, Get(kGpu) / 1e6,
              Get(kMem) / 1e6);
  if (R_SUCCEEDED(threadCreate(&g_thread, Watch, nullptr, nullptr, 0x4000, 0x3B, -2))) threadStart(&g_thread);
}

void Restore() {
  if (!g_ok) return;
  g_stop.store(true, std::memory_order_relaxed);
  // apm applies its own configuration again when the boost mode changes: through "fast load" and back to normal.
  appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
  appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
}

}  // namespace oblivion::clocks

#else

namespace oblivion::clocks {
void Start() {}
void Restore() {}
}  // namespace oblivion::clocks

#endif  // REX_PLATFORM_SWITCH
