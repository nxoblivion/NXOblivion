# OpenGL backend plan for the Switch port (phase 4a)

Goal: real Xenos rendering on Switch through desktop OpenGL 4.3 core (Mesa nouveau via EGL, devkitPro portlibs),
modelled on the SDK's Vulkan backend (`src/graphics/vulkan`, `src/ui/vulkan`) using its **host render targets** path.
Everything below is based on reading the SDK sources (branch `switch`) and on the measured probe results in
`ports/switch/glprobe/results-eden.txt`. Line counts are of the Vulkan files and are only a scale for the estimates.

## 0. What the probe established (Eden 0.2.1; hardware pending)

* Context: EGL 1.4 (Mesa), **OpenGL 4.3 core is the maximum** (4.4/4.5/4.6 contexts fail with EGL_BAD_MATCH 0x3009), GLSL 4.30,
  `Mesa 20.1.0-rc3`, renderer `NV120` (nouveau). The Mesa/nouveau driver is the portlibs copy that is linked statically into the
  NRO, so extension/limit values should be identical on hardware; what can differ is the GPU chip id string and the nvdrv/kernel
  behaviour underneath (Eden emulates it, hardware is the truth).
* Present: compute, SSBO, image load/store, buffer_storage, texture_view, clip_control, copy_image, texture_barrier,
  ARB_sample_shading, texture_storage_multisample (max 8 samples), direct_state_access, vertex_attrib_binding, indirect draw,
  base_instance, provoking_vertex, depth_clamp, viewport_array, cull_distance, shader_viewport_layer_array, derivative_control,
  gpu_shader5, s3tc/rgtc/bptc/ASTC-LDR, anisotropic 16x, polygon_offset_clamp, get_program_binary (but **0 binary formats**),
  parallel_shader_compile, KHR_debug, EXT_demote_to_helper_invocation, ARB_shader_ballot/group_vote.
* **Absent**: `ARB_gl_spirv`, `ARB_spirv_extensions` (so SPIR-V cannot be handed to GL directly, SPIRV-Cross is mandatory),
  `ARB_fragment_shader_interlock` / `NV_fragment_shader_interlock` (host-render-target path is the only option),
  `ARB_shader_stencil_export`, `ARB_sparse_buffer`, `ARB_sparse_texture`, no program binary formats (no on-disk program cache; cache
  the generated GLSL/SPIR-V instead).
* Limits: max texture 16384, 3D 2048, array layers 2048, **max SSBO block 128 MiB (134217728)**, 80 SSBO bindings, 16 SSBO blocks per
  stage, UBO 64 KiB / 84 bindings, 32 texture units per stage (192 combined), 192 image units, 8 colour attachments, compute group
  1024 x 1024 x 64 with 1024 invocations, 96 KiB shared memory, SSBO offset alignment 16, UBO offset alignment 256.
* Rendering works in Eden (screenshot `glprobe/screenshot-eden.png`: clear colour + triangle, pixel readback verified).
* **Eden limitation, not a driver result**: a compute shader that writes an SSBO does not run in Eden (Eden log:
  `Render.Vulkan <Error> CreateComputePipeline: Instruction ST is not implemented`). So compute-dependent milestones (texture
  loads, resolves) cannot be validated in Eden, only on hardware. The shaders still compile and link in Mesa.
* SPIRV-Cross (vulkan-sdk-1.4.341.0, core + GLSL backend, static, `libspirv-cross-core.a` 17 MB + `libspirv-cross-glsl.a` 24 MB with
  debug info) converted **97/97** SDK precompiled blobs (compute, vertex, fragment) to GLSL 430; Mesa compiled **97/97**, and
  **91/91 compute programs linked**. Needed fix-ups, all done in `glprobe/source/main.cpp::CrossCompile` and reusable:
  1. flatten `set`/`binding` pairs into one GL binding (`set*8 + binding`);
  2. Vulkan separate image + sampler -> GL combined units: `build_dummy_sampler_for_combined_images()` +
     `build_combined_image_samplers()`, assign a unit per combined pair (the backend must bind the sampler object to the same unit);
  3. `options.emit_push_constant_as_uniform_buffer = true` plus a `Binding` decoration (probe uses UBO binding 15) turns push
     constants into a std140 UBO (the default is a loose `uniform struct`, awkward to fill);
  4. insert `#extension GL_ARB_cull_distance : require` when the shader declares `gl_CullDistance` (otherwise Mesa rejects the
     `gl_PerVertex` redeclaration).
* Eden's own GL-over-Vulkan is not involved: the app talks nvdrv through Mesa, Eden emulates the Maxwell GPU on its Vulkan host.

## 1. Architecture decisions

1. **One GL context, owned by the command-processor thread.** EGL contexts are thread-bound; the Vulkan design (command-processor
   thread renders, UI thread presents guest output through a mailbox of `VkImage`s) does not map well. On Switch there is no UI
   thread and no windowing system, so the GL backend presents from `IssueSwap` itself. The SDK already supports a headless guest-GPU
   path (`GraphicsSystem::SetupGuestGpu`, `has_presentation()==false`), which is what the phase-3 `NullGraphicsSystem` uses.
   The GL graphics system derives from the real `rex::graphics::GraphicsSystem` (keeps the ring buffer, register file, vblank
   worker, interrupt dispatch, `CommandProcessor` base) and does *not* create a `rex::ui::Presenter`.
2. **No `ui::Window` / `WindowedAppContext` / `ui::Presenter` / `ImGui`** on Switch. A small `GlSwapPresenter` owned by the
   command processor replaces the presenter (blit/draw frontbuffer to the EGL surface, `eglSwapBuffers`). Overlay/UI drawers are
   out of scope for the first releases (the immediate drawer is optional, last).
3. **Shaders are compiled from SPIR-V by SPIRV-Cross on the device**, never through glslang at runtime:
   * Xenos microcode -> SPIR-V: reuse `SpirvShaderTranslator` unchanged (needs a third `Features` constructor from GL caps; set
     `max_storage_buffer_range = 128 MiB`, `spirv_version = 1.0`, `fragment_shader_sample_interlock = false`,
     `demote_to_helper_invocation = true` (Mesa has `GL_EXT_demote_to_helper_invocation`), `sample_rate_shading = true`,
     `clip_distance/cull_distance = true`).
   * the ~97 precompiled blobs in `src/graphics/shaders/vulkan_spirv/*.h`: same SPIRV-Cross path (proven by the probe).
   * the handful of GLSL source strings embedded in the Vulkan backend (swap gamma/FXAA compute, tessellation control/vertex,
     geometry shaders in `pipeline_cache.cpp`, placeholder pixel shader): hand-port to GLSL 430 text (they are short); glslang is
     then only needed at build time for `spv::Builder` (see section 5).
   * SPIR-V -> GLSL results are cached in memory and on SD (key = hash of SPIR-V + options); GL program binaries are unavailable.
4. **Shared memory (the 512 MiB guest-physical mirror) is split into 4 x 128 MiB SSBOs.** `max SSBO block = 128 MiB`
   is exactly the SDK's `max_storage_buffer_range` default; the translator already supports this
   (`GetSharedMemoryStorageBufferCountLog2`, `xe_shared_memory[N]` array of SSBOs, with separate bindings). Open risk: GLSL 4.30
   needs a dynamically *uniform* index into an SSBO array; the translator indexes by guest address (non-uniform). Mesa/nouveau
   probably accepts it, but if not, change the 4 access sites in `spirv_translator.cpp` (lines ~4059-4185) to a 4-way
   if/else chain (small, local). **Memory-budget risk:** on Switch GL buffers come out of the application's ~3.2 GiB; the guest
   pool (1.25 GiB) + newlib heap (640 MiB) + 512 MiB shared memory + textures/render targets does not fit. Mitigation to evaluate on
   hardware first: lazily allocate the four chunks (commit only touched chunks), and/or shrink the guest pool; there is no sparse
   buffer extension, so partial residency has to be implemented by chunk.
