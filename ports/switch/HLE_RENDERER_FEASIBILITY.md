# Feasibility: high-level emulation (HLE) of Oblivion's Xbox 360 D3D on Switch

Status: research only. Nothing was built, run or changed for this study. The date is 2026-10-16. The inputs were project `switch-port` @ f4a50d6, the pinned SDK
`thirdparty/rexglue-sdk` @ 8ff81f5, the GL backend worktree `out/sdk-gl` (branch `switch-gl`) @ 7c433ba, and `generated/default`
(the recompiled output) plus `tools/image.bin` (the loaded XEX image, base 0x82000000).

How claims are marked:
* **[evidence]**: seen directly in code, generated output or the image, with a reference.
* **[inferred]**: follows from the evidence with high confidence.
* **[guess]**: plausible but not verified. Function names such as "Resolve" or "DrawIndexedVertices" are labels inferred from
  behaviour. The binary has no symbols.

The analysis used throwaway Python scanners over `generated/default/*_recomp.*.cpp`. They parse the `// <ppc asm>` comments that codegen
emits before each statement. The method is described in each section so the results can be reproduced. The scanners themselves are not in the repo.

---

## TL;DR

1. **Hooking guest functions works in ReXGlue, with no SDK changes.**
   * Every recompiled function `sub_XXXXXXXX` is a *weak* symbol (an alias of `__imp__sub_XXXXXXXX`). A strong definition from host C++
     (`REX_HOOK` / `REX_HOOK_RAW`) replaces it for direct calls and for indirect calls through `PPCFuncMappings`.
   * The original stays callable as `__imp__sub_XXXXXXXX`.
   * Mid-function hooks (`[[midasm_hook]]`) are also supported by codegen.
2. **Oblivion's D3D is clearly identifiable.** It is the statically linked XDK (Nov 2005) `d3d9` library at about 0x82550000-0x8256FFFF:
   * 619 functions and about 32k PPC instructions;
   * 148 of them are called directly from outside the library (732 call sites from 302 functions);
   * all `Vd*` imports and every PM4 packet builder are in it.

   The draw, resolve, swap, clear, tiling, SetTexture/SetStreamSource/SetIndices and constant functions are located below by address.
3. **The usual "inline D3D" obstacle is real but contained.**
   * No code outside the library sets device dirty bits or builds PM4 packets. Exceptions: XGraphics' runtime shader-compiler state
     post-processor (0x8295xxxx) and three predicated-tiling helpers at 0x829B9xxx, which look like D3D themselves.
   * The inlined parts are `SetRenderState`/`SetSamplerState` (and the Get variants). They are inlined as indirect calls through
     function-pointer tables stored *inside the device struct*: 95 + 20 such call sites were found in game and Gamebryo code.
   * Because the targets are ordinary recompiled functions (about 230 tiny setters and getters), hooking the targets catches them all.
4. **The game uses predicated tiling.** Evidence: predicated `DRAW_INDX`/`EVENT_WRITE`, `SET_BIN_MASK/SELECT`, Gamebryo's
   "tiling scenario" strings, and BeginTiling/EndTiling-like functions called from Gamebryo's render-target-group code. It also uses
   occlusion-query-style `EVENT_WRITE_ZPD`, GPU fences (`EVENT_WRITE_SHD`, `WAIT_REG_MEM`) and a runtime HLSL/microcode compiler (XGraphics SSM).
   All 531 shipped shaders are precompiled Xenos microcode in `Data/Shaders/shaderpackage.sdp`, which makes them good candidates for ahead-of-time (AOT) translation.
5. **Recommendation: hybrid first, full HLE only if the hybrid stalls.**
   * Keep PM4 emulation, but use D3D-level hooks as *semantic side channels* for the expensive parts:
     * frontbuffer resolve + swap: present the host render target directly;
     * Resolve: keep the result on the GPU, never write back unless the CPU reads it;
     * tiling: collapse N tile passes into one, if measurements show more than one tile per frame;
     * texture create/upload: mark immutable textures so they are not re-hashed.
   * Estimate: **about 6-12 agent-days** for most of the obtainable win.
   * Full Unleashed-style HLE: **about 40-70 agent-days**, with high risk (estimate, see section 5).
   * **Measure on hardware before choosing.** Several of these wins depend on numbers nobody has yet: tiles per frame, resolves per frame,
     the cost split between texture invalidation, the shared-memory upload and the GPU work.

---

## 1. Can ReXGlue replace or hook guest functions?

Yes, by three mechanisms.

### 1a. Whole-function override: weak symbols + `REX_HOOK` (no codegen change)

