// Census of the game's Direct3D calls (docs/renut-port.md, milestone 1). PC test aid, built with
// -DOBLIVION_D3D_TRACE=ON and active with the environment variable OBLIVION_D3D_TRACE=1.
//
// Every function of d3d_trace_list.inc (tools/renut/gen_trace.py) is replaced by a wrapper that records the call and
// runs the game's own code. Written to d3d_trace.txt in the working directory:
//   - per function, the first calls with their arguments (r3-r8, f1), caller (lr), r3 on return;
//   - the full call sequence of the frames listed in OBLIVION_D3D_TRACE_FRAMES (default 60,600,3000), with the call
//     depth (0 = called from game code);
//   - every 600 frames, the call counts of the last 600 frames;
//   - every shader container (0x102A0Exx / 0x102A11xx) an argument r3-r6 of the shader creators points at (of every
//     function with OBLIVION_D3D_TRACE_SHADERS=1, much slower): a line "S" with the function and
//     register the first time each pair is seen, and the container itself in shader_capture/<vs|ps>_<XXH3>.bin (the
//     key reNut looks shaders up by), so shaders made at run time can be checked against shaderpackage.sdp's.
// A frame ends at each call of the swap function sub_8255F5E0 (VdSwap's caller).

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

namespace oblivion::d3d_trace {
namespace {

constexpr uint32_t kSwap = 0x8255F5E0;
constexpr uint32_t kCreateVertexShader = 0x82557058, kCreatePixelShader = 0x82556F68;
constexpr int kFirstCalls = 4;
constexpr uint64_t kWindowFrames = 600;

struct Stats {
  const char* kind = "";
  uint64_t calls = 0, outer = 0, window = 0;
  int logged = 0;
};

std::mutex g_mutex;
FILE* g_out = nullptr;
int g_enabled = -1;
bool g_all_shader_args = false;
std::atomic<uint64_t> g_frame{0};
std::set<uint64_t> g_full_frames;
std::unordered_map<uint32_t, Stats>* g_stats = nullptr;
thread_local int t_depth = 0;
uint8_t* g_base = nullptr;
std::set<std::pair<uint32_t, uint64_t>> g_container_sites;
std::set<uint64_t> g_containers;

uint8_t* HostAddress(uint32_t guest) {
  return g_base + guest + (guest >= 0xE0000000u ? 0x1000u : 0u);
}

// Whether [p, p + size) is committed, readable host memory (the guest's address space is reserved, not all mapped).
bool Readable(const uint8_t* p, size_t size) {
#ifdef _WIN32
  while (size) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !info.Protect)
      return false;
    const size_t left = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize - p;
    if (left >= size) return true;
    p += left;
    size -= left;
  }
  return true;
#else
  (void)p;
  (void)size;
  return false;
#endif
}

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Called with g_mutex held.
void NoteContainer(uint32_t addr, int reg, uint32_t guest, uint32_t lr) {
  if (guest < 0x10000) return;
  const uint8_t* p = HostAddress(guest);
  if (!Readable(p, 0x18)) return;
  const uint32_t magic = Be32(p);
  if ((magic & 0xFFFF0000u) != 0x102A0000u || (((magic >> 8) & 0xFF) != 0x0E && ((magic >> 8) & 0xFF) != 0x11) ||
      (magic & 0xFE))
    return;
  const uint32_t virt = Be32(p + 4), phys = Be32(p + 8);
  if (virt < 0x18 || virt >= 0x10000 || !phys || phys >= 0x100000 || !Readable(p, size_t(virt) + phys)) return;
  const uint64_t hash = XXH3_64bits(p, size_t(virt) + phys);
  if (g_container_sites.insert({addr, hash}).second)
    std::fprintf(g_out, "S sub_%08X r%d=%08X %s_%016llx v%02X size %u lr=%08X\n", addr, reg, guest,
                 (magic & 1) ? "vs" : "ps", (unsigned long long)hash, (magic >> 8) & 0xFF, virt + phys, lr);
  if (g_containers.insert(hash).second) {
    char name[64];
    std::snprintf(name, sizeof(name), "shader_capture/%s_%016llx.bin", (magic & 1) ? "vs" : "ps",
                  (unsigned long long)hash);
    if (FILE* f = std::fopen(name, "wb")) {
      std::fwrite(p, 1, size_t(virt) + phys, f);
      std::fclose(f);
    }
  }
}

