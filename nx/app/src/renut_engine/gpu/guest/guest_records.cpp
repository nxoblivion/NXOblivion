/**
 * @file    gpu/guest/guest_records.cpp
 */
#include "renut_engine/gpu/guest/guest_records.h"

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include <o1heap.h>
#include <rex/system/xmemory.h>

#include "renut_engine/gpu/log.h"

namespace renut::gpu::records {

namespace {

// Records are small (headers + a few host pointers); 64 MiB is ample.
constexpr u32 kHeapSize = 64u << 20;

std::mutex g_heap_mutex;  // o1heap is not internally synchronised
O1HeapInstance *g_heap = nullptr;
bool g_oom_logged = false;

std::shared_mutex g_registry_mutex;
std::unordered_map<u32, RecordKind> g_registry;
std::atomic<uint64_t> g_generation{0};

// oblivion (2026-11-10): what is at each allocation granule of the heap (guest_records.h): 0 nothing registered, a
// record's kind + 1, or kHeaderMark for a header of the native D3D. o1heap hands out multiples of O1HEAP_ALIGNMENT from
// its block's start; should an address ever be off that grid, the marks stop being trusted (g_marks_exact) and every
// question goes to the locked tables again, as before.
constexpr uint8_t kHeaderMark = 0xFF;
constexpr u32 kGranule = 16;  // O1HEAP_ALIGNMENT on a 64-bit host (two pointers): 4 MiB of marks for the 64 MiB heap
static_assert(O1HEAP_ALIGNMENT % kGranule == 0);
std::atomic<u32> g_heap_va{0};
std::atomic<bool> g_marks_exact{true};
std::atomic<uint8_t> g_marks[kHeapSize / kGranule];

void SetMark(u32 guest_va, uint8_t mark) {
  const u32 base = g_heap_va.load(std::memory_order_relaxed);
  const u32 offset = guest_va - base;
  if (!base || offset >= kHeapSize || offset % kGranule) {
    if (g_marks_exact.exchange(false, std::memory_order_acq_rel))
      RGPU_WARN("Record heap: {:08X} is off the mark grid; lookups take their locks again", guest_va);
    return;
  }
  g_marks[offset / kGranule].store(mark, std::memory_order_release);
}

// The mark at an address of the heap: -1 outside the heap (never ours), -2 the marks cannot say.
int MarkAt(u32 guest_va) {
  const u32 base = g_heap_va.load(std::memory_order_relaxed);
  const u32 offset = guest_va - base;
  if (!base || offset >= kHeapSize)
    return -1;
  if (!g_marks_exact.load(std::memory_order_acquire)) [[unlikely]]
    return -2;
  if (offset % kGranule)
    return 0;
  return g_marks[offset / kGranule].load(std::memory_order_acquire);
}

} // namespace

u32 AllocPhysicalBlock(u32 size) {
  // The physical window (64 KiB pages) maps host and guest addresses 1:1 in
  // offset, so a host pointer into it round-trips to a guest VA.
  rex::memory::BaseHeap *heap = REX_KERNEL_MEMORY()->LookupHeapByType(true, 64u * 1024u);
  if (!heap) {
    RGPU_ERROR("No guest physical heap");
    return 0;
  }
  u32 guest_va = 0;
  if (!heap->Alloc(size, 64u * 1024u,
                   rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                   rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, true,
                   &guest_va)) {
    RGPU_ERROR("Reserving {} bytes of guest physical memory failed", size);
    return 0;
  }
  return guest_va;
}

bool Init() {
  std::lock_guard lock(g_heap_mutex);
  if (g_heap)
    return true;

  auto *memory = REX_KERNEL_MEMORY();
  const u32 guest_va = AllocPhysicalBlock(kHeapSize);
  if (!guest_va) {
    RGPU_ERROR("Record heap: reserving {} bytes failed", kHeapSize);
    return false;
  }
  g_heap = o1heapInit(memory->TranslateVirtual<void *>(guest_va), kHeapSize);
  if (!g_heap) {
    RGPU_ERROR("Record heap: o1heapInit failed");
    return false;
  }
  g_heap_va.store(guest_va, std::memory_order_release);  // oblivion: the lock-free answers (MarkAt)
  return true;
}

void *AllocRaw(size_t size) {
  std::lock_guard lock(g_heap_mutex);
  if (!g_heap) {
    RGPU_ERROR("Record heap used before Init");
    return nullptr;
  }
  void *p = o1heapAllocate(g_heap, size ? size : 1);
  if (!p && !g_oom_logged) {
    RGPU_ERROR("Record heap exhausted ({} bytes requested)", size);
    g_oom_logged = true;
  }
  return p;
}

void FreeRaw(void *host) {
  if (!host)
    return;
  std::lock_guard lock(g_heap_mutex);
  if (g_heap)
    o1heapFree(g_heap, host);
}

void Register(u32 guest_va, RecordKind kind) {
  std::unique_lock lock(g_registry_mutex);
  g_registry[guest_va] = kind;
  SetMark(guest_va, uint8_t(u32(kind) + 1));
}

void Unregister(u32 guest_va) {
  std::unique_lock lock(g_registry_mutex);
  g_registry.erase(guest_va);
  SetMark(guest_va, 0);
  g_generation.fetch_add(1, std::memory_order_release);
}

void MarkHeader(u32 guest_va, bool owned) {
  SetMark(guest_va, owned ? kHeaderMark : uint8_t(0));
}

int HeaderMarked(u32 guest_va) {
  const int mark = MarkAt(guest_va);
  if (mark == -2)
    return -1;
  return mark == kHeaderMark ? 1 : 0;
}

uint64_t Generation() {
  return g_generation.load(std::memory_order_acquire);
}

bool Lookup(u32 guest_va, RecordKind *out_kind) {
  // oblivion (2026-11-10): without the lock where the marks can say (guest_records.h).
  const int mark = MarkAt(guest_va);
  if (mark != -2) {
    if (mark <= 0 || mark == kHeaderMark)
      return false;
    *out_kind = RecordKind(u32(mark) - 1);
    return true;
  }
  std::shared_lock lock(g_registry_mutex);
  auto it = g_registry.find(guest_va);
  if (it == g_registry.end())
    return false;
  *out_kind = it->second;
  return true;
}

} // namespace renut::gpu::records
