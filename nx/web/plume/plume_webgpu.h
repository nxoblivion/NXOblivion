//
// plume WebGPU back end (reOblivion web, 2026-11-18). GPL-3.0 with the rest of the port.
//
// The browser's WebGPU, which only the page's thread may drive while the game runs on workers (PROXY_TO_PTHREAD):
// this side (plume_webgpu.cpp, any thread) records plume's calls as command words and posts them in blocks to
// web/plume/webgpu_server.js (an Emscripten --pre-js) on the page's thread, which makes the WebGPU calls. Fences and the
// device's start-up are words in shared memory the server sets and the workers wait on (futexes).
//
// Shaders are RenderShaderFormat::WGSL: a WebGpuShaderBlob ("WGS1": stage, the binding line of tools/webgpu_glsl.py,
// the WGSL tint made). Renut's shaders index their textures and samplers by words of SharedConstants or of the push
// constants (bindless); WebGPU has no arrays of textures, so the converter gave each texture the shader reads a binding
// of its own, and the binding line says where its word is: this side reads the word from the CPU's copy of the bound
// constants at each draw and binds the descriptor set's texture at that index (bind group 0).
//
// Buffers: an upload or readback buffer has a CPU copy (map() returns it); at each submit the ranges of upload buffers
// the lists read (constant ranges, vertex and index views, copy sources, the vertex fetch window) are written to the
// GPU first (queue.writeBuffer). getDeviceAddress() is (id << 32): the deko3d-flavoured shaders read vertex fetch data
// from a storage binding at the address's low word, so the binding is the buffer the address's high word names.
//

#pragma once

#include "plume_render_interface.h"

#include <memory>

namespace plume {
    std::unique_ptr<RenderInterface> CreateWebGPUInterface();

    // RenderShaderFormat::WGSL's container: this header, the binding line (meta), then the WGSL (not terminated).
    struct WebGpuShaderBlob {
        static constexpr uint32_t Magic = 0x31534757;  // "WGS1"
        uint32_t magic = Magic;
        uint32_t stage = 0;     // 0 vertex, 3 geometry (none), 4 fragment, 5 compute
        uint32_t metaSize = 0;
        uint32_t wgslSize = 0;
    };

    // The blend constant of the next draws (plume has no call for it; Renut's pipelines use BLEND_FACTOR).
    void WebGPUSetBlendConstant(RenderCommandList *commandList, const float rgba[4]);
    // Whether a graphics or compute pipeline has a WebGPU pipeline (its shaders had a WebGPU form).
    bool WebGPUPipelineCreated(const RenderPipeline *pipeline);
    // The device was lost (the browser's GPU process restarted, or the tab's GPU was taken away).
    bool WebGPUDeviceLost(const RenderDevice *device);
    // A line on the browser's console and the page's log.
    void WebGPULog(const char *message);
    // The canvas's size in device pixels (the page's layout, not SDL's window), once the device is up.
    bool WebGPUCanvasSize(uint32_t &width, uint32_t &height);
}
