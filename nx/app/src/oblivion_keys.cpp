// oblivion (nx): the keys and entry points of katamari-nx / banjo-nx files this tree does not build, which reNut's
// renderer (src/renut_engine/gpu) still reads. After katamari-nx's app/src/katamari_keys.cpp (GPL-3.0).
//
// Not built here (nx/docs/bootstrap-notes.md, "Pruned"): banjo-nx's PM4 renderer (src/me), its enhancement table
// (src/cpu/banjo_enhancements.cpp), the profilers (src/profiler) and the frame limiter (renut_engine/frameHooks.cpp).
// Every definition below is inert (the defaults those files give, no-op profiler entry points) but the frame cap.

#include <rex/cvar.h>
#include <rex/platform.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "cpu/banjo_enhancements.h"
#include "profiler/profiler.h"
#include "renut_engine/perf/frame_stats.h"

// --- banjo-nx's PM4 renderer keys (src/me/masseffect/masseffect_native_draws.cpp, src/me/me_native_system.cpp) that
// banjo_renut_tuning.cpp declares and lists. Their katamari-nx defaults; nothing in this tree acts on them except
// reNut's own reads of the same names.
REXCVAR_DEFINE_INT32(banjo_background_compile_core, 2, "Oblivion/Not built",
                     "banjo-nx's native renderer: core of the background compile threads (not in this tree)");
REXCVAR_DEFINE_BOOL(banjo_gpu_blend_skip, true, "Oblivion/Not built",
                    "banjo-nx's native renderer: skip blends that leave the target unchanged (not in this tree)");
REXCVAR_DEFINE_BOOL(banjo_gpu_bool_spec, true, "Oblivion/Not built",
                    "banjo-nx's native renderer: pipelines specialised on bool constants (not in this tree)");
REXCVAR_DEFINE_BOOL(banjo_gpu_nvk_draw_contract, true, "Oblivion/Not built",
                    "banjo-nx's native renderer: NVK per-draw contract lookup (not in this tree)");
REXCVAR_DEFINE_BOOL(banjo_gpu_robustness_off, true, "Oblivion/Not built",
                    "banjo-nx's native renderer: pipelines without robustness (not in this tree)");
REXCVAR_DEFINE_BOOL(banjo_gpu_tess_grid, true, "Oblivion/Not built",
                    "banjo-nx's native renderer: tessellated patches as a grid (not in this tree)");
REXCVAR_DEFINE_INT32(banjo_prewarm_threads, 1, "Oblivion/Not built",
                     "banjo-nx's native renderer: pipeline prewarm threads (not in this tree)");
REXCVAR_DEFINE_INT32(banjo_ring_core, 1, "Oblivion/Not built",
                     "banjo-nx's native renderer: the ring thread's core (not in this tree)");
REXCVAR_DEFINE_BOOL(masseffect_native_nvk_cbufs, true, "Oblivion/Not built",
                    "banjo-nx's native renderer: NVK dirty cbuf slots only (not in this tree)");
REXCVAR_DEFINE_BOOL(masseffect_native_nvk_dynamic, true, "Oblivion/Not built",
                    "banjo-nx's native renderer: NVK dynamic state groups (not in this tree)");
REXCVAR_DEFINE_BOOL(masseffect_native_nvk_emission, true, "Oblivion/Not built",
                    "banjo-nx's native renderer: NVK draw emission in one go (not in this tree)");
REXCVAR_DEFINE_INT32(masseffect_native_nvk_measure, 64, "Oblivion/Not built",
                     "banjo-nx's native renderer: NVK part measurement period (not in this tree)");
REXCVAR_DEFINE_BOOL(masseffect_native_nvk_preload, true, "Oblivion/Not built",
                    "banjo-nx's native renderer: NVK cache hints (not in this tree)");
REXCVAR_DEFINE_BOOL(masseffect_native_set4_differences, true, "Oblivion/Not built",
                    "banjo-nx's native renderer: descriptor set 4 by differences (not in this tree)");

// --- The SDK's console profiler (sdk/src/ui/switch_perf.cpp) reads this key by name: with true it pauses each busy
// thread 1000 times a second and writes, every 10 s, every code address it saw to logs/rex/rex_profile.log next to the
// NRO (nx/tools/switch_hot_functions.py names them with the build's link map). Always written there, with or without
// it: the frame rate, each thread's CPU share and the console's clocks. katamari-nx defines the key in its enhancement
// table (not built here).
REXCVAR_DEFINE_BOOL(switch_profile_stacks, false, "Oblivion/Switch",
                    "Switch: sample every busy thread's code address 1000 times a second into logs/rex/rex_profile.log "
                    "(costs a few percent; for finding what is slow)");

