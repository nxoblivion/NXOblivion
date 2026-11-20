# NXOblivion: the Windows build of the recompilation

Title ID 425307D1 (`default.xex`, 11.3 MB). Recompiled to native code with the
ReXGlue SDK v0.10.0 (bundled in `thirdparty/rexglue-sdk`).

## Status (Windows x64, Release)
- Codegen is clean (0 analysis errors), project builds, links and runs.
- Boots through the kernel/VFS, Direct3D 12 GPU backend, SDL audio, input.
- Reaches the **main menu** and stays running for minutes without crashing. Audio initialises.
- Menu dialogs ("Start new game?") render correctly (matches Xenia) after SDK patch 3
  below. The same fix removed the green/red tints on the main-menu map cross-fade.
- New game reaches the loading screen; 3D in-game rendering works.
- Not tested in depth: gameplay, save/load, intro movies (Bink, Data2\Video).

## Clone
```

git clone --recurse-submodules https://github.com/nxoblivion/NXOblivion.git
```
- `assets/` (game data, about 5.9 GB) is not in the repository: extract your own copy there (see "Game data").
- `thirdparty/rexglue-sdk` is a submodule on branch `nxoblivion` of
  https://github.com/nxoblivion/rexglue-sdk: upstream v0.10.0 plus the ports' SDK changes (the patches listed below and more).
- `generated/` is not committed. The build regenerates it.
- `CMakeUserPresets.json` is machine-local. Copy `CMakeUserPresets.example.json` (Windows)
  and adapt it (e.g. inherit `mac-arm64-release`, prefix `out/install/mac-arm64`). It points `CMAKE_PREFIX_PATH`
  at your SDK install prefix.
- On Windows, re-apply the libmspack symlink-stub fix after cloning (see "Build the SDK").

## Prerequisites
Windows 11, Git, CMake >= 3.25, Ninja, LLVM/Clang (tested 22), Visual Studio
(for the MSVC environment: `tools/vsenv.bat` calls vcvars64), Python 3.

