/**
 * @file    gpu/d3d/hooks/state.cpp
 * @brief   N&B's D3D state calls, hooked with the native ports (d3d_native.h).
 *
 * The render and sampler state setters are the ones N&B's state tables name
 * (D3D::InitializeApiState puts them into every device's dispatch table,
 * which the game's inline SetRenderState / SetSamplerState call through).
 * The rest are D3DDevice_Set* and the CDevice internals that write the ring.
 */
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <iterator>
#include <mutex>
#include <unordered_map>

#include <rex/hook.h>

#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/log.h"
#include <rex/types.h>

#include "renut_engine/gpu/d3d/d3d_native.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/guest/guest_records.h"

using namespace renut::gpu;

namespace {

// oblivion (2026-11-03): XDK 2.0.2417's render state 57 (between ColorWriteEnable3 and TessellationMode), which N&B's
// table lacks (zuma-nx's ZumaSetRenderState57 for XDK 2135): Oblivion's table entry 57 is its own setter sub_82562838
// and getter sub_82562850. Nothing the renderer draws depends on it; the value is kept for the getter.
u32 g_oblivion_render_state57 = 0;
void OblivionSetRenderState57(u32 /*device*/, u32 value) {
  g_oblivion_render_state57 = value;
}
u32 OblivionGetRenderState57(u32 /*device*/) {
  return g_oblivion_render_state57;
}

template <u32 State>
void SetRenderStateHook(u32 device, u32 value) {
  native::SetRenderState(native::DeviceAt(device), State, value);
}

template <u32 State>
void SetSamplerStateHook(u32 device, u32 sampler, u32 value) {
  native::SetSamplerState(native::DeviceAt(device), sampler, State, value);
}

template <u32 State>
u32 GetRenderStateHook(u32 device) {
  return native::GetRenderState(native::DeviceAt(device), State);
}

template <u32 State>
u32 GetSamplerStateHook(u32 device, u32 sampler) {
  return native::GetSamplerState(native::DeviceAt(device), sampler, State);
}

// The device's AddRef of a resource it hands out (D3DResource_AddRef,
// resource.cpp).
u32 AddRefResource(u32 resource) {
  return native::AddRefResource(resource);
}

// D3DSurface *GetRenderTarget(D3DDevice*, DWORD Index) [0x82636ED0]
u32 D3DDevice_GetRenderTarget_hook(u32 device, u32 index) {
  const native::Device d = native::DeviceAt(device);
  const u32 surface = d && index < 4 ? d.W(native::off::RenderTargets + index * 4) : 0;
  if (surface)
    AddRefResource(surface);
  return surface;
}

// D3DSurface *GetDepthStencilSurface(D3DDevice*) [0x82636F10]
u32 D3DDevice_GetDepthStencilSurface_hook(u32 device) {
  const native::Device d = native::DeviceAt(device);
  const u32 surface = d ? d.W(native::off::DepthStencil) : 0;
  if (surface)
    AddRefResource(surface);
  return surface;
}

// D3DVertexBuffer *GetStreamSource(D3DDevice*, UINT StreamNumber,
//     UINT *pOffsetInBytes, UINT *pStride) [0x82636E40]: the offset is the
// stream's fetch constant address less the buffer's.
u32 D3DDevice_GetStreamSource_hook(u32 device, u32 stream, mapped_u32 offset, mapped_u32 stride) {
  const native::Device d = native::DeviceAt(device);
  if (!d || stream >= 16)
    return 0;
  const u32 buffer = d.W(native::off::StreamSources + stream * 4);
  u32 bytes = 0;
  if (buffer) {
    const u32 fetch = d.W(native::off::TextureFetch + (95 - stream) * 8);
    bytes = fetch - native::GpuAddress(GuestPtr<const be_u32>(buffer)[6]);
    AddRefResource(buffer);
  }
  if (offset)
    *offset = bytes;
  if (stride)
    *stride = u32(d.B(native::off::StreamStrides + stream)) << 2;
  return buffer;
}

// void D3DDevice_GetViewport(D3DDevice*, D3DVIEWPORT9*) [0x82244B18]
void D3DDevice_GetViewport_hook(u32 device, mapped_u32 viewport) {
  const native::Device d = native::DeviceAt(device);
  if (!d || !viewport)
    return;
  for (u32 i = 0; i < 4; ++i)  // fctidz, the low word
    viewport[i] = u32(native::TruncateToI64(d.F(0x3160 + i * 4)));  // katamari-nx: 0x3170->0x3160
  viewport[4] = d.W(0x3170);  // katamari-nx: 0x3180->0x3170
  viewport[5] = d.W(0x3174);  // katamari-nx: 0x3184->0x3174
}

// void D3DDevice_SetClipPlane(D3DDevice*, DWORD Index, const float *pPlane) [0x826370E8]
void D3DDevice_SetClipPlane_hook(u32 device, u32 index, mapped_u32 plane) {
  native::Device d = native::DeviceAt(device);
  if (!d || !plane || index >= 6)
    return;
  for (u32 i = 0; i < 4; ++i)
    d.SetW(0x2820 + index * 16 + i * 4, plane[i]);
  d.Dirty(4, u64(1) << (54 - index));
}

// void D3DDevice_SetVertexFetchConstant(D3DDevice*, UINT VertexFetchRegister,
//     D3DVertexBuffer*, UINT Offset) [0x82637138]: a vertex fetch slot of the
// 3 in a fetch constant, made a vertex fetch constant first.
void D3DDevice_SetVertexFetchConstant_hook(u32 device, u32 reg, u32 vertex_buffer, u32 offset) {
  native::Device d = native::DeviceAt(device);
  const auto *vb = vertex_buffer ? GuestPtr<const be_u32>(vertex_buffer) : nullptr;
  if (!d || !vb || reg > 0x5F)
    return;
  const u32 slot = (0x5F - reg) / 3;
  const u32 part = (0x5F - reg) - slot * 3;
  const u32 constant = native::off::TextureFetch + slot * 24;
  const u32 type = d.W(constant) & 3;
  if (type == 0 || type == 2)
    std::memset(d.bytes + constant, 0, 24);
  d.TouchRange(constant, 24);  // banjo-nx (2026-10-26, banjo_renut_tags)
  d.SetW(constant, native::Insert(d.W(constant), 1, 0, 2));
  d.Dirty(3, u64(1) << (31 - slot));
  d.SetW(constant + part * 8, native::GpuAddress(u32(vb[6]) + offset));
  d.SetW(constant + part * 8 + 4, native::Insert(u32(vb[7]) - offset, 1, 28, 2));
}

// The boolean and integer shader constants (D3DDevice_Set{Vertex,Pixel}
// ShaderConstant{B,I}): bits from 0x2780 (vertex) / 0x2790 (pixel), a word
// of x, y, z bytes each from 0x27A0 / 0x27E0.
void SetShaderConstantsB(u32 device, u32 base, u32 start, mapped_u32 data, u32 count) {
  native::Device d = native::DeviceAt(device);
  if (!d || !data)
    return;
  for (u32 i = 0; i < count; ++i, ++start) {
    const u32 word = base + (start >> 5) * 4;
    const u32 bit = 1u << (start & 31);
    d.SetW(word, (d.W(word) & ~bit) | ((u32(data[i]) & 1) << (start & 31)));
  }
  d.Dirty(4, u64(1) << 56);
}

void SetShaderConstantsI(u32 device, u32 base, u32 start, mapped_u32 data, u32 count) {
  native::Device d = native::DeviceAt(device);
  if (!d || !data)
    return;
  for (u32 i = 0; i < count; ++i) {
    const u32 x = u32(data[i * 4]) & 0xFF, y = u32(data[i * 4 + 1]) & 0xFF;
    const u32 z = u32(data[i * 4 + 2]) & 0xFF;
    d.SetW(base + (start + i) * 4, (z << 16) | (y << 8) | x);
  }
  d.Dirty(4, u64(1) << 56);
}

// oblivion (2026-11-03): XDK 2417's SetVertexShaderConstantF (sub_82555368) and SetPixelShaderConstantF
// (sub_825554B0), which later XDKs inline into the game: the registers to the device's float constants (0x780 /
// 0x1780, N&B's places too), their groups of four marked in m_Pending, 2417's words 2 / 3 (+16 / +24) and N&B's 0 / 1.
void SetShaderConstantsF(u32 device, u32 word, u32 base, u32 start, u32 data, u32 count) {
  native::Device d = native::DeviceAt(device);
  if (!d || !count || start >= 256)
    return;
  count = std::min(count, 256 - start);
  auto *dst = GuestPtr<u8>(device + base + start * 16);
  const auto *src = GuestPtr<const u8>(data);
  if (!dst || !src)
    return;
  std::memcpy(dst, src, count * 16);
  d.TouchRange(base + start * 16, count * 16);  // the snapshot deltas' chunks (banjo_renut_tags)
  const u32 first = start / 4, last = (start + count - 1) / 4;
  d.Dirty(word, (~u64(0) >> first) & (~u64(0) << (63 - last)));
}
void OblivionSetVertexShaderConstantF(u32 device, u32 start, u32 data, u32 count) {
  SetShaderConstantsF(device, 0, 0x780, start, data, count);
}
void OblivionSetPixelShaderConstantF(u32 device, u32 start, u32 data, u32 count) {
  SetShaderConstantsF(device, 1, 0x1780, start, data, count);
}

// oblivion (2026-11-03): Gamebryo's own copies of those setters, (device, start, data, scratch, count) -> 0, for BOOL,
// INT and FLOAT registers (all of them float constants on the 360): vertex sub_824BD970 / sub_824BDA40 / sub_824BDB00,
// pixel sub_824BDD50 / sub_824BDE28 / sub_824BDEF0. The BOOL and INT ones first write data's count * 4 words as floats
// (0 / 1, or the integer) to scratch; then every one sets the registers from data, a single register inline (the four
// floats and 2417's pending bit, which Renut never saw: the main menu's fade quad kept an older colour, opaque black) and
// more through 2417's setter.
enum class OblivionConstantKind { Bool, Int, Float };
template <OblivionConstantKind kind, bool pixel>
u32 OblivionGamebryoSetConstants(u32 device, u32 start, u32 data, u32 scratch, u32 count) {
  if constexpr (kind != OblivionConstantKind::Float) {
    const auto *in = GuestPtr<const be_u32>(data);
    auto *out = GuestPtr<be_u32>(scratch);
    for (u32 i = 0; in && out && i < count * 4; ++i) {
      const u32 word = in[i];
      const float value = kind == OblivionConstantKind::Bool ? (word ? 1.0f : 0.0f) : float(i32(word));
      out[i] = std::bit_cast<u32>(value);
    }
  }
  if (pixel)
    OblivionSetPixelShaderConstantF(device, start, data, count);
  else
    OblivionSetVertexShaderConstantF(device, start, data, count);
  return 0;
}
using OCK = OblivionConstantKind;
u32 OblivionGamebryoVertexB(u32 d, u32 s, u32 v, u32 t, u32 n) { return OblivionGamebryoSetConstants<OCK::Bool, false>(d, s, v, t, n); }
u32 OblivionGamebryoVertexI(u32 d, u32 s, u32 v, u32 t, u32 n) { return OblivionGamebryoSetConstants<OCK::Int, false>(d, s, v, t, n); }
u32 OblivionGamebryoVertexF(u32 d, u32 s, u32 v, u32 t, u32 n) { return OblivionGamebryoSetConstants<OCK::Float, false>(d, s, v, t, n); }
u32 OblivionGamebryoPixelB(u32 d, u32 s, u32 v, u32 t, u32 n) { return OblivionGamebryoSetConstants<OCK::Bool, true>(d, s, v, t, n); }
u32 OblivionGamebryoPixelI(u32 d, u32 s, u32 v, u32 t, u32 n) { return OblivionGamebryoSetConstants<OCK::Int, true>(d, s, v, t, n); }
u32 OblivionGamebryoPixelF(u32 d, u32 s, u32 v, u32 t, u32 n) { return OblivionGamebryoSetConstants<OCK::Float, true>(d, s, v, t, n); }

// void D3DDevice_SetVertexShaderConstantB(D3DDevice*, UINT StartRegister,
//     const BOOL*, UINT BoolCount) [0x821F0B40], and the others alike.
void D3DDevice_SetVertexShaderConstantB_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsB(device, 0x2780, start, data, count);
}
void D3DDevice_SetPixelShaderConstantB_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsB(device, 0x2790, start, data, count);
}
void D3DDevice_SetVertexShaderConstantI_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsI(device, 0x27A0, start, data, count);
}
void D3DDevice_SetPixelShaderConstantI_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsI(device, 0x27E0, start, data, count);
}