// --- banjo-nx's enhancement table (src/cpu/banjo_enhancements.cpp, not built: it lists the keys of every file above
// and of the hot-function natives). Its verify key, which reNut's tuning reads, and the two entry points it calls.
REXCVAR_DEFINE_BOOL(banjo_cpu_verify, false, "Oblivion/Not built",
                    "banjo-nx's verify configuration of the CPU enhancements (not in this tree)");

namespace banjo {
// Without the table there are no master switches: an enhancement is on when its own key is.
bool EnhancementActive(EnhancementGroup, bool own_key) { return own_key; }
void ApplyMasterSwitches() {}
void LogEnhancements() {}
void CodegenChecksInit() {}
void CodegenChecksReport(int) {}
void FormsSelfCheck() {}
}  // namespace banjo

// --- The profiler (src/profiler/profiler.cpp, not built): every entry point a no-op.
namespace banjo::profiler {
void Start() {}
void RecordTick(double) {}
void RecordDraw(double) {}
void RecordLimiterWait(double) {}
void FrameBoundary() {}
void SetGpuFrameMs(double) {}
void RecordHostPresent() {}
void SetRenderThreadMs(double) {}
void SetRendererInfo(const char*, const char*) {}
const char* SeriesName(Series) { return ""; }
double Current(Series) { return 0.0; }
bool HasValue(Series) { return false; }
std::string RendererApi() { return {}; }
std::string RendererDevice() { return {}; }
std::string ThreadCpuUsage() { return {}; }
}  // namespace banjo::profiler

// --- katamari_keys.cpp's frame limiter (renut_engine/frameHooks.cpp's renutFrameLimit), called at the end of reNut's
// Swap hook (d3d/hooks/device.cpp). oblivion (2026-11-03): a plain cap. The game asks for a frame every other refresh
// (30 fps), which reNut does not wait for; uncapped it ran at 200 fps and more on the PC with the window minimised
// (vsync then waits for nothing), and the game's physics is known to misbehave well above 60.
REXCVAR_DEFINE_DOUBLE(oblivion_renut_frame_cap, 60.0, "Oblivion/Renut",
                      "Renut's renderer: the most frames a second the game runs at (0: no cap)");

void renutFrameLimit() {
  using clock = std::chrono::steady_clock;
  static clock::time_point next{};
  const double cap = REXCVAR_GET(oblivion_renut_frame_cap);
  if (cap <= 0.0)
    return;
  const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / cap));
  const auto now = clock::now();
  if (next <= now || next - now > 2 * period) {
    next = now + period;  // late (or the cap changed): start again from now rather than catch up
    return;
  }
  // Sleep to about 2 ms before the frame's time (the sleep's granularity), then yield up to it.
  if (next - now > std::chrono::milliseconds(2))
    std::this_thread::sleep_until(next - std::chrono::milliseconds(2));
  while (clock::now() < next)
    std::this_thread::yield();
  next += period;
  // The "[oblivion] game thread" line of the log: the part of the frame the cap held the game back.
  renut::frame_stats::OnGameThreadLimiterWait(std::chrono::duration<double, std::milli>(clock::now() - now).count());
}

// Game frames presented: reNut's Present hook counts them (d3d/hooks/draw.cpp).
std::atomic<uint64_t> g_banjo_game_frames{0};

// --- katamari-nx's cpu/banjo_hot_guest.cpp (not built: native copies of Katamari's hot functions).
// Whether the generated code marks the store cards (katamari-nx's tools/store_cards.py post-codegen pass): it does not
// here (no post-codegen passes), so reNut never trusts them (banjo_renut_store_cards).
extern "C" const bool g_banjo_store_cards = false;
namespace banjo::hot {
void Report() {}  // the hook counters' report: no hooks in this tree
}  // namespace banjo::hot

#if !REX_PLATFORM_SWITCH
// --- katamari-nx's me/me_pc_stubs.cpp: the Switch profiler counters (sdk/src/ui/switch_perf.cpp there) on the PC.
extern "C" void RexSwitchPerfCount(unsigned) {}
#endif

#if REX_PLATFORM_SWITCH
// --- katamari-nx's profiler/cpu_profiler_switch.cpp (not built: its sampling profiler and thread registry). Its one
// entry point reNut reads, the registered threads' CPU time for banjo_renut_tuning.cpp's A/B halves: none registered.
#include "profiler/switch_cpu_profiler.h"
uint64_t os::switch_cpu_profiler::TotalThreadTicks() { return 0; }
#endif