## Game data
`assets/` must contain `default.xex`, `Oblivion.ini`, `data/` and `Data2/` from the
disc. The ISO is an ISO9660/UDF image (not an XDVDFS xiso, xdvdfs fails on it): mount it
with `Mount-DiskImage` and copy out with `robocopy`. Both `Data` and `Data2` are used
by the game (`D:\Data\...` and `D:\Data2\Video\`, `Voices Dialogue`).
The exe finds `assets/` by walking up from its own folder (see `src/oblivion_app.h`),
or pass the game folder as first argument / `--game_data_root`.

## Build the SDK (once)
`tools\build_sdk.bat` (configure + build + install all configs to
`thirdparty/rexglue-sdk/out/install/win-amd64`, also registers it in the CMake user
registry). Symlink fix: git symlinks are text stubs here (core.symlinks=false);
the stubs inside `thirdparty/libmspack` were replaced by copies of their targets.
Check for the same problem if a submodule build fails with odd include errors.

## Build the game
1. `CMakeUserPresets.json` (git-ignored) points `CMAKE_PREFIX_PATH` at the bundled SDK so
   other installed SDK versions are not picked up (presets `local-release`,
   `local-relwithdebinfo`).
2. `tools\build.bat [preset]` (default `local-release`) configures, runs codegen
   (`rexglue codegen oblivion_manifest.toml`, about 20 s) and builds (about 5 min cold).
   From Git Bash: `tools/rebuild.sh`. Codegen alone:
   `thirdparty/rexglue-sdk/out/install/win-amd64/bin/rexglue.exe codegen oblivion_manifest.toml`
   (add `--ignore-stamp` after changing the SDK).
3. Output: `out/build/local-release/oblivion.exe` (+ rexruntime.dll, rexgpu-xenos.dll).
   A PDB is produced also for Release (`-Wl,/DEBUG`).

## Run
`out\build\local-release\oblivion.exe --log_file=run.log --log_level=info`
(`tools/run.sh [seconds]` runs with a timeout and logs to `tools/run.log`;
`tools/trial.sh name <args>` runs, screenshots after 45 s via `tools/shot.ps1`).
The GPU plugin `xenos` is selected in code (`OnPreSetup`). Startup to the menu
takes about 30 s (the game spends it loading/hashing; no visible progress).

Input: a connected controller is used as player 1. Keyboard emulation is off by default;
enable it with `--mnk_mode=true` (Start = Enter/X, A = Space/Semicolon). Note that
the D3D12 backend is the only one compiled on Windows, so `--gpu_backend=vulkan` does nothing here.

Debug helpers in `tools/`:
- `ab.sh <name> [exe args]`: boot, wait 50 s, optionally send keys
  (`KEYS="vk:0D wait:5000 vk:20 wait:2500"`, via `keys.ps1`), screenshot to
  `tools/ab_<name>.png`, kill. With `--mnk_mode=true` that sequence opens "Start new game?".
- `xref.py callers|callees|asm <sub_X>`: cross-reference / disassembly of generated code.
- `bsa.py <bsa> list|extract <substr> <dir>`: Oblivion BSA (v103) reader.
- `ddx2png.py in.ddx out.png`: decode Xbox `.ddx` textures (tiled DXT1/3/5).
- `build_sdk_tests.bat`: builds the SDK's PPC instruction tests (`out/win-amd64/ppc_tests.exe`;
  all 1458 pass).
- Menu tile operator ids (for midasm tracing in the evaluator `sub_822FC0C8`, merge point
  0x822FC680, r26 = op, f31 = operand, 4(r27) = value): 2001 copy, 2002 add, 2003 sub,
  2004 mul, 2005 div, 2019 floor, 2021 onlyif, 2025 ceil, 2027 ref. XML `&true;` = 2, `&false;` = 1.

## Manifest (`oblivion_manifest.toml`) - what and why
Keys live under `[entrypoint]` (`[entrypoint.functions]`), not top level.
- Tail-call targets reported unresolved by the first codegen: 0x82578048, 0x825781C0,
  0x82824C98 (added as function entries).
- Function pointers found in data (CRT init table etc.): 0x82A2EC90, 0x8263B328,
  0x82712378, 0x82723B18.
- Many small virtual-method thunks/stubs the analyzer missed (Oblivion has no RTTI so
  the SDK's vtable scanner finds nothing). Found with the helper scripts:
  `tools/find_vtable_funcs.py` (code pointers in data tables) and
  `tools/find_gaps.py` (code not covered by any generated function, filtered to
  gaps outside other functions and ending in blr/bctr/b). Both need
  `tools/image.bin`, a dump of the loaded guest image: run once with
  `OBLIVION_DUMP_IMAGE=<path>` (hook in `src/oblivion_app.h`).
  Symptom of a missing one: `[FATAL] Call to invalid or unregistered function at guest address ...`.
- No setjmp/longjmp found (no function saving r14-r31 + f14-f31 to a buffer), so none configured.
- No midasm hooks or manual switch tables are needed so far.

## Project code (`src/`)
- `oblivion_app.h`: asset auto-discovery, GPU plugin default, image dump hook, FP guard install.
- `oblivion_fp_guard.h`: guest FP-exception guard (adapted from Kameo). Without it the
  host raises STATUS_FLOAT_INEXACT_RESULT (0xC000008F) in the first FP-heavy guest code.
  Windows uses a vectored handler; POSIX variant included (SIGFPE) for macOS/Linux.
- `crash_handler.h`: Windows-only crash log (`oblivion_crash.txt`, symbolize with `tools/sym.sh`).

## SDK patches (thirdparty/rexglue-sdk) - keep when updating
1. `src/codegen/function_scanner.cpp` (detectJumpTable): stop tracing the switch index
   register after the first `slwi`. It walked back through unrelated writers and picked a
   stale register (`switch (r30)` instead of r11 at 0x828761E4, function 0x82875FC0),
   causing a trap at the first menu-loading code.
2. `src/filesystem/virtual_file_system.cpp` (ResolvePath): device-less relative paths
   (e.g. `Plugins.txt`, `Data\Music\Special\TES4Title.xma`) are resolved against `game:\`.
3. `src/graphics/d3d12/command_processor.cpp` and `src/graphics/vulkan/command_processor.cpp`
   (vertex buffer residency in the draw path): removed the shortcut that skipped
   `shared_memory_->RequestRange` when a vertex fetch constant had the same address/size
   as the previous request. Those per-slot states survive frame boundaries, so a vertex
   buffer the CPU rewrote in place was never re-uploaded and the GPU kept drawing the
   first version. Oblivion's menu quads are rewritten in place as the tile layout settles
   (dialog height 55 -> 179), so the "Start new game?" frame stayed collapsed at its first
   layout and the parchment/edges were missing. Upstream Xenia has no such shortcut;
   RequestRange is cheap when the pages are still valid.
Rebuild with `tools\build_sdk.bat` after SDK edits (the game build copies the new
`rexgpu-xenos.dll`).

## Notes / other issues seen
- Host FS is case-insensitive on Windows (`Data` vs `data`, `Data2`). On macOS/Linux
  (case-sensitive) the VFS needs case-insensitive lookup or the folder names must be
  normalised; the game uses `D:\Data\`, `D:\data\` and `D:\Data2\`.
- The game tries to write `D:\temp` and `D:\Data\Shaders\Data\Fragments\Generated`
  (read-only game dir) - harmless so far.
- `BaseHeap::AllocFixed ... already reserved` errors during startup (about 50), harmless so far.

## macOS/iOS port notes
SDK has `mac-arm64` presets and MoltenVK in `thirdparty/moltenvk`. Keep Windows-only code
behind `#ifdef _WIN32` (done: crash handler, FP guard). Needed: case-insensitive VFS,
re-run the `find_*` helpers is not needed (manifest is platform independent), verify
the FP guard POSIX path (use `OnPostSetup`), and Vulkan backend for the xenos plugin
(`render_target_path_vulkan`). SDK patch 3 is already applied to the Vulkan backend
(macOS runs it through MoltenVK; there is no separate Metal backend in this SDK). Fresh clones on macOS get real symlinks, so the libmspack/MoltenVK
symlink-stub problem does not apply there.
