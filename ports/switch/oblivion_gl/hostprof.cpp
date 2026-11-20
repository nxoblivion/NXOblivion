// Host sampling profiler for the Switch port (switch_hostprof_hz > 0).
//
// The SDK reports the Horizon handle and name of every thread it creates (oblivion_thread_started / _named /
// _exiting, weak hooks in threading_posix.cpp and the GL compile workers). A sampler thread pauses each thread in
// turn (svcSetThreadActivity), reads its PC and LR (svcGetThreadContext3) and resumes it at once: nothing else
// happens while a thread is paused, so it cannot hold a lock the sampler needs. Every switch_hostprof_window_s
// seconds the per-thread PC / LR histograms are appended to sdmc:/oblivion/hostprof.txt as offsets from the image
// base (= ELF addresses of out/switch-gen/oblivion_gl.elf; tools/hostprof.py symbolises them).
// Works on hardware and in Eden (both implement thread pausing + GetThreadContext3 for the own process).
// With switch_hostprof_stack_depth > 0 the sampler also follows the paused thread's frame record chain (x29; GCC
// keeps frame records on AArch64, in Mesa too) inside its stack mapping and counts caller chains ('S' lines: LR,
// then the return addresses outwards), so blocked time (svcSleepThread under nanosleep, ...) gets its full caller.
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(switch_hostprof_hz, 0, "Switch",
                     "Host sampling profiler: sampling rounds per second over all SDK threads (0 = off); results in "
                     "sdmc:/oblivion/hostprof.txt (symbolise with tools/hostprof.py)");
REXCVAR_DEFINE_INT32(switch_hostprof_window_s, 20, "Switch", "Host sampling profiler: seconds per report window");
REXCVAR_DEFINE_INT32(switch_hostprof_start_s, 0, "Switch",
                     "Host sampling profiler: start sampling this many seconds after the guest starts");
REXCVAR_DEFINE_INT32(switch_hostprof_stack_depth, 16, "Switch",
                     "Host sampling profiler: caller frames recorded per sample by walking the frame records "
                     "(0 = PC / LR only)");

extern "C" char __start__[];  // libnx linker script: image base
extern "C" void oblivion_bootlog(const char* fmt, ...);

namespace {

constexpr int kMaxThreads = 128;
struct Slot {
  std::atomic<uint32_t> handle{0};
  std::atomic<uint32_t> generation{0};
  char name[40] = {};
};
Slot g_slots[kMaxThreads];
std::atomic<int> g_slot_count{0};
std::atomic_flag g_slot_lock = ATOMIC_FLAG_INIT;

struct SlotLock {
  SlotLock() { while (g_slot_lock.test_and_set(std::memory_order_acquire)) {} }
  ~SlotLock() { g_slot_lock.clear(std::memory_order_release); }
};

Slot* FindSlot(uint32_t handle) {
  const int n = g_slot_count.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i) {
    if (g_slots[i].handle.load(std::memory_order_relaxed) == handle) return &g_slots[i];
  }
  return nullptr;
}

struct ThreadHist {
  uint32_t generation = 0;
  uint64_t samples = 0, failures = 0;
  std::unordered_map<uint64_t, uint32_t> pc, lr;
  std::unordered_map<std::string, uint32_t> stacks;  // "lr,ret1,ret2,..." (image offsets, hex)
  uint64_t stack_lo = 0, stack_hi = 0;               // mapping that holds the thread's stack (cached)
};

constexpr int kMaxDepth = 32;

// Return addresses of the frame record chain starting at the paused thread's x29. Only addresses inside the stack
// mapping (from SP up) are read, and the chain must grow strictly towards the stack top, so a stale or foreign x29
// ends the walk instead of faulting.
int WalkFrames(ThreadHist& h, const ThreadContext& ctx, uint64_t* out, int max) {
  const uint64_t sp = ctx.sp;
  if (sp < h.stack_lo || sp >= h.stack_hi) {
    MemoryInfo mi = {};
    u32 page_info = 0;
    if (R_FAILED(svcQueryMemory(&mi, &page_info, sp)) || !(mi.perm & Perm_R)) return 0;
    h.stack_lo = mi.addr;
    h.stack_hi = mi.addr + mi.size;
  }
  int n = 0;
  uint64_t fp = ctx.fp;
  while (n < max && fp >= sp && fp + 16 <= h.stack_hi && !(fp & 7)) {
    const uint64_t* rec = reinterpret_cast<const uint64_t*>(fp);
    const uint64_t ret = rec[1];
    if (!ret) break;
    out[n++] = ret;
    if (rec[0] <= fp) break;
    fp = rec[0];
  }
  return n;
}