* Generated definitions use `DEFINE_REX_FUNC(name)`.
  * The template is `thirdparty/rexglue-sdk/resources/templates/codegen/pch_h.inja:57-84`, and the generated copy is
    `generated/default/oblivion_pch.h:57-78`.
  * The function body is emitted as `__imp__sub_X`, and `sub_X` is a `weak` alias of it (on Apple, a weak tail-call thunk).
* **Switch / GCC.** GCC drops the weak alias when attributes come before `extern "C"`. `ports/switch/compile-generated.sh:13-21`
  already rewrites the shadowed pch into the GCC form (`extern "C" __attribute__((alias(...), weak, noinline))`), the same form the SDK
  template has for GCC upstream (`pch_h.inja` comment at line 82-83). Overrides should therefore work in the Switch build.
  **Not verified**: check with `nm` that `sub_8255F5E0` is `W` in a generated object, and that a hook object linked into the NRO wins.
  A hook placed inside a static-library member that nothing else references would not be pulled in, so hooks must live in an object the NRO links directly.
* Calls in generated code go through the weak name, e.g. `sub_8255FBC0(ctx, base);` in `generated/default/oblivion_recomp.140.cpp`.
  Indirect calls go through `PPCFuncMappings[]`, which also stores the weak name: `generated/default/oblivion_init.cpp:28` and e.g.
  line 10906 `{ 0x8255F5E0, sub_8255F5E0 }`. **A strong `sub_X` therefore replaces both direct and indirect (`bctrl`) calls.** [evidence]
* Authoring macros are in `thirdparty/rexglue-sdk/include/rex/hook.h`:
  * `REX_HOOK(sub, fn)` (lines 37-63) uses `HostToGuestFunction` arg/return marshaling. On Switch it adds an optional import trace.
  * `REX_HOOK_RAW(name)` (line 66) gives raw `PPCContext& ctx, uint8_t* base`.
  * `REX_STUB*` (lines 69-86).
  * `REX_IMPORT(symbol, callable, sig)` (lines 251-255) is a typed call into any recompiled function. Use it on `__imp__sub_X` to call the original from inside a hook.
  * `rex::CallFrame` (lines 101-118) is for side calls.
* An existing user of the same mechanism is `REX_HOOK(rexcrt_RtlAllocateHeap, ...)` in
  `thirdparty/rexglue-sdk/src/kernel/crt/heap.cpp:361-364`, together with the manifest `[rexcrt]` table
  (`src/codegen/config.cpp:157-174`, `codegen_writer.cpp:195-245`, which strips the guest originals).
* Naming: `[entrypoint.functions] 0xADDR = { name = "..." }` is parsed (`src/codegen/config.cpp:177-221`, `fcfg.name`). This could give D3D
  functions readable names in the generated code. Not tested whether the name replaces `sub_X` in the emitted symbol.
  **Gotcha** (from the project memory): the table must be `[entrypoint.functions]`; a top-level `[functions]` is silently ignored.

### 1b. Mid-function hooks: `[[midasm_hook]]` (needs re-codegen)

* Parsed in `src/codegen/config.cpp:310-370`. Fields: `address`, `name`, `registers` (`r3`, `f1`, `v0`, `cr6`, `ctr`, `xer`, `fpscr`,
  `reserved`), `return`, `return_on_true/false`, `jump_address`, `jump_address_on_true/false`, `after_instruction`.
* Emission is in `src/codegen/builders/context.cpp:343-420`: it emits a call to `name(regs...)` and optionally `return;` or `goto loc_X;`.
  The `extern` declarations are emitted in `src/codegen/function_graph.cpp:412-430`, and labels for jump targets at line 577.
* In this project's manifest the key must be `[[entrypoint.midasm_hook]]`, because `LoadFromTable` receives the `[entrypoint]` table
  (`src/codegen/manifest.cpp:59-68,110-118`) [inferred].
* Cost: re-run codegen, then a full rebuild of the generated objects (`compile-generated.sh -c`, about 15 min on 8 cores per GL_BACKEND_PLAN 7).
* `oblivion_manifest.toml` currently uses only `[entrypoint.functions]` (vtable and gap entries). No hooks of any kind exist yet in
  `src/` or `ports/` (grep for `REX_HOOK|midasm` found none).

### 1c. Kernel import overrides

`__imp__Vd*` and similar are SDK kernel exports (`REX_EXPORT`, `hook.h:89-99`). `VdSwap` is already a host function, which is how the PM4
emulator gets the frame boundary today.

