# nx/ bootstrap: notes and evidence

Milestone "bootstrap" of the reNut port (`docs/renut-port.md`): a Windows PC build of Oblivion on katamari-nx's SDK fork
and app skeleton, with reNut's renderer compiled and linked in, none of its hooks registered, and the game on the SDK's
emulated Xenos GPU. Started 2026-11-03 on branch `renut-port`. GPL-3.0 (the port is based on ChanseyIsTheBest's GPL-3.0
katamari-nx, `out/ref/katamari-nx` at `dcd7e290`).

## Layout

| Path | What | From |
| --- | --- | --- |
| `nx/sdk/` | ReXGlue v0.10.0 fork with the Horizon layer and codegen options | katamari-nx `sdk/` (dcd7e290), minus `thirdparty/` |
| `nx/sdk/thirdparty/` | only the 6 files the fork changed are committed | `tools/fetch_thirdparty.py` fills the rest |
| `nx/app/` | the Oblivion app: `CMakeLists.txt`, `CMakePresets.json`, `oblivion_manifest.toml`, `src/` | katamari-nx `app/`, stripped |
| `nx/app/src/renut_engine/gpu/` | reNut's renderer as katamari-nx adapted it | katamari-nx, unchanged except `banjo_renut_select.*` |
| `nx/app/thirdparty/` | plume, reblue-XenosRecomp, uam, imgui, smaa (not committed) | `tools/fetch_deps.py` |
| `nx/app/assets` | junction to the extracted game (not committed) | `tools/link_assets.ps1` |
| `nx/app/generated/` | the code generator's output (not committed) | `tools/codegen.bat` or the build |

## Build and run

```bat
python nx\tools\fetch_thirdparty.py --source out\ref\katamari-nx\sdk  & rem SDK third-party sources: katamari-nx's exact tree
python nx\tools\fetch_thirdparty.py --source thirdparty\rexglue-sdk   & rem + the 9 FFmpeg files it lacks (see below)
python nx\tools\fetch_deps.py                                         & rem app third-party sources
powershell -ExecutionPolicy Bypass -File nx\tools\link_assets.ps1     & rem nx\app\assets -> assets\
nx\tools\build_host.bat                                               & rem nx\sdk\out\win-amd64\rexglue.exe
nx\tools\codegen.bat                                                  & rem nx\app\generated\default (log nx\out\codegen.log)
nx\tools\build_pc.bat                                                 & rem nx\app\out\build\win-amd64-release\oblivion.exe
```