// void D3DDevice_EndIndexedVertices(D3DDevice*) [0x8263ED90]: the inline end
// of BeginIndexedVertices, back to the ring pointer it saved.
void D3DDevice_EndIndexedVertices_hook(u32 device) {
  native::Device d = native::DeviceAt(device);
  if (d)
    d.SetW(native::off::Ring, d.W(0x3474));  // katamari-nx: 0x3484->0x3474
}

// void D3DDevice_SetTexture(D3DDevice*, DWORD Sampler, D3DBaseTexture*,
//     UINT64 PendingMask3) [0x821FE558]; the mask is the sampler's fetch
// constant bit, which the port derives from Sampler.
void D3DDevice_SetTexture_hook(u32 device, u32 sampler, u32 texture) {
  native::SetTexture(native::DeviceAt(device), sampler, texture);
}

// void D3DDevice_SetStreamSource(D3DDevice*, UINT StreamNumber, D3DVertexBuffer*,
//     UINT OffsetInBytes, UINT Stride, UINT64 PendingMask3) [0x821F9438]
void D3DDevice_SetStreamSource_hook(u32 device, u32 stream, u32 vertex_buffer, u32 offset,
                                    u32 stride) {
  native::SetStreamSource(native::DeviceAt(device), stream, vertex_buffer, offset, stride);
}

// void D3DDevice_SetIndices(D3DDevice*, D3DIndexBuffer*) [0x821DE438]
void D3DDevice_SetIndices_hook(u32 device, u32 index_buffer) {
  native::SetIndices(native::DeviceAt(device), index_buffer);
}

// void D3DDevice_SetVertexDeclaration(D3DDevice*, D3DVertexDeclaration*) [0x821D5060]
void D3DDevice_SetVertexDeclaration_hook(u32 device, u32 declaration) {
  native::SetVertexDeclaration(native::DeviceAt(device), declaration);
}

// oblivion (2026-11-03): XDK 2417's setters keep what they bind alive (the bind count of the game's objects, counted
// on the host for Renut's: native::OblivionRebind, hooks/resource.cpp). What the game bound in each slot is kept here
// (Renut's device holds a game texture's shadow instead); Oblivion has the one device.
struct OblivionBindings {
  u32 textures[32];
  u32 streams[16];
  u32 indices;
  u32 declaration;
  u32 render_targets[4];
  u32 depth_stencil;
};
OblivionBindings g_oblivion_bindings{};

u32 OblivionFence(native::Device d) {
  return d ? d.W(native::off::FenceTag) : 0;
}

// The game's textures through their shadow headers (hooks/resource.cpp, "Oblivion's textures").
void OblivionSetTexture(u32 device, u32 sampler, u32 texture) {
  native::Device d = native::DeviceAt(device);
  native::SetTexture(d, sampler, native::OblivionTextureShadow(texture));
  if (sampler < std::size(g_oblivion_bindings.textures))
    native::OblivionRebind(g_oblivion_bindings.textures[sampler], texture, OblivionFence(d));
}

void OblivionSetStreamSource(u32 device, u32 stream, u32 vertex_buffer, u32 offset, u32 stride) {
  native::Device d = native::DeviceAt(device);
  native::SetStreamSource(d, stream, native::OblivionVertexBufferShadow(vertex_buffer), offset, stride);
  if (stream < std::size(g_oblivion_bindings.streams))
    native::OblivionRebind(g_oblivion_bindings.streams[stream], vertex_buffer, OblivionFence(d));
}

void OblivionSetIndices(u32 device, u32 index_buffer) {
  native::Device d = native::DeviceAt(device);
  native::SetIndices(d, index_buffer);
  native::OblivionRebind(g_oblivion_bindings.indices, index_buffer, OblivionFence(d));
}

// 2417's SetVertexDeclaration (sub_82557130) references the declaration instead (AddRef, the old one's Release
// sub_82555DA0), as SetVertexShader / SetPixelShader do their shaders (hooks/draw.cpp).
void OblivionSetVertexDeclaration(u32 device, u32 declaration) {
  native::SetVertexDeclaration(native::DeviceAt(device), declaration);
  const u32 old = g_oblivion_bindings.declaration;
  g_oblivion_bindings.declaration = declaration;
  if (declaration)
    native::AddRefResource(declaration);
  if (old)
    native::ReleaseResource(old);
}

void OblivionSetRenderTarget(u32 device, u32 index, u32 surface) {
  native::Device d = native::DeviceAt(device);
  native::SetRenderTarget(d, index, surface);
  if (index < std::size(g_oblivion_bindings.render_targets))
    native::OblivionRebind(g_oblivion_bindings.render_targets[index], surface, OblivionFence(d));
}

void OblivionSetDepthStencilSurface(u32 device, u32 surface) {
  native::Device d = native::DeviceAt(device);
  native::SetDepthStencilSurface(d, surface);
  native::OblivionRebind(g_oblivion_bindings.depth_stencil, surface, OblivionFence(d));
}

// oblivion (2026-11-03): XDK 2417's SetFVF (sub_82557858) builds a declaration inside the device (+12304, 2417's
// layout) with sub_82553CC0 and sets it. sub_82553CC0 is pure (the FVF's bits, a table at 0x82023188): it builds the
// 2417 declaration object here in a scratch block, whose elements (from +36, D3DVERTEXELEMENT9's, ending in
// D3DDECL_END) make Renut's declaration, one per FVF, kept.
REX_IMPORT(__imp__sub_82553CC0, OblivionBuildFvfDeclaration, void(u32, u32));

std::mutex g_oblivion_fvf_mutex;
std::unordered_map<u32, u32> g_oblivion_fvf_declarations;
u32 g_oblivion_fvf_scratch = 0;

void OblivionSetFVF(u32 device, u32 fvf) {
  u32 declaration = 0;
  {
    std::lock_guard lock(g_oblivion_fvf_mutex);
    u32 &cached = g_oblivion_fvf_declarations[fvf];
    if (!cached) {
      if (!g_oblivion_fvf_scratch)
        g_oblivion_fvf_scratch = records::AllocPhysicalBlock(512);
      if (g_oblivion_fvf_scratch) {
        OblivionBuildFvfDeclaration(fvf, g_oblivion_fvf_scratch);
        cached = native::CreateVertexDeclaration(g_oblivion_fvf_scratch + 36);
      }
    }
    declaration = cached;
  }
  OblivionSetVertexDeclaration(device, declaration);
}