**Conclusion for Q1:** everything an Unleashed-style approach needs is present: function replacement, calling the original, and mid-asm patches.
UnleashedRecomp's `GUEST_FUNCTION_HOOK` maps 1:1 onto `REX_HOOK`/`REX_HOOK_RAW` [inferred; UnleashedRecomp details are from public
knowledge, not checked here].

---

## 2. Locating Oblivion's D3D functions

### 2a. What the binary contains

From strings in `tools/image.bin`:
* **No symbols.** The XEX has no export names for D3D.
* Strings identify the stack:
  * Gamebryo: `NiXenon Gamebryo Version 2.0.1.1`, `NiXenonRenderer`, `NiXenonRenderTargetGroupData`, ...
  * XGraphics: `d:\xenon\nov05\core\private\xtl\graphics\xgraphics\ucode\...` (runtime microcode compiler / SSM state compiler, **Nov 2005 XDK**)
  * D3DX: `D3DX: (WARN) ...`, `#File created by Microsoft (R) D3DX library`.

  [evidence]
* The D3D library has no strings of its own (release build), so it has to be found structurally.

### 2b. Method A: cross-reference the `Vd*` imports [evidence]

The functions that call the `__imp__Vd*` thunks in generated code all sit in 0x8255D7A8-0x8256D930:

| guest function | imports | role (label) |
|---|---|---|
| `sub_8255D7A8` | VdInitializeRingBuffer, VdEnableRingBufferRPtrWriteBack, VdSetSystemCommandBufferGpuIdentifierAddress, MmGetPhysicalAddress; emits `ME_INIT` | ring buffer init |
| `sub_8255F5E0` | **VdSwap**, VdGetSystemCommandBuffer, VdPersistDisplay | Swap (internal) |
| `sub_8256A108` | VdInitializeEDRAM, VdGetGraphicsAsicID, VdIsHSIOTrainingSucceeded, ExGetXConfigSetting | CreateDevice / device init [guess] |
| `sub_82569B68` / `sub_82569B40` | VdInitializeEngines / VdShutdownEngines, VdSetGraphicsInterruptCallback | engine init/shutdown |
| `sub_8256A320` | VdShutdownEngines, VdSetGraphicsInterruptCallback | device destroy [guess] |
| `sub_8256A028`, `sub_82569D80`, `sub_8256D3B0`, `sub_8256BD60` | VdSetDisplayMode, VdQueryVideoMode, VdQueryVideoFlags | display mode |
| `sub_8256B740`, `sub_8256B8A8` | VdGetCurrentDisplayGamma | gamma |
| `sub_8256BC78` | VdRetrainEDRAM(Worker) | EDRAM retrain |
| `sub_8256D930` | VdInitializeScalerCommandBuffer | scaler |

From about 0x82570000 the neighbours use XAudio/XMA imports (`sub_825707D0`, `sub_82571EB0`, `sub_82577160`), which are other XDK libraries.
**D3D ≈ 0x82550000-0x8256FFFF.** The lower bound is not pinned exactly; 0x82550000-0x82554FFF may contain D3DX/XGraphics helpers.
* In that range there are 619 functions and 32,438 instructions.
* 148 of them have direct callers outside the range: 732 call sites from 302 distinct functions.
* The callers sit mainly in 0x8211xxxx (game), 0x8247-0x824Dxxxx, 0x8278-0x827Exxxx (Gamebryo NiXenonRenderer, 238 sites into 75 D3D
  functions from 0x8275-0x8283xxxx), and 0x8282/0x828E/0x8290xxxx.

### 2c. Method B: scan for PM4 type-3 header constants [evidence]

* Method: find `lis rX,hi` + `ori/addi rX,rX,lo` pairs whose value has the form `0b11 << 30 | (n-1) << 16 | opcode << 8 | p`, where p in {0,1} is the
  **predicate bit**.
* Every hit is inside the D3D range, except `sub_8295BE70/8295C048/8295C228` (`SET_STATE`, `IM_LOAD`, `IM_LOAD_IMMEDIATE`). Those three are
  XGraphics' `statepostprocessor.cpp` (assert strings at 0x8205E330) and are reached only through a function table at 0x820EB848. The
  other exception is `sub_829B94D8`/`sub_829B9610` (tiling).
* No PM4 header is built by game or Gamebryo code.