`build_pc.bat` builds the SDK from source (`REXSDK_DIR=nx/sdk`, katamari-nx's way) together with the game code and the
app; its codegen step re-runs the generator by itself when `codegen.d`'s inputs change (the generator binary is not
one of them: after an SDK codegen patch run `codegen.bat`, which passes `--ignore-stamp`). The app build also writes
`rexglue.exe` and `rexgpu-xenos.dll` into `nx/sdk/out/win-amd64` (the SDK's output folder, shared with the host build).
Run: `bash nx/tools/run_pc.sh [seconds]` (minimised, muted, log in `nx/out/run.log`).

## Status (2026-11-03)

- Builds: `nx/app/out/build/win-amd64-release/oblivion.exe` (+ `rexruntime.dll`, `rexgpu-xenos.dll`), with
  `banjo_renut_gpu` (all of reNut's renderer sources, plume, its 21 host shaders, XenosRecomp and a shader cache of 0
  entries) linked in.
- Boots on the emulated GPU (D3D12, RTX 3090): 60 s run, no crash, no `[error]` besides the 433
  `BaseHeap::AllocFixed ... already reserved` lines the root baseline also prints (433 in a 60 s baseline run too).
  `VdSwap: 1 swaps (1024x600 ...)` 0.9 s after start, then 300 swaps every ~10.0 s (30 fps) up to 1500 at 50 s; the
  presenting thread changes at ~40 s. A trace run shows the menu's loads: the six BSAs, `Oblivion.esm`,
  `Data\Shaders\shaderpackage.sdp`, `Meshes\TestSphere.NIF`, `Music\Special\TES4Title.xma` (the main menu's theme).
- No pixels were checked (runs are minimised; no capture tool in this tree yet).
- Untested: the Vulkan back end of the emulated GPU (`--gpu_backend=vulkan`), Debug builds, the Switch build.

## Findings and fixes (evidence)

### SDK third-party sources

- katamari-nx `dcd7e290` commits the full `sdk/thirdparty` (18,874 files), not only the changed ones as its
  `sdk/README.md` says; zuma-nx and feedingfrenzy-nx commit only the 6 changed files (`CMakeLists.txt`,
  `FFmpeg/config_masseffect_wmv3.h`, `FFmpeg/config_switch_aarch64.h`, `FFmpeg/libavcodec/{fft,mdct}_template.c`,
  `ffmpeg-overlay/codec_list.c`; identical bytes in all three). nx/ commits those 6 and fetches the rest with zuma-nx's
  `tools/fetch_thirdparty.py` (katamari-nx has no copy of it).
- katamari-nx's committed copy is **incomplete**: FFmpeg's own `.gitignore` kept 9 files of the xenia FFmpeg branch out
  of its commit (`config.h`, `libavutil/avconfig.h`, `libavutil/ffversion.h`, `libavcodec/{bsf,codec,parser}_list.c`,
  `libavformat/{demuxer,muxer,protocol}_list.c`). Evidence: the first host build failed in every libavutil file with
  `'libavutil/avconfig.h' file not found`; `git -C thirdparty/rexglue-sdk/thirdparty/FFmpeg ls-files` lists them all
  (submodule `0604b464c7`).
- The local `thirdparty/rexglue-sdk` (the project's fork, newer than v0.10.0) as `--source`: compared with
  `diff -rq --strip-trailing-cr` against katamari-nx's complete copy, it differs only in the fork's own changes
  (`thirdparty/CMakeLists.txt`, `FFmpeg/libavcodec/wmaprodec.c`, `crypto/sha256.cpp`, `glslang/CMakeLists.txt`,
  `ffmpeg-overlay/codec_list.c`, the FFmpeg templates, an extra `SPIRV-Cross/`) plus files katamari-nx lost to nested
  `.gitignore`s. `fetch_thirdparty.py` only copies files that are missing, so the order used here gives katamari-nx's
  exact tree plus the 9 FFmpeg files: first `--source out/ref/katamari-nx/sdk` (0 differences afterwards:
  `diff -rq out/ref/katamari-nx/sdk/thirdparty nx/sdk/thirdparty` empty), then `--source thirdparty/rexglue-sdk`
  (adds the 9 FFmpeg files and ~45 unused docs/build files; its `SPIRV-Cross/` was deleted again, v0.10.0 has none).
  On a fresh checkout without `out/ref`, `fetch_thirdparty.py` with no argument clones upstream v0.10.0 (`c94f5eb`),
  which is what the fork is based on.
- The SDK is built for the host as `nx/sdk/out/host`; the fork writes `rexglue.exe` to `nx/sdk/out/win-amd64/`.

### VFS index written outside the game folder

The fork's `HostPathDevice` (`sdk/src/filesystem/devices/host_path_index.cpp`) writes an index of the game folder to
`<parent of the game folder>/cache/vfs_index_<name>.bin`. The codegen resolved the `nx/app/assets` junction to the
repository's `assets/` and wrote `cache/vfs_index_assets.bin` in the repository root (deleted again). `pc_env.bat` and
`codegen.bat` set `REX_VFS_INDEX=false` (cvars read `REX_<NAME>` from the environment, `sdk/src/core/cvar.cpp`
`ApplyEnvironment`); the run script passes `--vfs_index=false`.

### dxc binaries

katamari-nx's `reblue-XenosRecomp/thirdparty/dxc-bin` has no `bin/` or `lib/` (its `.gitignore` drops `[Bb]in/`).
gpu.cmake needs `bin/x64/dxc.exe` for the host shaders and XenosRecomp links `dxcompiler`. `fetch_deps.py` takes them
from `renderbag/dxc-bin` (the submodule URL in reblue-XenosRecomp's `.gitmodules`) at `12395e36`, whose `inc/` and
`CMakeLists.txt` are byte-identical to katamari-nx's copy.

### SDK patches from the repository's SDK (nxoblivion/rexglue-sdk)

The root build's three Oblivion fixes (`README.md`, "SDK patches") were not in katamari-nx's SDK. Without patch 1 the
first run died 1 s into the guest with `exception 0xC000001D` (`__builtin_trap`, "Switch case out of range") in
`sub_82875FC0` (`llvm-symbolizer --relative-address 0x20220f4`: `oblivion_recomp.93.cpp:29448`): `switch
(ctx.r30.u32)` instead of r11 at `0x828761E4`, exactly the README's description. Applied to `nx/sdk` with `git apply
--ignore-whitespace` from `git -C thirdparty/rexglue-sdk format-patch -1`:
`3abd07c` (codegen: stop jump-table index tracing at the first `slwi`, hunk offset +22 lines after katamari-nx's
`indirect_calls` addition), `ec4c02d` (VFS: device-less relative paths against `game:`; `Plugins.txt` and
`ArchiveInvalidation.txt` now resolve to `game:\...`) and `66dc36d` (D3D12 + Vulkan command processors: re-request
vertex buffers rewritten in place). After regenerating, `sub_82875FC0` switches on `ctx.r11.u32` and the run passes.

Added for test runs: cvar `vd_swap_log_every` (`sdk/src/kernel/xboxkrnl/xboxkrnl_video.cpp`, default 0): one log line
every N guest `VdSwap`s with the count and the front buffer size.

### Shared user data

The app is named `oblivion` like the root build, so both use `Documents\oblivion` (config, saves, the emulated GPU's
shader storage: "Translated 51 shaders from the storage" on nx's first run came from the root build's runs). Harmless
so far; rename the app if the two must not share.

### Code generation

The repository's `oblivion_manifest.toml` works unchanged with the fork's generator (all 205 function entries kept; only
the header comment differs). 307 files, 17.8 s. Its warnings are the upstream generator's: function `0x828E4CA0` (a
manifest entry, "undiscovered code gap") has no blocks and becomes an empty stub, as in the root `generated/`
(`DEFINE_REX_FUNC(sub_828E4CA0) { REX_FUNC_PROLOGUE(); }` in both); the unresolved branches to `0x824BC99C` (from
`0x824BCD84`, in `sub_824BC9F4`) and `0x828DC918` / `0x828DC9E4` (from `0x828DC9C0` / `0x828DC9DC`) produce the same
`REX_FATAL` lines in the root `generated/` and in nx's. The fork's optimised codegen keys (katamari-nx's
`renut_config.toml`: `non_volatile_as_local`, `elide_lr_stores`, ...) are **not** used: each needs a per-game audit. (2026-11-10: audited and on, `oblivion_manifest.toml` and docs/renut-port.md.)

### Hooks

`banjo_renut_select.h`: katamari-nx's `KATAMARI_RENUT_HOOKS` is `OBLIVION_RENUT_HOOKS` (CMake option, OFF). With it
off, `BANJO_RENUT_HOOK` / `BANJO_RENUT_HOOK_RAW` expand to an unused `static constexpr` pointer / `static` function, so
no `extern "C" sub_X` is defined and no generated function is replaced. Turning it ON is an `#error` until
`OBLIVION_RENUT_HOOK_ADDRESSES_ARE_OBLIVIONS` is defined (the hook table is still Katamari's: 24 + 11 + 39 + 4 + 243
`BANJO_RENUT_HOOK`s in `d3d/hooks/{device,draw,resource,shader,state}.cpp`). `guest/widescreen.cpp`'s N&B hooks are
behind `KATAMARI_BANJO_ADDRESS_HOOKS` (undefined, so not compiled), and `hooks_common.h`'s `RENUT_NOOP` macros (which
define `REX_FUNC`s directly) are not used anywhere.

### Renderer selection

katamari-nx's `banjo_renderer` (default `"native"`, which there meant reNut) is `oblivion_renderer`, allowed
`"emulated"` (default) and `"renut"`; `"renut"` falls back to the emulated GPU with a warning while
`OBLIVION_RENUT_HOOKS` is 0. The Switch-without-Vulkan branch of `LatchActive` (`BANJO_SWITCH_NO_VULKAN`, always
reNut on deko3d) was dropped: it comes back with the Switch build.

### Pruned katamari-nx app sources

Not copied: `src/me` (banjo-nx's PM4 renderer and its D3D hooks at N&B's addresses), `src/native` (nfsmw-nx's
renderer, captures), `src/cpu` (enhancement table, hot-function shards, natives, codegen self-checks), `src/profiler`
(except the headers), `banjo_pgo.*`, `katamari_keys.cpp`, `katamari_builtin_config.h.in`, `renut_app.h`,
`renut_engine/*` outside `gpu/`, `display_sync.*` and `perf/`, `config/`, `overrides.toml`, `renut_config.toml`,
`indirect_promotions.txt`, the katamari tomls, `CMakeLists-linux.cmake`, `icon/`. `app/CMakeLists.txt` lost the
post-codegen step (`tools/post_codegen.py`) and its `BANJO_GEN_*` switches, the Switch LTO / PGO block, the EDRAM
glslang shaders and the SDK sources the PM4 renderer compiled into the app.

Kept from outside `gpu/` because the renderer includes them: `renut_engine/display_sync.{h,cpp}`,
`renut_engine/perf/frame_stats.{h,cpp}`, `me/me_platform_compat.h` (SIMDe NEON on x86, Switch thread helpers),
`me/masseffect/banjo_pmull_fingerprint.h`, `profiler/profiler.h`, `profiler/switch_cpu_profiler.h` (Switch only),
`cpu/banjo_enhancements.h`. Stubbed in `src/oblivion_keys.cpp`: 15 keys of the PM4 renderer that
`banjo_renut_tuning.cpp` declares, `banjo_cpu_verify`, `banjo::EnhancementActive` (own key only), the profiler entry
points (no-ops), `renutFrameLimit` (no cap), `g_banjo_game_frames`.
