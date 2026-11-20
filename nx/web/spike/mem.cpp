// Spike: wasm64 + pthreads, a 4.5 GiB guest window from malloc, touched sparsely from threads.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
int main() {
  const size_t size = 0x120000000ull;
  auto t0 = std::chrono::steady_clock::now();
  uint8_t* base = (uint8_t*)aligned_alloc(65536, size);
  auto t1 = std::chrono::steady_clock::now();
  printf("window %p size %zx alloc %lld ms\n", base, size, (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
  if (!base) { printf("FAIL alloc\n"); return 1; }
  std::atomic<uint64_t> sum{0};
  std::vector<std::thread> ts;
  const uint32_t addrs[] = {0x40000000u, 0x82000000u, 0xA0000000u, 0xBFFF0000u, 0xC0001000u, 0xFFFF0000u};
  for (int i = 0; i < 6; i++) ts.emplace_back([&, i] {
    volatile uint32_t* p = (uint32_t*)(base + addrs[i]);
    for (int k = 0; k < 1024; k++) p[k * 16] = addrs[i] + k;
    uint64_t s = 0; for (int k = 0; k < 1024; k++) s += p[k * 16];
    sum += s;
  });
  for (auto& t : ts) t.join();
  // A small guest-like loop: base + 32-bit ea, to get a rough wasm64 memory speed.
  auto t2 = std::chrono::steady_clock::now();
  uint32_t ea = 0x40000000u; uint64_t acc = 0;
  for (uint32_t i = 0; i < 200000000u; i++) { ea = 0x40000000u + ((ea * 1103515245u + 12345u) & 0xFFFFFCu); acc += *(volatile uint32_t*)(base + ea); }
  auto t3 = std::chrono::steady_clock::now();
  printf("threads ok sum=%llu loads 200M in %lld ms acc=%llu\n", (unsigned long long)sum.load(), (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count(), (unsigned long long)acc);
  printf("SPIKE DONE\n");
  return 0;
}