| function | PM4 opcodes built (p = predicated variant seen) | callers | label |
|---|---|---|---|
| `sub_8255E490` | DRAW_INDX (p), EVENT_WRITE (p), SET_BIN_MASK, EVENT_WRITE_EXT | Gamebryo `sub_8278DD40`, `sub_8278E2C8`, `sub_8278EFF8`, `sub_8278F310`, `sub_82792388` x2, `sub_827929F0` | **DrawIndexedVertices** [guess] (args: dev, prim type, base, start, count from table 0x8206D678) |
| `sub_8255E250` | same | `sub_8278EFF8`, `sub_82792388`, `sub_827929F0` | DrawVertices [guess] |
| `sub_8255DEF8` | same | `sub_8278DD40` (stride 24, then `sub_8255E230`) | Begin/Draw-UP style, ring-embedded vertices [guess] |
| `sub_8255DBD0` | same | `sub_8255DE70` | another draw variant |
| `sub_825693B0` | (calls `sub_82568DE0`: IM_LOAD, SET_CONSTANT (p), INVALIDATE_STATE, EVENT_WRITE) | all four draws | **pre-draw state flush** (dirty state → PM4) [inferred] |
| `sub_825604D0` (883 insns) | IM_LOAD_IMMEDIATE, SET_CONSTANT, DRAW_INDX_2 (p), WAIT_REG_MEM, EVENT_WRITE, SET_BIN_MASK | `sub_8255FBC0`, Gamebryo `sub_8278C138`, `sub_829B9A48` x2 | **Resolve** [guess, strong] |
| `sub_8255FBC0` | calls Resolve, `sub_8255F5E0` (VdSwap), `sub_8255F470` | game `sub_8211DF78` (frame present) | Present/Swap wrapper: resolve to front buffer + swap [inferred] |
| `sub_8255B7A8` / `sub_8255BED8` | IM_LOAD_IMMEDIATE, DRAW_INDX_2 | `sub_8278C4D0`, `sub_8211D598`, `sub_8211DF78` | **Clear** (internal / API) [guess] |
| `sub_8255B388` | IM_LOAD_IMMEDIATE, DRAW_INDX_2 | `sub_82555DE0` | internal quad draw (init/clear) |
| `sub_829B9610` | (calls Clear, SET_BIN_MASK) | Gamebryo `sub_82789870` (virtual, no direct callers) | **BeginTiling** [guess] |
| `sub_829B9A48` | (calls Resolve x2, SET_BIN_SELECT via `sub_82563F30`) | Gamebryo `sub_8278C138` (virtual) | **EndTiling** (resolve per tile) [guess] |
| `sub_829B94D8` | SET_BIN_MASK; sets device flag byte +10432 | `sub_82789870` x6, `sub_8278C138` x2, `sub_829B9A48` x3 | SetPredication/tile select helper [guess] |
| `sub_82565230` → `sub_82564930` | EVENT_WRITE, EVENT_WRITE_EXT, **EVENT_WRITE_ZPD** | Gamebryo `sub_82780900` x2, `sub_82782B10` x4 | occlusion query Begin/End [guess] |
| `sub_8255D488` | (kick) | 45 callers inside D3D | **KickOff / segment flush** [inferred] |
| `sub_8255CCF0` → `sub_8255C8A0` | INDIRECT_BUFFER | KickOff path, `sub_8256E290` (KeUnlockL2, async) | ring → IB submission |
| `sub_8255CF20`, `sub_8255EC78`, `sub_8255F470`, `sub_82568A98` | WAIT_REG_MEM, INTERRUPT | | fences / BlockUntilIdle [guess] |
| `sub_8255CC18`, `sub_8256B258`, `sub_8256EB30` | EVENT_WRITE_SHD | | fence/timestamp writes (CPU-visible memory) |
| `sub_82565F50` | WAIT_FOR_IDLE, REG_TO_MEM | | GPU register readback (queries) |
| `sub_8256B9F0` / `sub_8256BAD0` | COND_WRITE | | gamma/display |

### 2d. Method C: API boundary and argument shapes [evidence: disassembly via `tools/xref.py asm`]

| function | ext. call sites | shape | label |
|---|---|---|---|
| `sub_82563C20` | 7 (Gamebryo) | (dev, stream, vb, offset, stride); refcounts the VB, stores at dev+12556+8*stream, sets pending bit 0x400 | **SetStreamSource** |
| `sub_82563CC0` | 5 | (dev, ib) → dev+12532, refcount | **SetIndices** |
| `sub_8255B230` | 14 | (dev, stage, tex); copies the fetch constant into dev shadow at (stage+48)*24 | **SetTexture** |
| `sub_82557130` | 10 | (dev, decl) → dev+11408, pending 0x80/0x400 | **SetVertexDeclaration** |
| `sub_82555368`, `sub_825554B0`, `sub_82555908` | 10/10/9 | VMX copy of N float4 into dev+(start+120)*16 | **Set{Vertex,Pixel}ShaderConstantF** variants [guess which is which] |
| `sub_825556F8` etc. | 5 | packs bools into dev+(start+2552)*4, pending bit 63 | **SetShaderConstantB/I** [guess] |
| `sub_8255AD20` | 11 | allocs a 40-byte object (`sub_82111868`), calls header init `sub_8255A820` | **CreateTexture**-family [guess] |
| `sub_8255EF80` | 48 | D3D resource Release (common refcount byte) | Release |
| `sub_8255E730` / `sub_8255E748` | 17 / 13 | device AddRef / Release (Release → `sub_8256A320`) | |
| `sub_8255E890` / `sub_8255E7F8` | 53 / 53 | critical section + owner thread id at dev+10376 | Acquire/ReleaseThreadOwnership [guess] |

