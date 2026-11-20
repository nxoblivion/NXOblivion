/**
 * @file        rex/core/memory_web.cpp
 * @brief       SDK memory API (rex::memory) for WebAssembly (reOblivion web, 2026-11-17)
 *
 * The browser gives a wasm module one linear memory and nothing else: no mmap aliasing, no page protection, no
 * page faults. So the guest window is a single 4.5 GB block of the wasm heap (memory64; the browser commits it
 * lazily, as the spike in nx/web/spike measured: 3 ms), taken with sbrk rather than malloc: mimalloc (-sMALLOC=mimalloc)
 * gets its memory from emmalloc, which refuses a block of 4 GB or more. emmalloc takes later memory past it. And:
 *
 * - Views are carved into it without aliasing: MapFileView returns the address it was given. The 360's physical
 *   mirrors (0xA0000000, 0xC0000000, 0xE0000000) are folded onto the 0xA0000000 window by the address arithmetic
 *   instead (rex::memory::detail::GuestHostDelta, REX_PHYS_HOST_OFFSET in the generated code).
 * - Commit is a no-op (fresh wasm memory is zero). Decommit and release zero the range, so a later commit reads
 *   zeros as on the 360.
 * - Protection is only bookkeeping (QueryProtect answers what Protect set): nothing can fault. reNut's renderer
 *   does not need write watching (it proves its caches with the store cards), and MMIO accesses read plain memory.
 *
 * Memory outside the window (AllocFixed without an address) is ordinary heap memory.
 */

#include <rex/platform.h>

#if REX_PLATFORM_WEB

#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

#include <rex/logging.h>
#include <rex/memory/utils.h>

namespace rex::memory {

namespace {

constexpr size_t kPage = 0x1000;
constexpr FileMappingHandle kWebMapping = 1;

constexpr size_t AlignUp(size_t v) { return (v + kPage - 1) & ~(kPage - 1); }

uint8_t* g_window = nullptr;
size_t g_window_size = 0;
std::mutex g_protect_mutex;
std::vector<uint8_t> g_protect;  // a PageAccess per 4 KB page of the window

bool InWindow(const void* p, size_t len) {
  if (!g_window || !p) return false;
  const uintptr_t a = reinterpret_cast<uintptr_t>(p);
  const uintptr_t b = reinterpret_cast<uintptr_t>(g_window);
  return a >= b && a + len <= b + g_window_size;
}

void SetProtect(const void* p, size_t len, PageAccess access) {
  const size_t first = (static_cast<const uint8_t*>(p) - g_window) / kPage;
  const size_t last = (static_cast<const uint8_t*>(p) - g_window + len + kPage - 1) / kPage;
  std::lock_guard<std::mutex> lock(g_protect_mutex);
  for (size_t i = first; i < last && i < g_protect.size(); ++i) g_protect[i] = uint8_t(access);
}

}  // namespace

size_t page_size() { return kPage; }

size_t allocation_granularity() { return kPage; }

bool IsWritableExecutableMemorySupported() { return false; }

void* GetFileMappingBase(FileMappingHandle handle) { return handle == kWebMapping ? g_window : nullptr; }

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type, PageAccess access) {
  if (length == 0) return nullptr;
  if (!base_address) {
    const size_t len = AlignUp(length);
    void* p = aligned_alloc(kPage, len);
    if (p) std::memset(p, 0, len);
    return p;
  }
  if (!InWindow(base_address, length)) {
    REXLOG_ERROR("AllocFixed on the web outside the guest window: 0x{:016X} (0x{:X} bytes)",
                 reinterpret_cast<uintptr_t>(base_address), length);
    return nullptr;
  }
  if (allocation_type != AllocationType::kReserve) SetProtect(base_address, length, access);
  return base_address;
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  if (!base_address) return false;
  if (!InWindow(base_address, length ? length : 1)) {
    if (deallocation_type == DeallocationType::kRelease) free(base_address);
    return true;
  }
  if (length == 0) return false;
  switch (deallocation_type) {
    case DeallocationType::kDecommit:
    case DeallocationType::kRelease:
      std::memset(base_address, 0, length);
      SetProtect(base_address, length, PageAccess::kNoAccess);
      return true;
    default:
      return false;
  }
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  if (out_old_access) *out_old_access = PageAccess::kNoAccess;
  if (!InWindow(base_address, length)) return false;
  if (out_old_access) {
    std::lock_guard<std::mutex> lock(g_protect_mutex);
    *out_old_access = PageAccess(g_protect[(static_cast<uint8_t*>(base_address) - g_window) / kPage]);
  }
  SetProtect(base_address, length, access);
  return true;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  access_out = PageAccess::kNoAccess;
  if (!InWindow(base_address, 1)) {
    length = 0;
    return false;
  }
  std::lock_guard<std::mutex> lock(g_protect_mutex);
  const size_t first = (static_cast<uint8_t*>(base_address) - g_window) / kPage;
  const uint8_t a = g_protect[first];
  size_t n = 1;
  const size_t limit = length ? (length + kPage - 1) / kPage : 1;
  while (n < limit && first + n < g_protect.size() && g_protect[first + n] == a) ++n;
  length = n * kPage;
  access_out = PageAccess(a);
  return true;
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length, PageAccess access,
                                          bool commit) {
  (void)path;
  (void)access;
  (void)commit;
  if (!g_window) {
    g_window_size = AlignUp(length);
    // 64 KB alignment: the guest heaps' largest page, and what the generated code assumes of base (32 at least).
    // Once per run: sbrk's memory is not given back, so a second mapping (none is made) reuses the first block.
    static uint8_t* block = nullptr;
    static size_t block_size = 0;
    if (!block || block_size < g_window_size) {
      void* p = sbrk(static_cast<intptr_t>(g_window_size + 0x10000));
      block = p == reinterpret_cast<void*>(-1) ? nullptr : static_cast<uint8_t*>(p);
      block_size = block ? g_window_size : 0;
    }
    g_window = block ? reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(block) + 0xFFFF) & ~uintptr_t(0xFFFF))
                     : nullptr;
    if (!g_window) {
      REXLOG_ERROR("Could not allocate the guest window of 0x{:X} bytes from the wasm heap", length);
      g_window_size = 0;
      return kFileMappingHandleInvalid;
    }
    g_protect.assign(g_window_size / kPage, uint8_t(PageAccess::kNoAccess));
  }
  return kWebMapping;
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path) {
  (void)path;
  if (handle == kWebMapping && g_window) {
    // the block stays (sbrk has no free); the one mapping of a run closes only at its end
    g_window = nullptr;
    g_window_size = 0;
    g_protect.clear();
  }
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length, PageAccess access,
                  size_t file_offset) {
  (void)access;
  (void)file_offset;  // no aliasing: each view is its own part of the window
  if (handle != kWebMapping || !InWindow(base_address, length)) return nullptr;
  return base_address;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address, size_t length) {
  (void)base_address;
  (void)length;
  return handle == kWebMapping;
}

}  // namespace rex::memory

#endif  // REX_PLATFORM_WEB