// void D3DDevice_SetViewport(D3DDevice*, const D3DVIEWPORT9*) [0x821F2090]
void D3DDevice_SetViewport_hook(u32 device, u32 viewport) {
  native::SetViewport(native::DeviceAt(device), viewport);
}

// void D3D::SetViewport(D3DDevice*, float X, float Y, float Width, float Height,
//     float MinZ, float MaxZ) [0x821F2110]
void D3D_SetViewport_hook(u32 device, float x, float y, float width, float height, float min_z,
                          float max_z) {
  native::SetViewport(native::DeviceAt(device), x, y, width, height, min_z, max_z);
}

// void D3DDevice_SetScissorRect(D3DDevice*, const RECT*) [0x821F2368]
void D3DDevice_SetScissorRect_hook(u32 device, mapped_u32 rect) {
  if (!rect)
    return;
  native::SetScissorRect(native::DeviceAt(device), i32(u32(rect[0])), i32(u32(rect[1])),
                         i32(u32(rect[2])), i32(u32(rect[3])));
}

// void D3D::SetSurfaceClip(CDevice*, int, int, int, int) [0x821F2460]: the
// window scissor and per-tile clip packets, nothing else.
void D3D_SetSurfaceClip_hook(u32 /*device*/, u32, u32, u32, u32) {}

// void D3DDevice_SetRenderTarget(D3DDevice*, DWORD Index, D3DSurface*) [0x82248BD0]
void D3DDevice_SetRenderTarget_hook(u32 device, u32 index, u32 surface) {
  native::SetRenderTarget(native::DeviceAt(device), index, surface);
}

// void D3DDevice_SetDepthStencilSurface(D3DDevice*, D3DSurface*) [0x82248FB0]
void D3DDevice_SetDepthStencilSurface_hook(u32 device, u32 surface) {
  native::SetDepthStencilSurface(native::DeviceAt(device), surface);
}

// void D3D::SetSurfaceInfo(CDevice*, D3DSurface*) [0x82248F20]
void D3D_SetSurfaceInfo_hook(u32 device, u32 surface) {
  native::SetSurfaceInfo(native::DeviceAt(device), surface);
}

// void D3DDevice_SetPredication(D3DDevice*, DWORD PredicationMask) [0x821F9A48]
void D3DDevice_SetPredication_hook(u32 device, u32 mask) {
  native::SetPredication(native::DeviceAt(device), mask);
}

// void D3DDevice_SetCommandBufferPredication(D3DDevice*, DWORD TilePredication,
//     DWORD RunPredication) [0x826411C0]
void D3DDevice_SetCommandBufferPredication_hook(u32 device, u32 tile_predication,
                                                u32 run_predication) {
  native::SetCommandBufferPredication(native::DeviceAt(device), tile_predication,
                                      run_predication);
}

// void D3DDevice_SetShaderGPRAllocation(D3DDevice*, DWORD Flags,
//     DWORD VertexShaderCount, DWORD PixelShaderCount) [0x821EFD38]
void D3DDevice_SetShaderGPRAllocation_hook(u32 device, u32 flags, u32 vertex_count,
                                           u32 pixel_count) {
  native::SetShaderGPRAllocation(native::DeviceAt(device), flags, vertex_count, pixel_count);
}

// void D3D::CleanupGprAllocationSetup(CDevice*) [0x82637208]
void D3D_CleanupGprAllocationSetup_hook(u32 device) {
  native::CleanupGprAllocationSetup(native::DeviceAt(device));
}

// void D3DDevice_SetScreenExtentQueryMode(D3DDevice*, D3DSCREENEXTENTQUERYMODE)
// [0x826414C0]
void D3DDevice_SetScreenExtentQueryMode_hook(u32 device, u32 mode) {
  native::SetScreenExtentQueryMode(native::DeviceAt(device), mode);
}

// void D3DDevice_UnsetAll(D3DDevice*) [0x82634C10]
void D3DDevice_UnsetAll_hook(u32 device) {
  native::UnsetAll(native::DeviceAt(device));
}

// void D3D::ResetAllState(CDevice*, BOOL) [0x82255C98]
void D3D_ResetAllState_hook(u32 device, u32 /*skip_packets*/) {
  native::ResetAllState(native::DeviceAt(device));
}

// void D3D::InvalidateConstants(D3DDevice*) [0x82634B90]
void D3D_InvalidateConstants_hook(u32 device) {
  native::InvalidateConstants(native::DeviceAt(device));
}

// void D3DDevice_BlockUntilIdle(D3DDevice*) [0x82640010] (IDA: the second
// D3D::InvalidateConstants): waits for the device's current fence (N&B calls
// it before releasing a device).
void D3DDevice_BlockUntilIdle_hook(u32 device) {
  native::InvalidateConstantsAndWait(native::DeviceAt(device));
}

} // namespace