Device struct layout facts (all [evidence] from the code above):
* the ring write pointer is at dev+0;
* 64-bit pending/dirty masks are at dev+32 and dev+48 (accessed as `addi rX,dev,16; ld/std 16/32(rX)`);
* the state flags byte is at dev+10432;
* render-state and sampler-state call tables are at dev+96 and dev+484 (see section 3c);
* the device size is at least 14 KB (fields up to dev+14048).

### 2e. How to name the rest (reproducible plan)

1. Build caller/callee graphs from `generated/default` (same regexes as `tools/xref.py`).
2. Classify by PM4 opcodes emitted, kernel imports used, dirty bits set, and object offsets touched.
3. Cross-check against XDK D3D semantics. Use Xenia's knowledge of the packet streams; the Xenia/SDK command processor already decodes everything these functions emit.
4. Validate dynamically. Hook a candidate with `REX_HOOK_RAW` that logs `r3..r10`, calls `__imp__sub_X`, and compares with the PM4 the GL backend sees for the same draw.

---

## 3. What D3D surface Oblivion uses, and what makes HLE hard

### 3a. Inventory

* **Device, ring and fences.**
  * Own ring buffer plus indirect buffers (IBs); KickOff (`sub_8255D488`).
  * GPU fences via `EVENT_WRITE_SHD` writing to CPU-visible memory, and `WAIT_REG_MEM`.
  * An async submission thread (`ExCreateThread` in `sub_8256EB30`, `KeUnlockL2` in `sub_8256E290`).
  * The CPU side polls fence and timestamp memory. An HLE renderer must keep writing those values, or hook every wait.
* **Draws.** Four draw entry points plus the ring-embedded (UP) path `sub_8255DEF8` + `sub_8255E230`. Primitive types are converted through the table at 0x8206D678.
* **State.**
  * Render and sampler states go through the device call tables (3c).
  * Vertex declarations, stream sources, indices and textures go through functions.
  * Float, bool and int shader constants go through functions into the device shadow.
  * The shadow is flushed by `sub_825693B0`/`sub_82568DE0` before each draw.
* **Shaders.**
  * 531 precompiled Xenos shaders in `assets/data/Shaders/shaderpackage.sdp` (BE header: version 100, 531 entries, `name[256] + size + blob`; blob magics 0x102A0E00 x241 and 0x102A0E01 x290, i.e. XDK compiled vertex/pixel shader containers [inferred]). Names such as `SLS1000.vso`, `HDR%03i.pso`, `GRASS1%03i.vso`.
  * Plus Gamebryo NSF/HLSL in `Data/Shaders/Data/*.NSF`, `Data/Xenon/*.vsh|psh|hlsl` and a runtime compiler: `bCompileShaders:General`, XGraphics SSM/compiler sources linked in, "Microcode validation failed...".
  * Whether the runtime compiler is actually used at run time is **unknown** [guess: probably only for a few Gamebryo default shaders and the Bink/movie path].
  * D3D's own clear/resolve shaders are embedded (`IM_LOAD_IMMEDIATE`).
* **Render targets, EDRAM, resolve.**
  * Gamebryo `NiXenonRenderTargetGroupData` creates EDRAM render targets ("Failed to generate a tiling scenario for render target group").
  * Resolves go through `sub_825604D0`: per tile in EndTiling, explicit from Gamebryo `sub_8278C138`, and the frontbuffer resolve in the present path.
* **Predicated tiling: yes [evidence].**
  * Predicated `DRAW_INDX`/`EVENT_WRITE`/`SET_CONSTANT` headers (low bit 1: `ori ...,8705` = 0x2201, `17921` = 0x4601, `11521` = 0x2D01), plus SET_BIN_MASK/SELECT.
  * Begin/EndTiling-like functions are called from Gamebryo virtuals.
  * The number of tiles per frame is **unknown**. It depends on resolution, MSAA and formats; measure by counting distinct SET_BIN_SELECT values per frame.
