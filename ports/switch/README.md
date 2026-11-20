# Nintendo Switch homebrew port (devkitPro / libnx) - phase 1 + 2 + 3

Phase 1: the ReXGlue SDK runtime compiles for `aarch64-none-elf` and the generated game code compiles.
Phase 2 (below): memory layer, headless entry point, `oblivion.nro` links. Phase 3 (see "Null Xenos GPU" and "Boot status"): null-Xenos GPU, kernel modules, paced null audio; in Eden the game loads
Oblivion.esm and the UI tile trees and now reaches its steady main loop (main menu, swaps increasing); hardware untested.

Branches: project `switch-port`, SDK submodule `thirdparty/rexglue-sdk` branch `switch`
(from `oblivion`, local only).

## Toolchain (docker image `devkitpro/devkita64:latest`)
- GCC (devkitA64) 15.2.0, C++23 OK (`<format>`, `<bit>`, `<expected>`, `<print>` verified). **No clang** in
  the image, so the SDK's "clang required" check is relaxed for Switch (GCC >= 14).
- libnx (`/opt/devkitpro/libnx`), CMake 3.31.6, Ninja.
- Portlibs in `/opt/devkitpro/portlibs/switch`: SDL2 (+image/mixer/ttf/net/gfx), EGL, GLESv1_CM, GLESv2, glad,
  glfw3, deko3d (`libnx/lib/libdeko3d.a`), zlib, zstd, lz4, lzma, xxhash, libpng, freetype, mbedtls, opus, ogg, curl...
  **No SDL3, no Vulkan, no libavcodec in portlibs** (the SDK's vendored FFmpeg is built in-tree instead).
- CMake toolchain file: `$DEVKITPRO/cmake/Switch.cmake` (sets `CMAKE_SYSTEM_NAME=NintendoSwitch`,
  `-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec -fPIC -D__SWITCH__`,
  prefix path `portlibs/switch` + `libnx`, defines `nx_create_nro()` helpers). Inside the container `DEVKITPRO`
  is already set, so the preset can reference `$env{DEVKITPRO}`.
- `devkitA64/bin` is not on PATH by default in the image; `sdk-docker.sh` adds it.

## Commands
```
# hello world NRO (official application template, prints + svcOutputDebugString)
ports/switch/sdk-docker.sh 'cd ports/switch/hello && make'        # -> hello.nro

# configure + build SDK runtime for Switch
ports/switch/sdk-docker.sh 'cd thirdparty/rexglue-sdk && cmake --preset switch-release && cmake --build out/build/switch-release -j8'
#   -> thirdparty/rexglue-sdk/out/switch-arm64/librexruntime.a (+ libfmt, libspdlog, liblibavcodec, ...)

# syntax-check all generated game sources (or pass -c to emit objects into out/switch-gen/)
ports/switch/sdk-docker.sh 'ports/switch/compile-generated.sh -fsyntax-only'
```
On Git Bash set `MSYS_NO_PATHCONV=1` (the script does). Raw docker form:
`docker run --rm -v "C:/path/to/NXOblivion:/work" -w /work devkitpro/devkita64 bash -c '...'`.

## SDK changes (branch `switch`)
- `CMakePresets.json`: `switch-base` / `switch-release` / `switch-debug` (Ninja, Switch.cmake, Vulkan/Tracy/FidelityFX/
  perf counters/tests OFF).
- Top-level `CMakeLists.txt`: `REX_SWITCH` (from `CMAKE_SYSTEM_NAME`), platform `switch-arm64`, `REX_PLATFORM_SWITCH`,
  `_GNU_SOURCE` (newlib hides fileno/strdup/pread/... otherwise), GCC allowed, `-ffp-contract=off` instead of
  `-ffp-model=strict`, no D3D12/Vulkan requirement, codegen tool/`rexglue` not built, rexgpu-xenos plugin skipped.
- `rexruntime` is STATIC on Switch.
- `thirdparty/CMakeLists.txt`: SDL3 is NOT built; `SDL3::SDL3` is an INTERFACE target exposing only the vendored
  SDL3 headers (compile-only). FFmpeg gets a generated overlay `config.h` (linux-aarch64 config minus Linux-only
  probes) because upstream `config.h` has no Switch entry. spdlog gets `SPDLOG_NO_TZ_OFFSET`.
- `include/rex/platform.h`: `REX_PLATFORM_SWITCH` (not Linux, not Mac).
- Reused POSIX files (small `#if REX_PLATFORM_SWITCH` fixes): atomic, clock, dbg, filesystem, console, env, socket,
  threading, system, math_gcc, xam_net, xsocket, sha256, stfs_xbox.h (timegm), renderdoc_api, window_sdl.

## STUBS (compile only; each must be implemented before anything runs)
Whole-file stubs (`src/core/`, listed under `REX_SWITCH` in `src/core/CMakeLists.txt`):
| file | replaces | must eventually do |
|---|---|---|
| `memory_switch.cpp` | `memory_posix.cpp` | the central one: reserve/commit/decommit/protect inside the 4 GiB guest range, query protect, file-backed views for physical heaps (libnx virtmem + svcMapPhysicalMemory / svcSetMemoryPermission, or software-translated memory). All allocators log + return failure. `page_size()`=0x1000. |
| `mapped_memory_switch.cpp` | `mapped_memory_posix.cpp` | `MappedMemory::Open` (read file into buffer / paged), chunked writer. Returns nullptr. |
| `exception_handler_switch.cpp` | `exception_handler_posix.cpp` | Hook `__libnx_exception_handler`: AArch64 data abort -> `rex::arch::Exception`, dispatch handlers (MMIO, protected-page callbacks), resume at patched PC. `Install` only logs. |
| `seh_switch.cpp` | `seh_posix.cpp` | Translate faults inside guest SEH scopes into `SehException`. `seh_initialize` only logs, `seh_rethrow` is REX_FATAL. |
| `fiber_switch.cpp` | `fiber_posix.cpp` | Host fibers (no ucontext in newlib): asm swap of x19-x30/sp/d8-d15 or libnx coroutines. All entry points REX_FATAL. |
| `dynlib_switch.cpp` | `dynlib_posix.cpp` | No dlopen; `Load` always fails. GPU plugin must be linked statically. |
| `src/ui/surface_switch.cpp` | `surface_gnulinux.cpp` | Native window surface (NWindow/EGL) for the chosen GPU backend. Empty. |

Partial stubs / gaps inside reused files:
- `threading_posix.cpp`: no signals on libnx, so `install_signal_handler` is a no-op (thread Suspend/Resume and
  `QueueUserCallback` cannot interrupt a running thread; callbacks only drain in alertable waits), no
  SCHED_FIFO / `set_priority` / `priority()` (-> svcSetThreadPriority), no CPU affinity (reports all CPUs;
  -> svcSetThreadCoreMask), `current_thread_system_id`/`system_id` derive from `pthread_t`.
  Needs a real cooperative or SVC-based suspend mechanism.
- `window_sdl.cpp`: `CreateSurfaceImpl` returns nullptr on Switch.
- `SDL3::SDL3` is headers only. window/input/audio (`rexui`, `rexinput`, `rexaudio`) use SDL3 API calls that will not
  link: needs either SDL2-portlib shims or native libnx (hid/audout/NWindow) implementations.
- GPU: no backend at all (`rexgpu-xenos` skipped, D3D12/Vulkan off). Needs a deko3d or GLES backend.
- Not implemented: `gpu_plugin_loader.cpp` (dlopen based) will fail at runtime; FFmpeg NEON asm builds but is untested on target.

## Current state
- Hello NRO builds.
- SDK runtime static libs compile and archive for aarch64 (0 errors): fmt, spdlog, libavutil, libavcodec, mspack,
  aes128, o1heap, xxhash, disasm, tiny-aes and `librexruntime.a` (rexcore + filesystem + ui + input + audio + system +
  kernel objects). Not linked into any executable.
- All 151 generated `generated/default/*.cpp` pass `g++ -fsyntax-only` for aarch64 with GCC 15 (`-w`); the first one
  (`oblivion_recomp.0.cpp`) compiled to an object (~20 s). Game `src/` (main.cpp / oblivion_app) is not ported yet.
- Windows build is unaffected by design (all changes gated on `REX_SWITCH` / `REX_PLATFORM_SWITCH`); a Windows
  configure of the SDK with clang + `rexcore/rexfilesystem/rexui` objects built fine apart from a pre-existing
  need for the preset's `-march=x86-64-v2`.

---

# Phase 2 status

## Build everything (exact commands, Git Bash, Docker running)
```
ports/switch/sdk-docker.sh 'cd thirdparty/rexglue-sdk && cmake --preset switch-release && cmake --build out/build/switch-release -j8'
ports/switch/sdk-docker.sh 'ports/switch/compile-generated.sh -c'      # 151 objects -> out/switch-gen/*.o  (~10 min)
ports/switch/sdk-docker.sh 'ports/switch/oblivion/build.sh'             # -> ports/switch/oblivion/oblivion.nro (~51 MB)
ports/switch/sdk-docker.sh 'cd ports/switch/memprobe && make'           # -> ports/switch/memprobe/memprobe.nro
```
`compile-generated.sh` compiles `generated/default/*.cpp` from stdin against a *shadow copy* of the generated
headers (`out/switch-gen/inc/`), because the generated `oblivion_pch.h` uses `__attribute__((alias))` in front of `extern "C"`, which GCC silently
drops (the weak `sub_XXXXXXXX` symbols then do not exist, giving link errors). This is fixed in the SDK template
(`resources/templates/codegen/pch_h.inja`: GCC branch of `DEFINE_REX_FUNC`), so a
regeneration with the new SDK makes the shadow unnecessary. The `generated/` folder was NOT touched (the Windows
build uses it).

## SD card layout (what to copy to the Switch)
```
sdmc:/switch/oblivion/oblivion.nro        (from ports/switch/oblivion/oblivion.nro)
sdmc:/switch/memprobe/memprobe.nro        (optional, from ports/switch/memprobe/memprobe.nro)
sdmc:/oblivion/assets/default.xex          the repo assets/ folder (~5.9 GB), same tree: default.xex, Oblivion.esm,
sdmc:/oblivion/assets/Oblivion.esm         *.bsa, Data/... . FAT32 caps files at 4 GiB: use exFAT if any file is bigger.
sdmc:/oblivion/oblivion.toml              optional rex cvars (log_level, rexcrt_heap_size_mb, ...)
sdmc:/oblivion/log.txt                    written by the app (spdlog, debug level)
sdmc:/rex_crash.txt                       written by the fault handler on a fatal host exception
sdmc:/memprobe.txt                        written by memprobe
```
Run with **title takeover** (open hbmenu by holding R while launching a game, not via the album/applet): applet
mode gives a small memory budget. `main.cpp` logs total/used memory at start.

From the PC: `nxlink -a <switch-ip> ports/switch/memprobe/memprobe.nro` (hbmenu: press Y for netloader), or copy to the SD.

## Eden emulator (test bench; replaces Ryujinx)
Official release v0.2.1 (Windows msvc zip) from the project's own host:
`https://stable.eden-emu.dev/v0.2.1/Eden-Windows-v0.2.1-amd64-msvc-standard.zip`
(release list: `https://git.eden-emu.dev/eden-emu/eden/releases`). Unzipped into `tools/eden/app/` (git-ignored).
* Portable mode: an empty `user/` folder next to `eden.exe` / `eden-cli.exe` makes Eden keep everything there.
  - virtual SD card (`sdmc:`): `tools/eden/app/user/sdmc/`
  - logs: `tools/eden/app/user/log/eden_log.txt`; `svcOutputDebugString` appears as `Debug.Emulated` lines.
  - keys go in `tools/eden/app/user/keys/` (`prod.keys`, `title.keys`); without portable mode: `%APPDATA%\eden\keys`.
    firmware goes to `user/nand/system/Contents/registered/`.
* **Homebrew NROs run WITHOUT prod.keys or firmware** (Eden synthesises system archives and a hard-coded firmware
  version); nothing about keys is printed for NRO launches. Games (NSP/XCI) would need them; they are not needed here.
* Launch: `tools/eden/app/eden-cli.exe -g C:/path/to/app.nro` (headless-ish, opens a window; it never exits by itself,
  kill it or use `timeout 60`). Typical test loop:
  ```
  cd tools/eden/app
  timeout 90 ./eden-cli.exe -g C:/path/to/NXOblivion/ports/switch/oblivion/oblivion.nro
  cat user/sdmc/oblivion/log.txt
  ```
* The game assets are visible on the virtual SD card through a directory junction (no copy):
  `New-Item -ItemType Junction -Path tools\eden\app\user\sdmc\oblivion\assets -Target <repo>\assets`
* Eden specifics that differ from hardware: no hbloader, so `envGetOwnProcessHandle()` is 0 (memprobe falls back to the
  pseudo handle); the libnx default heap swallows all free memory (hence `__libnx_initheap` overrides); page
  permissions on heap memory are NOT enforced (a write to a read-only page does not fault); unmapped guest reads
  print `HW.Memory <Error> Unmapped Read8 @ ...` in the Eden log and return 0 instead of faulting.

## memprobe results (Eden 0.2.1; real hardware still pending)
Full output: `ports/switch/memprobe/results-eden-0.2.1.txt`. Findings:
* Total memory 3285 MiB for an Application; regions: ASLR 511 GiB, Alias 64 GiB, Heap 8 GiB, Stack 2 GiB.
* 4.5 GiB / 16 GiB virtmem reservations: fine.
* `shmemCreate` -> `0x4201` (Kernel NotImplemented): shared-memory aliases are NOT available in Eden (privileged SVC).
* `svcMapProcessMemory` with the pseudo own-process handle -> `0xE401` (InvalidHandle); needs a real handle (hbloader only).
* `svcMapMemory` heap -> stack region: works (4 MiB moved; source becomes inaccessible, i.e. a move, not an alias).
  The first ~1 MiB of the stack region is already occupied by the libnx main-thread stack (mapping at its very start
  fails with `0xD401`); the last 64 KiB works.
* `svcMapPhysicalMemory` -> `0xFA01` ("System Resource Size is zero").
* `svcSetMemoryPermission(R)` succeeds but a write still succeeds without a fault (Eden does not enforce it).
* **Hardware still has to confirm** whether `shmemCreate` / `svcMapProcessMemory` work there; the design below does not
  depend on them.

## Memory design (src/core/memory_switch.cpp) - "linear windows"
The first design (aliased views backed by shared memory) is not usable where `shmemCreate` is unavailable. Current design,
which needs only `svcMapMemory`:
* Guest address `g` is at `base + g` exactly as in the POSIX arena (generated code and runtime unchanged);
  `base = StackRegionAddress + 2 MiB - 0x40000000`, i.e. the guest range 0x40000000..0xBFDFFFFF is the 2 GiB stack region.
* Existing windows (virtmem reservations): `0x40000000..0x83FFFFFF` (v40000000 heap incl. title thread stacks at
  0x70000000+, the 0x7F MMIO area, the XEX heap v80000000 capped to 64 MiB) and `0xA0000000..0xBFDFFFFF` (physical, 2 MiB
  short). The raw "physical" view is the A0000000 window itself (`physical_membase = base + 0xA0000000`): no aliases.
* `Memory::LookupHeapByType` redirects every allocation to those two heaps (the v00000000 4 KiB heap, C0000000 and E0000000
  views do not exist on Switch; 4 KiB requests are rounded to 64 KiB units).
* Commit = take a contiguous run of 64 KiB units from a **pool** (the second part of the process heap, set in
  `__libnx_initheap` via `SwitchSetGuestPool`: 640 MiB newlib + 1280 MiB pool) and `svcMapMemory` it onto the guest address.
  Decommit unmaps whole units back to the pool (partial units are zeroed). A fault in a window demand-commits.
* No per-page protection, no write-watch, no fault-driven MMIO (documented, GPU-only needs).
* Costs/limits: guest memory is capped by the pool (1.25 GiB); guest accesses below 0x40000000 (the v00000000 heap, NULL
  page) are unmapped; 4 KiB-granular page protection semantics are gone.

## memprobe (ports/switch/memprobe)
Prints and writes `sdmc:/memprobe.txt`: (d) applet type, Total/Used memory, address-space regions; (a) 1 / 4.5 / 16 GiB
VA reservations via virtmem; (b) b1 one 512 MiB `shmemCreate` object mapped 4 times with `svcMapSharedMemory` at fixed
addresses inside the reservation, plus write/read-through check, partial-size map, permission change on shmem pages;
b2 `svcMapMemory`; b3 `svcMapProcessMemory` with the own-process handle; b4 `svcMapPhysicalMemory`; (c) write-fault
tests (`__libnx_exception_handler`, re-grant write permission, `svcReturnFromException(0)`) on shmem, process-memory and
plain heap pages; max `svcSetHeapSize`.
**Results: NOT available yet** (no Switch environment could be run here). Please run it on the real Switch (title takeover
AND applet mode) and send back `sdmc:/memprobe.txt`.

### Not used: aliasing and software-translation alternatives
(First design, aliased views over `svcCreateSharedMemory` segments, is in git history: commit "shared-memory-segment memory layer".)
Software translation, if the linear windows ever prove too small (design only, not implemented):
Collapse the aliases in software onto one canonical mapping (512 MiB physical block at `base + 0x100000000`, 0x90 onto 0x80):
1. `REX_PHYS_HOST_OFFSET(addr)` (pch_h.inja, ppc_config_h.inja) and `PhysicalHostOffset` (xmemory.h) become a rebase macro:
   a >= 0xA0000000 -> 0x100000000 + (a & 0x1FFFFFFF) (+0x1000 for >= 0xE0000000); 0x7Fxxxxxx -> 0x100000000 + (a & 0xFFFFFF);
   0x9xxxxxxx -> 0x8xxxxxxx. Every generated `base + (u32)x + OFFSET` site goes through that one macro (about 3 ALU ops per
   guest access, but needs regenerating nothing but the macro).
2. `Memory::TranslateVirtual/TranslatePhysical`, `HostToGuestVirtual`, `GuestPtr`, `BaseHeap::TranslateRelative` apply the
   same rebase; `MapViews` maps only the canonical views.
3. Without aliasing the remaining problem is committing pages at exact addresses of the 4 GiB range (needs
   `svcMapMemory` into the stack region, or a guest-page-table translation, which is a bigger change).
Horizon has no lazy commit, so commit must allocate real pages (the segment scheme already does that).

## Headless boot (ports/switch/oblivion)
`main.cpp` does what `ReXApp::ConstructRuntime/LaunchModule` do, without a window: `Runtime::Setup` with
`kernel_init = rex::kernel::InitializeKernel` (xboxkrnl/xam/xbdm modules; **required**: without it no kernel variable import is
patched (KeTimeStampBundle, VdHSIOCalibrationLock, ...) and the game spins on garbage pointers), `config.graphics =
NullGraphicsSystem` (below), `NopAudioSystem` (paced null driver), no input factory, `game_data_root = sdmc:/oblivion/assets`,
`register_modules`, `LoadXexImage("game:\default.xex")`, `PrepareModuleLaunch`, `Resume`, `Wait`. `__libnx_initheap` allocates one
bounded heap block: 640 MiB newlib heap + 1280 MiB guest pool (smaller on small budgets).
`oblivion_app.h`, `oblivion_fp_guard.h` and `crash_handler.h` belong to the windowed ReXApp and are not used by this entry point.

Other stubs replaced: `mapped_memory_switch.cpp`, `fiber_switch.cpp` (thread-fiber conversion only), `exception_handler_switch.cpp`
(demand commit + crash log), `libc_switch.cpp` (newlib gaps).

## Null Xenos GPU (phase 3): `rex::graphics::NullGraphicsSystem`
Files: `include/rex/graphics/null_graphics_system.h`, `src/graphics/null_graphics_system.cpp` (compiled into `rexruntime` only under
`REX_SWITCH`, listed in `src/system/CMakeLists.txt`; nothing on other platforms references it).
Design: a self-contained `system::IGraphicsSystem`. The real `GraphicsSystem`/`CommandProcessor` need `ui::GraphicsProvider`,
`Presenter`, windows and the plugin loader, none of which exist on Switch, and there was no null-GPU code in the tree or history, so the
packet interpreter re-implements the semantics of `src/graphics/command_processor.cpp` that a guest can observe:
* `SetupGuestGpu` registers the MMIO range `0x7FC80000-0x7FC8FFFF` with `Memory::AddVirtualMappedRange`. Generated code reaches GPU
  registers through `REX_MM_LOAD/STORE_*` -> `MMIOHandler::CheckLoad/CheckStore`, which works on Switch without any fault handler.
  Writes to `CP_RB_WPTR` (0x01C5) wake the ring worker; reads return the constants of the real `ReadRegister` (`RB_EDRAM_TIMING`,
  `RB_BC_CONTROL`, `D1MODE_V_COUNTER`, interrupt status = vblank, viewport size).
* "GPU Commands (null)" worker thread (an `XHostThread`): consumes the primary ring from its read index to WPTR (wraps) and writes the
  read pointer back to the `VdEnableRingBufferRPtrWriteBack` address after each batch. PM4: type 0/1/2 register writes (scratch
  register write-back); type 3: `INTERRUPT` (dispatches the guest callback), `XE_SWAP` (counted, nothing presented),
  `INDIRECT_BUFFER(_PFD)`, `WAIT_REG_MEM` (register or memory, GPU endian swap, coherency register always coherent, warns once if
  stuck), `REG_RMW`, `REG_TO_MEM`, `MEM_WRITE`, `COND_WRITE`, `EVENT_WRITE`, `EVENT_WRITE_SHD` (fences, optionally the swap counter),
  `EVENT_WRITE_EXT`, `EVENT_WRITE_ZPD` (fake sample counts). Draws are counted and skipped; everything else (shaders, constants,
  state) is consumed without effect. No resolves, textures, shaders or render targets.
* "GPU VSync (null)" thread: 60 Hz (steady clock, no catch-up bursts), `ExecuteInterrupt(callback, {0, user_data})`, so the D3D vblank
  ISR runs 60 times a second.
* Log line every 5 s: `GPU heartbeat: swaps=.. vblanks=.. packets=.. draws_skipped=.. irq=.. waits=.. rd=.. wr=..`.
  `app heartbeat` (uptime, host memory) every 10 s plus guest thread call stacks (`threads: tN ...`) in `main.cpp`.

Related SDK changes (all `REX_PLATFORM_SWITCH`-gated except the harmless `PosixFileHandle` close guard): `NopAudioSystem::CreateDriver`
returns a `NullAudioDriver` (discards samples, paces the guest audio callback at 5.33 ms/frame; with `X_STATUS_NOT_IMPLEMENTED` the
game destroys its renderer during init), directory opens work (newlib `open()` cannot open directories: descriptor-less `FileHandle`),
`NtCreateFile` logs `[open] <path>` for every successful open (boot markers), `NtReadFile` warns on failures, `InvalidFunctionTrap`
prints the guest call stack, `REX_HOOK` has an optional kernel-import trace (`rex::g_import_trace`, 0 = off, 1 = all threads,
N+1 = guest thread N).

## Boot status (Eden 0.2.1, headless, null GPU) - phase 3
Evidence (`sdmc:/oblivion/log.txt`, one clean run; t = seconds after the first log line):
* t+0: arena mapped, XEX image loaded, `NullGraphicsSystem` up; `SetInterruptCallback`, 64 KiB ring buffer and RPtr write-back set.
* t+3: BSA archives (Meshes/Textures/Sounds/Voices/Misc) and `Oblivion.ini` opened; D3D device creation succeeds (it used to destroy the
  renderer immediately), shader fragments + `shaderpackage.sdp` read; `GPU heartbeat: ... vblanks=300 (60.0/s)`.
* t+17: `bethesda softworks HD720p.bik` opened (intro movie thread), `final_master_xml.dat` (menu XML database) read.
* t+23: **`D:\Data\Oblivion.esm` opened and parsed** (about 17 s in Eden).
* t+37..40: UI tile trees loaded (`Menus\strings.xml`, `loading_menu.xml`, `Main\hud_main_menu.xml`, `Options\main_menu.xml`), music
  stream `Music\Explore\atmosphere_01.xma` opened. First swap (the intro movie) counted: `swaps=1`.
* **Update (phase 3b): the "shutdown" was not a shutdown, it was a deadlock in the intro-movie handover, and it is fixed.** After the
  menu tiles load, `main()` calls `sub_8211DF18`, which sets the stop flag and waits for the intro-movie (Bink) thread; the game then
  enters its real main loop (`0x82115778`). Two Switch bugs kept the movie thread alive forever:
  1. **Frozen guest clock.** newlib's `clock_gettime`/`clock_getres` do not give a usable clock under libnx, so
     `Clock::QueryGuestUptimeMillis()` was 0 forever and `KeTimeStampBundle.tick_count` never advanced; Bink's `BinkWait` thought no
     time passed and slept (`KeDelayExecutionThread` loop). Fixed in `clock_posix.cpp` (Switch: `armGetSystemTick/Freq`).
  2. **GPU interrupt CPU number.** With the clock fixed the movie thread blocked on a D3D GPU fence: the CP sat in a
     `WAIT_REG_MEM mem==0` (memory dword at the D3D device's `+10388` struct) preceded by `INTERRUPT cpu_mask=4`. The guest ISR
     (`sub_8255C268`) clears *its own CPU's bit* (`1 << lbz 268(r13)`, the PCR processor number) in that dword. The real
     `GraphicsSystem::DispatchInterruptCallback` calls `thread->SetActiveCpu(cpu)` first; `NullGraphicsSystem::Dispatch` did not, so the
     bit for CPU 2 was never cleared. Fixed (`SetActiveCpu` in `Dispatch`).
  Result in Eden (3 consecutive clean runs, 130 s each): `Oblivion iv logo.bik` and `Map loop.bik` (main-menu background) open, the main
  loop runs, `GPU heartbeat: swaps=2579 (21.8/s) vblanks=7501 (60.0/s)` (`swaps` keeps increasing; ~22 swaps/s is the Eden/JIT speed with the
  null GPU, vblank thread holds 60/s), 433 `AllocFixed ... already reserved` errors (identical count to the Windows baseline) and no
  other errors. Diagnostics that remain: `GPU stuck WAIT_REG_MEM` now dumps the last 48 PM4 packets and the raw memory around the address
  (log.txt), and `InvalidFunctionTrap` dumps all guest registers and the memory at r30/r31.
  How it was found: the Windows trace showed the same VFS lookups but no texture-fallback storm; the Switch thread dumps showed
  the movie thread (t12) in `BinkWait` -> `Sleep(1)`; a heartbeat read of `KeTimeStampBundle` showed `tick=0`; after the clock fix the
  GPU heartbeat showed `rd != wr` plus the `WAIT_REG_MEM stuck` warning and the packet history ending in `INTERRUPT(4)`.
  Not fully understood: before fix 2, two runs in a row crashed ~2 s after `final_master_xml.dat` opened (null object in the UI
  tile code, `sub_82113290` called with r4 = NULL, after six `.bsa` reads of the very first block had returned 0 bytes with SUCCESS in one
  of them) while others passed it; after fix 2 this did not recur in 4 runs. Treat early-boot nondeterminism (one startup hang in
  a CRT critical section, `0x82110F00`) as unresolved; if it recurs, check `pread` (lseek+read emulation in `libc_switch.cpp`) and
  thread scheduling.
Eden timing (indicative only; a busy PC made Eden 2-3x slower in other runs): about 25 s from process start to `Oblivion.esm` open and
about 40 s to the UI tile trees; main loop after about 60 s. Guest swaps ~22/s in Eden; the vblank thread holds 60.0/s. Real hardware numbers: pending.

Bugs found and fixed in phase 3 (each stopped the boot before): missing `kernel_init` (variable imports stayed unpatched; values such as
`0x000100AD` are the raw import records `0x00010000 | ordinal`, which also was the old "unmapped read near 0x000100BD" spin: the
`KeTimeStampBundle` pointer was the ordinal record and the tick-count read loop spun on it), audio driver `NOT_IMPLEMENTED`, directory
handles, GPU register writes (WPTR) not reaching the GPU (now MMIO callbacks).
Page zeroing: `CommitRun` already `memset`s every unit before mapping it (recycled pool units included), so freshly committed guest
memory reads as zero; no change was needed.

## Remaining blockers (ranked)
1. ~~The game leaves its init sequence and shuts down~~ FIXED in phase 3b (frozen clock + GPU interrupt CPU number, see "Boot status").
   Next: the game now runs its main loop in Eden with the null GPU, so a real GPU backend is what is needed to see anything; the guest
   frame rate (~22 swaps/s in Eden, null GPU) has to be re-measured on hardware.
2. Real hardware: run `memprobe` and `oblivion.nro` with title takeover.
3. Memory layer choice (shared-memory aliasing when `shmemCreate` works, otherwise linear windows) is NOT implemented in this phase; only
   the linear-window layer ran (`Switch guest arena: ...` in the log). The earlier aliasing design is SDK commit `d321328` (superseded);
   a startup probe (`shmemCreate` 1 MiB, map twice, check aliasing, unmap) selecting between the two layers is the next step.
4. `NtSuspendThread` still goes through the `pthread_kill` stub (not hit in these runs); implement with `svcSetThreadActivity`.
5. Input (libnx hid) and real audio (audout); the SDL3-based drivers cannot link.
6. A real GPU backend (deko3d/GLES, see `ports/switch/glprobe`) for rendering.
7. Guest memory below 0x40000000 does not exist; FAT32 4 GiB file limit. Case-insensitive lookups work (the host-path device has a
   case-insensitive fallback; the `data`/`Data` mismatch was not a problem).

## Rebuild and run (exact commands, Git Bash, Docker running)
```
# 1. only after a manifest change: regenerate code (Windows tool, writes only generated/)
thirdparty/rexglue-sdk/out/install/win-amd64/bin/rexglue.exe codegen oblivion_manifest.toml --ignore-stamp
# 2. SDK runtime (incl. NullGraphicsSystem)
ports/switch/sdk-docker.sh 'cd thirdparty/rexglue-sdk && cmake --preset switch-release && cmake --build out/build/switch-release -j8'
# 3. generated objects (~10 min; refreshes the shadow pch in out/switch-gen/inc), only after step 1
ports/switch/sdk-docker.sh 'ports/switch/compile-generated.sh -c'
# 4. app + nro
ports/switch/sdk-docker.sh 'ports/switch/oblivion/build.sh'          # -> ports/switch/oblivion/oblivion.nro
#   (ports/switch/dock.sh is the same runner with a named container `p3b-<pid>`)
# 5. run in Eden (kill leftovers first: two instances fight over log.txt)
taskkill //F //IM eden-cli.exe; rm -f tools/eden/app/user/sdmc/oblivion/log*.txt
cd tools/eden/app && timeout 300 ./eden-cli.exe -g C:/path/to/NXOblivion/ports/switch/oblivion/oblivion.nro
grep -a "\[open\]\|heartbeat" user/sdmc/oblivion/log.txt
```
Do not `docker kill` all containers while a compile is running (it leaves truncated `.o` files; find them with `aarch64-none-elf-readelf -h`).

## GL build: controller input and SD-card package (phase 5)
The shipping target is the OpenGL build (`ports/switch/oblivion_gl/`, see `GL_BACKEND_PLAN.md` section 7 and 7.1):
libnx HID controller driver in the SDK (`src/input/switch/`), cvars `switch_button_layout` and the debug `switch_input_script`,
applet-mode check, `package.sh` (-> `out/switch-package/`) and `copy-assets.ps1`. Install instructions for the user: `ports/switch/package/INSTALL.md`.
Files on the SD card: `switch/oblivion/oblivion.nro`, `oblivion/oblivion.toml`, `oblivion/log.txt`, `oblivion/assets/...`.

## GL build: new game reaches the prison cell (phase 6)
The new game now loads in Eden (about 18 min of loading screen, then the intro movie) and renders the character-creation scene in the prison cell (`oblivion_gl/screenshot-eden-ingame.png`). Details, evidence and what is left (speed ~0.4 fps in Eden, missing hair, skipped draws) are in `GL_BACKEND_PLAN.md` section 7.2. Key fixes: guest threads on all 3 cores, guest windows reserved before any thread exists, compact GL shared-memory store, distinct PS float-constant block, frontbuffer swizzle (red/blue). Debug cvar `switch_diag = true` enables the heavy main-thread profiler.