// Render states, by their index in N&B's table (off_82E506F0).
// oblivion: generated by tools/renut/gen_state_hooks.py from the game's state tables (render 0x82A5FA38,
// sampler 0x82A5FEC8) and zuma-nx's (XDK 2135) order; the template index is N&B's.
BANJO_RENUT_HOOK(sub_82561C10, SetRenderStateHook<10>);  // D3DDevice_SetRenderState_ZEnable  // oblivion: table entry 10
BANJO_RENUT_HOOK(sub_82561C98, SetRenderStateHook<11>);  // D3DDevice_SetRenderState_ZFunc  // oblivion: table entry 11
BANJO_RENUT_HOOK(sub_82561C60, SetRenderStateHook<12>);  // D3DDevice_SetRenderState_ZWriteEnable  // oblivion: table entry 12
BANJO_RENUT_HOOK(sub_82561558, SetRenderStateHook<13>);  // D3DDevice_SetRenderState_FillMode  // oblivion: table entry 13
BANJO_RENUT_HOOK(sub_82561520, SetRenderStateHook<14>);  // D3DDevice_SetRenderState_CullMode  // oblivion: table entry 14
BANJO_RENUT_HOOK(sub_825615D8, SetRenderStateHook<15>);  // D3DDevice_SetRenderState_AlphaBlendEnable  // oblivion: table entry 15
BANJO_RENUT_HOOK(sub_825619A8, SetRenderStateHook<16>);  // D3DDevice_SetRenderState_SeparateAlphaBlendEnable  // oblivion: table entry 16
BANJO_RENUT_HOOK(sub_82561AF0, SetRenderStateHook<17>);  // D3DDevice_SetRenderState_BlendFactor  // oblivion: table entry 17
BANJO_RENUT_HOOK(sub_82561710, SetRenderStateHook<18>);  // D3DDevice_SetRenderState_SrcBlend  // oblivion: table entry 18
BANJO_RENUT_HOOK(sub_825617A8, SetRenderStateHook<19>);  // D3DDevice_SetRenderState_DestBlend  // oblivion: table entry 19
BANJO_RENUT_HOOK(sub_82561678, SetRenderStateHook<20>);  // D3DDevice_SetRenderState_BlendOp  // oblivion: table entry 20
BANJO_RENUT_HOOK(sub_825618B8, SetRenderStateHook<21>);  // D3DDevice_SetRenderState_SrcBlendAlpha  // oblivion: table entry 21
BANJO_RENUT_HOOK(sub_82561930, SetRenderStateHook<22>);  // D3DDevice_SetRenderState_DestBlendAlpha  // oblivion: table entry 22
BANJO_RENUT_HOOK(sub_82561840, SetRenderStateHook<23>);  // D3DDevice_SetRenderState_BlendOpAlpha  // oblivion: table entry 23
BANJO_RENUT_HOOK(sub_82561590, SetRenderStateHook<24>);  // D3DDevice_SetRenderState_AlphaTestEnable  // oblivion: table entry 24
BANJO_RENUT_HOOK(sub_82561A48, SetRenderStateHook<25>);  // D3DDevice_SetRenderState_AlphaRef  // oblivion: table entry 25
BANJO_RENUT_HOOK(sub_82561AB8, SetRenderStateHook<26>);  // D3DDevice_SetRenderState_AlphaFunc  // oblivion: table entry 26
BANJO_RENUT_HOOK(sub_82561CE0, SetRenderStateHook<27>);  // D3DDevice_SetRenderState_StencilEnable  // oblivion: table entry 27
BANJO_RENUT_HOOK(sub_82561D30, SetRenderStateHook<28>);  // D3DDevice_SetRenderState_TwoSidedStencilMode  // oblivion: table entry 28
BANJO_RENUT_HOOK(sub_82561DB0, SetRenderStateHook<29>);  // D3DDevice_SetRenderState_StencilFail  // oblivion: table entry 29
BANJO_RENUT_HOOK(sub_82561DF8, SetRenderStateHook<30>);  // D3DDevice_SetRenderState_StencilZFail  // oblivion: table entry 30
BANJO_RENUT_HOOK(sub_82561E40, SetRenderStateHook<31>);  // D3DDevice_SetRenderState_StencilPass  // oblivion: table entry 31
BANJO_RENUT_HOOK(sub_82561D78, SetRenderStateHook<32>);  // D3DDevice_SetRenderState_StencilFunc  // oblivion: table entry 32
BANJO_RENUT_HOOK(sub_82561F78, SetRenderStateHook<33>);  // D3DDevice_SetRenderState_StencilRef  // oblivion: table entry 33
BANJO_RENUT_HOOK(sub_82561FA0, SetRenderStateHook<34>);  // D3DDevice_SetRenderState_StencilMask  // oblivion: table entry 34
BANJO_RENUT_HOOK(sub_82561FC8, SetRenderStateHook<35>);  // D3DDevice_SetRenderState_StencilWriteMask  // oblivion: table entry 35
BANJO_RENUT_HOOK(sub_82561EB0, SetRenderStateHook<36>);  // D3DDevice_SetRenderState_CCWStencilFail  // oblivion: table entry 36
BANJO_RENUT_HOOK(sub_82561EF8, SetRenderStateHook<37>);  // D3DDevice_SetRenderState_CCWStencilZFail  // oblivion: table entry 37
BANJO_RENUT_HOOK(sub_82561F40, SetRenderStateHook<38>);  // D3DDevice_SetRenderState_CCWStencilPass  // oblivion: table entry 38
BANJO_RENUT_HOOK(sub_82561E78, SetRenderStateHook<39>);  // D3DDevice_SetRenderState_CCWStencilFunc  // oblivion: table entry 39
BANJO_RENUT_HOOK(sub_82561FF0, SetRenderStateHook<40>);  // D3DDevice_SetRenderState_CCWStencilRef  // oblivion: table entry 40
BANJO_RENUT_HOOK(sub_82562018, SetRenderStateHook<41>);  // D3DDevice_SetRenderState_CCWStencilMask  // oblivion: table entry 41
BANJO_RENUT_HOOK(sub_82562040, SetRenderStateHook<42>);  // D3DDevice_SetRenderState_CCWStencilWriteMask  // oblivion: table entry 42
BANJO_RENUT_HOOK(sub_82562068, SetRenderStateHook<43>);  // D3DDevice_SetRenderState_ClipPlaneEnable  // oblivion: table entry 43
BANJO_RENUT_HOOK(sub_825623B0, SetRenderStateHook<44>);  // D3DDevice_SetRenderState_PointSize  // oblivion: table entry 44
BANJO_RENUT_HOOK(sub_82562408, SetRenderStateHook<45>);  // D3DDevice_SetRenderState_PointSizeMin  // oblivion: table entry 45
BANJO_RENUT_HOOK(sub_825623A0, SetRenderStateHook<46>);  // D3DDevice_SetRenderState_PointSpriteEnable  // oblivion: table entry 46
BANJO_RENUT_HOOK(sub_82562458, SetRenderStateHook<47>);  // D3DDevice_SetRenderState_PointSizeMax  // oblivion: table entry 47
BANJO_RENUT_HOOK(sub_82562218, SetRenderStateHook<48>);  // D3DDevice_SetRenderState_MultiSampleAntiAlias  // oblivion: table entry 48
BANJO_RENUT_HOOK(sub_82562250, SetRenderStateHook<49>);  // D3DDevice_SetRenderState_MultiSampleMask  // oblivion: table entry 49
BANJO_RENUT_HOOK(sub_82564108, SetRenderStateHook<50>);  // D3DDevice_SetRenderState_ScissorTestEnable  // oblivion: table entry 50
BANJO_RENUT_HOOK(sub_825620B0, SetRenderStateHook<51>);  // D3DDevice_SetRenderState_SlopeScaleDepthBias  // oblivion: table entry 51
BANJO_RENUT_HOOK(sub_82562170, SetRenderStateHook<52>);  // D3DDevice_SetRenderState_DepthBias  // oblivion: table entry 52
BANJO_RENUT_HOOK(sub_82562280, SetRenderStateHook<53>);  // D3DDevice_SetRenderState_ColorWriteEnable  // oblivion: table entry 53
BANJO_RENUT_HOOK(sub_825622C8, SetRenderStateHook<54>);  // D3DDevice_SetRenderState_ColorWriteEnable1  // oblivion: table entry 54
BANJO_RENUT_HOOK(sub_82562310, SetRenderStateHook<55>);  // D3DDevice_SetRenderState_ColorWriteEnable2  // oblivion: table entry 55
BANJO_RENUT_HOOK(sub_82562358, SetRenderStateHook<56>);  // D3DDevice_SetRenderState_ColorWriteEnable3  // oblivion: table entry 56
BANJO_RENUT_HOOK(sub_82562838, OblivionSetRenderState57);  // XDK 2135/2417 render state 57 (setter)  // oblivion: table entry 57
BANJO_RENUT_HOOK(sub_82562BF8, SetRenderStateHook<57>);  // D3DDevice_SetRenderState_TessellationMode  // oblivion: table entry 58
BANJO_RENUT_HOOK(sub_82562B98, SetRenderStateHook<58>);  // D3DDevice_SetRenderState_MinTessellationLevel  // oblivion: table entry 59
BANJO_RENUT_HOOK(sub_82562BC8, SetRenderStateHook<59>);  // D3DDevice_SetRenderState_MaxTessellationLevel  // oblivion: table entry 60
BANJO_RENUT_HOOK(sub_825624A8, SetRenderStateHook<60>);  // D3DDevice_SetRenderState_Wrap0  // oblivion: table entry 61
BANJO_RENUT_HOOK(sub_825624D8, SetRenderStateHook<61>);  // D3DDevice_SetRenderState_Wrap1  // oblivion: table entry 62
BANJO_RENUT_HOOK(sub_82562508, SetRenderStateHook<62>);  // D3DDevice_SetRenderState_Wrap2  // oblivion: table entry 63
BANJO_RENUT_HOOK(sub_82562538, SetRenderStateHook<63>);  // D3DDevice_SetRenderState_Wrap3  // oblivion: table entry 64
BANJO_RENUT_HOOK(sub_82562568, SetRenderStateHook<64>);  // D3DDevice_SetRenderState_Wrap4  // oblivion: table entry 65
BANJO_RENUT_HOOK(sub_82562598, SetRenderStateHook<65>);  // D3DDevice_SetRenderState_Wrap5  // oblivion: table entry 66
BANJO_RENUT_HOOK(sub_825625C8, SetRenderStateHook<66>);  // D3DDevice_SetRenderState_Wrap6  // oblivion: table entry 67
BANJO_RENUT_HOOK(sub_825625F8, SetRenderStateHook<67>);  // D3DDevice_SetRenderState_Wrap7  // oblivion: table entry 68
BANJO_RENUT_HOOK(sub_82562620, SetRenderStateHook<68>);  // D3DDevice_SetRenderState_Wrap8  // oblivion: table entry 69
BANJO_RENUT_HOOK(sub_82562650, SetRenderStateHook<69>);  // D3DDevice_SetRenderState_Wrap9  // oblivion: table entry 70
BANJO_RENUT_HOOK(sub_82562680, SetRenderStateHook<70>);  // D3DDevice_SetRenderState_Wrap10  // oblivion: table entry 71
BANJO_RENUT_HOOK(sub_825626B0, SetRenderStateHook<71>);  // D3DDevice_SetRenderState_Wrap11  // oblivion: table entry 72
BANJO_RENUT_HOOK(sub_825626E0, SetRenderStateHook<72>);  // D3DDevice_SetRenderState_Wrap12  // oblivion: table entry 73
BANJO_RENUT_HOOK(sub_82562710, SetRenderStateHook<73>);  // D3DDevice_SetRenderState_Wrap13  // oblivion: table entry 74
BANJO_RENUT_HOOK(sub_82562740, SetRenderStateHook<74>);  // D3DDevice_SetRenderState_Wrap14  // oblivion: table entry 75
BANJO_RENUT_HOOK(sub_82562770, SetRenderStateHook<75>);  // D3DDevice_SetRenderState_Wrap15  // oblivion: table entry 76
BANJO_RENUT_HOOK(sub_82562858, SetRenderStateHook<76>);  // D3DDevice_SetRenderState_ViewportEnable  // oblivion: table entry 77
BANJO_RENUT_HOOK(sub_825628B8, SetRenderStateHook<77>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable  // oblivion: table entry 78
BANJO_RENUT_HOOK(sub_82562970, SetRenderStateHook<78>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable1  // oblivion: table entry 79
BANJO_RENUT_HOOK(sub_82562A28, SetRenderStateHook<79>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable2  // oblivion: table entry 80
BANJO_RENUT_HOOK(sub_82562AE0, SetRenderStateHook<80>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable3  // oblivion: table entry 81
BANJO_RENUT_HOOK(sub_82562C28, SetRenderStateHook<81>);  // D3DDevice_SetRenderState_HalfPixelOffset  // oblivion: table entry 82
BANJO_RENUT_HOOK(sub_82562C60, SetRenderStateHook<82>);  // D3DDevice_SetRenderState_PrimitiveResetEnable  // oblivion: table entry 83
BANJO_RENUT_HOOK(sub_82562C98, SetRenderStateHook<83>);  // D3DDevice_SetRenderState_PrimitiveResetIndex  // oblivion: table entry 84
BANJO_RENUT_HOOK(sub_82562CC0, SetRenderStateHook<84>);  // D3DDevice_SetRenderState_AlphaToMaskEnable  // oblivion: table entry 85
BANJO_RENUT_HOOK(sub_82562CF8, SetRenderStateHook<85>);  // D3DDevice_SetRenderState_AlphaToMaskOffsets  // oblivion: table entry 86
BANJO_RENUT_HOOK(sub_82562D38, SetRenderStateHook<86>);  // D3DDevice_SetRenderState_GuardBand_X  // oblivion: table entry 87
BANJO_RENUT_HOOK(sub_82562D68, SetRenderStateHook<87>);  // D3DDevice_SetRenderState_GuardBand_Y  // oblivion: table entry 88
BANJO_RENUT_HOOK(sub_82562D98, SetRenderStateHook<88>);  // D3DDevice_SetRenderState_DiscardBand_X  // oblivion: table entry 89
BANJO_RENUT_HOOK(sub_82562DC8, SetRenderStateHook<89>);  // D3DDevice_SetRenderState_DiscardBand_Y  // oblivion: table entry 90
BANJO_RENUT_HOOK(sub_82562DF8, SetRenderStateHook<90>);  // D3DDevice_SetRenderState_HiStencilEnable  // oblivion: table entry 91
BANJO_RENUT_HOOK(sub_82562E30, SetRenderStateHook<91>);  // D3DDevice_SetRenderState_HiStencilWriteEnable  // oblivion: table entry 92
BANJO_RENUT_HOOK(sub_82562E68, SetRenderStateHook<92>);  // D3DDevice_SetRenderState_HiStencilFunc  // oblivion: table entry 93
BANJO_RENUT_HOOK(sub_82562EA0, SetRenderStateHook<93>);  // D3DDevice_SetRenderState_HiStencilRef  // oblivion: table entry 94
BANJO_RENUT_HOOK(sub_82562EC8, SetRenderStateHook<94>);  // D3DDevice_SetRenderState_PresentInterval  // oblivion: table entry 95
BANJO_RENUT_HOOK(sub_82562ED8, SetRenderStateHook<95>);  // D3DDevice_SetRenderState_PresentImmediateThreshold  // oblivion: table entry 96
// oblivion: not in XDK 2417: BANJO_RENUT_HOOK(sub_XXXXXXXX, SetRenderStateHook<96>);  // D3DDevice_SetRenderState_HiZEnable
// oblivion: not in XDK 2417: BANJO_RENUT_HOOK(sub_XXXXXXXX, SetRenderStateHook<97>);  // D3DDevice_SetRenderState_HiZWriteEnable
// oblivion: not in XDK 2417: BANJO_RENUT_HOOK(sub_XXXXXXXX, SetRenderStateHook<98>);  // D3DDevice_SetRenderState_LastPixel
// oblivion: not in XDK 2417: BANJO_RENUT_HOOK(sub_XXXXXXXX, SetRenderStateHook<99>);  // D3DDevice_SetRenderState_LineWidth
// oblivion: not in XDK 2417: BANJO_RENUT_HOOK(sub_XXXXXXXX, SetRenderStateHook<100>);  // D3DDevice_SetRenderState_Buffer2Frames
BANJO_RENUT_HOOK(sub_82561C58, GetRenderStateHook<10>);  // D3DDevice_GetRenderState_ZEnable  // oblivion: table entry 10
BANJO_RENUT_HOOK(sub_82561CD0, GetRenderStateHook<11>);  // D3DDevice_GetRenderState_ZFunc  // oblivion: table entry 11
BANJO_RENUT_HOOK(sub_82561C88, GetRenderStateHook<12>);  // D3DDevice_GetRenderState_ZWriteEnable  // oblivion: table entry 12
BANJO_RENUT_HOOK(sub_82561580, GetRenderStateHook<13>);  // D3DDevice_GetRenderState_FillMode  // oblivion: table entry 13
BANJO_RENUT_HOOK(sub_82561548, GetRenderStateHook<14>);  // D3DDevice_GetRenderState_CullMode  // oblivion: table entry 14
BANJO_RENUT_HOOK(sub_82561668, GetRenderStateHook<15>);  // D3DDevice_GetRenderState_AlphaBlendEnable  // oblivion: table entry 15
BANJO_RENUT_HOOK(sub_82561A38, GetRenderStateHook<16>);  // D3DDevice_GetRenderState_SeparateAlphaBlendEnable  // oblivion: table entry 16
BANJO_RENUT_HOOK(sub_82561B90, GetRenderStateHook<17>);  // D3DDevice_GetRenderState_BlendFactor  // oblivion: table entry 17
BANJO_RENUT_HOOK(sub_82561798, GetRenderStateHook<18>);  // D3DDevice_GetRenderState_SrcBlend  // oblivion: table entry 18
BANJO_RENUT_HOOK(sub_82561830, GetRenderStateHook<19>);  // D3DDevice_GetRenderState_DestBlend  // oblivion: table entry 19
BANJO_RENUT_HOOK(sub_82561700, GetRenderStateHook<20>);  // D3DDevice_GetRenderState_BlendOp  // oblivion: table entry 20
BANJO_RENUT_HOOK(sub_82561920, GetRenderStateHook<21>);  // D3DDevice_GetRenderState_SrcBlendAlpha  // oblivion: table entry 21
BANJO_RENUT_HOOK(sub_82561998, GetRenderStateHook<22>);  // D3DDevice_GetRenderState_DestBlendAlpha  // oblivion: table entry 22
BANJO_RENUT_HOOK(sub_825618A8, GetRenderStateHook<23>);  // D3DDevice_GetRenderState_BlendOpAlpha  // oblivion: table entry 23
BANJO_RENUT_HOOK(sub_825615C8, GetRenderStateHook<24>);  // D3DDevice_GetRenderState_AlphaTestEnable  // oblivion: table entry 24
BANJO_RENUT_HOOK(sub_82561A88, GetRenderStateHook<25>);  // D3DDevice_GetRenderState_AlphaRef  // oblivion: table entry 25
BANJO_RENUT_HOOK(sub_82561AE0, GetRenderStateHook<26>);  // D3DDevice_GetRenderState_AlphaFunc  // oblivion: table entry 26
BANJO_RENUT_HOOK(sub_82561D28, GetRenderStateHook<27>);  // D3DDevice_GetRenderState_StencilEnable  // oblivion: table entry 27
BANJO_RENUT_HOOK(sub_82561D68, GetRenderStateHook<28>);  // D3DDevice_GetRenderState_TwoSidedStencilMode  // oblivion: table entry 28
BANJO_RENUT_HOOK(sub_82561DE8, GetRenderStateHook<29>);  // D3DDevice_GetRenderState_StencilFail  // oblivion: table entry 29
BANJO_RENUT_HOOK(sub_82561E30, GetRenderStateHook<30>);  // D3DDevice_GetRenderState_StencilZFail  // oblivion: table entry 30
BANJO_RENUT_HOOK(sub_82561E68, GetRenderStateHook<31>);  // D3DDevice_GetRenderState_StencilPass  // oblivion: table entry 31
BANJO_RENUT_HOOK(sub_82561DA0, GetRenderStateHook<32>);  // D3DDevice_GetRenderState_StencilFunc  // oblivion: table entry 32
BANJO_RENUT_HOOK(sub_82561F98, GetRenderStateHook<33>);  // D3DDevice_GetRenderState_StencilRef  // oblivion: table entry 33
BANJO_RENUT_HOOK(sub_82561FC0, GetRenderStateHook<34>);  // D3DDevice_GetRenderState_StencilMask  // oblivion: table entry 34
BANJO_RENUT_HOOK(sub_82561FE8, GetRenderStateHook<35>);  // D3DDevice_GetRenderState_StencilWriteMask  // oblivion: table entry 35
BANJO_RENUT_HOOK(sub_82561EE8, GetRenderStateHook<36>);  // D3DDevice_GetRenderState_CCWStencilFail  // oblivion: table entry 36
BANJO_RENUT_HOOK(sub_82561F30, GetRenderStateHook<37>);  // D3DDevice_GetRenderState_CCWStencilZFail  // oblivion: table entry 37
BANJO_RENUT_HOOK(sub_82561F68, GetRenderStateHook<38>);  // D3DDevice_GetRenderState_CCWStencilPass  // oblivion: table entry 38
BANJO_RENUT_HOOK(sub_82561EA0, GetRenderStateHook<39>);  // D3DDevice_GetRenderState_CCWStencilFunc  // oblivion: table entry 39
BANJO_RENUT_HOOK(sub_82562010, GetRenderStateHook<40>);  // D3DDevice_GetRenderState_CCWStencilRef  // oblivion: table entry 40
BANJO_RENUT_HOOK(sub_82562038, GetRenderStateHook<41>);  // D3DDevice_GetRenderState_CCWStencilMask  // oblivion: table entry 41
BANJO_RENUT_HOOK(sub_82562060, GetRenderStateHook<42>);  // D3DDevice_GetRenderState_CCWStencilWriteMask  // oblivion: table entry 42
BANJO_RENUT_HOOK(sub_82562098, GetRenderStateHook<43>);  // D3DDevice_GetRenderState_ClipPlaneEnable  // oblivion: table entry 43
BANJO_RENUT_HOOK(sub_825623F8, GetRenderStateHook<44>);  // D3DDevice_GetRenderState_PointSize  // oblivion: table entry 44
BANJO_RENUT_HOOK(sub_82562448, GetRenderStateHook<45>);  // D3DDevice_GetRenderState_PointSizeMin  // oblivion: table entry 45
BANJO_RENUT_HOOK(sub_825623A8, GetRenderStateHook<46>);  // D3DDevice_GetRenderState_PointSpriteEnable  // oblivion: table entry 46
BANJO_RENUT_HOOK(sub_82562498, GetRenderStateHook<47>);  // D3DDevice_GetRenderState_PointSizeMax  // oblivion: table entry 47
BANJO_RENUT_HOOK(sub_82562240, GetRenderStateHook<48>);  // D3DDevice_GetRenderState_MultiSampleAntiAlias  // oblivion: table entry 48
BANJO_RENUT_HOOK(sub_82562278, GetRenderStateHook<49>);  // D3DDevice_GetRenderState_MultiSampleMask  // oblivion: table entry 49
BANJO_RENUT_HOOK(sub_825620A8, GetRenderStateHook<50>);  // D3DDevice_GetRenderState_ScissorTestEnable  // oblivion: table entry 50
BANJO_RENUT_HOOK(sub_82562150, GetRenderStateHook<51>);  // D3DDevice_GetRenderState_SlopeScaleDepthBias  // oblivion: table entry 51
BANJO_RENUT_HOOK(sub_82562208, GetRenderStateHook<52>);  // D3DDevice_GetRenderState_DepthBias  // oblivion: table entry 52
BANJO_RENUT_HOOK(sub_825622C0, GetRenderStateHook<53>);  // D3DDevice_GetRenderState_ColorWriteEnable  // oblivion: table entry 53
BANJO_RENUT_HOOK(sub_82562308, GetRenderStateHook<54>);  // D3DDevice_GetRenderState_ColorWriteEnable1  // oblivion: table entry 54
BANJO_RENUT_HOOK(sub_82562350, GetRenderStateHook<55>);  // D3DDevice_GetRenderState_ColorWriteEnable2  // oblivion: table entry 55
BANJO_RENUT_HOOK(sub_82562398, GetRenderStateHook<56>);  // D3DDevice_GetRenderState_ColorWriteEnable3  // oblivion: table entry 56
BANJO_RENUT_HOOK(sub_82562850, OblivionGetRenderState57);  // XDK 2135/2417 render state 57 (getter)  // oblivion: table entry 57
BANJO_RENUT_HOOK(sub_82562C18, GetRenderStateHook<57>);  // D3DDevice_GetRenderState_TessellationMode  // oblivion: table entry 58
BANJO_RENUT_HOOK(sub_82562BB8, GetRenderStateHook<58>);  // D3DDevice_GetRenderState_MinTessellationLevel  // oblivion: table entry 59
BANJO_RENUT_HOOK(sub_82562BE8, GetRenderStateHook<59>);  // D3DDevice_GetRenderState_MaxTessellationLevel  // oblivion: table entry 60
BANJO_RENUT_HOOK(sub_82562798, GetRenderStateHook<60>);  // D3DDevice_GetRenderState_Wrap0  // oblivion: table entry 61
BANJO_RENUT_HOOK(sub_825627A8, GetRenderStateHook<61>);  // D3DDevice_GetRenderState_Wrap1  // oblivion: table entry 62
BANJO_RENUT_HOOK(sub_825627B8, GetRenderStateHook<62>);  // D3DDevice_GetRenderState_Wrap2  // oblivion: table entry 63
BANJO_RENUT_HOOK(sub_825627C8, GetRenderStateHook<63>);  // D3DDevice_GetRenderState_Wrap3  // oblivion: table entry 64
BANJO_RENUT_HOOK(sub_825627D8, GetRenderStateHook<64>);  // D3DDevice_GetRenderState_Wrap4  // oblivion: table entry 65
BANJO_RENUT_HOOK(sub_825627E8, GetRenderStateHook<65>);  // D3DDevice_GetRenderState_Wrap5  // oblivion: table entry 66
BANJO_RENUT_HOOK(sub_825627F8, GetRenderStateHook<66>);  // D3DDevice_GetRenderState_Wrap6  // oblivion: table entry 67
BANJO_RENUT_HOOK(sub_82562808, GetRenderStateHook<67>);  // D3DDevice_GetRenderState_Wrap7  // oblivion: table entry 68
BANJO_RENUT_HOOK(sub_82562818, GetRenderStateHook<68>);  // D3DDevice_GetRenderState_Wrap8  // oblivion: table entry 69
BANJO_RENUT_HOOK(sub_82562828, GetRenderStateHook<69>);  // D3DDevice_GetRenderState_Wrap9  // oblivion: table entry 70
// oblivion: shared by 16 table entries, left alone: BANJO_RENUT_HOOK(sub_823A29C8, GetRenderStateHook<70>);  // D3DDevice_GetRenderState_Wrap10
// oblivion: shared by 16 table entries, left alone: BANJO_RENUT_HOOK(sub_823A29C8, GetRenderStateHook<71>);  // D3DDevice_GetRenderState_Wrap11
// oblivion: shared by 16 table entries, left alone: BANJO_RENUT_HOOK(sub_823A29C8, GetRenderStateHook<72>);  // D3DDevice_GetRenderState_Wrap12
// oblivion: shared by 16 table entries, left alone: BANJO_RENUT_HOOK(sub_823A29C8, GetRenderStateHook<73>);  // D3DDevice_GetRenderState_Wrap13
// oblivion: shared by 16 table entries, left alone: BANJO_RENUT_HOOK(sub_823A29C8, GetRenderStateHook<74>);  // D3DDevice_GetRenderState_Wrap14
// oblivion: shared by 16 table entries, left alone: BANJO_RENUT_HOOK(sub_823A29C8, GetRenderStateHook<75>);  // D3DDevice_GetRenderState_Wrap15
BANJO_RENUT_HOOK(sub_825628A8, GetRenderStateHook<76>);  // D3DDevice_GetRenderState_ViewportEnable  // oblivion: table entry 77
BANJO_RENUT_HOOK(sub_82562968, GetRenderStateHook<77>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable  // oblivion: table entry 78
BANJO_RENUT_HOOK(sub_82562A20, GetRenderStateHook<78>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable1  // oblivion: table entry 79
BANJO_RENUT_HOOK(sub_82562AD8, GetRenderStateHook<79>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable2  // oblivion: table entry 80
BANJO_RENUT_HOOK(sub_82562B90, GetRenderStateHook<80>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable3  // oblivion: table entry 81
BANJO_RENUT_HOOK(sub_82562C50, GetRenderStateHook<81>);  // D3DDevice_GetRenderState_HalfPixelOffset  // oblivion: table entry 82
BANJO_RENUT_HOOK(sub_82562C88, GetRenderStateHook<82>);  // D3DDevice_GetRenderState_PrimitiveResetEnable  // oblivion: table entry 83
BANJO_RENUT_HOOK(sub_82562CB8, GetRenderStateHook<83>);  // D3DDevice_GetRenderState_PrimitiveResetIndex  // oblivion: table entry 84
BANJO_RENUT_HOOK(sub_82562CE8, GetRenderStateHook<84>);  // D3DDevice_GetRenderState_AlphaToMaskEnable  // oblivion: table entry 85
BANJO_RENUT_HOOK(sub_82562D20, GetRenderStateHook<85>);  // D3DDevice_GetRenderState_AlphaToMaskOffsets  // oblivion: table entry 86
BANJO_RENUT_HOOK(sub_82562D60, GetRenderStateHook<86>);  // D3DDevice_GetRenderState_GuardBand_X  // oblivion: table entry 87
BANJO_RENUT_HOOK(sub_82562D90, GetRenderStateHook<87>);  // D3DDevice_GetRenderState_GuardBand_Y  // oblivion: table entry 88
BANJO_RENUT_HOOK(sub_82562DC0, GetRenderStateHook<88>);  // D3DDevice_GetRenderState_DiscardBand_X  // oblivion: table entry 89
BANJO_RENUT_HOOK(sub_82562DF0, GetRenderStateHook<89>);  // D3DDevice_GetRenderState_DiscardBand_Y  // oblivion: table entry 90
BANJO_RENUT_HOOK(sub_82562E20, GetRenderStateHook<90>);  // D3DDevice_GetRenderState_HiStencilEnable  // oblivion: table entry 91
BANJO_RENUT_HOOK(sub_82562E58, GetRenderStateHook<91>);  // D3DDevice_GetRenderState_HiStencilWriteEnable  // oblivion: table entry 92
BANJO_RENUT_HOOK(sub_82562E90, GetRenderStateHook<92>);  // D3DDevice_GetRenderState_HiStencilFunc  // oblivion: table entry 93
BANJO_RENUT_HOOK(sub_82562EC0, GetRenderStateHook<93>);  // D3DDevice_GetRenderState_HiStencilRef  // oblivion: table entry 94
BANJO_RENUT_HOOK(sub_82562ED0, GetRenderStateHook<94>);  // D3DDevice_GetRenderState_PresentInterval  // oblivion: table entry 95
BANJO_RENUT_HOOK(sub_82562EE8, GetRenderStateHook<95>);  // D3DDevice_GetRenderState_PresentImmediateThreshold  // oblivion: table entry 96
// oblivion: not in XDK 2417: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<96>);  // D3DDevice_GetRenderState_HiZEnable
// oblivion: not in XDK 2417: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<97>);  // D3DDevice_GetRenderState_HiZWriteEnable
// oblivion: not in XDK 2417: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<98>);  // D3DDevice_GetRenderState_LastPixel
// oblivion: not in XDK 2417: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<99>);  // D3DDevice_GetRenderState_LineWidth
// oblivion: not in XDK 2417: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<100>);  // D3DDevice_GetRenderState_Buffer2Frames
BANJO_RENUT_HOOK(sub_82563638, SetSamplerStateHook<0>);  // D3DDevice_SetSamplerState_AddressU  // oblivion: table entry 0
BANJO_RENUT_HOOK(sub_82563688, SetSamplerStateHook<1>);  // D3DDevice_SetSamplerState_AddressV  // oblivion: table entry 1
BANJO_RENUT_HOOK(sub_825636D8, SetSamplerStateHook<2>);  // D3DDevice_SetSamplerState_AddressW  // oblivion: table entry 2
BANJO_RENUT_HOOK(sub_825635C8, SetSamplerStateHook<3>);  // D3DDevice_SetSamplerState_BorderColor  // oblivion: table entry 3
BANJO_RENUT_HOOK(sub_82563088, SetSamplerStateHook<4>);  // D3DDevice_SetSamplerState_MagFilter  // oblivion: table entry 4
BANJO_RENUT_HOOK(sub_82562EF8, SetSamplerStateHook<5>);  // D3DDevice_SetSamplerState_MinFilter  // oblivion: table entry 5
BANJO_RENUT_HOOK(sub_82563218, SetSamplerStateHook<6>);  // D3DDevice_SetSamplerState_MipFilter  // oblivion: table entry 6
BANJO_RENUT_HOOK(sub_82563428, SetSamplerStateHook<7>);  // D3DDevice_SetSamplerState_MipMapLodBias  // oblivion: table entry 7
BANJO_RENUT_HOOK(sub_825634C8, SetSamplerStateHook<8>);  // D3DDevice_SetSamplerState_MaxMipLevel  // oblivion: table entry 8
BANJO_RENUT_HOOK(sub_82563308, SetSamplerStateHook<9>);  // D3DDevice_SetSamplerState_MaxAnisotropy  // oblivion: table entry 9
BANJO_RENUT_HOOK(sub_82563180, SetSamplerStateHook<10>);  // D3DDevice_SetSamplerState_MagFilterZ  // oblivion: table entry 10
BANJO_RENUT_HOOK(sub_82562FF0, SetSamplerStateHook<11>);  // D3DDevice_SetSamplerState_MinFilterZ  // oblivion: table entry 11
BANJO_RENUT_HOOK(sub_82563270, SetSamplerStateHook<12>);  // D3DDevice_SetSamplerState_SeparateZFilterEnable  // oblivion: table entry 12
BANJO_RENUT_HOOK(sub_82563548, SetSamplerStateHook<13>);  // D3DDevice_SetSamplerState_MinMipLevel  // oblivion: table entry 13
BANJO_RENUT_HOOK(sub_82563728, SetSamplerStateHook<14>);  // D3DDevice_SetSamplerState_TrilinearThreshold  // oblivion: table entry 14
BANJO_RENUT_HOOK(sub_82563380, SetSamplerStateHook<15>);  // D3DDevice_SetSamplerState_AnisotropyBias  // oblivion: table entry 15
BANJO_RENUT_HOOK(sub_82563780, SetSamplerStateHook<16>);  // D3DDevice_SetSamplerState_HGradientExpBias  // oblivion: table entry 16
BANJO_RENUT_HOOK(sub_825637D8, SetSamplerStateHook<17>);  // D3DDevice_SetSamplerState_VGradientExpBias  // oblivion: table entry 17
BANJO_RENUT_HOOK(sub_82563830, SetSamplerStateHook<18>);  // D3DDevice_SetSamplerState_WhiteBorderColorW  // oblivion: table entry 18
BANJO_RENUT_HOOK(sub_82563888, SetSamplerStateHook<19>);  // D3DDevice_SetSamplerState_PointBorderEnable  // oblivion: table entry 19
BANJO_RENUT_HOOK(sub_82563670, GetSamplerStateHook<0>);  // D3DDevice_GetSamplerState_AddressU  // oblivion: table entry 0
BANJO_RENUT_HOOK(sub_825636C0, GetSamplerStateHook<1>);  // D3DDevice_GetSamplerState_AddressV  // oblivion: table entry 1
BANJO_RENUT_HOOK(sub_82563710, GetSamplerStateHook<2>);  // D3DDevice_GetSamplerState_AddressW  // oblivion: table entry 2
BANJO_RENUT_HOOK(sub_82563618, GetSamplerStateHook<3>);  // D3DDevice_GetSamplerState_BorderColor  // oblivion: table entry 3
BANJO_RENUT_HOOK(sub_82563148, GetSamplerStateHook<4>);  // D3DDevice_GetSamplerState_MagFilter  // oblivion: table entry 4
BANJO_RENUT_HOOK(sub_82562FB8, GetSamplerStateHook<5>);  // D3DDevice_GetSamplerState_MinFilter  // oblivion: table entry 5
BANJO_RENUT_HOOK(sub_82563258, GetSamplerStateHook<6>);  // D3DDevice_GetSamplerState_MipFilter  // oblivion: table entry 6
BANJO_RENUT_HOOK(sub_82563488, GetSamplerStateHook<7>);  // D3DDevice_GetSamplerState_MipMapLodBias  // oblivion: table entry 7
BANJO_RENUT_HOOK(sub_82563538, GetSamplerStateHook<8>);  // D3DDevice_GetSamplerState_MaxMipLevel  // oblivion: table entry 8
BANJO_RENUT_HOOK(sub_82563370, GetSamplerStateHook<9>);  // D3DDevice_GetSamplerState_MaxAnisotropy  // oblivion: table entry 9
BANJO_RENUT_HOOK(sub_82563208, GetSamplerStateHook<10>);  // D3DDevice_GetSamplerState_MagFilterZ  // oblivion: table entry 10
BANJO_RENUT_HOOK(sub_82563078, GetSamplerStateHook<11>);  // D3DDevice_GetSamplerState_MinFilterZ  // oblivion: table entry 11
BANJO_RENUT_HOOK(sub_825632F8, GetSamplerStateHook<12>);  // D3DDevice_GetSamplerState_SeparateZFilterEnable  // oblivion: table entry 12
BANJO_RENUT_HOOK(sub_825635B8, GetSamplerStateHook<13>);  // D3DDevice_GetSamplerState_MinMipLevel  // oblivion: table entry 13
BANJO_RENUT_HOOK(sub_82563768, GetSamplerStateHook<14>);  // D3DDevice_GetSamplerState_TrilinearThreshold  // oblivion: table entry 14
BANJO_RENUT_HOOK(sub_825633E0, GetSamplerStateHook<15>);  // D3DDevice_GetSamplerState_AnisotropyBias  // oblivion: table entry 15
BANJO_RENUT_HOOK(sub_825637C0, GetSamplerStateHook<16>);  // D3DDevice_GetSamplerState_HGradientExpBias  // oblivion: table entry 16
BANJO_RENUT_HOOK(sub_82563818, GetSamplerStateHook<17>);  // D3DDevice_GetSamplerState_VGradientExpBias  // oblivion: table entry 17
BANJO_RENUT_HOOK(sub_82563870, GetSamplerStateHook<18>);  // D3DDevice_GetSamplerState_WhiteBorderColorW  // oblivion: table entry 18
BANJO_RENUT_HOOK(sub_825638D0, GetSamplerStateHook<19>);  // D3DDevice_GetSamplerState_PointBorderEnable  // oblivion: table entry 19

// Sampler states, by their index in N&B's table (off_82E50BD0).

BANJO_RENUT_HOOK(sub_8255B230, OblivionSetTexture);  // D3DDevice_SetTexture  // katamari-nx: N&B sub_821FE558, signature 0.82 (next 0.39)  // oblivion: katamari sub_824D2640, high  // oblivion: the game's texture's shadow, and 2417's binding (adapter)
// oblivion: not in XDK 2417: BANJO_RENUT_HOOK(sub_824A3D40, D3DDevice_SetTexture_hook);  // D3DDevice_SetTexture_Inline  // katamari-nx: N&B sub_82BF2578, signature 1.00 (next 0.71)
BANJO_RENUT_HOOK(sub_82563C20, OblivionSetStreamSource);  // D3DDevice_SetStreamSource  // katamari-nx: N&B sub_821F9438, signature 0.83 (next 0.38)  // oblivion: katamari sub_824DDFE0, high  // oblivion: 2417's binding (adapter)
BANJO_RENUT_HOOK(sub_82563CC0, OblivionSetIndices);  // D3DDevice_SetIndices  // katamari-nx: N&B sub_821DE438, signature 0.76 (next 0.34)  // oblivion: katamari sub_824DE188, high  // oblivion: 2417's binding (adapter)
BANJO_RENUT_HOOK(sub_82557858, OblivionSetFVF);  // D3DDevice_SetFVF  // oblivion: the FVF's declaration (2417's sub_82553CC0, pure) as Renut's, one per FVF
BANJO_RENUT_HOOK(sub_82557130, OblivionSetVertexDeclaration);  // D3DDevice_SetVertexDeclaration  // katamari-nx: N&B sub_821D5060, D3DDevice_SetVertexDeclaration: 5 instructions, VertexDeclaration +11812 (N&B +11824); sig 0.67/0.43  // oblivion: katamari sub_824D4B48, high  // oblivion: 2417's reference (adapter)
BANJO_RENUT_HOOK(sub_825639B0, D3DDevice_SetViewport_hook);  // D3DDevice_SetViewport  // katamari-nx: N&B sub_821F2090, D3DDevice_SetViewport: sig 0.63/0.30  // oblivion: katamari sub_824DE730, high
// oblivion: not in XDK 2417: BANJO_RENUT_HOOK(sub_824DE4B8, D3D_SetViewport_hook);  // D3D_SetViewport  // katamari-nx: N&B sub_821F2110, D3D_SetViewport: sig 0.64/0.27
BANJO_RENUT_HOOK(sub_825638E8, D3DDevice_SetScissorRect_hook);  // D3DDevice_SetScissorRect  // katamari-nx: N&B sub_821F2368, D3DDevice_SetScissorRect: Present's tail calls it; sig 0.49/0.24  // oblivion: katamari sub_824DDEE0, high
BANJO_RENUT_HOOK(sub_825612A0, D3D_SetSurfaceClip_hook);  // D3D_SetSurfaceClip  // katamari-nx: N&B sub_821F2460, signature 0.83 (next 0.37)  // oblivion: katamari sub_824DB8C8, medium
BANJO_RENUT_HOOK(sub_82564118, OblivionSetRenderTarget);  // D3DDevice_SetRenderTarget  // katamari-nx: N&B sub_82248BD0, D3DDevice_SetRenderTarget: InitializePresentationParameters calls it after CreateSurface, as N&B's; sig 0.62/0.31  // oblivion: katamari sub_824DE868, high  // oblivion: 2417's binding (adapter)
BANJO_RENUT_HOOK(sub_82564480, OblivionSetDepthStencilSurface);  // D3DDevice_SetDepthStencilSurface  // katamari-nx: N&B sub_82248FB0, D3DDevice_SetDepthStencilSurface: likewise after the depth surface; sig 0.71/0.30  // oblivion: katamari sub_824DEBD0, high  // oblivion: 2417's binding (adapter)
BANJO_RENUT_HOOK(sub_82564040, D3D_SetSurfaceInfo_hook);  // D3D_SetSurfaceInfo  // katamari-nx: N&B sub_82248F20, D3D_SetSurfaceInfo: sig 0.57/0.19; full viewport 0x820FAEB4  // oblivion: katamari sub_824DE7D0, high
BANJO_RENUT_HOOK(sub_829B94D8, D3DDevice_SetPredication_hook);  // D3DDevice_SetPredication  // katamari-nx: N&B sub_821F9A48, D3DDevice_SetPredication: sig 0.69 (SetCommandBufferPredication's best is the same function at 0.63: XDK 2.0.5632 has one)  // oblivion: katamari sub_824EFA58, medium
// oblivion: not in XDK 2417: BANJO_RENUT_UNHOOKED_HOOK_SHARED(sub_826411C0, D3DDevice_SetCommandBufferPredication_hook);  // D3DDevice_SetCommandBufferPredication  // katamari-nx: not hooked (D3DDevice_SetCommandBufferPredication: see SetPredication)
// oblivion: not in XDK 2417: BANJO_RENUT_HOOK(sub_824D4CD8, D3DDevice_SetShaderGPRAllocation_hook);  // D3DDevice_SetShaderGPRAllocation  // katamari-nx: N&B sub_821EFD38, signature 0.78 (next 0.30)
BANJO_RENUT_HOOK(sub_8255D608, D3D_CleanupGprAllocationSetup_hook);  // D3D_CleanupGprAllocationSetup  // katamari-nx: N&B sub_82637208, signature 0.98 (next 0.13)  // oblivion: katamari sub_824D3FE0, low
BANJO_RENUT_HOOK(sub_82565720, D3DDevice_SetScreenExtentQueryMode_hook);  // D3DDevice_SetScreenExtentQueryMode  // katamari-nx: N&B sub_826414C0, signature 1.00 (next 0.75)  // oblivion: katamari sub_824E67C8, medium
// oblivion: not in XDK 2417: BANJO_RENUT_HOOK(sub_824D0488, D3DDevice_UnsetAll_hook);  // D3DDevice_UnsetAll  // katamari-nx: N&B sub_82634C10, signature 0.81 (next 0.34)
BANJO_RENUT_HOOK(sub_82563F30, D3D_ResetAllState_hook);  // D3D_ResetAllState  // katamari-nx: N&B sub_82255C98, D3D_ResetAllState: sig 0.73/0.33; the device init sub_824E18D0 calls it last  // oblivion: katamari sub_824DE3C0, medium
// oblivion: not in XDK 2417: BANJO_RENUT_UNHOOKED_HOOK_SHARED(sub_82634B90, D3D_InvalidateConstants_hook);  // D3D_InvalidateConstants  // katamari-nx: not hooked (D3D_InvalidateConstants: ambiguous (0.65 / 0.61))
BANJO_RENUT_HOOK(sub_8255D6D8, D3DDevice_BlockUntilIdle_hook);  // D3DDevice_BlockUntilIdle  // katamari-nx: N&B sub_82640010, signature 0.87 (next 0.63)  // oblivion: katamari sub_824E0518, high
// Through a thunk (j_D3DDevice_SetRenderTarget 0x82636EC8).
BANJO_RENUT_HOOK(sub_82564650, OblivionSetRenderTarget);  // D3DDevice_SetRenderTarget_Thunk  // katamari-nx: not hooked (signature too weak or ambiguous (sub_8257F318 1.00, next 1.00))  // oblivion: katamari sub_82636EC8, high, called by the game  // oblivion: `b 0x82564118` (adapter)
// oblivion: not in XDK 2417: BANJO_RENUT_UNHOOKED_HOOK(sub_82636ED0, D3DDevice_GetRenderTarget_hook);  // D3DDevice_GetRenderTarget  // katamari-nx: not hooked (signature too weak or ambiguous (sub_824B12B8 0.63, next 0.63))
// oblivion: not in XDK 2417: BANJO_RENUT_UNHOOKED_HOOK(sub_82636F10, D3DDevice_GetDepthStencilSurface_hook);  // D3DDevice_GetDepthStencilSurface  // katamari-nx: not hooked (signature too weak or ambiguous (sub_824B12B8 0.69, next 0.69))
// oblivion: not in XDK 2417: BANJO_RENUT_UNHOOKED_HOOK(sub_82636E40, D3DDevice_GetStreamSource_hook);  // D3DDevice_GetStreamSource  // katamari-nx: not hooked (signature too weak or ambiguous (sub_824DE100 0.56, next 0.42))
BANJO_RENUT_HOOK(sub_82563BF8, D3DDevice_GetViewport_hook);  // D3DDevice_GetViewport  // katamari-nx: not hooked (signature too weak or ambiguous (sub_824DC1A0 0.20, next 0.11))  // oblivion: katamari sub_82244B18, high, called by the game
BANJO_RENUT_UNHOOKED_HOOK(sub_82563E60, D3DDevice_SetClipPlane_hook);  // D3DDevice_SetClipPlane  // katamari-nx: not hooked (signature too weak or ambiguous (sub_824D5048 0.34, next 0.33))  // oblivion: katamari sub_826370E8, medium
// oblivion: not in XDK 2417: BANJO_RENUT_UNHOOKED_HOOK(sub_82637138, D3DDevice_SetVertexFetchConstant_hook);  // D3DDevice_SetVertexFetchConstant  // katamari-nx: not hooked (signature too weak or ambiguous (sub_824DE100 0.38, next 0.29))
BANJO_RENUT_HOOK(sub_82555368, OblivionSetVertexShaderConstantF);  // D3DDevice_SetVertexShaderConstantF  // oblivion: (device, start, data, count): 0x780, m_Pending word 0 (2417's word 2)
BANJO_RENUT_HOOK(sub_825554B0, OblivionSetPixelShaderConstantF);  // D3DDevice_SetPixelShaderConstantF  // oblivion: 0x1780, m_Pending word 1 (2417's word 3)
BANJO_RENUT_HOOK(sub_824BD970, OblivionGamebryoVertexB);  // oblivion: Gamebryo's VS BOOL setter (inline single register)
BANJO_RENUT_HOOK(sub_824BDA40, OblivionGamebryoVertexI);  // oblivion: Gamebryo's VS INT setter
BANJO_RENUT_HOOK(sub_824BDB00, OblivionGamebryoVertexF);  // oblivion: Gamebryo's VS FLOAT setter
BANJO_RENUT_HOOK(sub_824BDD50, OblivionGamebryoPixelB);  // oblivion: Gamebryo's PS BOOL setter
BANJO_RENUT_HOOK(sub_824BDE28, OblivionGamebryoPixelI);  // oblivion: Gamebryo's PS INT setter
BANJO_RENUT_HOOK(sub_824BDEF0, OblivionGamebryoPixelF);  // oblivion: Gamebryo's PS FLOAT setter
BANJO_RENUT_HOOK(sub_825555F8, D3DDevice_SetVertexShaderConstantB_hook);  // D3DDevice_SetVertexShaderConstantB  // katamari-nx: N&B sub_821F0B40, signature 0.84 (next 0.77)  // oblivion: katamari sub_824D4470, high
BANJO_RENUT_HOOK(sub_82555650, D3DDevice_SetPixelShaderConstantB_hook);  // D3DDevice_SetPixelShaderConstantB  // katamari-nx: N&B sub_82237208, signature 0.84 (next 0.77)  // oblivion: katamari sub_824D44D0, high
BANJO_RENUT_HOOK(sub_825556A8, D3DDevice_SetVertexShaderConstantI_hook);  // D3DDevice_SetVertexShaderConstantI  // katamari-nx: not hooked (signature too weak or ambiguous (sub_824D4588 0.33, next 0.33))  // oblivion: katamari sub_826374B0, high, called by the game
BANJO_RENUT_HOOK(sub_825556F8, D3DDevice_SetPixelShaderConstantI_hook);  // D3DDevice_SetPixelShaderConstantI  // katamari-nx: not hooked (signature too weak or ambiguous (sub_824D4588 0.33, next 0.33))  // oblivion: katamari sub_82637500, high, called by the game
BANJO_RENUT_UNHOOKED_HOOK(sub_8255E230, D3DDevice_EndIndexedVertices_hook);  // oblivion: OblivionEndIndexedVertices (hooks/draw.cpp) takes it  // D3DDevice_EndIndexedVertices  // katamari-nx: not hooked (signature too weak or ambiguous (sub_8255A1E8 0.63, next 0.31))  // oblivion: katamari sub_8263ED90, high, called by the game

// Render state getters, by their index in N&B's table (off_82E506F0, field 0).

// Sampler state getters (off_82E50BD0, field 0).