Thread g_sampler;

void WriteWindow(FILE* f, double t, double window_s, std::vector<ThreadHist>& hist) {
  std::fprintf(f, "W %.1f %.1f\n", t, window_s);
  for (int i = 0; i < int(hist.size()); ++i) {
    ThreadHist& h = hist[i];
    if (!h.samples && !h.failures) continue;
    std::fprintf(f, "T %d %s|%llu|%llu\n", i, g_slots[i].name[0] ? g_slots[i].name : "?",
                 (unsigned long long)h.samples, (unsigned long long)h.failures);
    auto dump = [&](char tag, std::unordered_map<uint64_t, uint32_t>& m, size_t limit) {
      std::vector<std::pair<uint32_t, uint64_t>> v;
      v.reserve(m.size());
      for (auto& kv : m) v.push_back({kv.second, kv.first});
      std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
      for (size_t k = 0; k < v.size() && k < limit; ++k) {
        std::fprintf(f, "%c %llx %u\n", tag, (unsigned long long)v[k].second, v[k].first);
      }
      m.clear();
    };
    dump('P', h.pc, 400);
    dump('L', h.lr, 200);
    if (!h.stacks.empty()) {
      std::vector<std::pair<uint32_t, const std::string*>> v;
      v.reserve(h.stacks.size());
      for (auto& kv : h.stacks) v.push_back({kv.second, &kv.first});
      std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
      for (size_t k = 0; k < v.size() && k < 150; ++k) std::fprintf(f, "S %u %s\n", v[k].first, v[k].second->c_str());
      h.stacks.clear();
    }
    h.samples = h.failures = 0;
  }
  std::fflush(f);
}

