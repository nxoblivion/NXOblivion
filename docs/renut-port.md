# reNut's renderer for Oblivion (Xbox 360 -> PC -> Switch)

Branch `renut-port`. Started 2026-11-03. Licence: the port becomes GPL-3.0 (the user's decision): it reuses
masterspike52's reNut renderer (permission given to the user on Discord, credit it) as adapted for the Switch by
ChanseyIsTheBest's GPL-3.0 ports (katamari-nx, zuma-nx, feedingfrenzy-nx, renut-nx: plume deko3d back end, Direct3D
layers moved to other XDK builds). Reference clones (not committed): `out/ref/{reNut,renut-nx,katamari-nx,zuma-nx,
feedingfrenzy-nx}`.

## What reNut is

reNut does not emulate the Xenos GPU. It re-implements the game's statically linked XDK Direct3D, function by
function, on a guest `D3DDevice` kept byte-exact; draws take a snapshot of the device and a render thread draws it with
plume (Vulkan / D3D12 on PC, deko3d on Switch). No PM4 ring is written or parsed, no EDRAM is modelled. Shaders are
translated ahead of time (XenosRecomp, keyed by XXH3 of the shader container). The per-game part is `gpu/d3d/*`: the
hook table (game addresses) and the device layout (offsets), both tied to the XDK build.

## Oblivion's facts

- XDK: D3D9 / D3DX9 / XGRAPHC **2.0.2417** (default.xex static library list). Between Zuma (2.0.2135) and Feeding
  Frenzy (2.0.2571); Katamari is 2.0.5632, N&B's update 2.0.7978 (reNut's layout).
- Direct3D library: about 0x82550000-0x8256FFFF (619 functions), plus tiling helpers at 0x829B9xxx and XGraphics'
  state post-processor at 0x8295xxxx. Addresses found so far: `ports/switch/HLE_RENDERER_FEASIBILITY.md`.
- State tables: 97 render states at 0x82A5FA38, 20 sampler states at 0x82A5FEC8 (12-byte {getter, setter, default}),
  copied to the device at +0x60 / +0x1E4: the same shape as Zuma's XDK 2.0.2135 (zuma-notes: render state 57 is one N&B
  lacks, N&B's 57-95 are 58-96, N&B's 96-100 do not exist).
- Game engine: Gamebryo; predicated tiling (Begin/EndTiling), resolves, HDR, occlusion queries (EVENT_WRITE_ZPD),
  531 precompiled shaders in Data/Shaders/shaderpackage.sdp plus XGraphics' run-time shader compiler.

## Plan (milestones; each verified before the next)

1. **Census** (PC, emulated GPU): a trace of every call from game code into the D3D library (wrappers generated over
   weak `sub_` symbols), the device address, a frame's call sequence in the menus and in game.
2. **Hook map**: every reNut hook placed at Oblivion's function or marked unhooked, each with evidence
   (`tools/renut/hook_map.txt`); state tables paired by index.
3. **Device layout**: Oblivion's offsets for every field reNut reads (`gpu/d3d/d3d.h`, `d3d_native.h`), resource header
   layouts (XDK 2135-style 12-byte resource head expected), guest data (format table, caps, VdGlobalDevice slot).
4. **PC renderer boots**: renderer sources in `src/renut/`, plume Vulkan, the selector cvar (`oblivion_renderer =
   renut`), Clear + Present on screen.
