// banjo-nx: the frame statistics Renut's renderer reports into (renut_engine/perf/frame_stats.h, copied from Renut by
// tools/renut_gpu_import.py). Renut's own frame_stats.cpp also hooks the game's main loop (appMainTickPreDraw /
// appMainDraw), which banjo-nx's FPS code hooks already, so this keeps the numbers without those hooks: a game frame is
// one D3DDevice_Swap, its length the time between two.
#include "renut_engine/perf/frame_stats.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

#include "renut_engine/gpu/log.h"

// Counter 0 of the console profiler: presented frames, which the Status Monitor overlay's FPS (SaltyNX) and the
// profiler's "game N fps" are made from (sdk/src/ui/switch_perf.cpp; a stub on the PC, me_pc_stubs.cpp).
extern "C" void RexSwitchPerfCount(unsigned id);

#include "profiler/profiler.h"                      // the on-screen profiler's GPU, present and render thread rows
#include "renut_engine/gpu/banjo_renut_tuning.h"  // the [renut] report and its in-run A/B

namespace renut::frame_stats {

namespace {

using Clock = std::chrono::steady_clock;

struct State {
  std::mutex mutex;
  Snapshot snap;
  Clock::time_point last_frame{};
  Clock::time_point window_start{};
  int window_frames = 0;
  double window_worst = 0.0;
  double gpu_sum = 0.0, render_sum = 0.0, wait_sum = 0.0;
  int gpu_n = 0, render_n = 0;
  // oblivion (2026-11-10): the "[oblivion] game thread" line, over 10 s. Waits of the thread that calls Swap only
  // (other guest threads block on fences too).
  std::thread::id game_thread{};
  Clock::time_point split_start{};
  int split_frames = 0;
  double split_worst = 0.0, split_renderer = 0.0, split_limiter = 0.0;
  double split_render_busy = 0.0, split_gpu = 0.0;
  int split_render_n = 0, split_gpu_n = 0;
};

State& S() {
  static State s;
  return s;
}

}  // namespace

void OnGameFrame() {
  banjo::renut_gpu::StatsGameFrame();
  State& s = S();
  std::lock_guard lock(s.mutex);
  const auto now = Clock::now();
  if (s.last_frame != Clock::time_point{}) {
    const double ms = std::chrono::duration<double, std::milli>(now - s.last_frame).count();
    Snapshot& snap = s.snap;
    if (snap.history_count < Snapshot::kHistory) {
      snap.history[snap.history_count++] = float(ms);
    } else {
      std::copy(snap.history + 1, snap.history + Snapshot::kHistory, snap.history);
      snap.history[Snapshot::kHistory - 1] = float(ms);
    }
    s.window_worst = std::max(s.window_worst, ms);
    s.split_worst = std::max(s.split_worst, ms);
    ++s.window_frames;
  } else {
    s.window_start = now;
  }
  s.last_frame = now;
  // oblivion (2026-11-10): the game thread's frame, split (frame_stats.h).
  s.game_thread = std::this_thread::get_id();
  if (s.split_start == Clock::time_point{}) {
    s.split_start = now;
  } else {
    ++s.split_frames;
    const double seconds = std::chrono::duration<double>(now - s.split_start).count();
    if (seconds >= 10.0) {
      const double n = double(s.split_frames);
      const double frame = seconds * 1000.0 / n;
      const double renderer = s.split_renderer / n, limiter = s.split_limiter / n;
      RGPU_INFO("[oblivion] game thread, {:.1f} s: {} frames of {:.1f} ms ({:.1f} fps, the longest {:.0f} ms): {:.1f} ms "
                "working, {:.1f} ms waiting for the render thread, {:.1f} ms in the frame cap's wait; render thread "
                "busy {:.1f} ms a frame, GPU {:.1f} ms a frame",
                seconds, s.split_frames, frame, n / seconds, s.split_worst,
                std::max(0.0, frame - renderer - limiter), renderer, limiter,
                s.split_render_n ? s.split_render_busy / s.split_render_n : 0.0,
                s.split_gpu_n ? s.split_gpu / s.split_gpu_n : 0.0);
      s.split_start = now;
      s.split_frames = 0;
      s.split_worst = s.split_renderer = s.split_limiter = s.split_render_busy = s.split_gpu = 0.0;
      s.split_render_n = s.split_gpu_n = 0;
    }
  }
  const double window = std::chrono::duration<double>(now - s.window_start).count();
  if (window >= 1.0 && s.window_frames) {
    Snapshot& snap = s.snap;
    snap.fps = s.window_frames / window;
    snap.frame_ms = window * 1000.0 / s.window_frames;
    snap.worst_ms = s.window_worst;
    snap.cpu_ms = std::max(0.0, snap.frame_ms - s.wait_sum / s.window_frames);
    if (s.gpu_n) {
      snap.gpu_ms = s.gpu_sum / s.gpu_n;
      snap.gpu_known = true;
    }
    if (s.render_n) {
      snap.render_thread_ms = s.render_sum / s.render_n;
      snap.render_thread_known = true;
    }
    s.window_start = now;
    s.window_frames = 0;
    s.window_worst = 0.0;
    s.gpu_sum = s.render_sum = s.wait_sum = 0.0;
    s.gpu_n = s.render_n = 0;
  }
}

void OnGpuFrame(double ms) {
  banjo::renut_gpu::StatsGpuFrame(ms);
  banjo::profiler::SetGpuFrameMs(ms);  // the profiler window's "GPU Frame" (package 27: nobody fed it before)
  State& s = S();
  std::lock_guard lock(s.mutex);
  s.gpu_sum += ms;
  ++s.gpu_n;
  s.split_gpu += ms;
  ++s.split_gpu_n;
}

void OnRenderThreadFrame(double busy_ms) {
  banjo::renut_gpu::StatsRenderThreadFrame(busy_ms);
  // The profiler window (package 27): the render thread's frame ends with its present, so this is also the time
  // between two host presents ("Present"); one thread, as RecordHostPresent needs.
  banjo::profiler::SetRenderThreadMs(busy_ms);
  banjo::profiler::RecordHostPresent();
  // Package 27c: the presented frame for the console's FPS. Only banjo-nx's own renderer (and the SDK's) counted
  // them, so with Renut's renderer the Status Monitor kept an old value (19.7) and the profile said "game 0.0 fps".
  RexSwitchPerfCount(0);
  State& s = S();
  std::lock_guard lock(s.mutex);
  s.render_sum += busy_ms;
  ++s.render_n;
  s.split_render_busy += busy_ms;
  ++s.split_render_n;
}

void OnGameThreadWait(double ms) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  s.wait_sum += ms;
  if (std::this_thread::get_id() == s.game_thread) s.split_renderer += ms;
}

void OnGameThreadLimiterWait(double ms) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  if (std::this_thread::get_id() == s.game_thread) s.split_limiter += ms;
}

Snapshot Get() {
  State& s = S();
  std::lock_guard lock(s.mutex);
  return s.snap;
}

}  // namespace renut::frame_stats