void SamplerMain(void*) {
  const int hz = REXCVAR_GET(switch_hostprof_hz);
  const u64 period_ns = 1000000000ull / u64(std::max(1, hz));
  if (REXCVAR_GET(switch_hostprof_start_s) > 0) {
    svcSleepThread(s64(REXCVAR_GET(switch_hostprof_start_s)) * 1000000000ll);
  }
  // Keep the previous session's profile (a played session is otherwise lost on the next start).
  std::remove("sdmc:/oblivion/hostprof.prev.txt");
  std::rename("sdmc:/oblivion/hostprof.txt", "sdmc:/oblivion/hostprof.prev.txt");
  FILE* f = std::fopen("sdmc:/oblivion/hostprof.txt", "w");
  if (!f) {
    oblivion_bootlog("hostprof: cannot open sdmc:/oblivion/hostprof.txt");
    return;
  }
  // __start__ is an absolute linker symbol (0 under PIE): take the start of the mapping that holds our code
  // (.text is the first segment of the NRO, at image offset 0).
  uint64_t base = reinterpret_cast<uint64_t>(__start__);
  {
    MemoryInfo mi = {};
    u32 page_info = 0;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &page_info, reinterpret_cast<u64>(&SamplerMain)))) base = mi.addr;
  }
  std::fprintf(f, "# hostprof v1 hz=%d base=%llx\n", hz, (unsigned long long)base);
  std::fflush(f);
  oblivion_bootlog("hostprof: sampling %d Hz, image base 0x%llx", hz, (unsigned long long)base);
  std::vector<ThreadHist> hist(kMaxThreads);
  const u64 freq = armGetSystemTickFreq();
  const u64 t_start = armGetSystemTick();
  u64 t_window = t_start;
  const u64 window_ticks = u64(std::max(1, REXCVAR_GET(switch_hostprof_window_s))) * freq;
  ThreadContext ctx;
  const int depth = std::clamp(REXCVAR_GET(switch_hostprof_stack_depth), 0, kMaxDepth);
  uint64_t frames[kMaxDepth];
  std::string key;
  for (;;) {
    const u64 round_t0 = armGetSystemTick();
    const int n = g_slot_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
      const uint32_t handle = g_slots[i].handle.load(std::memory_order_acquire);
      if (!handle) continue;
      ThreadHist& h = hist[i];
      const uint32_t gen = g_slots[i].generation.load(std::memory_order_relaxed);
      if (h.generation != gen) {
        h = ThreadHist();
        h.generation = gen;
      }
      if (R_FAILED(svcSetThreadActivity(handle, ThreadActivity_Paused))) {
        ++h.failures;
        continue;
      }
      const Result rc = svcGetThreadContext3(&ctx, handle);
      // The stack is only read while the thread is paused (it cannot exit and unmap it meanwhile).
      const int nf = R_SUCCEEDED(rc) && depth ? WalkFrames(h, ctx, frames, depth) : 0;
      svcSetThreadActivity(handle, ThreadActivity_Runnable);
      if (R_FAILED(rc)) {
        ++h.failures;
        continue;
      }
      ++h.samples;
      ++h.pc[ctx.pc.x - base];
      ++h.lr[ctx.lr - base];
      if (depth) {
        // LR first (the caller when the PC is in a leaf such as an svc stub); skipped when the innermost frame
        // record already holds it.
        char buf[24];
        key.clear();
        if (!nf || frames[0] != ctx.lr) {
          std::snprintf(buf, sizeof(buf), "%llx", (unsigned long long)(ctx.lr - base));
          key += buf;
        }
        for (int k = 0; k < nf; ++k) {
          std::snprintf(buf, sizeof(buf), key.empty() ? "%llx" : ",%llx", (unsigned long long)(frames[k] - base));
          key += buf;
        }
        ++h.stacks[key];
      }
    }
    const u64 now = armGetSystemTick();
    if (now - t_window >= window_ticks) {
      WriteWindow(f, double(now - t_start) / double(freq), double(now - t_window) / double(freq), hist);
      t_window = now;
    }
    const u64 spent_ns = armTicksToNs(armGetSystemTick() - round_t0);
    svcSleepThread(spent_ns < period_ns ? s64(period_ns - spent_ns) : 1000000ll);
  }
}

}  // namespace

extern "C" void oblivion_thread_started(uint32_t handle) {
  if (!handle) return;
  SlotLock lock;
  Slot* s = FindSlot(handle);
  if (!s) {
    // Reuse a slot of an exited thread before growing.
    const int n = g_slot_count.load(std::memory_order_relaxed);
    for (int i = 0; i < n && !s; ++i) {
      if (!g_slots[i].handle.load(std::memory_order_relaxed)) s = &g_slots[i];
    }
    if (!s) {
      if (n >= kMaxThreads) return;
      s = &g_slots[n];
      g_slot_count.store(n + 1, std::memory_order_release);
    }
  }
  s->name[0] = 0;
  s->generation.fetch_add(1, std::memory_order_relaxed);
  s->handle.store(handle, std::memory_order_release);
}

extern "C" void oblivion_thread_named(uint32_t handle, const char* name) {
  SlotLock lock;
  if (Slot* s = FindSlot(handle)) {
    std::snprintf(s->name, sizeof(s->name), "%s", name ? name : "");
    for (char* c = s->name; *c; ++c) {
      if (*c == '|' || *c == '\n') *c = '_';
    }
  }
}

extern "C" void oblivion_thread_exiting(uint32_t handle) {
  SlotLock lock;
  if (Slot* s = FindSlot(handle)) s->handle.store(0, std::memory_order_release);
}

// Called by main.cpp once the guest is about to start.
void HostProfilerStart() {
  if (REXCVAR_GET(switch_hostprof_hz) <= 0) return;
  // Above the guest threads (0x2C) so the sampling rounds keep their rate under load.
  if (R_SUCCEEDED(threadCreate(&g_sampler, SamplerMain, nullptr, nullptr, 0x10000, 0x24, -2))) {
    svcSetThreadCoreMask(g_sampler.handle, -1, 0x7);
    threadStart(&g_sampler);
  }
}