* **Occlusion queries**: EVENT_WRITE_ZPD from Gamebryo `sub_82780900`/`sub_82782B10` [guess: occlusion culling or sun visibility].
* **Memexport**: cannot be decided from CPU code. It needs a scan of the shader microcode for export allocs. The GL backend has no
  memexport, and nothing visibly broke for it so far [guess: not used].
* **Precompiled command buffers (BeginCommandBuffer/RunCommandBuffer)**: no evidence of them. INDIRECT_BUFFER is only emitted by
  the KickOff path. [guess: not used by Gamebryo 2.0.1]
* **Textures and buffers**: created through D3D, but data is written by the CPU into physical memory (XDK Lock is cheap or inline
  [guess]). Dynamic vertex data also goes into the ring directly (UP path).

### 3b. The "inline D3D writes the device struct / ring" obstacle, assessed

The scans covered the whole recompiled program:
* 64-bit read-modify-write of a field with `or/ori/oris`: 133 functions do this inside D3D. Outside D3D, only 8 do, all at offset 0 in
  0x828E-0x8292xxxx (other libraries, not the device).
* 32-bit RMW on offsets 16-60: none in the Gamebryo range.
* Loads and stores of device-only offsets (10376, 10384, 10396, 10432-10434, 11064, 12532, 13536, 13572, 13652): outside 0x82550000-0x82578000,
  only `sub_829B94D8/9610/9A48` (tiling helpers that are D3D themselves).
* PM4 header construction: only D3D + XGraphics SSM.

**Conclusion [inferred]:**
* Game and Gamebryo code do **not** write PM4 or device dirty state inline, except through the call tables below.
* The well-known XDK inlines (Set* writing device fields, `D3DDevice_SetRenderState` macro) are present here in their *call-table* form.
  That is the friendliest form for HLE.
* Caveat: inline *reads* of device fields by game code were not scanned exhaustively (for example the inline `GetRenderState` via the getter table, or reading
  `dev->m_pRing`). An HLE that bypasses the real D3D must keep those fields plausible, or hook the getters too.

### 3c. The render/sampler-state call tables [evidence]

* `sub_82569C30` (device state init, called from `sub_8256A108`) iterates two default tables in `.data`. They are 12-byte entries
  `{default, set_fn, get_fn}`:
  * render states: 97 entries at **0x82A5FA38**;
  * sampler states: 20 entries at **0x82A5FEC8**.
* For each entry it copies `set_fn` to `dev + 96 + i*4` and `get_fn` to `dev + 564 + i*4` (sampler: `dev+484`, `dev+952`), then calls
  the setter with the default.
* In game code, `D3DDevice_SetRenderState(dev, D3DRS_x, v)` is therefore `lwz r11, (96 + D3DRS_x)(dev); mtctr; bctrl` with r3 = dev.
  D3DRS_x is the XDK byte-offset enum, e.g. ZENABLE = 40.
* Scan result for `lwz rA,N(rB)` → `mtctr rA` → `bctrl` with r3 = rB:
  * **95 render-state and 20 sampler-state call sites outside D3D**, in game 0x8211-0x824Dxxxx, Gamebryo 0x827xxxxx, 0x82A4xxxx, and elsewhere;
  * e.g. `sub_8211C908` (RS 40/48/60/72/76/308, SS 0/4/16/20), `sub_827E7BB0` (12 states), `sub_827E6F80`, `sub_827A95F8` (sampler).
* The setter and getter functions are about 230 tiny recompiled functions in 0x82561520-0x825636xx. None has a direct caller. Their only
  references are the default tables (0x82A5FABC-0x82A5FFB0).
* Since `bctrl` dispatch goes through `PPCFuncMappings` to the weak symbol, **overriding those setters intercepts every inline SetRenderState and
  SetSamplerState**. An alternative is to hook nothing and read the resulting register shadow at draw time (3d).

### 3d. What still makes full HLE hard (Oblivion-specific)

1. **Predicated tiling semantics.** Gamebryo lays out render-target groups for 10 MiB of EDRAM, draws the scene once per tile (predicated), and resolves per tile.
   * A native renderer must collapse this to one pass. Otherwise it redraws N times.
   * It must also know which resolves form one logical surface.
   * Feasible (hook Begin/EndTiling), but this is the main semantic gap.
2. **EDRAM aliasing tricks.** Render targets created at overlapping EDRAM offsets, then re-interpreted (format reinterpretation), and resolves with
   format conversion, clear-on-resolve, and exponent bias. These must be reproduced by hand at the D3D level.