bool Enabled() {
  if (g_enabled < 0) {
    std::lock_guard lock(g_mutex);
    if (g_enabled < 0) {
      const char* e = std::getenv("OBLIVION_D3D_TRACE");
      g_enabled = e && *e && *e != '0';
      if (g_enabled) {
        g_out = std::fopen("d3d_trace.txt", "w");
        g_stats = new std::unordered_map<uint32_t, Stats>();
        const char* all = std::getenv("OBLIVION_D3D_TRACE_SHADERS");
        g_all_shader_args = all && *all && *all != '0';
        std::string frames = std::getenv("OBLIVION_D3D_TRACE_FRAMES") ? std::getenv("OBLIVION_D3D_TRACE_FRAMES")
                                                                     : "60,600,3000";
        for (const char* p = frames.c_str(); *p;) {
          char* end;
          uint64_t f = std::strtoull(p, &end, 10);
          if (end == p) break;
          g_full_frames.insert(f);
          p = *end ? end + 1 : end;
        }
        std::error_code ec;
        std::filesystem::create_directories("shader_capture", ec);
        if (!g_out) g_enabled = 0;
      }
    }
  }
  return g_enabled > 0;
}

void DumpWindow(uint64_t frame) {
  std::fprintf(g_out, "== counts, frames %llu-%llu\n", (unsigned long long)(frame - kWindowFrames),
               (unsigned long long)frame);
  for (auto& [addr, s] : *g_stats) {
    if (s.window) std::fprintf(g_out, "  sub_%08X %-5s %8llu\n", addr, s.kind, (unsigned long long)s.window);
    s.window = 0;
  }
  std::fflush(g_out);
}

class Scope {
 public:
  Scope(uint32_t addr, const char* kind, PPCContext& ctx, uint8_t* base) : addr_(addr), ctx_(ctx) {
    if (!Enabled()) {
      active_ = false;
      return;
    }
    depth_ = t_depth++;
    std::lock_guard lock(g_mutex);
    Stats& s = (*g_stats)[addr];
    s.kind = kind;
    ++s.calls;
    ++s.window;
    if (depth_ == 0) ++s.outer;
    g_base = base;
    // The check costs a VirtualQuery per argument: only at the shader creators unless OBLIVION_D3D_TRACE_SHADERS=1
    // asks for every function's (in game, every function's is about a frame a second).
    if (addr == kCreateVertexShader || addr == kCreatePixelShader || g_all_shader_args) {
      NoteContainer(addr, 3, ctx.r3.u32, uint32_t(ctx.lr));
      NoteContainer(addr, 4, ctx.r4.u32, uint32_t(ctx.lr));
      NoteContainer(addr, 5, ctx.r5.u32, uint32_t(ctx.lr));
      NoteContainer(addr, 6, ctx.r6.u32, uint32_t(ctx.lr));
    }
    uint64_t frame = g_frame.load(std::memory_order_relaxed);
    full_ = g_full_frames.count(frame) != 0;
    log_first_ = s.logged < kFirstCalls && depth_ == 0;
    if (log_first_) ++s.logged;
    if (full_ || log_first_) {
      std::fprintf(g_out, "%s f%llu d%d sub_%08X %s r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X f1=%g lr=%08X",
                   full_ ? "F" : "1", (unsigned long long)frame, depth_, addr, kind, ctx.r3.u32, ctx.r4.u32,
                   ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.f1.f64, uint32_t(ctx.lr));
    }
  }
  ~Scope() {
    if (!active_) return;
    --t_depth;
    std::lock_guard lock(g_mutex);
    if (full_ || log_first_) std::fprintf(g_out, " -> %08X\n", ctx_.r3.u32);
    if (addr_ == kSwap && depth_ == 0) {
      uint64_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
      if (frame % kWindowFrames == 0) DumpWindow(frame);
    }
  }

 private:
  uint32_t addr_;
  PPCContext& ctx_;
  bool active_ = true, full_ = false, log_first_ = false;
  int depth_ = 0;
};

}  // namespace
}  // namespace oblivion::d3d_trace

#define TRACE_FN(addr, kind)                                                          \
  REX_EXTERN(__imp__sub_##addr);                                                      \
  extern "C" REX_FUNC(sub_##addr) {                                                   \
    ::oblivion::d3d_trace::Scope scope_(0x##addr##u, kind, ctx, base);                   \
    __imp__sub_##addr(ctx, base);                                                     \
  }
#include "d3d_trace_list.inc"
#undef TRACE_FN