5. **Render targets: `Path::kHostRenderTargets`** only. EDRAM is emulated by host textures plus "transfer" (ownership) draws and
   resolve compute shaders; no interlock. `IsHostDepthEncodingDifferent` etc. are answered like the Vulkan class.
   Missing `shader_stencil_export` is already handled by the SDK (code path taken when the extension is absent,
   `render_target_cache.cpp` ~4417/4846: stencil bits written by per-bit stencil-test passes).

## 2. Class map (Vulkan -> GL)

Legend: SHARE = reuse the existing backend-agnostic base class/code as is; ADAPT = copy the Vulkan class and replace API calls;
NEW = no Vulkan analogue needed or very different. Estimated new GL lines in brackets (Vulkan size for scale).

| Vulkan | GL equivalent | Notes |
|---|---|---|
| `ui::vulkan::VulkanProvider/Instance/Device` (170+633+869) | `ui::gl::GlProvider` [~400] | EGL display/surface/context from `nwindowGetDefault()` (probe code), loads entry points via glad (4.3) + `eglGetProcAddress`, exposes caps struct (limits, extension bits) used by the caches. `CreateImmediateDrawer` returns nullptr first. |
| `GraphicsProvider` base | SHARE | |
| `VulkanGraphicsSystem` (35) | `GlGraphicsSystem : GraphicsSystem` [~60] | `CreateProvider`, `CreateCommandProcessor`; selected from `main.cpp` instead of `NullGraphicsSystem`. |
| `VulkanCommandProcessor` (6827 + 844 header) | `GlCommandProcessor : CommandProcessor` [~3000-3500] | Base class does ring parsing, register writes, packets, `IssueSwap` plumbing. GL class implements `SetupContext/ShutdownContext` (MakeCurrent, create caches), `IssueDraw`, `IssueCopy`, `IssueSwap`, gamma-ramp hooks, `ExecutePacketType3_EVENT_WRITE_ZPD`, `WriteRegister*`. **Drops entirely**: `DeferredCommandBuffer` (427+588), submissions/fences/`CheckSubmissionFence...`, barrier batching (`PushBufferMemoryBarrier/SubmitBarriers/PendingBarrier`), descriptor pools and transient descriptor layout management (`LinkedTypeDescriptorSetAllocator`, `SingleLayoutDescriptorSetPool`), pipeline layouts, scratch buffer sparse bind logic. **Replaces**: descriptor sets by binding points (SSBO 0..3 shared memory, UBOs for system/float/bool-loop/fetch constants at fixed bindings, combined texture units), barriers by `glMemoryBarrier`/`glTextureBarrier` at the points where the Vulkan code pushes barriers (after memexport, after compute, between RT transfer and draw), occlusion queries by `GL_SAMPLES_PASSED` query objects, readback by persistent-mapped buffers + `glFenceSync`. |
| `VulkanPipelineCache` (3702 + 473) | `GlPipelineCache` [~1800] | Pipeline = linked GL program (VS [+GS/TCS/TES] + PS) keyed like the Vulkan one; render-state (blend, depth, stencil, cull, polygon mode, color write masks, clip control) is applied with immediate GL state calls at draw time with a small state-shadow to skip redundant calls. Geometry-shader emulation of rectangle lists/point sprites/quad lists works unchanged in GL 4.3 (geometry + tessellation exist). Shader module creation becomes `ShaderFromSpirv(GLenum)` through SPIRV-Cross. Async compile via `ARB_parallel_shader_compile` (`GL_COMPLETION_STATUS`) or a second shared EGL context in a worker thread (to be tested on hardware). |
| `VulkanShader` (72) | `GlShader` [~100] | holds SPIR-V, generated GLSL, `GLuint` shader per stage/modification. |
| `VulkanSharedMemory` (364) + base `SharedMemory` (578) | `GlSharedMemory : SharedMemory` [~300] | 4 x `glBufferStorage(128 MiB)` (`GL_DYNAMIC_STORAGE_BIT`, optionally lazily created), page-validity logic stays in base; upload via the staging buffer (below) and `glCopyBufferSubData`; `Use(usage)` becomes memory barriers only. No sparse binding. |
| `VulkanPrimitiveProcessor` (234) + base (1567) | `GlPrimitiveProcessor` [~150] | `InitializeCommon(fullDrawIndexUint32=true, triangle_fans_supported_without_conversion=false/keep, ...)`: copy the Vulkan flags (geometry-shader emulation allowed). Index upload to a GL element buffer from the upload ring; primitive restart via `GL_PRIMITIVE_RESTART_FIXED_INDEX` (index-type specific) or the base-class conversions. |
| `VulkanRenderTargetCache` (6155 + 884) + base (1388 + 689) | `GlRenderTargetCache : RenderTargetCache` [~3500] | The biggest part. Host RT images -> GL textures (`glTextureStorage2D/2DMultisample`, `ARB_texture_view` for format reinterpretation and the 2x/4x MSAA views), render passes/framebuffers -> FBO cache keyed like the Vulkan framebuffer key. Ownership-transfer shaders (SPIR-V built with `spv::Builder`) run unchanged through SPIRV-Cross (input = sampled texture, MSAA via `texelFetch`, `SampleRateShading`; stencil via the SDK's no-export path). Dump/resolve to EDRAM-buffer compute passes use the same `resolve_*`/`host_depth_store_*` blobs (97 blobs include them). Subpass dependencies become `glTextureBarrier()` (`ARB_texture_barrier` present) + `glMemoryBarrier`. Needs `glClipControl(GL_UPPER_LEFT/LOWER_LEFT, GL_ZERO_TO_ONE)`, `glDepthRange` and `GL_DEPTH_CLAMP`. |
| `VulkanTextureCache` (3261 + 394) + base (1120 + 624) | `GlTextureCache : TextureCache` [~1800] | Textures -> GL textures with immutable storage; `texture_load_*` compute blobs write a scratch SSBO (`glDispatchCompute`), then `glTexSubImage*` from the buffer bound as `GL_PIXEL_UNPACK_BUFFER` (replaces `vkCmdCopyBufferToImage`). Native DXT1/3/5, RGTC, BPTC, ASTC are available so the DXT decompression fallbacks are not needed. Swizzle via `GL_TEXTURE_SWIZZLE_RGBA` (replaces image-view component swizzle; `image_view_format_swizzle = true`). Samplers via sampler objects, one unit per (texture, sampler) pair resolved from SPIRV-Cross's combined-sampler list; anisotropy via `GL_TEXTURE_MAX_ANISOTROPY`. Scaled (resolution scale) textures: use the `_scaled` blobs, max size 16384 is enough. |
| `VulkanSubmissionTracker`, `VulkanUploadBufferPool`, `GraphicsUploadBufferPool` | `GlUploadRing` [~250] | One persistent-mapped (`ARB_buffer_storage`, `GL_MAP_PERSISTENT_BIT|COHERENT`) ring per purpose with `glFenceSync` markers; replaces submission index tracking. `CompletedSubmissionUpdated` hooks are fed by fence polling. |
| `VulkanPresenter` (2666 + header) | `GlSwapPresenter` [~600 for M1-M3; +500 for post effects] | See section 4. No swapchain/mailbox. Default framebuffer of the EGL surface (1280x720 handheld, 1920x1080 docked, re-query on operation-mode change). |
| `Presenter` base (guest output paint flow, FSR/CAS) | not used initially | CAS/FSR shader blobs (`guest_output_*_ps.h`) are in `src/ui/shaders/vulkan_spirv` and could be revived through SPIRV-Cross later; start with bilinear + gamma. |
| `VulkanImmediateDrawer` (1019) | `GlImmediateDrawer` [~400, optional] | Only for ImGui/overlays; blobs `immediate_vs/ps` are in `src/ui/shaders/vulkan_spirv`. Last. |
| `VulkanUiSamplers`, `vulkan_util`, `vulkan_mem_alloc`, `vulkan_moltenvk`, `spirv_tools_context` | not needed | `GlUtil` helpers (error check, debug labels) [~100]. |
| `ui::vulkan::VulkanDevice` as dependency of `SpirvShaderTranslator::Features` | new `Features(const GlCaps&)` ctor, and `#if` the Vulkan ctor out on Switch | Only shared file that includes `ui/vulkan/device.h`. Everything else in `src/graphics/*.cpp`, `pipeline/*`, `util/*` is already backend-neutral. |

Counted new code: about 12-14k lines of C++ (Vulkan backend + ui::vulkan is ~28k lines; roughly half disappears because there is no
explicit synchronisation, descriptor, or swapchain management).

## 3. Vulkan features without a direct GL 4.3 equivalent (substitutes)

| Vulkan feature used by the SDK | GL substitute / decision |
|---|---|
| Push constants | std140 UBO (SPIRV-Cross `emit_push_constant_as_uniform_buffer`), reserved binding, updated with `glBufferSubData` from the upload ring |
| Descriptor sets (0 shared mem + EDRAM, 1 constants, 2/3 textures) | Flatten to GL binding spaces (`set*8+binding`); fixed binding map in one header shared by backend and SPIRV-Cross remap |
| Separate image + sampler | Combined texture units + sampler objects bound to the same unit (list from `get_combined_image_samplers()`); up to 32 units per stage |
| Render passes / subpass dependencies / framebuffers | FBO cache + `glTextureBarrier`, `glMemoryBarrier`; `glInvalidateFramebuffer` for load/store ops "don't care" |
| Pipeline objects with dynamic state | immediate GL state + state shadow |
| Fragment shader interlock | not available -> host RT path only (decision above) |
| `shader_stencil_export` | SDK fallback (per-bit stencil passes) |
| Sparse buffers (shared memory) | none; four 128 MiB SSBOs, lazy creation, maybe chunk-level residency |
| `maxStorageBufferRange` limit | 128 MiB matches `Features` default; 4 SSBO array (check non-uniform indexing) |
| Image view format swizzle | `GL_TEXTURE_SWIZZLE_RGBA` per texture + sampler/shader fix-up where one texture is used with different swizzles (`ARB_texture_view` clones, present) |
| Float depth / D24 | `GL_DEPTH_COMPONENT32F`, `GL_DEPTH24_STENCIL8`, `GL_DEPTH32F_STENCIL8`; `IsHostDepthEncodingDifferent` as in Vulkan class |
| `VK_EXT_depth_clip_enable` | `GL_DEPTH_CLAMP` |
| Occlusion queries (precise) | `GL_SAMPLES_PASSED` queries, result read back via fence-polled `GL_QUERY_RESULT_AVAILABLE` |
| Memexport (vertex/pixel shader stores into shared memory) | SSBO writes in VS/FS are legal in GL 4.3 (`vertex_pipeline_stores_and_atomics`, `fragment_stores_and_atomics` = true), `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT)` afterwards |
| Pipeline cache on disk | program binaries unsupported (0 formats): persist SPIR-V and generated GLSL, rely on `ARB_parallel_shader_compile` |
| glslang at runtime for embedded GLSL | hand-port the embedded shaders to GLSL 4.30 text; keep glslang only for `spv::Builder` |
| Timeline semaphores / fences | `glFenceSync` + `glClientWaitSync` |
| Tessellation (xenos adaptive/continuous) | GL 4.3 has TCS/TES; the SDK generates a Vulkan GLSL tessellation control/vertex shader pair; same port as the geometry shaders |
| Multithreaded compile/upload | single context first; shared contexts only after hardware testing |

Open questions needing hardware: nvmap memory cost of GL buffers; whether non-uniform SSBO-array indexing is accepted; compute
paths at all (Eden cannot run SSBO-writing compute); behaviour of `eglSwapBuffers` pacing vs the 60 Hz guest vblank; shader compile time of the
translator's big GLSL on the A57 cores (hundreds of shaders per level, each potentially hundreds of ms; the plan to cache SPIRV-Cross
output and compile ahead on a worker is therefore important).