5. **Shaders**: dump containers (image + shaderpackage.sdp + run-time captures), XenosRecomp, shader cache; 0 missing.
6. **PC parity**: logos, menus, in game (exterior, interior, HDR, water, Bink) the same as the emulated GPU.
7. **Switch**: deko3d back end, shader package (SPIR-V -> GLSL -> uam), devkitPro build, Eden runs.
8. **Performance** on Switch (render thread, per-draw fast path, threads), then installer integration (shaders built on
   the user's PC; GPL source published with releases).

## Log

- 2026-11-03: references cloned; PC baseline (`tools/build.bat local-release`, D3D12 emulated GPU) builds and boots.
- 2026-11-03: census (`tools/renut/d3d_lib.py`): 638 library functions, 160 called from game code; the trace build
  (`-DOBLIVION_D3D_TRACE=ON`, `src/renut/d3d_trace.cpp`): 2,400 menu frames, 180 functions seen.
- 2026-11-03: shaders (`tools/renut/shader_dump.py`): `shaderpackage.sdp` holds 531 containers, 451 distinct, all
  version 0x0E (none stored in the executable). Feeding Frenzy's `container_0e.h` decoded pixel-shader literals wrong for
  Oblivion (its one sample was a vertex shader): the 0x0E literal field is (unified register + 48) << 4, the pixel
  shaders' already 256-511 (`0x22F0` = c255 of the pixel bank); fixed in `tools/renut/hlsl/container_0e.h`.
  `tools/renut/hlsl/oblivion_hlsl.cpp` (katamari-nx's HLSL-only front end + the 0x0E conversion, `build.sh`): 451 of
  451 translated (pixel literals come out as `float4 c255 = ...(0, 1, -1, 0)`), and all 451 compile to DXIL with the
  Windows SDK's DXC (`-T ps_6_0/vs_6_0 -HV 2021`, the translator's arguments). SPIR-V needs a DXC with SPIR-V codegen
  (the SDK's lacks it).
- 2026-11-03: run-time shaders. The trace build now saves every shader container an argument points at
  (`shader_capture/`, "S" lines with the caller). Creators: CreateVertexShader `sub_82557058`, CreatePixelShader
  `sub_82556F68` (the container in r3). 120 s of menus: 473 distinct containers created = the SDP's 451 + 19 from
  Gamebryo's fragment cache `Data/Shaders/Data/Fragments/Cached/DefaultCache.psc` (stored whole; `shader_dump.py` now
  scans it: 470 from the user's files) + 3 the game compiles at boot (`sub_8211C3A8` -> D3DX compiler wrapper
  `sub_82484BE0`, from source text at `0x8206E348` / `0x8206E768`: Bink's YUV -> RGB, ps_2_0 x2 + vs_2_0:
  `ps_7a0ce24f6a7ded83`, `ps_819818509cd1e1ed`, `vs_66497d2fc8fccfac`). Those three are in no file, so the shipped
  build needs port shaders of its own for them (as Peggle 2's movie shaders). Captures made by non-creator functions
  (`sub_82555E98`, `sub_825558B8`) read freed buffers and are junk. 473 of 473 translate and compile to DXIL. Still to
  check: shaders the fragment system creates in game (cache misses).
- 2026-11-03: hook map (`tools/renut/hook_map.txt`, `field_pairs.txt`): 134 katamari hooks paired (85 high, 20 medium,
  2 low, 27 absent in 2417), 31 device functions with no reNut counterpart (BeginIndexedVertices, Set{V,P}SConstantF,
  SetFVF, shader / declaration Release, queries, two inline sampler-filter setters ...). Frame boundary: Swap
  `sub_8255F5E0` (Gamebryo's menu frames call Sync + Swap without Present).
- 2026-11-03: what Oblivion's own code touches on the device (`tools/renut/device_inline.py`: registers loaded from the
  Gamebryo renderer's device fields +652 / +3780, or passed as r3 to device functions): only the state dispatch tables
  (+0x60 / +0x1E4 called inline), as in Zuma. So reNut's device keeps katamari's internal layout and only the four
  tables move to XDK 2135/2417's offsets (+0x60 / +0x234 / +0x1E4 / +0x3B8, Zuma's code).
- 2026-11-03: state hooks (`tools/renut/gen_state_hooks.py`): 208 (Oblivion index = N&B's below 57, +1 from 57 to 95,
  its own state 57; 0-9 and the shared Wrap10-15 getter left alone). Check: under that mapping all 96 render-state and
  20 sampler-state defaults of Oblivion's tables equal N&B's. `tools/renut/remap_hooks.py` rewrites the rest of the
  table from the hook map (94 hooked, 10 katamari-unhooked ones hooked because the game calls them, 6 adapters).
- 2026-11-03: bootstrap tree `nx/` (`nx/docs/bootstrap-notes.md`): katamari-nx's SDK fork (`nx/sdk`, third-party files
  fetched by `nx/tools/fetch_thirdparty.py`) + a stripped app (`nx/app`, root manifest unchanged) with reNut's renderer
  compiled and linked, no hook registered (`OBLIVION_RENUT_HOOKS` OFF; ON is an `#error` until the table is Oblivion's),
  `oblivion_renderer = "emulated"` by default. Needed the root SDK's 3 Oblivion patches (`3abd07c`, `ec4c02d`,
  `66dc36d`): without the jump-table one `sub_82875FC0` traps 1 s in. PC build boots on the emulated D3D12 GPU: 60 s,
  no crash, 30 swaps/s from 0.9 s (1500 at 50 s), main menu theme loaded.
- 2026-11-03: in-game census (trace build, autosave loaded, frames 3600-4200): per 600 frames SetVertexShaderConstantF
  221k, SetStreamSource 195k, SetTexture 142k, VS Release 140k, DrawIndexed 70k, VB / IB Lock ~10k each, Resolve 7k;
  CreateIndexBuffer 140, CreateVertexBuffer 11; no texture made through the API alone (the game's CreateTexture and
  its D3DX loaders' SetTextureHeader headers). `tools/renut/unhooked_reach.py` lists the D3D functions the game still
  reaches unhooked that touch the device; `tools/renut/asm.py` prints a recompiled function's PowerPC.
- 2026-11-03: **resources** (zuma-nx's model). Textures, their level surfaces and volumes stay the game's XDK 2417
  objects (Common: count in the low byte, type in bits 16-18, bind count in units of 0x80000; Fence +4; fetch constant
  +16..+40; a level's texture +44, face (+24 >> 26) & 63, level (+32 >> 2) & 15); the game's own CreateTexture,
  GetSurfaceLevel, GetDesc, GetLevelCount, AddRef, Release, GetType and Destroy run on them. Renut sees a shadow header
  per texture (N&B's layout, the fetch constant copied at every use): SetTexture, Resolve / EndTiling's destination,
  the locks. Vertex and index buffers, CreateSurface's surfaces, shaders and declarations are Renut's; every function
  taking a resource dispatches on OwnsHeader / records::Lookup. 2417's setters keep what they bind alive (bind count
  up on the new, device fence at +4 and bind count down on the old, Destroy at count and bind count 0; shaders and
  declarations by AddRef / Release): done for the game's textures with Renut's fence (the game's Destroy waits for it
  through Renut's BlockOnFence) and counted on the host for Renut's objects (a release to 0 while bound destroys at
  the unbinding). 2417's index format 1 is 32-bit (N&B's INDEX32 6).
- 2026-11-03: the rest of what the game reaches: Set{Vertex,Pixel}ShaderConstantF (to 0x780 / 0x1780, groups in
  m_Pending words 0 / 1; 2417 marks words 2 / 3, Renut's register-packet and fetch masks), SetFVF (2417's own pure
  FVF -> declaration builder `sub_82553CC0` into a scratch block, one Renut declaration per FVF), Begin /
  EndIndexedVertices (the scratch, the draw queued at End as DrawIndexedVerticesUP's), thread owner set / clear /
  query, queries (host: done at the end's fence, all visible), IsBusy / BlockUntilNotBusy, buffer GetDesc, IB Unlock,
  shader / declaration Release. What `unhooked_reach.py` still lists is the D3DX microcode assembler's own context
  (displacements 9360-9780 of it, not the device) and two printf helpers. XenosRecomp takes 0x0E containers
  (feedingfrenzy-nx's branch, Oblivion's `container_0e.h`); the dump folder `nx/out/renut_shader_dump` has 473 (the
  470 from the files and the 3 Bink shaders compiled at boot); the in-game trace's 5 other "captures" came from
  `sub_82555E98` (freed buffers, junk) and crashed XenosRecomp.
- 2026-11-03: **reNut renders on PC** (`-DOBLIVION_RENUT_HOOKS=ON`, `--oblivion_renderer=renut`). Test captures:
  `--oblivion_renut_shots=<swap>|<n>s,...` + `--oblivion_renut_shot_dir` write `shot_*.bmp` while minimized
  (`renut::gpu::CaptureFrame`); `--oblivion_diag_skip_vs=<hash>,...` leaves those vertex shaders' draws out. Fixes on
  the way to the menu and in game:
  - Swap's front buffer is the game's texture (CreateTexture's, +13976): presented through its shadow.
  - Float constants: 2417's single-register constant setter is inlined all over Gamebryo (three copies each for vertex
    `sub_824BD970/DA40/DB00` and pixel `sub_824BDD50/DE28/DEF0` registers, hooked; more inside its constant appliers,
    e.g. `sub_8286D6F8`). They write the floats and mark the group in m_Pending word 2 / 3 (2417's), which N&B's layout
    uses as tag words, so the snapshot deltas kept older values: the main menu's fade quad came out opaque black (a
    quarter of the screen), the HDR composite's texRatio stayed the scene's matrix (the left two fifths smeared).
    `d3d::PendingVertexGroups / PendingPixelGroups` take words 2 / 3 with 0 / 1 wherever the dirty groups are read.
  - Gamebryo's own vertex buffers (XGSetVertexBufferHeader-style, 20 bytes: Common, Fence, BaseFlush, fetch constant
    at +12): a shadow in N&B's layout per buffer, refreshed at every SetStreamSource; Lock / Unlock / GetDesc of them
    run 2417's code. (Read with N&B's offsets the first save load faulted in ConvertVertices.)
  - XenosRecomp's 0x0E support: `nx/patches/reblue-XenosRecomp-container-0e.patch` + `container_0e.h`, applied by
    `nx/tools/fetch_deps.py`.
  Result: logos, Bink, the main menu and the Imperial Prison (shadow map, 2x MSAA HDR scene, bloom and luminance chain,
  composite, HUD) look right in the captures (not yet compared pixel by pixel with the emulated GPU, which has no
  minimized capture); no crash in 110 s runs. PC rate in the cell 14-25 fps with the render
  thread at 1.2-1.5 ms and the GPU at 1.5-2 ms a frame (the game thread is the limit; not looked at yet).
- 2026-11-03: **PC rate: 14-25 -> 60 fps** (the cap). `nx/tools/pcprof.py` (samples a running oblivion.exe's threads
  with SuspendThread / GetThreadContext, symbols from the PDB) put the game thread in a guest `Sleep(0)` most of the
  time: `sub_82789040(this, 30000)`, the game's wait for its frames in flight, spins while `this+640` is not 0. The
  count goes up once a frame (`sub_82789018`) and down in a D3DDevice_InsertCallback callback (`sub_827890A0`), which
  reNut runs at a guest thread's next D3D call once the render thread has reached it; the waiting loop makes none, so
  it waited ~4.4 s of every 5 for another thread's D3D calls. The hook runs the ready callbacks itself and waits on the
  render thread's progress (`render_thread::WaitForFenceProgress`), with the original's limit as time (10 us a try).
  Uncapped that gave 200+ fps minimized, so `renutFrameLimit` (an empty stub here) became a plain cap,
  `--oblivion_renut_frame_cap` (60; 0: none). Imperial Prison: 60.00 fps, render thread 1.2 ms and GPU 1.0 ms a frame.
- 2026-11-04: **Switch build: reNut's renderer on deko3d draws the title screen in Eden.** Built in devkitPro's
  container (`nx/tools/switch_docker.sh`, `build_switch.sh`; host shader tools `deko3d_build_tools.sh`; the game's
  shaders translated by `switch_shaders.sh` from the PC build's XenosRecomp, into `nx/app/out/renut_switch`, not
  committed). Settings compiled in (`nx/app/oblivion_switch_defaults.toml`), the player's `oblivion.toml` beside the NRO
  (`sdmc:/switch/oblivion-renut/`); the game from `game_root/` beside it or `sdmc:/oblivion/assets`. Eden runs:
  `nx/tools/run_eden.sh` in a private Eden copy (`tools/eden-renut`, git-ignored; its SD and NAND folders its own).
  - Eden v0.2.1 aborted every deko3d program (devkitPro's deko_basic too) at dkQueueCreate: its macro engine keeps a
    reference into an ankerl map across an insert into it (std::length_error on the GPU thread). Fixed on Eden's master
    after the release; `nx/tools/eden_patch_macro.py` patches the private copy's eden-cli.exe (two lookups swapped).
  - Without a loader the SDK used the current-process pseudo-handle for svcMapProcessMemory (enough in Ryujinx); Eden
    resolves that handle as Horizon does (no pseudo-handles), so no guest view was mapped (850 000 unmapped accesses
    a run). `guest_memory_switch.cpp` now gets a real handle the way hbloader does (the pseudo-handle copied to itself
    over a private session), in RexGmInit before any lock: in RexGmCommit it deadlocked on virtmemLock, which
    threadCreate takes too.
  - Debugging aids: Eden's GDB stub (`EDEN_GDB=1 run_eden.sh`), `nx/tools/eden_rsp.py` (every guest thread's pc, lr and
    stack return addresses after N seconds) and `eden_rsp_symbolize.sh`; `eden_shot.ps1` (the Eden window, unfocused).
  Result: logos and the title screen ("Press Start", the map panning) at 13-14 fps in Eden; the picture stops after
  ~2.5 minutes (FPS 0), not yet looked at.
- 2026-11-04: the title-screen stop in Eden. Oblivion suspends its audio thread (NtSuspendThread from sub_8211CDB8 /
  sub_826436E0) while that thread may hold a spinlock at DISPATCH_LEVEL (sub_8257F428), then spins on that lock in
  sub_825707D0. On the 360 a suspension is an APC and waits until the thread lowers its IRQL below DISPATCH; the
  Switch build paused the host thread at once (svcSetThreadActivity), so the two deadlocked. `XThread::Suspend` on
  Switch now looks at the target's guest PCR: at DISPATCH or above, the suspension is owed instead, and the thread
  takes it itself when it lowers its IRQL (KfLowerIrql, EndDPCImpersonation: it waits until resumed). Eden shows the
  title map still panning at 30 fps (the cap) after 220 s. `nx/tools/eden_shots.sh`: a run with a capture every N s.
- 2026-11-04: in game in Eden. A scripted run (input_script: Start, then A through the menus) reaches the new game:
  the intro loading screens, the prison cell and character creation.
  - Eden's OpenGL backend crashed (an access violation in ntdll) on the in-game shaders; its Vulkan backend
    (backend=1 in the private copy's config) runs them.
  - The scene was garbled in screen-aligned blocks and speckles while the menus were clean. Not MSAA, Zcull, depth
    compression or any of the Switch-only renderer options (all turned off: the same picture). The common factor:
    every scene pixel shader spilled to scratch memory, up to 255 registers plus 70-123 KB a warp, in 143 KB of code
    for one shader, and Eden ran the scene at 5 fps. XenosRecomp had emitted these 50 shaders as its switch(pc) state
    machine: their loops are predicated backward jumps (cjmp), not LoopStart/LoopEnd, which its structurizer refused.
    It now structures a conditional jump back as a do-while loop over the region it closes
    (`nx/patches/reblue-XenosRecomp-do-while.patch`): no shader is a state machine any more, none spills (that pixel
    shader: 3.8 KB, 46 registers), and Eden shows character creation correctly at the 30 fps cap. The PC build with
    the same shaders still renders the cell as before.
  - The queue's scratch is now an option (`oblivion_deko3d_scratch_kb`, default 128; `nx/patches/plume-scratch-option.patch`);
    deko3d_variant_cache.py runs in the devkitA64 container (the Linux uam) and names the variants uam fails on;
    run_eden.sh copies the PC-built DKSH cache to the card. nx/app/thirdparty is fetched, not committed: changes to it
    are patches in nx/patches (CRLF kept: nx/.gitattributes).
- 2026-11-04: past the first autosave in Eden. Leaving character creation autosaves; Eden closed there twice (an access
  violation in eden-cli.exe +0x95414). Its FS trace showed the last call: DeleteDirectory of
  `.../Headers/00000001/autosave.header`, whose parent folder did not exist yet. The content manager removes an old
  header with std::filesystem::remove, devkitPro's remove() tries rmdir when unlink fails, and Eden's DeleteDirectory
  dereferences the missing parent (the console returns "path not found"). The header is now removed only when it is
  a file (`nx/sdk/src/system/xam/content_manager.cpp`). Eden then writes the autosave and the run walks through the
  prison at 29-30 fps for the rest of its 560 s.
- 2026-11-04: the deko3d shader chain on Windows (for the Switch installer, which runs it on the player's PC):
  `nx/tools/deko3d_build_tools_win.bat` builds spirv-cross (clang-cl), uam and zstd (MinGW-w64 gcc: uam's Mesa code
  wants POSIX threads), all with static runtimes. The scripts find them through `nx/tools/deko3d_tools.py`
  (DEKO3D_TOOLS overrides). Checked against the container's: all 473 shaders give byte-identical GLSL and DKSH; the
  whole chain (SPIR-V -> GLSL -> passes -> package) takes 27 s, the DKSH cache of 582 variants 3 s.
  - From the game's files alone `shader_dump.py` finds 470 of the 473 shaders; the other three are the Bink movie
    shaders the game compiles at boot. Their table entries are now data of the port shaders (`PORT_TABLE` in
    `tools/renut/port_shaders/port_shaders.py`, `--table`), and `renut_switch_shader_cache.py --installer` makes the
    Switch build's shader table from XenosRecomp's output for the 470 plus those: the same table as the one made
    from all 473 containers.
- 2026-11-04: the installer builds the reNut port. `bash nx/tools/build_kit.sh` makes `nx/out/oblivion-renut-installer`:
  the same GUI and clang/lld kit as the GL port's installer (ports/switch/installer), whose kit.json now also names
  the NRO's place (`nroPath`) and a `shaders` stage. On the player's PC that stage dumps the shaders from the game
  files (C# `ShaderDump` + `Xxh3`, the same files as shader_dump.py), runs XenosRecomp and the deko3d scripts on a
  CPython bundled in the kit (`nx/tools/kit_python.py`), adds the port shaders, and writes `deko3d_glsl.bin` (the NRO
  reads it beside itself) and the DKSH cache; the shader table .cpp is compiled with the game code. The kit holds no
  game-derived bytes: the port shaders' GLSL is made from captured containers when the kit is built, the pipeline
  lists hold only hashes and states. The installer built the NRO in 1:39 (lld) from the ISO; Eden runs it from the
  installed files (`EDEN_SD_AS_IS=1 nx/tools/run_eden.sh`).
  - Windows Defender flagged the locally built XenosRecomp.exe as `Trojan:Win32/Bearfoos.B!ml` (a machine-learning
    verdict) and quarantined it. Players' PCs may do the same: the build needs it signed, or reported to Microsoft as
    a false positive, before the installer ships. Once allowed on this PC (Windows Security, Protection history) and
    relinked, it stayed: a fresh installer run (150 s, its own XenosRecomp step: 470 shaders, 0 failed) gave the same
    deko3d_glsl.bin, DKSH cache and NRO code as the run tested in Eden (the NRO's drawn icon differs by a few bytes).
- 2026-11-04: named reOblivion for players. The Homebrew Menu title is "reOblivion", and the card holds
  `sd:/switch/reoblivion/reoblivion.nro` with its files (deko3d_glsl.bin, deko3d_shaders.bin, oblivion.toml, logs,
  saves); the installer kit is `nx/out/reoblivion-installer`. reNut stays credited in the NRO's author line. A card
  that has the earlier `switch/oblivion-renut` folder can have it renamed: everything in it is found beside the NRO.
  - The FPS counter is back on the Switch: the SDK's debug overlay (game FPS and frame time), open from the start
    (`switch_fps_overlay`, default on; L+R+Up hides or shows it, L+R and the right stick move it). It had shown
    nothing with reNut's renderer: the app now gives it reNut's frame statistics (renut_engine/perf/frame_stats).

- 2026-11-10: **why the console runs at 15-20 fps: the game thread** (v0.1.19 in handheld mode, the user's report).
  Measured without the console (not connected): the PC build and the Switch build in Eden, both with the frame cap
  off (`--oblivion_renut_frame_cap=0`, `EDEN_CAP=0 nx/tools/run_eden.sh`), in the Imperial Prison (the scripted new
  game: the cell, 294 draws a frame, then character creation, up to 490).
  - PC (5950X): 230 fps = 4.3 ms a frame. `nx/tools/pcprof.py`: the game thread is busy 88 % of the time, three
    quarters of it in recompiled game code spread flat over hundreds of functions (the hottest 2.4 %); the render
    thread works 0.98 ms a frame, the GPU 0.8 ms. Eden: 26-41 fps with the render thread at 2-6 ms a frame. So a frame
    costs what the game's own code costs; reNut's render thread and the GPU wait for it. A new line in the log says so
    on the console too, every 10 s: `[oblivion] game thread, 10.0 s: 344 frames of 29.1 ms (...): 27.9 ms working,
    1.2 ms waiting for the render thread, 0.0 ms in the frame cap's wait; render thread busy 5.7 ms a frame, GPU ...`
    (renut_engine/perf/frame_stats.cpp; the waits are the game thread's own: fences, frames in flight, the cap).
  - Cause 1, the generated code. nx/'s manifest used none of the SDK fork's codegen keys (bootstrap-notes.md: "each
    needs a per-game audit"): every guest register lived in the PPCContext in memory, every function ran the
    `__savegprlr` / `__restgprlr` helpers, and each of the game's 1,879 inline Interlocked operations (Gamebryo's
    reference counts) took the runtime's global mutex twice with three full memory barriers. The emulated-GPU port's
    pch patch for the last (`ports/switch/compile-generated.sh`, "~10 % of the main thread") had not come along.
    Now on in `nx/app/oblivion_manifest.toml`, with the audit's evidence written next to each key: registers as C++
    locals (`cr/ctr/xer/reserved_as_local`, `non_volatile_as_local`, `non_argument_as_local`, `elide_lr_stores`),
    `skip_msr` (all 1,879 regions hold one lwarx / stwcx. pair and register arithmetic: the compare-and-swap is atomic
    by itself), the floating-point mode and inline-arithmetic keys, `setjmp_address` / `longjmp_address`. The audit
    tools are katamari-nx's (`nx/tools/codegen_audit`): 133 functions keep r12 / r11 in the context (exception
    funclets, stack helpers), v64-v127 stay in the context everywhere (two math functions read v123-v126 from their
    callers), four functions never return (longjmp, XamLoaderLaunchTitle's caller, two error functions): with those
    the audits list only the generator's own setjmp handling.
  - Two changes to the SDK fork's generator for it: the host jmp_buf table of setjmp is one locked table instead of a
    thread_local one (the installer's clang build has no thread-local storage), and a stwcx. is
    `__atomic_compare_exchange_n` (REX_GUEST_CAS) instead of the legacy `__sync` builtin, after which GCC puts a
    `dmb ish` on the Switch's A57 (katamari-nx's guest_cas.py as generator code).
  - Result: PC 230 -> 300 fps (the captures at 120 s and 300 s differ from the old build's in 156 of 614,400 pixels:
    torch flicker); Eden, the same eight 10-second windows of the run: 25.8 / 26.0 / 36.3 / 41.3 / 41.7 / 26.8 / 26.1 /
    35.8 fps -> 36.5 / 33.8 / 50.4 / 55.5 / 58.1 / 39.3 / 33.9 / 49.0 (+30 to +45 %; the game thread 26-28 ms of a
    29 ms frame in the slowest). The compare-and-swap change alone shows nothing in Eden (an emulated `dmb` costs it
    little); it is for the console.
  - Cause 2, the clocks. Horizon runs homebrew at CPU 1020 MHz and, in handheld mode, GPU 307.2 MHz. The emulated-GPU
    port raised them (clkrst: CPU 1785, measured on the console in v0.1.11); katamari-nx asks for the handheld GPU
    profile (banjo_gpu_boost); this build did neither. `nx/app/src/oblivion_switch_clocks.cpp` (`switch_clocks`, on):
    CPU 1785 MHz, GPU 460.8 MHz handheld / 768 docked, memory 1600 MHz, each one of the system's own operating points,
    set through clkrst and applied again every 2 s; `[clocks]` lines in the log, and rex_profile.log has the real
    clocks and the temperature every 10 s. Eden only shows that the calls are made; the effect needs the console.
  - The "60 FPS" patch (xenia-canary's game patch, `0x8255F547 = 0x01`): the byte is the immediate of `li r10,2` at
    0x8255F544 in D3D::SynchronizeToPresentationInterval (`sub_8255F470`), the "two refreshes" case of the present
    interval the game asks for (`iPresentInterval=2` in Oblivion.ini); the patch fits this executable exactly. reNut
    replaces that function (D3D_SynchronizeToPresentationInterval_hook) and never waits for the game's interval: the
    rate's limit here is `oblivion_renut_frame_cap` (60 on the PC, 30 in the Switch's oblivion.toml). So the patch
    would change nothing, and "60 fps" is that key at 60; the PC build runs so already (and 300 without a cap).
  - Round 2, guest memory accesses without `volatile`. The generated load / store macros go through volatile
    pointers, so the compiler can keep nothing of the guest's memory in a register; the upstream ports take it off
    with two post-codegen scripts (pch_no_volatile.py, measured at +15-20 % on the console by masseffect-nx, and
    loop_barriers.py, without which GCC hoists the load of a guest polling loop and the thread spins for ever). Here
    it is a key of this tree's generator, `plain_guest_accesses`, so the PC build, the devkitPro build and the
    installer's build on the player's PC all get it from the manifest: the pch's REX_GUEST_VOLATILE is empty (the
    MMIO macros keep volatile) and every loop header gets `REX_LOOP_BARRIER()`, a compiler barrier over the guest's
    4 GB window (marathon-nx's narrow form). A loop header is a label that a jump emitted at or after it reaches
    (branches, switch tables, the jump tables found late at their bctr): 27,862 of the code's 185,409 labels
    (`sdk/src/codegen/function_graph.cpp`). The Switch build takes no memory fault in play (rex_profile.log), so
    nothing depends on the instructions' shape. PC: 300 fps as before (x86 forwards the repeated loads anyway), the
    captures equal (128 and 137 of 614,400 pixels). Eden: 41.0 / 35.1 / 54.6 / 58.6 / 60.3 / 43.5 / 37.3 / 51.0 fps
    in the eight windows (60 is Eden's own limit): +35 to +60 % over v0.1.19, the game thread 23-27 ms a frame in the
    slowest.
  - Round 3, from the Switch build's own profile. The SDK's sampler reported three addresses a thread under Eden
    (it calls a thread active by its CPU time, which an emulator does not give: `sdk/src/ui/switch_perf.cpp` now also
    by its samples), then: the game thread 65 % of all busy samples, the render thread 20 %, the rest small; the game
    thread flat (the hottest game function 2.6 %), with libnx's mutexLock / mutexUnlock on top (6 %) under
    `records::Lookup` <- `OblivionIsRenutObject` <- OblivionRebind / OblivionTextureShadow / OblivionVertexBufferShadow.
    That is this port's own glue (2026-11-03, "every function taking a resource dispatches on OwnsHeader /
    records::Lookup"): every SetTexture and SetStreamSource asked "Renut's object or the game's?" up to seven times,
    each answer a mutex and a hash set plus a shared mutex (four libnx mutex operations) and a hash map, and then
    took the shadow map's mutex. Now without locks: both kinds of Renut object come from the one record heap, so an
    address outside its block is the game's, and inside it a mark per 16-byte granule says record, header or nothing
    (`guest/guest_records.cpp`); a found shadow is remembered in a slot table (`OblivionShadowMemo`, the maps never
    lose an entry). PC: 300 -> 355 fps (the game thread 3.1 -> 2.6 ms). Eden: 52.6 / 47.4 / 60 / 60 / 60 / 53.7 /
    49.4 / 59.8 fps in the eight windows, the game thread 20 ms a frame in the heaviest (27 before, 28 in v0.1.19's
    lighter version of the same window): about twice v0.1.19's rate where Eden's 60 does not cap it.
  - A fault seen on the way, not from these changes: in the Switch build the player's bare arms have orange shards
    sticking out (the idle camera at ~350 s of the scripted run, `nx/out/eden-p2plain/shot_360.png`; the shoulders in
    character creation). The PC build draws the arms right (`nx/out/pcperf5/shot_360s.png`), and v0.1.19's captures
    of 2026-11-04 show the same shards, also with every optional Switch path off (eden-r35). So: the deko3d side
    (shader chain or back end), skinned skin-shader geometry. Found on 2026-11-11, see that entry.
  - The installer (the build a console gets). Kit v0.1.20 (`nx/out/reoblivion-installer`): its reference compile
    (clang, 141 files) has no thread-local relocation, and the installer built the NRO from the ISO in 2:10 (598
    shader variants, 0 failed). In Eden (`EDEN_SD_AS_IS=1`, out/eden-p4inst) it runs the same eight windows at
    52.2 / 47.1 / 60 / 60 / 60 / 54.9 / 50.3 / 59.9 fps: clang -O2 and the devkitPro build's GCC -O3 are level here.
    49 shader variants were compiled at run time (the kit's lists do not know them; the console adds them to its
    cache). The first kit build of the night failed: Windows Defender took XenosRecomp.exe again while the script
    copied it (Trojan:Win32/Bearfoos.B!ml, the third time), after the script had already deleted the previous
    installer folder. `nx/tools/build_kit.sh` now reads that file before anything else and assembles the installer
    beside its place (`<out>.new`), moving it there only when complete (the old one's `work` folder is kept). The
    second build went through untouched. The verdict itself is still open (signing, or a report to Microsoft).
  - For the next console run: `switch_profile_stacks = true` in oblivion.toml makes the SDK's profiler sample every
    busy thread (the key was only defined in katamari-nx's app); `nx/tools/switch_hot_functions.py` names the samples
    with `app/out/switch/oblivion.map`. Oblivion.ini is the PC game's (shadows, grass, LOD fades, `iNumHavokThreads=3`,
    `SStartingCell`): a lighter profile for the Switch is possible later; not touched yet.
- 2026-11-11: **the shards on the player's arms: BGRA vertex attributes** (v0.1.21).
  - What was wrong. A D3DCOLOR vertex element (format 6, read ZYXW over the swapped word) went to the GPU as a
    B8G8R8A8 attribute: a real format on Vulkan and D3D12, on deko3d the "bgra" bit of Maxwell's attribute word
    (`plume_deko3d.cpp`, `isBgra`). Oblivion keeps two things in such elements: vertex colours, and the four bone
    indices of its skinned meshes (BLENDINDICES0, three stream layouts). Eden does not apply the bit, so the first
    and third byte arrived exchanged: red for blue in vertex colours (nothing in the prison shows it), and the first
    bone index for the third in every skinned vertex: the bare arms came out as flat orange frames. (The clothes and
    the head looked right all the same; which vertices survive the exchange was not looked into.)
  - How it was found. Everything optional off changed nothing; the renderer's self-checks were all 0; the converted
    vertex bytes were the same on PC and Switch; the bone matrices of the skinned draws were sane (a new line of the
    draw log, `[drawlog-bones]`, for SM3001 / SM3019 / SM3015: 18 bones set, each of unit scale). That left what the
    GPU does with the bytes: with those elements expanded to floats on the CPU the arms were right
    (`nx/out/eden-s5bgra/shot_360.png`).
  - The fix. `oblivion_bgra_vertex_attributes` (true on PC; false in `nx/app/oblivion_switch_defaults.toml`): the
    element is copied as before, its first and third bytes are exchanged in the chunk that is already in cache, and it
    is bound as R8G8B8A8 (`HostVertexElement::swap_rb`, `render/vertex_format.cpp`). A stream with such an element
    does not take stream_convert_cs (`FindExportedStream`); Oblivion exports no vertices anyway.
    `oblivion_diag_vertex_layouts` logs every element of every declaration.
  - Verified. PC with the switch set to false (the new path on a GPU known to be right): arms and colours as before
    (out/pcperf10 against out/pcperf5). Eden, devkitPro build with its own defaults (out/eden-s6swap): arms right, 11
    layouts "red and blue exchanged", the eight windows 54.1 / 48.7 / 60 / 60 / 60 / 54.5 / 50.7 / 60 fps (52.6 /
    47.4 / 60 / 60 / 60 / 53.7 / 49.4 / 59.8 before: no cost). Kit v0.1.21 and the installer's clang build from the
    ISO (2:09), in Eden (out/eden-s6inst): arms right, 49.9 / 44.5 / 60 / 60 / 60 / 53.8 / 50.2 / 59.4 fps (52.2 /
    47.1 / ... on 2026-11-10: the first two windows 2-3 fps lower in this run, the others equal; one run each, so
    not told apart from run-to-run variation).
  - Not known: whether the console ever showed the shards. The Tegra's own GPU has the bgra bit for exactly this, so
    probably only Eden did; the Switch build no longer depends on it either way.