3. **CPU-written resource memory.**
   * Textures and vertex buffers are physical memory that the CPU fills directly.
   * With no reliable Unlock or update notification, an HLE renderer still needs invalidation: hashing or page watching. Switch has no
     page protection in the current memory layer ("linear windows", see the switch-port memory notes).
   * **So HLE does not remove texture/VB change detection by itself.** It only allows smarter policies, such as treating textures created
     from DDS loads as immutable.
4. **Fences and timestamps** written by EVENT_WRITE_SHD and read by CPU code, including the async submission thread and the ring read-pointer write-back.
   An HLE must keep D3D's fence protocol alive, or hook every wait (`sub_8255F470`, `sub_8255CF20`, `sub_8255EC78`, ...).
5. **Runtime-compiled shaders and Bink.**
   * Not every shader is in the `.sdp` file, so a runtime translation fallback is still required.
   * The SDK translator already exists. The AOT step is an optimisation, not a requirement.
6. **Unknown device-field consumers.** Game code may read device fields: getters, the ring pointer, the frame counter.
   * A "bypass D3D" HLE has to fake them.
   * A "shadow" HLE avoids this by letting real D3D run and only replacing submission.
7. **No symbols.**
   * Each of the 148 API functions and about 230 setters must be identified by hand and checked dynamically.
   * The Nov 2005 XDK is early: struct layouts and the render-state enum differ from the later XDK headers people remember [guess].

---

## 4. Hybrid alternative: PM4 emulation + selective D3D-level hooks

### Where the time goes (from the existing notes)

* **CPU-side resolves and CPU frontbuffer detile** were the original hotspots.
* As of `out/sdk-gl` 7c433ba (project v0.1.9), resolves are already a compute shader into the shared-memory store, with lazy write-back
  (`gl_gpu_resolve`). The frontbuffer is detiled by a fragment shader (`gl_gpu_front`); see `src/graphics/gl/command_processor.cpp:42-48`.
* So part of the "resolve" win is already taken at the PM4 level.
* What remains is the texture-cache work (hash and untile of resolved and CPU textures), the shared-memory upload, the snapshot lookups, and the possible tile redraws.
* **No hardware perf numbers exist in the repo** (no perf.txt found). Eden numbers (0.4 fps in the cell) are dominated by the JIT and are not representative.

### Hooks that carry semantic information cheaply

All of these are `REX_HOOK_RAW` overrides that call `__imp__sub_X`, so the PM4 stream stays intact. They pass a side-channel message to the GL command processor (for example a tagged queue entry consumed at the matching PM4 position, or a marker register write).

| hook | gives the backend | expected win | effort |
|---|---|---|---|
| Present wrapper `sub_8255FBC0` (game `sub_8211DF78` → Resolve + Swap) | "the next resolve targets the front buffer" | Present the host render target directly: no resolve into the store, no front detile, no write-back. One less full-screen resolve per frame. | 1-2 d |
| Resolve `sub_825604D0` | source surface, dest texture object/address, rect, format, clear flags | Mark the dest as "GPU-only until the CPU touches it". Skip write-back and re-hash; bind the host texture directly when sampled. | 2-4 d |
| Begin/EndTiling `sub_829B9610` / `sub_829B9A48` (+ `sub_829B94D8`) | tile count and rects | If N > 1: execute only the first tile pass with a full-size host render target and drop the other N-1 replays (or force a 1-tile scenario in Gamebryo). Up to N-fold fewer draws in the main pass. Needs a host-RT path not limited by the 10 MiB EDRAM address space. | 3-6 d (only if measured N > 1) |
| CreateTexture family `sub_8255AD20` + Gamebryo source-texture upload (`NiXenonSourceTextureData`, to locate) | "static texture, uploaded once" | Untile once, never re-hash. | 2-3 d |
| Draw entry points (4) | one counter per draw | Validation and profiling only. | 0.5 d |

Notes:
* "Captures most of the win cheaply": **probably yes for presentation and resolves, unknown for tiling and textures** until measured.
* The hybrid keeps the proven PM4 path for everything Oblivion does that we have not catalogued (fences, queries, Bink, runtime shaders).
* Risk is low because every hook can fall back to `__imp__` and be disabled with a cvar.
* An even cheaper variant of the tile collapse is possible at the PM4 level alone. Detect the pattern "same IB replayed with different
  SET_BIN_SELECT" and execute only one predicated pass. The D3D hooks make that detection exact instead of heuristic.

---

## 5. Recommendation, estimates, risks, phased plan

### Options compared (rough agent-day estimates, not measurements)