## 4. Milestones (first visible milestone = present from the real CommandProcessor)

**M0 - done.** Capability probe, SPIRV-Cross vendored/built, 97/97 SDK blobs through SPIRV-Cross->Mesa.

**M1 - first visible: clear colour from the real CommandProcessor (est. 1.5-2.5k lines).**
`GlProvider` (EGL + glad), `GlGraphicsSystem`, `GlCommandProcessor` with `SetupContext` (create context on the CP thread),
`IssueSwap` -> clear to a colour that changes per swap (so a live ring buffer is visibly proven) + `eglSwapBuffers`; `IssueDraw`
and `IssueCopy` return true without doing anything; build the shared `rexgpu-xenos` sources into a static lib for `REX_SWITCH`
(`src/graphics/CMakeLists.txt` currently `return()`s for Switch before defining the library; add an `if(REX_SWITCH)` GL variant
and the `src/graphics/gl` source list, still no change for other platforms), switch `ports/switch/oblivion/main.cpp` from
`NullGraphicsSystem` to `GlGraphicsSystem` (phase-3 owns that file). Also needed at this stage: the `Features` ctor split (to
compile `spirv_translator.cpp` without Vulkan), glslang's `SPIRV` library for Switch (only `spv::Builder`), `shader.cpp`,
`command_processor.cpp`, `register_file`, `primitive_processor.cpp` base compiled for aarch64. Verify in Eden by screenshot
(graphics only, no compute needed). Optional M1b: CPU-detile the raw frontbuffer from guest memory (32bpp, 32x32 tiles) and show it,
as a debugging view (shows what the guest wrote even before resolves work).

**M2 - shaders and draws that touch no render target (est. +3k).**
`GlSharedMemory`, `GlPrimitiveProcessor`, `GlUploadRing`, `GlShader`, `GlPipelineCache` with the SPIR-V -> SPIRV-Cross -> link
pipeline, geometry-shader primitive emulation port, constant UBOs, vertex fetch from the shared-memory SSBOs. Validation: log shader
translate/compile statistics for the real game's first frames (how many unique shaders compile and how fast), `GL_DEBUG` errors;
draw into a plain offscreen RGBA8 target to check non-empty output with a debug readback.

**M3 - textures and a real guest-sourced swap (est. +2k).**
`GlTextureCache`, `texture_load_*` compute path, swap from the guest frontbuffer texture (what `RequestSwapTexture` yields) drawn to the
screen with a bilinear shader. Needs hardware for the compute loads (Eden lacks `ST`); until then validate with host DXT upload
(native compressed formats do not need compute) plus unit checks.

**M4 - host render targets and resolves (est. +4k).**
`GlRenderTargetCache` (host RT textures, FBOs, ownership transfers), real `IssueDraw`, `IssueCopy` resolve (`resolve_*` blobs into shared
memory), then the frontbuffer actually contains the rendered game frame. This is the "game is visible" milestone; Eden can only prove
the graphics half, hardware proves resolves.

**M5 - presentation quality (est. +0.8k).** Gamma ramp (`apply_gamma_*` blobs through SPIRV-Cross), FXAA luma, aspect-correct scaling to
handheld/docked sizes, vsync pacing against the guest vblank worker.

**M6 - performance and robustness (open ended).** Persistent GLSL cache on SD, async compile, state shadowing, MSAA 2x/4x, EDRAM
accuracy options, memory-budget tuning (chunked shared memory), occlusion queries, memexport readback fast paths, optional
`GlImmediateDrawer` for overlays.

Rough effort: M1 about 2-3 days of focused work, M2 about 1 week, M3 about 1 week, M4 about 2 weeks (largest, debugging against a game),
M5 2-3 days, M6 ongoing. These are estimates for planning, not measurements.

## 5. Build-system outline (REX_SWITCH only; keep other platforms untouched)

* SPIRV-Cross: done (`thirdparty/SPIRV-Cross` submodule, `if(REX_SWITCH) add_subdirectory(SPIRV-Cross EXCLUDE_FROM_ALL)` in
  `thirdparty/CMakeLists.txt`; targets `spirv-cross-core`, `spirv-cross-glsl`, `spirv-cross-util`). With exceptions on (SPIRV-Cross
  throws `CompilerError`; the SDK is built with exceptions already).