// ---- Filesystem mutation trace (link-time wraps of libnx fsFs*; switch_fs_trace) -------------------------------
// Every create / delete / rename the process issues goes to boot.txt (written synchronously) BEFORE the call, with
// the caller's image offset: Eden crashed (null directory in its IFileSystem create/delete path) on one of these.
REXCVAR_DEFINE_BOOL(switch_fs_trace, false, "Switch",
                    "Debug: log every sdmc create/delete/rename (libnx fsFs*) to boot.txt before issuing it");

namespace {
thread_local bool t_in_fs_trace = false;
uint64_t ImageBase() {
  static uint64_t base = 0;
  if (!base) {
    MemoryInfo mi = {};
    u32 pi = 0;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, reinterpret_cast<u64>(&ImageBase)))) base = mi.addr;
  }
  return base;
}
void FsTrace(const char* op, const char* a, const char* b, void* caller) {
  if (t_in_fs_trace || !REXCVAR_GET(switch_fs_trace)) return;
  if (a && std::strstr(a, "boot.txt")) return;  // the trace's own appends
  // fsdev_open issues CreateFile after it has taken an fd slot: a nested fopen (the boot.txt append) there
  // clobbers that file (hp7 lost hostprof.txt this way), so fopen-created files are not traced.
  if (!b && std::strcmp(op, "CreateFile") == 0) return;
  t_in_fs_trace = true;
  oblivion_bootlog("fs: %s '%s'%s%s%s (caller +0x%llx)", op, a ? a : "", b ? " -> '" : "", b ? b : "", b ? "'" : "",
                   (unsigned long long)(reinterpret_cast<uint64_t>(caller) - ImageBase()));
  t_in_fs_trace = false;
}
}  // namespace

extern "C" {
Result __real_fsFsCreateFile(FsFileSystem* fs, const char* path, s64 size, u32 option);
Result __real_fsFsDeleteFile(FsFileSystem* fs, const char* path);
Result __real_fsFsCreateDirectory(FsFileSystem* fs, const char* path);
Result __real_fsFsDeleteDirectory(FsFileSystem* fs, const char* path);
Result __real_fsFsDeleteDirectoryRecursively(FsFileSystem* fs, const char* path);
Result __real_fsFsRenameFile(FsFileSystem* fs, const char* cur_path, const char* new_path);
Result __real_fsFsRenameDirectory(FsFileSystem* fs, const char* cur_path, const char* new_path);

Result __wrap_fsFsCreateFile(FsFileSystem* fs, const char* path, s64 size, u32 option) {
  FsTrace("CreateFile", path, nullptr, __builtin_return_address(0));
  return __real_fsFsCreateFile(fs, path, size, option);
}
Result __wrap_fsFsDeleteFile(FsFileSystem* fs, const char* path) {
  FsTrace("DeleteFile", path, nullptr, __builtin_return_address(0));
  return __real_fsFsDeleteFile(fs, path);
}
Result __wrap_fsFsCreateDirectory(FsFileSystem* fs, const char* path) {
  FsTrace("CreateDirectory", path, nullptr, __builtin_return_address(0));
  return __real_fsFsCreateDirectory(fs, path);
}
Result __wrap_fsFsDeleteDirectory(FsFileSystem* fs, const char* path) {
  FsTrace("DeleteDirectory", path, nullptr, __builtin_return_address(0));
  return __real_fsFsDeleteDirectory(fs, path);
}
Result __wrap_fsFsDeleteDirectoryRecursively(FsFileSystem* fs, const char* path) {
  FsTrace("DeleteDirectoryRecursively", path, nullptr, __builtin_return_address(0));
  return __real_fsFsDeleteDirectoryRecursively(fs, path);
}
Result __wrap_fsFsRenameFile(FsFileSystem* fs, const char* cur_path, const char* new_path) {
  FsTrace("RenameFile", cur_path, new_path, __builtin_return_address(0));
  return __real_fsFsRenameFile(fs, cur_path, new_path);
}
Result __wrap_fsFsRenameDirectory(FsFileSystem* fs, const char* cur_path, const char* new_path) {
  FsTrace("RenameDirectory", cur_path, new_path, __builtin_return_address(0));
  return __real_fsFsRenameDirectory(fs, cur_path, new_path);
}
}