| option | effort | risk | expected gain |
|---|---|---|---|
| A. Hybrid hooks (above) on the existing GL backend | **6-12 agent-days** | low; additive, cvar-gated | Removes the remaining resolve/front costs. The tile and texture gains depend on measurements. |
| B. "Shadow HLE": real D3D runs (keeps device fields, fences, getters), hook only Draw/Clear/Resolve/Swap/Begin/EndTiling, read state from the device shadow, drop PM4 submission | 20-35 | medium; needs the device shadow layout; fences must be faked | Adds: no PM4 parse, no EDRAM model, one pass for tiles. Still needs texture/VB change detection and shader translation. |
| C. Full Unleashed-style HLE: hook about 150 API functions + about 230 state setters, native GL renderer, AOT shader translation | **40-70** | high: no symbols, early XDK, tiling/EDRAM semantics, fences, CPU-written resources, runtime shaders, Bink | Highest ceiling (native renderer, AOT shaders, no Xenos model). The win over B is mostly cleanliness, not speed. |

Breakdown for C:
* identification and validation of the API: 6-10 days;
* renderer core (state, buffers with endian swap, textures with untile on create, render targets, resolve, tiling collapse, clears, queries, fences): 18-30 days;
* shader path (reuse the SDK Xenos → SPIR-V → GLSL translator with a simpler binding model; AOT pre-translate the 531 `.sdp` shaders into an SD cache, with a runtime fallback): 4-7 days;
* parity debugging across menus, interiors, exteriors, HDR, water and Bink: 12-25 days.

### Risks

* **The hardware bottleneck is unknown.** If the cost is CPU-side guest code (A57 cores running the recompiled game), no renderer change reaches 10 fps. The 0.2.0 milestone needs real profiles first.
* **Tiles per frame are unknown.** If Oblivion uses a single tile at its chosen resolution, the tile-collapse benefit is zero.
* **Weak-symbol override on Switch GCC is not yet demonstrated** in this repo; check with `nm` (1.a).
* **CPU/GPU memory coherence.** Lazy write-back plus D3D-level knowledge needs care if the game reads resolved data on the CPU, for example HDR luminance or screenshots for save thumbnails.
* **Early XDK.** Struct and enum knowledge from later XDKs may not apply.

### Phased plan

1. **Phase 0, measure (1-2 days, hardware).**
   * Collect the GL perf counters already in the tree on a real Switch in the prison cell and outdoors: frame time split, resolves/frame, texture invalidations/frame, store upload MB/frame.
   * Count distinct SET_BIN_SELECT values per frame (tiles).
   * Proof-of-concept hook: `REX_HOOK_RAW(sub_8255F5E0)` that logs and calls `__imp__sub_8255F5E0`. Verify with `nm` that the override wins in the NRO.
2. **Phase 1, hybrid present and resolve (3-6 days).** Present-wrapper and Resolve hooks with a side channel into the GL command processor; host-RT direct present; GPU-only resolve destinations. Gate with `gl_hle_present`, `gl_hle_resolve`.
3. **Phase 2, conditional (3-6 days).** Only if Phase 0 shows N > 1 tiles or texture churn: the tiling collapse and the static-texture marking.
4. **Phase 3, decision point.** If the GPU path is still the bottleneck after phases 1-2, take option B rather than C first. Both share the hooks, the name table and the shader cache, so B can grow into C.
5. **Parallel, cheap:** name the identified functions in the manifest (`[entrypoint.functions] 0x... = { name = "D3D_Resolve" }`) to make profiles and traces readable. This needs a codegen rerun, so batch it with the next unrelated codegen.

### Addresses (for quick reference)

| label | address |
|---|---|
| Present wrapper | 0x8255FBC0 (game caller 0x8211DF78) |
| Swap (VdSwap) | 0x8255F5E0 |
| Resolve | 0x825604D0 |
| Clear | 0x8255BED8 → 0x8255B7A8 |
| DrawIndexedVertices / DrawVertices / UP draws | 0x8255E490 / 0x8255E250 / 0x8255DEF8, 0x8255DBD0 |
| pre-draw flush | 0x825693B0 → 0x82568DE0 |
| KickOff | 0x8255D488 |
| ring init | 0x8255D7A8 |
| device init | 0x8256A108 |
| render/sampler state tables | dev+96 / dev+484, defaults 0x82A5FA38 / 0x82A5FEC8 |
| tiling | 0x829B9610 (begin), 0x829B9A48 (end), 0x829B94D8 (helper); Gamebryo callers 0x82789870 / 0x8278C138 |
| SetStreamSource / SetIndices / SetTexture / SetVertexDeclaration | 0x82563C20 / 0x82563CC0 / 0x8255B230 / 0x82557130 |
| occlusion query | 0x82565230 / 0x82564930 |