* glslang `SPIRV` + `SPIRV-Tools`-free subset for `spv::Builder` (`spirv_builder.h` includes `SPIRV/SpvBuilder.h`); `thirdparty/glslang` is
  in the tree, need a Switch-gated target for its SPIRV sources only (no frontend, no `glslang::` HLSL).
* New dirs: `src/graphics/gl/`, `include/rex/graphics/gl/`, `src/ui/gl/`, `include/rex/ui/gl/`; new `REXGPU_XENOS_GL_SOURCES` list
  beside `REXGPU_XENOS_VULKAN_SOURCES`; on Switch build `rexgpu-xenos` as a STATIC library (no dlopen) and link
  `-lglad -lEGL -lglapi -ldrm_nouveau` (order as in the example Makefile; `drm_nouveau` last of the Mesa libs).
* The generated game and `oblivion.nro` must link the GL graphics system statically; `gpu_plugin_loader.cpp` (dlopen) is not used.

## 6. Reproducing the probe

```
ports/switch/sdk-docker.sh 'cd thirdparty/rexglue-sdk/thirdparty/SPIRV-Cross && cmake -S . -B /work/thirdparty/rexglue-sdk/out/spirv-cross-switch -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_ENABLE_GLSL=ON -DSPIRV_CROSS_ENABLE_HLSL=OFF -DSPIRV_CROSS_ENABLE_MSL=OFF -DSPIRV_CROSS_ENABLE_CPP=OFF -DSPIRV_CROSS_ENABLE_REFLECT=OFF -DSPIRV_CROSS_ENABLE_C_API=OFF -DSPIRV_CROSS_ENABLE_UTIL=ON -DSPIRV_CROSS_FORCE_PIC=ON && cmake --build /work/thirdparty/rexglue-sdk/out/spirv-cross-switch -j6'
ports/switch/sdk-docker.sh 'cd ports/switch/glprobe && make'                 # -> glprobe.nro
powershell -NoProfile -File ports/switch/glprobe/run-eden.ps1                # runs in Eden, screenshot, results-eden.txt, kills Eden
```

## 7. Status (phase 4c, branch `switch-gl` of the SDK worktree `out/sdk-gl`)

Built and verified in Eden 0.2.1 (real hardware untested). Sources: `src/graphics/gl/*`, `include/rex/graphics/gl/graphics_system.h`, harness `ports/switch/oblivion_gl/`.

**The game's real frames are now visible in Eden**: the Bethesda Softworks and Bethesda Game Studios logo videos, the intro movie, and the title screen ("The Elder Scrolls IV: Oblivion / Press Start") over the Oblivion map background.
Screenshots: `ports/switch/oblivion_gl/screenshot-eden-logo.png`, `screenshot-eden-logo2.png`, `screenshot-eden-menu-bg.png`, `screenshot-eden-menu.png` (and `screenshot-eden-m1.png`, the earlier clear-colour milestone).

Root causes found and fixed (all of them were needed; "every draw is black" had three independent causes):
1. **Viewport y flip.** `GetHostViewportInfo(origin_bottom_left=true)` flips NDC y against the viewport height, and Xenos viewports without a scale (clears, resolves, UI) are 8192 px high, so everything landed in rows 7600+ of an 8192-row viewport, outside the 600-row render target. The GL backend now keeps the Xenos row order (`origin_bottom_left=false`, `glClipControl(LOWER_LEFT, ZERO_TO_ONE)`): framebuffer row r == guest row r, which is also what the CPU resolve and the CPU front-buffer decode assume.
2. **Eden drops every draw whose vertex shader reads an SSBO** (a vertex shader with one `xe_shared_memory[0].shared_memory[i]` load and an overridden position draws nothing; UBO and TBO reads are fine). Shared memory is therefore read through four `R32UI` texture buffers (units 28..31): `spirv_to_glsl.cpp` rewrites `xe_shared_memory[k].shared_memory[e]` to `texelFetch(xe_sm_tbo_k, int(e) - xe_sm_base[k]).x`. This also works on real Mesa.
3. **Guest physical aliases were not mapped on Switch** (generated code, not GL): D3D writes the VS/PS microcode through one alias of physical memory and reads it back through another (here 0xC0000000 vs 0xA0000000). The Switch memory layer has no aliasing (`memory_switch.cpp`), reads through the unmapped C0/E0 views returned zeros, so Bink's vertex shader reached the CP with zeroed control flow and ALU words (empty shader, nothing drawn). Fix: `ports/switch/compile-generated.sh` patches `REX_PHYS_HOST_OFFSET` in the shadowed `oblivion_pch.h` to fold C0000000 onto A0000000 and E0000000 onto A0000000+0x1000 (same quirk as the Windows macro), and all 151 generated objects are rebuilt (`compile-generated.sh -c`, ~15 min on 8 cores). After this the VS hash equals the Windows one (`161091CC82B3A2E9`).
4. **Shared memory growth OOM**: chunk buffers are now windows `[lo, hi)` inside their 128 MiB chunk (4 MiB granules, grown by union with a GPU copy, per-chunk base uploaded per draw in the `xe_sm_base[4]` uniform; guest-DMA index buffer offsets are relative to the window). Previously a 64 -> 72 MiB growth failed with GL_OUT_OF_MEMORY and silently dropped all indexed UI draws.

Bisect tooling that is still in the tree (all default off): `gl_debug_ps` (1 = magenta, 2 = interpolator 0, 3/4 = strip early-z/invariant/precise, 5 = trivial main keeping the declarations, 9 = trivial shader), `gl_debug_ps_strip`, `gl_debug_vs` (1 = fullscreen triangle after the real body, 2 = trivial VS, 3..6 = trivial body with UBO/TBO probes), `gl_debug_raster` (bitmask: 1 no scissor, 2 no cull, 4 force colour write/no blend, 8 no depth/stencil, 16 viewport = RT, 32/64/128 trivial program variants), `gl_max_viewport`, `gl_log_draws_from` (logs 12 draws incl. a post-draw pixel probe), `gl_dump_shaders` (GLSL + ucode disassembly), `gl_screenshot_swaps`, `gl_dump_rt_swaps`.
`run-eden.sh` now writes every run to its own `sdmc:/oblivion/run_<tag>/` and `out/gl-<tag>/` (no deletions) and truncates `log.txt`; the harness logs at info level (the debug level made the VFS spam rotate the GL lines out).

### 7.1 Controller input and install package (phase 5)

* **Input**: `src/input/switch/switch_input_driver.cpp` (SDK branch `switch-gl`) is a libnx HID driver (`padConfigureInput(1, NpadStandard)`, `padInitializeDefault`, `padUpdate` per guest poll). On `REX_PLATFORM_SWITCH` `CreateDefaultInputSystem` registers it (plus the NOP driver) instead of SDL/MnK/XInput; Windows/Linux code paths are unchanged. It always reports one connected XInput gamepad (player 1 = handheld, Pro Controller or Joy-Con pair) with full capabilities, state with packet numbers, and keystrokes (button, trigger and stick-direction VKs with repeat) so `XamInputGetState/GetCapabilities/GetKeystroke` all work. Vibration is accepted and ignored.
* **Mapping**: Plus = Start, Minus = Back, L/R = LB/RB, ZL/ZR = triggers (255), stick clicks, d-pad, sticks passed through as int16 (libnx +y = up = XInput). Face buttons: cvar `switch_button_layout` = `xbox` (default, positional: Switch A/right = Xbox B, Switch B/bottom = Xbox A, X/top = Y, Y/left = X) or `nintendo` (by label).
* **Debug cvars (default off/empty)**: `switch_input_script = "swap:button:holdSwaps,..."` (swap = GPU swap number, buttons in Xbox names: a b x y start back lb rb lt rt up down left right lthumb rthumb lsup lsdown lsleft lsright), merged into the real pad. The driver logs every guest-visible button change (`input: guest pad buttons=...`).
* **Verified in Eden**: Start on "Press Start" opens the main menu (New / Load / Options / Credits), d-pad right moves the highlight, A opens the Options submenu (Return/Gameplay/Video/Audio/Controls/Downloads), B returns, left+A on New shows "Start new game?" (Yes/No), A on Yes starts loading the intro ("Loading..." screen with the Dragonfire quote and a progress bar). The load then stalls with the bar nearly full: after 25 min (23100 swaps) the loading screen was still shown, the main guest thread was running but no new log activity after the last shader link ~15 s after pressing Yes (not investigated; likely a missing feature in the GL backend or a kernel/memory issue). Screenshots: `screenshot-eden-mainmenu.png`, `screenshot-eden-options.png`, `screenshot-eden-newgame.png`, `screenshot-eden-loading.png`. The menu timing in swaps varies by run (input is accepted from ~1750..2500 swaps, depending on background loading), so scripts should repeat Start every ~60 swaps from 1700. Example: `run-eden.sh 900 tag 'switch_input_script = "1700:start:10,1760:start:10,1820:start:10,1950:right:10,2010:right:10,2080:a:10"'`.
* **Eden keyboard defaults** (`tools/eden-gl/user/config/qt-config.ini`, player 0): Plus = M, Minus = N, Switch A = C, B = X, X = V, Y = Z, left stick = W/A/S/D (Shift = half), d-pad = arrow keys, L = Q, R = E, ZL = R, ZR = T, stick clicks = F / G. Change in Eden: Emulation > Configure > Controls.
* **Shipping NRO**: `oblivion_gl/build.sh` now builds an NRO with NACP "Oblivion" / "ReXGlue port" / "0.1.0" and `oblivion_gl/icon.jpg` (256x256, cropped from the title screenshot). Paths: config `sdmc:/oblivion/oblivion.toml`, log `sdmc:/oblivion/log.txt` (flushed per line, info level), assets `sdmc:/oblivion/assets/`. Applet mode (not Application/SystemApplication, or < 2000 MiB total memory) shows a libnx console message ("hold R while starting a game") and exits on Plus; a missing `default.xex` shows a console message too (before EGL starts).
* **Package**: `ports/switch/package.sh` -> `out/switch-package/` (`switch/oblivion/oblivion.nro`, `oblivion/oblivion.toml`, `INSTALL.md` from `ports/switch/package/`, `copy-assets.ps1`, `oblivion-switch.zip`; no game assets). `ports/switch/copy-assets.ps1 <drive|folder> [-Source dir]` robocopies the 166 asset files to `<target>/oblivion/assets/` keeping the case of `default.xex`, `Oblivion.ini`, `data`, `Data2`. In Eden the sdmc `oblivion/assets` folder is a real dir whose `data`/`Data2` are junctions to the repo assets.
* **Not done**: NSP forwarder (needs tools/keys), rumble, audio, real hardware.

### 7.2 New game loads into the prison cell (phase 6)

**Result**: in Eden the new game now loads and renders the character-creation scene in the prison cell with the real 3D scene behind the Race menu: `screenshot-eden-ingame.png` (UI only before the link fix: `screenshot-eden-chargen-ui.png`). Reaching it takes about 45-50 min in Eden from launch (title ~8 min, menu navigation, then ~18 min loading screen, ~5 min intro movie, then the scene) and it runs at ~0.4 frames/s there.

The "stuck on the loading screen" was NOT a deadlock. Evidence: the main guest thread (t6) kept changing call stacks; a `switch_diag` memory dump of Oblivion's task queue showed the item it was working on advancing through `Effects\stoneDustCloud.NIF`, `Characters\_Male\*.kf`, `Dungeons\Chargen\*.NIF` (the prison pieces), `skeleton.NIF`...; after 17-18 min the loading screen ended and `OblivionIntro.bik` was opened (logged `[open]`). Some single items take 3-5 min in Eden (Havok skeleton/ragdoll setup, FP/VMX heavy), the progress bar sits "almost full" meanwhile, and the GPU shows `draws=0` on the fast 30 swaps/s loading frames. What was really wrong, in order of importance:
1. **Everything the guest ran was pinned to ONE core.** libnx pthreads are created with core -2 (process default core = pinned to a single core); at one priority Horizon never preempts, so the ~20 guest threads, GPU thread and workers cooperated only through yields (hand-offs between main thread and loader workers cost seconds, e.g. a 1.5 s ping-pong on one semaphore). `ThreadStartRoutine` now calls `svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, 0x7)` (SDK `threading_posix.cpp`). Load time went from "not finished after 25 min" to ~18 min. (Not proven by a clean A/B, the single-core build was never allowed to finish.)
2. **Startup flake `svcMapMemory ... 0xD401`** (seen once: the game then hangs at boot at lr 82110F00): a thread created before the guest windows were reserved in libnx virtmem (spdlog) gets its 0x21000 stack mirror placed inside a window (about every second launch; the stack region is ASLRed and the windows cover 80% of it). `real_main` now calls `SwitchInitGuestArena()` first; `memory_switch.cpp` logs any mapped block inside a window on init/commit failure.
3. **GL_OUT_OF_MEMORY in the shared memory mirror.** The cell uses geometry spread over 60..110 MiB of the last 128 MiB chunk; the windowed mirror needed a 48-52 MiB buffer and Eden/nouveau fails such allocations (48 MiB ok after freeing, 52 MiB never; 24 MiB failed while 20 MiB were resident) -> all those draws and vertex fetches failed ("PM4_DRAW_INDX Failed in backend", 1000s of log lines, only the clear colour on screen). The mirror is now a **compact store**: 1 MiB granules are packed on demand into one R32UI texture buffer (grown in 8 MiB steps) plus a 512-entry page-table texture buffer; the shader helper `xe_sm_fetch(a)` = `store[pt[a>>18] + (a & 0x3FFFF)]` (units 30/31). If growing fails the old store is dropped and all pages re-upload (`InvalidateAllPages`). Guest-DMA index buffers use `GetHostRange` (must be contiguous in the store, else the draw fails with `index_buffer_not_contiguous`).
4. **VS/PS link failures** `definitions of uniform block XeFloatConstants do not match` (vertex and pixel float-constant arrays have different sizes; first hit by the 3D scene): the pixel-stage block is renamed `XeFloatConstantsPS` in `spirv_to_glsl.cpp`.
5. **Colours were wrong in every earlier Eden screenshot (red and blue swapped; menus blue, loading screens blue, video skin blue)**. The frontbuffer fetch constant has swizzle `0xA0A` (B,G,R,1); the present path ignored it. `IssueSwap` now applies the fetch swizzle (and logs it when it changes). The screenshots in 7 / 7.1 are the pre-fix blue ones; `screenshot-eden-ingame.png`, `screenshot-eden-chargen-ui.png` are correct.

New diagnostics (default off or cheap): `gpu diag:` and `malloc:` lines in the 10 s heartbeat (swaps, packets, last PM4 opcode, WAIT_REG_MEM state), cvar `switch_diag = true` (20 ms main-thread histogram `prof t6`, dump of Oblivion's task queue / worker registers; the queue address 0x82B05240 is Oblivion specific), `gl_screenshot_from` / `gl_screenshot_from_every`. Test recipe: `run-eden.sh 6000 tag 'switch_input_script = "1700:start:10,1760:start:10,1820:start:10,1950:right:10,2010:right:10,2080:a:10,2250:b:10,2320:left:10,2380:left:10,2450:a:10,2550:a:10"
gl_screenshot_swaps = 1000
gl_screenshot_from = 21000
gl_screenshot_from_every = 10'`. The intro movie can only be skipped with `bAllowBinkSkipping=1` in Oblivion.ini (the harness copy in `tools/eden-gl/.../assets` is a hard link to the repo file, replace it by a copy first).

Known after this phase:
* **Performance**: ~0.4 swaps/s in the cell (about 200 draws/frame, ~1000 PM4 packets/frame, 192 draws still `skipped` per frame at that moment); the host shows one saturated thread; the main guest thread mostly sleep-polls (waiting for the GPU). Loading itself is CPU bound (Eden JIT). Real hardware numbers unknown.
* **Render glitches** seen in the cell: the character has no hair (bald head although "Hair: Natural"), 3D scene otherwise plausible. Not investigated (alpha-tested/blended hair or skinning).
* Still missing GL features as before (memexport, tessellation, MSAA, depth resolve, cube/3D textures); the 192 skipped draws per frame have not been analysed.

What is not done / known issues
* The title screen is reached after ~1650 swaps (about 5 minutes in Eden); nothing past "Press Start" has been tried (no input injection in the harness).
* Colours of the logo video look plausible but have not been compared pixel by pixel with the Windows build.
* Still missing: memexport, tessellation, MSAA, depth resolve to memory, cube/3D/stacked textures, `GL_KHR_parallel_shader_compile` (hangs Eden). Program links take about 1 s each in Eden and stall the CP, a few late programs are skipped for a couple of frames.
* After the title screen the guest still logs `Unmapped Read64 @ 0x28` bursts in Eden (null guest pointer in some thread); not investigated.
* Real hardware (Mesa nouveau on a Switch) is untested; Eden-specific workarounds (TBO instead of SSBO) are harmless there.
* The SDK `thirdparty/rexglue-sdk` submodule pointer was not bumped (the GL work lives on `switch-gl` of `out/sdk-gl`).
* Commands: `ports/switch/oblivion_gl/rebuild.sh` (SDK GL lib + nro; run `docker.sh <tag> 'cd out/sdk-gl && cmake --build out/build/switch-release --target rexruntime rexgpu-gl -j10'` when common graphics sources change), `ports/switch/oblivion_gl/run-eden.sh <seconds> <tag> "<toml lines>"` (private Eden copy `tools/eden-gl`), `shot2png.sh` (or `python ports/switch/oblivion_gl/ppm2png.py in.ppm out.png` on the host).

### 7.3 Audio (phase 7) - implemented, UNTESTED

The SDK's real `AudioSystem` and `XmaDecoder`/`XmaContext` (FFmpeg `xmaframes` decoder, already part of the Switch `liblibavcodec.a`) were already compiled into `librexruntime.a`; only the output driver was missing. New (SDK branch `switch-gl`, `REX_SWITCH` only): `src/audio/switch/switch_audio_system.cpp` + `include/rex/audio/switch/switch_audio_system.h` (`rex::audio::switch_hw::SwitchAudioSystem`), selected in `oblivion_gl/main.cpp` instead of `NopAudioSystem`. The driver uses libnx `audout` (48 kHz stereo s16). `SubmitFrame` copies the guest frame (6 ch planar big-endian float, 256 samples) into a queue; an output thread (core mask 7) folds it with the SDK's `conversion::sequential_6_BE_to_interleaved_2_LE` (generic ARM path, honours StereoFold/gain), submits one 256-sample audout buffer per frame (4 in flight) and releases the guest semaphore per frame consumed (silence on underrun), like the SDL callback. Cvar `switch_audio_dump = "sdmc:/..../x.wav"` records exactly what is sent to audout (header refreshed ~1/s).
Status: builds and links; an aborted Eden run only showed `audout 48000 Hz, 2 ch` initialising, frames being consumed and the dump file growing. Content (RMS, music, XMA correctness), performance impact and real hardware are NOT verified; the planned WAV/RMS/spectrogram check and with/without timing were skipped on request. The `switch_audio=false` cvar was not added.

### 7.4 v0.1.12: shader compile thread + persistent program cache (in-game freeze on the first 3D frame)

**Root cause (proven from the v0.1.11 hardware log).** Mesa 20.1.0-rc3 (nouveau, GLSL -> TGSI -> nv50_ir) needs 0.5 to 10.5 s of
CPU to compile and link ONE translated program on the Switch: `program #3 ready 1761 ms`, `#4 3461 ms`, `#5 10510 ms`, `#6 3590 ms`;
the perf line of that second shows `link=2 (10467 ms)` inside `draws=62 (10584 ms)` and `gpu thread: cpu=6784 ms`. The translation
(ucode -> SPIR-V -> GLSL) is cheap (7-90 ms, compile worker). Before v0.1.12 `glCompileShader`/`glLinkProgram` ran on the GPU (command
processor) thread inside `IssueDraw`, so:
* menu after "Press Start": 4 new programs = ~20 s without a single swap (issue B);
* first 3D frame in-game: dozens of new programs at once -> minutes without a swap, CP "executing" a DRAW_INDX (opcode 0x22) and
  advancing ~40 packets/min, the perf report (driven by swaps) silent, music running (issue A). Host malloc grew ~60 MiB meanwhile
  (Mesa compiler memory). `GL_NUM_PROGRAM_BINARY_FORMATS` is 0 on this Mesa/nouveau (no program binaries, no disk shader cache).

**Changes (SDK `switch-gl`).**
* `gl_link_thread` (default true): the compile worker gets a shared surfaceless EGL context (`GlContext::CreateSharedContext`,
  `EGL_KHR_surfaceless_context`) and does translate + `glCompileShader` + `glLinkProgram`; the program is handed over with a fence
  (state `kWorkerLinked`; the GPU thread checks the fence without waiting). Draws whose program is not ready are skipped (as before
  for translation), so frames keep coming while programs compile. The first 8 handed-over programs are validated on the GPU
  thread (`glIsProgram` + `GL_LINK_STATUS`); on failure it logs an error and falls back to linking on the GPU thread. If the context
  cannot be created / made current: same fallback (old behaviour). Compile thread: 8 MiB stack, core mask 0-2 (was pinned to the
  default core), priority `gl_compile_thread_priority` (0x2C) for programs a draw waits for, `gl_warm_thread_priority` (0x2E) for
  pre-compilation.
* Persistent program cache `gl_program_cache` (default true), file `gl_program_cache_path` = `sdmc:/oblivion/cache/gl_programs.bin`:
  every program the game creates is appended as records (guest ucode of the VS/PS in host order + hash, translation modification of
  each stage). At start (end of `SetupContext`) the file is read, the shaders are re-created and analysed, and up to
  `gl_program_warm_max` (800) programs are queued in first-use order for background compilation; the game's later `LoadShader`
  hits the pre-created shader objects and `GetProgram` finds the program ready. Pre-compilation pauses (programs stay translated,
  linked on demand) while malloc headroom < `gl_warm_min_headroom_mb` (512). Header `RXGLPC01` + format 1 + `kCacheGenTag` (bump it
  in `pipeline_cache.cpp` when the translator / GLSL post-processing changes); a mismatching file is restarted. Translated GLSL is not
  cached: the translation also fills per-shader binding info the draw path needs, and it is only ~2 % of the cost.
* Diagnostics: perf report also fires from draws/copies when no swap happened for > 1 s (`perf (NO SWAP for >1 s, frame stalled)`),
  new line 7 = per-draw wall time histogram (<1/<5/<20/<100/<1000/>=1000 ms), slowest draw, first-use draws (Mesa variant build at
  the first draw with a program) and their time, draws skipped for not-ready programs, compile-thread links and their time, and a
  "now:" snapshot. `perf::Activity` scopes record what the GPU thread is doing (draw / sync link / texture hash or upload with base +
  size + format / shared memory upload / store grow / resolve flush wait / swap ...) and for how long; any instance > 2 s logs
  `GL slow: ...`. The 10 s heartbeat prints `gpu now: ...` (GPU thread phase + duration + draw index, compile thread phase + shader
  hashes, queue sizes), `GL memory (approx)` (textures, render targets, snapshots, fixed buffers, linked programs), `guest pool`
  (committed / peak / size, per window) and the real malloc headroom.

**Memory (v0.1.11 numbers).** `initheap: malloc=0x617d0000 pool=0x60000000`: newlib malloc region 1559 MiB, guest pool 1536 MiB.
`malloc: arena 717 MiB, free 19 MiB` only meant 19 MiB free *inside* the arena; the arena grows by sbrk up to the region end, so the
real headroom was ~860 MiB (now printed). Mesa nouveau allocates every GL buffer / texture from the newlib heap (libdrm_nouveau calls
malloc + nvMapCreate), so the ~700 MiB are: fixed GL buffers (UBO ring 8 + constant ring 16 + texture ring 48 + staging 32 + store
32+ MiB = ~136 MiB), render targets / snapshots, the texture cache (never evicts), Mesa compiler state of every linked program, and (not measured)
the runtime (guest code tables, XEX image copy, spdlog etc.). The guest pool usage is now logged; whether the pool can shrink
(towards more malloc) is decided by the `guest pool: ... peak` line of the next run.

### 7.5 v0.1.13 link-fix: compile-thread programs were never handed over (black screen in v0.1.12)

**Symptom (v0.1.12 on hardware and in Eden).** Only the FPS overlay, ~30 fps. The compile thread compiled and linked
(`program #1 ... compiled+linked in 1854 ms on the compile thread`, perf `links 1`), `pending=0`, but every second
`skipped (program not ready) ~60`, `first-use draws 0`, the queue empty, and `first program linked on the compile thread is
valid` never appeared. In Eden, `GL shared memory: allocating the 8 MiB store failed ... error 501` repeated ~1300 times.

**Root cause.** The handover. A worker-linked program (`kWorkerLinked`) became usable only when
`glClientWaitSync(p.fence, 0, 0)` on the GPU thread returned signalled. Any other result, including `GL_WAIT_FAILED`, returned
"not ready" with no timeout, log or fallback. So the validation step, and with it the fallback, was never reached. The 501
(`GL_INVALID_VALUE`) is the error a `glClientWaitSync` on a sync object unknown to the calling context leaves behind; it was
then picked up by the next unrelated `glGetError` (store allocation). So the fence created on the compile thread's
"shared" context was not visible to the GPU thread's context. Either sync objects or the whole share group are not shared
on the Switch EGL; the share probe in the next run tells which.

**Fix (SDK `switch-gl`).**
* Share probe at start-up. The GPU thread creates a buffer of an odd size, a program and a fence. The compile thread checks
  them with `glIsBuffer`/`GL_BUFFER_SIZE`, `glIsProgram` and `glIsSync` (log `share probe: buffer .. program .. sync ..`).
  If no objects are shared, it does not link (`does NOT share objects`): it only translates, and linking stays on the GPU
  thread. If only syncs are not shared, programs are handed over without fences.
* Handover. `GL_WAIT_FAILED` clears the GL error, logs, and switches to fence-less handover. A fence that has not
  signalled after `gl_link_fence_timeout_ms` (500) is ignored. The link itself is finished on the CPU, because the worker
  waits on `GL_LINK_STATUS`.
* `gl_program_max_wait_ms` (2000): a draw's program that has waited longer is translated and linked on the GPU thread,
  at most one per frame (it stalls that frame). The log line is `waited N ms for the compile thread ... deadline link #k`.
* `gl_link_thread_watchdog_ms` (3000). No compile-thread program (one a draw asked for) may still be unusable on the GPU
  thread. The fallback to GPU-thread linking fires in three cases: the compile thread linked requested programs but none
  became usable; it linked nothing and is not linking; or 30 s passed. The log line is `COMPILE THREAD FALLBACK: <why>`.
* Start-up logs: `GL compile thread setup: ...` (cvars), `shared context created / NOT created`, `started`,
  `made current (renderer ...)`, `share probe`, `first job received`, `first draw waiting ...`.
* Perf line 7 adds `handed over N | deadline sync links N (ms)`.
* The shared-memory store allocation clears earlier GL errors first.
* Translation is serialised by a mutex, because both threads may now translate.

`gl_link_thread=false` is unchanged: the compile thread only translates, and `GetProgram` links synchronously on the GPU
thread (`kGlslReady` -> `StartLink`, then `kLinking` -> blocking `FinishLink`). This is the v0.1.11 behaviour. The watchdog
is inactive. The deadline link only applies to programs not yet translated.

### 7.6 v0.1.14 shader-compile: no 3D in-game for minutes (v0.1.13 on hardware)

Worked on in Eden overnight (2026-10-16/17). Eden runs the same Mesa (it is linked into the NRO), so compile behaviour
and EGL behaviour match the hardware; only the speed differs (the Switch CPU is roughly 2-4x slower than Eden's JIT).

**Root causes (proven in Eden).**
1. **The compile thread never shared anything with the GPU thread.** devkitPro's Mesa 20.1 Switch EGL driver
   (`egl_switch.c` in `libEGL.a`) ignores `eglCreateContext`'s `share_context`: its `switch_create_context` calls
   `st_api::create_context(..., shared_stctx = NULL)` (disassembly: `mov x4, #0` before `blr [st_api+40]`; the
   `share_list` argument is never even read). v0.1.13's share probe therefore failed (`share probe: buffer false
   program false sync false`) and every program was linked synchronously on the GPU thread (v0.1.11 behaviour).
2. **The in-game shaders translate to giant GLSL.** About 30 programs are needed for the first in-game frame
   (prison cell / character creation); about 15 of them had 90-300 KB of GLSL (a pixel shader with only ~60 Xenos
   instructions and 2 texture fetches was 139 KB). Mesa needed 27-163 s for ONE such program in Eden (more on the
   Switch). Size drivers: SPIRV-Cross expands SPIR-V `NMin`/`NMax`/`NClamp` (used by every Xenos min/max, the
   `0 * x = 0` multiplication rule, saturation, gamma) into `isnan()` select chains; the per-component texture
   swizzle / signedness code (if/else and a switch per component); the `0 * x = 0` rule as
   `min(|a|, |b|) == 0 ? 0 : a * b` per product. With 1 compile slot (the GPU thread) the first complete in-game
   frame took ~10 min in Eden and would take far longer on the Switch.
3. **(Eden only) Eden crashed** (`0xC0000409` in `nvoglv64.dll`, NVIDIA Vulkan driver, on Eden's GPU thread) on the
   first dispatch of the GPU-resolve compute shader, in every variant tried (SSBO or image-buffer store, with or
   without stores / source fetches). With lazy resolves the first dispatch only happens when something reads
   resolved data through memory (logo -> menu transition, in-game). The compute shader had never run on hardware
   either (hardware menus only used lazy snapshots, `materialized=0`).

**Fixes (SDK `switch-gl`).**
* `gl_egl_share_fix` (true): while the compile threads' contexts are created, the display's `st_api` pointer
  (`_EGLDisplay::DriverData + 8`) is replaced by a copy whose `create_context` passes the main context's
  `st_context_iface` (`switch context + 104`) as `shared_stctx`. The offsets come from the disassembly and are
  checked at run time (`st_api == st_gl_api_create()`, `st_api::get_current() == main stctx`), otherwise nothing is
  patched. Log: `GL: EGL share fix armed`, then `share probe: buffer true (size 1236) program true sync true`.
* `gl_compile_threads` (2): one shared context per compile thread (cores 0-2). `gl_link_small_first`: urgent
  programs are translated first, then the smallest GLSL is linked first, so most draws appear early and the giant
  programs come last (frames keep coming, the scene fills in). No GPU-thread deadline links while compile threads
  can link (`gl_deadline_link_with_workers` = false; a deadline link of a giant program froze all frames for 7 min
  in Eden). `gl_link_fence` default false (Mesa links on the CPU; a fence + flush on a second nouveau context is
  not thread safe).
* Smaller GLSL: `gl_native_min_max` (NMin/NMax/NClamp -> FMin/FMax/FClamp in the SPIR-V before SPIRV-Cross: Maxwell
  FMNMX already returns the non-NaN operand), `gl_compact_texture_fetch` (translator feature
  `compact_texture_result`: branch-free swizzle, one vector signedness pass per fetch with the gamma/bias code
  behind one branch), `gl_cheap_mul_zero_rule` (translator feature `cheap_mul_zero_rule`: NaN product -> 0; differs
  only for NaN inputs). Both translator features default off, so Vulkan/Windows are unchanged. Result: the 139 KB
  pixel shader became 90 KB, 27 s -> 13.6 s in Eden; menu/UI programs 20-30 % smaller and ~2x faster.
* `gl_gpu_resolve_compute` (false): the compute shader is never dispatched. Resolves still become lazy GPU
  snapshots (front buffer, textures that match exactly); when memory needs the data (texture lookups that do not
  match a snapshot exactly, vertex/index fetches, store uploads), the record is converted to an asynchronous
  readback of the snapshot (PBO + fence) and converted on the CPU like the old CPU resolve path, and its store pages
  are invalidated (`ConvertLazyToCpu`). The compute shader now writes the store through an image buffer instead of
  an SSBO; `gl_gpu_resolve_debug` selects bisect variants.
* `gl_snapshot_subrect` (true): Oblivion's HDR luminance chain resolves 256x152 / 64x40 / 16x16 / 8x8 rectangles
  (multiples of 8) and samples them as 256x150 / ... / 1x1 textures. Such textures now use a GPU copy of the
  top-left part of the snapshot instead of a synchronous readback (in Eden ~40 readbacks/s, ~300 ms/s).
* `gl_nvc0_code_area_mb` (8): a soak run hung ~5 min in-game in Eden right after a giant program was first used
  (Eden log: `NVDRV Ioctl1: Could not find DeviceFD=0`, then an endless stream of `Unmapped Read64 @ 0x8 / 0x18`,
  every guest/GL thread blocked). 0x8/0x18 are `nouveau_heap::next` / `start`. Mesa 20.1 nvc0 starts with a
  512 KiB shader code segment; when it is full, `nvc0_program_upload` evicts the shaders linked *before* the
  builtin library node and `nvc0_screen_resize_text_area` frees the heap root and the library node while nodes
  behind the library still point at them (devkitPro mesa `switch-20.1.0-rc3`, `nvc0_program.c:828-855`,
  `nvc0_screen.c:873`): a use-after-free, and shaders left in the old code BO. Suspected (not proven) cause of the
  hang. The code area is now grown once to 8 MiB (Mesa's own maximum) right after the main context is created, when
  only the library is in the heap, and the library is uploaded again (`nvc0_program_library_upload`). All offsets are
  checked at run time (log `GL: shader code area grown from 512 KiB to 8 MiB (rc 0, library re-uploaded true)`).
  A run with this build did not hang (see below). Mesa/libdrm messages now go to `sdmc:/oblivion/stderr.txt`.
* Diagnostics: `gl_dump_big_glsl_kb` (48; with `gl_dump_shaders`, programs above this size are dumped with their
  ucode), every link >= 3 s is logged with its GLSL size, `gl_mesa_env` (Mesa environment variables, e.g.
  `NV50_PROG_OPTIMIZE=1`).

**Eden result (default config, i.e. the hardware config).** New game -> prison cell renders: the walls and clothes
appear ~1 min after the scene starts, the full scene (head, eyes) ~2.5 min after; ~3-4 fps in Eden while compiling.
Screenshot: `ports/switch/oblivion_gl/screenshot-eden-v0114-ingame.png`. Earlier steps: `out/gl-o14ig1/s7500.png`
(v0.1.13 with gl_gpu_resolve=false: renders after ~10 min of GPU-thread links), `out/gl-o14s1a/s7200.png`
(partial scene while giant programs compile).

**Final v0.1.14 build (SDK 7c93919), Eden, default config, cold program cache.** New game: in-game scene ~01:23, last
skipped draw 01:26 (full scene ~3 min after the scene started), then stable for 19 min at ~12 fps in Eden
(malloc headroom steady at 371 MiB of Eden's smaller 1100 MiB region). Screenshot:
`ports/switch/oblivion_gl/screenshot-eden-v0114-ingame-final.png`. Hardware-ready NRO:
`out/switch-package-v0.1.14/switch/oblivion/oblivion.nro` (= `ports/switch/oblivion_gl/oblivion_gl.nro`).

Still open: the remaining giant programs (PS `9D3317FF702C7610` 203 KB, `C2E8027CA6AA0BB2` 177 KB, ~40-50 s each in
Eden, likely 2-3 min each on the Switch) - the vertex/pixel ALU translation (Xenos register array, per-component
`0 * x` handling, `precise`) is the next size target. The second Eden copy `tools/eden-gl2` still hits a guest
`[FATAL] Call to invalid or unregistered function at 0x0` at ~20 s (game data directory listing differs through the
junctions); use `tools/eden-gl` for real runs.

### 7.7 v0.1.16 perf-waits: CPU waste and the first autosave
- Host sampling profiler (`ports/switch/oblivion_gl/hostprof.cpp`, cvars `switch_hostprof_hz` (0 = off),
  `switch_hostprof_window_s`, `switch_hostprof_start_s`): pauses each registered thread, reads PC/LR with
  svcGetThreadContext3, writes `sdmc:/oblivion/hostprof.txt`; `tools/hostprof.py` symbolises it (needs
  `out/switch-gen/oblivion_gl.syms` of the same build). Works in Eden and on hardware.
- Found: `WaitMultiple` slept `min(remaining, 1 ms)` truncated to `sleep_for(0)`, so the audio worker
  (WaitAny, 500 ms timeout) busy-spun a whole core of the 3. Now event-driven (per-waiter condvar notified by
  every signal site).
- `GlCommandProcessor::WriteRegistersFromMem`: constant ranges go straight into the register file (was ~6 % of
  the GPU thread through the per-register WriteRegister path).
- Chargen (Eden, 20 fps = 3 vblanks): main guest thread ~99 % busy, ~30 % of it in the D3D fence spin
  (`sub_82569A20`/`sub_8255CE38`); GPU thread ~45 % blocked, then memmove (shared-memory staging) 12 %,
  XXH3 (texture/VB hashes) 5 %, WriteRegister 5 %. Guest and GPU thread run serialised (frame fence latency
  ~= frame time, 0 frames in flight).
- In-game Eden "crash" at ~104 s = the first autosave: libstdc++ `create_directories` on
  `sdmc:/oblivion/user/<xuid>/425307D1/...` threw EIO (game terminated) or Eden itself crashed in its
  IFileSystem CreateDirectory. Now `rex::filesystem::CreateDirectories` mkdirs each component on Switch. Eden
  still exits silently at that point (Eden bug, `user/` stays empty); hardware expected fine.
- `switch_fs_trace` logs sdmc create/delete/rename with the caller offset (not fopen-created files: a nested
  fopen inside fsdev_open clobbers the file being opened).

### 7.8 v0.1.17 shader-cores: in-game slowdown = in-game shader compiles on the game's cores
- Switch v0.1.16 overlay in the prison cell: FPS 0.6, IDLE 74 ms per 1512 ms frame (GPU thread never waits for
  commands), SHD 0. Eden (hp11): the in-game phase at ~9 fps coincides exactly with the urgent compile queue
  (10 programs, 10-42 s each in Eden); once empty, 27 fps. The compile threads ran at 0x2C like the game and GPU
  threads, i.e. took 2 of the 3 cores. Default is now 0x2E (below them).
- Texture "uploads" (TEX on the overlay) were mostly failed attempts: k_DXT5A was unsupported and the texture was
  re-hashed and retried on every draw. DXT5A -> RGTC1; failed textures wait for a content change.
- Type-0 register writes use the range path (WriteRegister was still ~5% of the GPU thread).
- Eden after these: in-game steady 30 fps (vsync) once the shaders are in the cache (hp12).
- tools/merge_gl_caches.py merges gl_programs / gl_binaries / gl_nvc0 from several machines (same header only), so
  shaders compiled in Eden can be shipped to the console.
- Eden autosave crash: Eden's CreateDirectory fails for deeper new directories; the parent tree
  user/<xuid>/425307D1/{00000001,Headers/00000001} pre-created on the host avoids it (only the leaf is created).
