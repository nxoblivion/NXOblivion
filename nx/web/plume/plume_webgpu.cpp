//
// plume WebGPU back end (reOblivion web, 2026-11-18): the client side; see plume_webgpu.h and webgpu_server.js, whose
// OP table must match WebGpuOp here. GPL-3.0 with the rest of the port.
//

#include "plume_webgpu.h"

#include <emscripten.h>
#include <emscripten/threading.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace plume {
namespace {

enum WebGpuOp : uint32_t {
    OP_CREATE_BUFFER = 1, OP_DESTROY = 2, OP_CREATE_TEXTURE = 3, OP_CREATE_VIEW = 4, OP_CREATE_SAMPLER = 5,
    OP_CREATE_SHADER = 6, OP_CREATE_BGL = 7, OP_CREATE_PIPELINE_LAYOUT = 8, OP_CREATE_RENDER_PIPELINE = 9,
    OP_CREATE_COMPUTE_PIPELINE = 10, OP_CREATE_BIND_GROUP = 11, OP_WRITE_BUFFER = 12, OP_SUBMIT = 13, OP_PRESENT = 14,
    OP_CREATE_FRAMEBUFFER = 15, OP_WRITE_TEXTURE = 16, OP_SIGNAL = 17, OP_CONFIGURE = 18, OP_LOG = 19,
    // command list ops (inside SUBMIT)
    OP_SET_FRAMEBUFFER = 100, OP_CLEAR_COLOR = 101, OP_CLEAR_DS = 102, OP_SET_PIPELINE = 103, OP_SET_BIND_GROUP = 104,
    OP_SET_VERTEX_BUFFER = 105, OP_SET_INDEX_BUFFER = 106, OP_SET_VIEWPORT = 107, OP_SET_SCISSOR = 108,
    OP_SET_BLEND_CONSTANT = 109, OP_SET_STENCIL_REF = 110, OP_DRAW = 111, OP_DRAW_INDEXED = 112, OP_DISPATCH = 113,
    OP_COPY_B2B = 114, OP_COPY_B2T = 115, OP_COPY_T2B = 116, OP_COPY_T2T = 117, OP_RESOLVE = 118, OP_BARRIER = 119,
    OP_SET_COMPUTE_PIPELINE = 120, OP_SET_COMPUTE_BIND_GROUP = 121, OP_SET_PUSH = 122, OP_SET_COMPUTE_PUSH = 123,
};

// Objects the server makes itself at start-up (webgpu_server.js init); the client's ids start after them.
enum ReservedId : uint32_t {
    ID_PUSH_RING = 1, ID_DUMMY_2D = 2, ID_DUMMY_CUBE = 3, ID_DUMMY_2D_ARRAY = 4, ID_DUMMY_3D = 5, ID_DUMMY_SAMPLER = 6,
    ID_DUMMY_2D_UINT = 7, ID_DUMMY_2D_SINT = 8, ID_DUMMY_STORAGE = 9, ID_DUMMY_2D_UNFILTERABLE = 10, ID_FIRST = 16,
};

// The status block's words (webgpu_server.js).
enum StatusWord : uint32_t {
    ST_STATE = 0, ST_FEATURES = 1, ST_CANVAS_W = 2, ST_CANVAS_H = 3, ST_PRESENTS = 4, ST_MAX_TEX_2D = 5,
    ST_MAX_STORAGE_MB = 6, ST_MAX_INTERSTAGE = 7, ST_MAX_VERTEX_BUFFERS = 8, ST_MAX_COLOR_BYTES = 9, ST_ERRORS = 10,
    ST_CONSUMED = 11, ST_COUNT = 32,
};
enum FeatureBit : uint32_t { FEAT_BC = 1, FEAT_FLOAT32_FILTERABLE = 2, FEAT_D32S8 = 4, FEAT_DUAL_SOURCE = 8 };

// View dimensions, as webgpu_server.js's VIEW_DIM.
enum ViewDim : uint32_t { VD_2D = 2, VD_1D = 1, VD_3D = 3, VD_CUBE = 4, VD_2D_ARRAY = 5, VD_CUBE_ARRAY = 6 };
// Sample types, as SAMPLE_TYPE.
enum SampleKind : uint32_t { SK_FLOAT = 0, SK_UNFILTERABLE = 1, SK_UINT = 2, SK_SINT = 3, SK_DEPTH = 4 };
enum Visibility : uint32_t { VIS_VERTEX = 1, VIS_FRAGMENT = 2, VIS_COMPUTE = 4 };
// Bind group layout entry kinds, as bglEntry.
enum EntryKind : uint32_t { EK_UBO = 1, EK_STORAGE_READ = 2, EK_STORAGE = 3, EK_SAMPLER = 4, EK_SAMPLER_NF = 5, EK_TEXTURE = 6 };

alignas(64) int32_t g_status[ST_COUNT];
// Fence words: never freed (the server may still set one after its fence is gone).
int32_t g_fenceWords[4096];
std::atomic<uint32_t> g_fenceNext{0};

inline int32_t LoadStatus(uint32_t i) { return __atomic_load_n(&g_status[i], __ATOMIC_ACQUIRE); }

void WaitWord(int32_t *word, int32_t atLeast) {
    for (;;) {
        const int32_t v = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        if (v >= atLeast || LoadStatus(ST_STATE) < 0)
            return;
        emscripten_futex_wait(word, uint32_t(v), 100.0);
    }
}

void WaitNonZero(int32_t *word) {
    for (;;) {
        const int32_t v = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        if (v != 0)
            return;
        emscripten_futex_wait(word, 0, 100.0);
    }
}

uint32_t FloatBits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

struct Words {
    std::vector<uint32_t> w;
    size_t begin(uint32_t op) {
        w.push_back(op);
        w.push_back(0);
        return w.size();
    }
    void end(size_t at) { w[at - 1] = uint32_t(w.size() - at); }
    void u(uint32_t v) { w.push_back(v); }
    void i(int32_t v) { w.push_back(uint32_t(v)); }
    void u64(uint64_t v) {
        w.push_back(uint32_t(v));
        w.push_back(uint32_t(v >> 32));
    }
    void f(float v) { w.push_back(FloatBits(v)); }
    void bytes(const void *p, size_t n) {
        const size_t at = w.size();
        w.resize(at + (n + 3) / 4, 0);
        if (n)
            memcpy(&w[at], p, n);
    }
};

// ---- formats ----------------------------------------------------------------------------------------------------

bool IsDepthFormat(RenderFormat f) {
    return f == RenderFormat::D32_FLOAT || f == RenderFormat::D32_FLOAT_S8_UINT || f == RenderFormat::D16_UNORM ||
           f == RenderFormat::D24_UNORM_S8_UINT;
}
bool HasStencil(RenderFormat f) {
    return f == RenderFormat::D32_FLOAT_S8_UINT || f == RenderFormat::D24_UNORM_S8_UINT || f == RenderFormat::S8_UINT;
}
bool IsUintFormat(RenderFormat f) {
    switch (f) {
    case RenderFormat::R32G32B32A32_UINT: case RenderFormat::R16G16B16A16_UINT: case RenderFormat::R32G32_UINT:
    case RenderFormat::R8G8B8A8_UINT: case RenderFormat::R16G16_UINT: case RenderFormat::R32_UINT:
    case RenderFormat::R8G8_UINT: case RenderFormat::R16_UINT: case RenderFormat::R8_UINT: case RenderFormat::S8_UINT:
        return true;
    default:
        return false;
    }
}
bool IsSintFormat(RenderFormat f) {
    switch (f) {
    case RenderFormat::R32G32B32A32_SINT: case RenderFormat::R16G16B16A16_SINT: case RenderFormat::R32G32_SINT:
    case RenderFormat::R8G8B8A8_SINT: case RenderFormat::R16G16_SINT: case RenderFormat::R32_SINT:
    case RenderFormat::R8G8_SINT: case RenderFormat::R16_SINT: case RenderFormat::R8_SINT:
        return true;
    default:
        return false;
    }
}
bool Is32Float(RenderFormat f) {
    switch (f) {
    case RenderFormat::R32G32B32A32_TYPELESS: case RenderFormat::R32G32B32A32_FLOAT: case RenderFormat::R32G32_TYPELESS:
    case RenderFormat::R32G32_FLOAT: case RenderFormat::R32_TYPELESS: case RenderFormat::R32_FLOAT:
        return true;
    default:
        return false;
    }
}
bool IsBlockFormat(RenderFormat f) { return f >= RenderFormat::BC1_TYPELESS && f <= RenderFormat::BC7_UNORM_SRGB; }
bool StorageCapable(RenderFormat f) {
    switch (f) {
    case RenderFormat::R8G8B8A8_UNORM: case RenderFormat::R8G8B8A8_SNORM: case RenderFormat::R8G8B8A8_UINT:
    case RenderFormat::R8G8B8A8_SINT: case RenderFormat::R16G16B16A16_FLOAT: case RenderFormat::R16G16B16A16_UINT:
    case RenderFormat::R16G16B16A16_SINT: case RenderFormat::R32G32B32A32_FLOAT: case RenderFormat::R32G32B32A32_UINT:
    case RenderFormat::R32G32B32A32_SINT: case RenderFormat::R32_FLOAT: case RenderFormat::R32_UINT:
    case RenderFormat::R32_SINT: case RenderFormat::R32G32_FLOAT: case RenderFormat::R32G32_UINT:
    case RenderFormat::R32G32_SINT:
        return true;
    default:
        return false;
    }
}

// The sample kinds a view of this format (and aspect) can bind as.
bool KindFits(RenderFormat f, uint32_t aspect, uint32_t kind, bool float32Filterable) {
    if (HasStencil(f) && (aspect == 2 || f == RenderFormat::S8_UINT))
        return kind == SK_UINT;
    if (IsDepthFormat(f))
        return kind == SK_UNFILTERABLE || kind == SK_DEPTH;
    if (IsUintFormat(f))
        return kind == SK_UINT;
    if (IsSintFormat(f))
        return kind == SK_SINT;
    if (kind == SK_UNFILTERABLE)
        return true;
    if (kind == SK_FLOAT)
        return !Is32Float(f) || float32Filterable;
    return false;
}

// ---- the stream to the server -----------------------------------------------------------------------------------

struct WebGpuCommandList;
struct BindGroupCache;

// Draws recorded and dropped before the server saw them, on the console every 10 s while any were dropped (a draw
// without a WebGPU pipeline draws nothing and says nothing otherwise).
struct DrawStats {
    std::atomic<uint64_t> drawn{0}, noPipeline{0}, noForm{0}, noFramebuffer{0};
    std::atomic<uint64_t> pipelinesMade{0}, pipelinesWithoutForm{0};
    double last = 0;
};
DrawStats g_drawStats;

void ReportDrawStats() {  // with Global's mutex held (the one submitting)
    DrawStats &d = g_drawStats;
    const double now = emscripten_get_now();
    if (now - d.last < 10000.0)
        return;
    d.last = now;
    const uint64_t drawn = d.drawn.exchange(0), noPipeline = d.noPipeline.exchange(0), noForm = d.noForm.exchange(0),
                   noFramebuffer = d.noFramebuffer.exchange(0);
    if (noPipeline + noForm + noFramebuffer)
        std::fprintf(stderr, "[webgpu] 10 s: %llu draws, dropped %llu without a pipeline, %llu whose pipeline has no WebGPU "
                     "form, %llu without a framebuffer; pipelines %llu made, %llu without a form\n",
                     (unsigned long long)drawn, (unsigned long long)noPipeline, (unsigned long long)noForm,
                     (unsigned long long)noFramebuffer, (unsigned long long)d.pipelinesMade.load(),
                     (unsigned long long)d.pipelinesWithoutForm.load());
}

struct Global {
    std::mutex mutex;
    Words stream;
    std::deque<std::pair<int32_t, void *>> posted;  // blocks the server has not consumed yet
    int32_t seq = 0;
    std::vector<uint32_t> pendingDestroys;
    std::vector<uint32_t> idleDestroys;  // destroyed once no command list is open
    int openLists = 0;
    std::atomic<uint32_t> nextId{ID_FIRST};
    bool ready = false;
    uint32_t features = 0;

    uint32_t newId() { return nextId.fetch_add(1, std::memory_order_relaxed); }

    void flushLocked() {
        while (!posted.empty() && posted.front().first <= LoadStatus(ST_CONSUMED)) {
            free(posted.front().second);
            posted.pop_front();
        }
        if (stream.w.empty())
            return;
        const size_t bytes = stream.w.size() * 4;
        void *block = malloc(bytes);
        memcpy(block, stream.w.data(), bytes);
        stream.w.clear();
        const int32_t s = ++seq;
        posted.emplace_back(s, block);
        MAIN_THREAD_ASYNC_EM_ASM({ WebGpuServer.process($0, $1, $2); }, double(uintptr_t(block)), double(bytes),
                                 double(s));
    }

    void flush() {
        std::lock_guard<std::mutex> lock(mutex);
        flushLocked();
    }

    // An object's id goes: after the next submit (a list being recorded may still name it), or once no list is open.
    void destroyLocked(uint32_t id);

    void emitDestroysLocked() {
        if (openLists <= 0) {
            pendingDestroys.insert(pendingDestroys.end(), idleDestroys.begin(), idleDestroys.end());
            idleDestroys.clear();
        }
        if (pendingDestroys.empty())
            return;
        const size_t at = stream.begin(OP_DESTROY);
        for (uint32_t id : pendingDestroys)
            stream.u(id);
        stream.end(at);
        pendingDestroys.clear();
    }

    void log(const std::string &text) {
        std::lock_guard<std::mutex> lock(mutex);
        const size_t at = stream.begin(OP_LOG);
        stream.u(uint32_t(text.size()));
        stream.bytes(text.data(), text.size());
        stream.end(at);
        flushLocked();
    }
};

Global &G() {
    static Global *g = new Global();
    return *g;
}

void Logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void Logf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    G().log(buf);
}

// ---- bind group cache -----------------------------------------------------------------------------------------

struct VecHash {
    size_t operator()(const std::vector<uint32_t> &v) const {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t x : v) {
            h ^= x;
            h *= 1099511628211ull;
        }
        return size_t(h ^ (h >> 29));
    }
};

// Bind groups and layouts by their contents (all under Global::mutex). A bind group naming an object that goes is
// dropped (its id destroyed with the object's).
struct Cache {
    std::unordered_map<std::vector<uint32_t>, uint32_t, VecHash> bindGroups;
    std::unordered_map<uint32_t, std::vector<std::vector<uint32_t>>> byObject;
    std::unordered_map<std::vector<uint32_t>, uint32_t, VecHash> layouts;  // bind group layouts
    std::unordered_map<std::vector<uint32_t>, uint32_t, VecHash> pipelineLayouts;
    uint32_t emptyLayout = 0;

    void forget(uint32_t objectId, Global &g) {
        auto it = byObject.find(objectId);
        if (it == byObject.end())
            return;
        for (auto &key : it->second) {
            auto b = bindGroups.find(key);
            if (b != bindGroups.end()) {
                g.pendingDestroys.push_back(b->second);
                bindGroups.erase(b);
            }
        }
        byObject.erase(it);
    }
};

Cache &C() {
    static Cache *c = new Cache();
    return *c;
}

void Global::destroyLocked(uint32_t id) {
    C().forget(id, *this);
    if (openLists > 0)
        idleDestroys.push_back(id);
    else
        pendingDestroys.push_back(id);
}

void DestroyId(uint32_t id) {
    if (!id)
        return;
    Global &g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    g.destroyLocked(id);
}

// A bind group layout from (binding, visibility, kind, x, y, z) entries.
uint32_t LayoutLocked(const std::vector<uint32_t> &entries) {
    Global &g = G();
    Cache &c = C();
    auto it = c.layouts.find(entries);
    if (it != c.layouts.end())
        return it->second;
    const uint32_t id = g.newId();
    const size_t at = g.stream.begin(OP_CREATE_BGL);
    g.stream.u(id);
    g.stream.u(uint32_t(entries.size() / 6));
    for (uint32_t x : entries)
        g.stream.u(x);
    g.stream.end(at);
    c.layouts.emplace(entries, id);
    return id;
}

uint32_t PipelineLayoutLocked(const uint32_t (&groups)[4], uint32_t count) {
    Global &g = G();
    Cache &c = C();
    std::vector<uint32_t> key(groups, groups + count);
    auto it = c.pipelineLayouts.find(key);
    if (it != c.pipelineLayouts.end())
        return it->second;
    const uint32_t id = g.newId();
    const size_t at = g.stream.begin(OP_CREATE_PIPELINE_LAYOUT);
    g.stream.u(id);
    g.stream.u(count);
    for (uint32_t k = 0; k < count; k++)
        g.stream.u(groups[k]);
    g.stream.end(at);
    c.pipelineLayouts.emplace(std::move(key), id);
    return id;
}

// entries: (binding, kind 1 buffer / 0 other, object, offset, size) quintets.
uint32_t BindGroupLocked(uint32_t layout, const std::vector<uint32_t> &entries) {
    Global &g = G();
    Cache &c = C();
    std::vector<uint32_t> key;
    key.reserve(entries.size() + 1);
    key.push_back(layout);
    key.insert(key.end(), entries.begin(), entries.end());
    auto it = c.bindGroups.find(key);
    if (it != c.bindGroups.end())
        return it->second;
    if (c.bindGroups.size() > 32768) {
        for (auto &kv : c.bindGroups)
            g.pendingDestroys.push_back(kv.second);
        c.bindGroups.clear();
        c.byObject.clear();
    }
    const uint32_t id = g.newId();
    const size_t at = g.stream.begin(OP_CREATE_BIND_GROUP);
    g.stream.u(id);
    g.stream.u(layout);
    g.stream.u(uint32_t(entries.size() / 5));
    for (uint32_t x : entries)
        g.stream.u(x);
    g.stream.end(at);
    for (size_t k = 0; k + 4 < entries.size(); k += 5) {
        const uint32_t obj = entries[k + 2];
        if (obj >= ID_FIRST)
            c.byObject[obj].push_back(key);
    }
    c.bindGroups.emplace(std::move(key), id);
    return id;
}

// ---- objects ------------------------------------------------------------------------------------------------------

struct WebGpuDevice;

struct WebGpuBufferFormattedView : RenderBufferFormattedView {};

struct WebGpuBuffer : RenderBuffer {
    uint32_t id = 0;
    RenderBufferDesc desc;
    uint8_t *shadow = nullptr;  // upload and readback heaps

    explicit WebGpuBuffer(const RenderBufferDesc &d) : desc(d) {
        id = G().newId();
        const bool cpu = desc.heapType == RenderHeapType::UPLOAD || desc.heapType == RenderHeapType::READBACK ||
                         desc.heapType == RenderHeapType::GPU_UPLOAD;
        if (cpu) {
            const size_t size = (desc.size + 255) & ~size_t(255);
            shadow = static_cast<uint8_t *>(aligned_alloc(256, size ? size : 256));
            memset(shadow, 0, size ? size : 256);
        }
        Global &g = G();
        std::lock_guard<std::mutex> lock(g.mutex);
        const size_t at = g.stream.begin(OP_CREATE_BUFFER);
        g.stream.u(id);
        g.stream.u64(desc.size);
        g.stream.u(desc.heapType == RenderHeapType::READBACK ? 2 : desc.heapType == RenderHeapType::DEFAULT ? 0 : 1);
        g.stream.u64(uint64_t(uintptr_t(shadow)));
        g.stream.end(at);
    }
    ~WebGpuBuffer() override {
        DestroyId(id);
        // The server may still read the copy (a submit not yet processed): freed once it has caught up.
        if (shadow) {
            Global &g = G();
            g.flush();
            std::lock_guard<std::mutex> lock(g.mutex);
            const int32_t s = g.seq;
            g.posted.emplace_back(s, shadow);
        }
    }
    void *map(uint32_t, const RenderRange *) override { return shadow; }
    void unmap(uint32_t, const RenderRange *) override {}
    std::unique_ptr<RenderBufferFormattedView> createBufferFormattedView(RenderFormat) override {
        return std::make_unique<WebGpuBufferFormattedView>();
    }
    void setName(const std::string &) override {}
    uint64_t getDeviceAddress() const override { return uint64_t(id) << 32; }
};

struct WebGpuTexture;

struct WebGpuTextureView : RenderTextureView {
    uint32_t id = 0;
    const WebGpuTexture *texture = nullptr;
    uint32_t dim = VD_2D;
    uint32_t aspect = 0;
    RenderFormat format = RenderFormat::UNKNOWN;
    ~WebGpuTextureView() override { DestroyId(id); }
};

uint32_t ViewAspect(RenderFormat textureFormat, RenderFormat viewFormat) {
    if (!IsDepthFormat(textureFormat) && textureFormat != RenderFormat::S8_UINT)
        return 0;
    if (textureFormat == RenderFormat::S8_UINT)
        return 0;
    if (HasStencil(textureFormat) && (IsUintFormat(viewFormat) || viewFormat == RenderFormat::S8_UINT))
        return 2;
    return 1;
}

uint32_t CreateViewLocked(const WebGpuTexture *t, RenderFormat format, uint32_t dim, uint32_t baseMip, uint32_t mips,
                          uint32_t baseLayer, uint32_t layers, uint32_t aspect);

struct WebGpuTexture : RenderTexture {
    uint32_t id = 0;
    RenderTextureDesc desc;
    uint32_t layers = 1;
    bool external = false;  // a swap chain's
    mutable std::mutex viewMutex;
    mutable std::map<uint32_t, uint32_t> derived;  // (dim | aspect << 8) -> view id

    explicit WebGpuTexture(const RenderTextureDesc &d) : desc(d) {
        desc.optimizedClearValue = nullptr;
        id = G().newId();
        layers = desc.dimension == RenderTextureDimension::TEXTURE_3D ? 1 : std::max(1u, desc.arraySize);
        uint32_t flags = 0;
        if (desc.flags & RenderTextureFlag::RENDER_TARGET)
            flags |= 1;
        if (desc.flags & RenderTextureFlag::DEPTH_TARGET)
            flags |= 2;
        if ((desc.flags & (RenderTextureFlag::STORAGE | RenderTextureFlag::UNORDERED_ACCESS)) && StorageCapable(desc.format))
            flags |= 4;
        Global &g = G();
        std::lock_guard<std::mutex> lock(g.mutex);
        const size_t at = g.stream.begin(OP_CREATE_TEXTURE);
        g.stream.u(id);
        g.stream.u(uint32_t(desc.dimension));
        g.stream.u(std::max(1u, desc.width));
        g.stream.u(std::max(1u, desc.height));
        g.stream.u(desc.dimension == RenderTextureDimension::TEXTURE_3D ? std::max(1u, desc.depth) : layers);
        g.stream.u(std::max(1u, desc.mipLevels));
        g.stream.u(1);  // single-sampled only (getSampleCountsSupported)
        g.stream.u(uint32_t(desc.format));
        g.stream.u(flags);
        g.stream.end(at);
    }
    ~WebGpuTexture() override {
        Global &g = G();
        std::lock_guard<std::mutex> lock(g.mutex);
        for (auto &kv : derived)
            g.destroyLocked(kv.second);
        g.destroyLocked(id);
    }

    uint32_t defaultDim() const {
        if (desc.dimension == RenderTextureDimension::TEXTURE_3D)
            return VD_3D;
        if ((desc.flags & RenderTextureFlag::CUBE) && layers >= 6)
            return layers > 6 ? VD_CUBE_ARRAY : VD_CUBE;
        return layers > 1 ? VD_2D_ARRAY : VD_2D;
    }

    // A view of the whole texture as dim (Global::mutex held).
    uint32_t viewLocked(uint32_t dim, uint32_t aspect) const {
        if (dim == VD_3D && desc.dimension != RenderTextureDimension::TEXTURE_3D)
            return 0;
        if (dim != VD_3D && desc.dimension == RenderTextureDimension::TEXTURE_3D)
            return 0;
        if ((dim == VD_CUBE || dim == VD_CUBE_ARRAY) && (layers < 6 || desc.width != desc.height))
            return 0;
        const uint32_t key = dim | (aspect << 8);
        std::lock_guard<std::mutex> lock(viewMutex);
        auto it = derived.find(key);
        if (it != derived.end())
            return it->second;
        uint32_t count = layers;
        if (dim == VD_2D || dim == VD_1D || dim == VD_3D)
            count = 1;
        else if (dim == VD_CUBE)
            count = 6;
        else if (dim == VD_CUBE_ARRAY)
            count = layers / 6 * 6;
        const uint32_t v = CreateViewLocked(this, desc.format, dim, 0, std::max(1u, desc.mipLevels), 0, count, aspect);
        derived.emplace(key, v);
        return v;
    }

    std::unique_ptr<RenderTextureView> createTextureView(const RenderTextureViewDesc &d) const override {
        auto view = std::make_unique<WebGpuTextureView>();
        view->texture = this;
        view->format = d.format == RenderFormat::UNKNOWN ? desc.format : d.format;
        const uint32_t baseLayer = std::min(d.arrayIndex, layers - 1);
        const uint32_t available = layers - baseLayer;
        uint32_t count = d.arraySize == UINT32_MAX ? available : std::min(d.arraySize, available);
        switch (d.dimension) {
        case RenderTextureViewDimension::TEXTURE_3D:
            view->dim = VD_3D;
            count = 1;
            break;
        case RenderTextureViewDimension::TEXTURE_CUBE:
            view->dim = count > 6 ? VD_CUBE_ARRAY : VD_CUBE;
            count = view->dim == VD_CUBE ? 6 : count / 6 * 6;
            break;
        case RenderTextureViewDimension::TEXTURE_1D:
        case RenderTextureViewDimension::TEXTURE_2D:
        default:
            view->dim = count > 1 ? VD_2D_ARRAY : VD_2D;
            break;
        }
        const uint32_t mips = std::max(1u, desc.mipLevels);
        const uint32_t baseMip = std::min(d.mipSlice, mips - 1);
        const uint32_t mipCount = d.mipLevels == UINT32_MAX ? mips - baseMip : std::min(d.mipLevels, mips - baseMip);
        view->aspect = ViewAspect(desc.format, view->format);
        Global &g = G();
        std::lock_guard<std::mutex> lock(g.mutex);
        view->id = CreateViewLocked(this, view->format, view->dim, baseMip, std::max(1u, mipCount), baseLayer,
                                    std::max(1u, count), view->aspect);
        return view;
    }
    void setName(const std::string &) override {}
};

uint32_t CreateViewLocked(const WebGpuTexture *t, RenderFormat format, uint32_t dim, uint32_t baseMip, uint32_t mips,
                          uint32_t baseLayer, uint32_t layers, uint32_t aspect) {
    Global &g = G();
    const uint32_t id = g.newId();
    const size_t at = g.stream.begin(OP_CREATE_VIEW);
    g.stream.u(id);
    g.stream.u(t->id);
    g.stream.u(uint32_t(format));
    g.stream.u(dim);
    g.stream.u(baseMip);
    g.stream.u(mips);
    g.stream.u(baseLayer);
    g.stream.u(layers);
    g.stream.u(aspect);
    g.stream.end(at);
    return id;
}

struct WebGpuSampler : RenderSampler {
    uint32_t id = 0;
    bool comparison = false;
    explicit WebGpuSampler(const RenderSamplerDesc &d) {
        id = G().newId();
        comparison = d.comparisonEnabled;
        Global &g = G();
        std::lock_guard<std::mutex> lock(g.mutex);
        const size_t at = g.stream.begin(OP_CREATE_SAMPLER);
        g.stream.u(id);
        g.stream.u(uint32_t(d.minFilter));
        g.stream.u(uint32_t(d.magFilter));
        g.stream.u(uint32_t(d.mipmapMode));
        g.stream.u(uint32_t(d.addressU));
        g.stream.u(uint32_t(d.addressV));
        g.stream.u(uint32_t(d.addressW));
        g.stream.f(std::max(0.0f, d.minLOD));
        g.stream.f(std::min(32.0f, std::max(d.minLOD, d.maxLOD)));
        g.stream.u(d.comparisonEnabled ? uint32_t(d.comparisonFunc) : 0);
        g.stream.u(d.anisotropyEnabled ? std::max(1u, d.maxAnisotropy) : 1);
        g.stream.end(at);
    }
    ~WebGpuSampler() override { DestroyId(id); }
};

struct WebGpuDescriptorSet : RenderDescriptorSet {
    struct Slot {
        const WebGpuBuffer *buffer = nullptr;
        uint64_t size = 0;
        const WebGpuTexture *texture = nullptr;
        const WebGpuTextureView *view = nullptr;
        const WebGpuSampler *sampler = nullptr;
    };
    struct Range {
        uint32_t binding, base, count;
        RenderDescriptorRangeType type;
    };
    std::vector<Slot> slots;
    std::vector<Range> ranges;

    explicit WebGpuDescriptorSet(const RenderDescriptorSetDesc &d) {
        uint32_t base = 0;
        for (uint32_t r = 0; r < d.descriptorRangesCount; r++) {
            const RenderDescriptorRange &range = d.descriptorRanges[r];
            uint32_t count = range.count;
            if (d.lastRangeIsBoundless && r + 1 == d.descriptorRangesCount)
                count = std::max(count, d.boundlessRangeSize);
            ranges.push_back({range.binding, base, count, range.type});
            base += count;
        }
        slots.resize(base);
    }
    void setBuffer(uint32_t index, const RenderBuffer *buffer, uint64_t size, const RenderBufferStructuredView *,
                   const RenderBufferFormattedView *) override {
        if (index >= slots.size())
            return;
        slots[index].buffer = static_cast<const WebGpuBuffer *>(buffer);
        slots[index].size = size;
    }
    void setTexture(uint32_t index, const RenderTexture *texture, RenderTextureLayout, const RenderTextureView *view) override {
        if (index >= slots.size())
            return;
        slots[index].texture = static_cast<const WebGpuTexture *>(texture);
        slots[index].view = static_cast<const WebGpuTextureView *>(view);
    }
    void setSampler(uint32_t index, const RenderSampler *sampler) override {
        if (index >= slots.size())
            return;
        slots[index].sampler = static_cast<const WebGpuSampler *>(sampler);
    }
    void setAccelerationStructure(uint32_t, const RenderAccelerationStructure *) override {}

    const Slot *atBinding(uint32_t binding) const {
        for (const Range &r : ranges)
            if (r.binding == binding && r.count)
                return &slots[r.base];
        return nullptr;
    }
};

// ---- shaders ----------------------------------------------------------------------------------------------------

struct TexBind {
    uint32_t binding = 0, dim = VD_2D, heapSet = 0, word = 0, kind = SK_FLOAT, visibility = 0;
    bool push = false, multisampled = false;
};
struct SmpBind {
    uint32_t binding = 0, heapSet = 0, word = 0, visibility = 0;
    bool push = false;
};
struct SsboBind {
    uint32_t slot = 0, set = 0, binding = 0, visibility = 0;
    bool write = false;
};

struct WebGpuShader : RenderShader {
    uint32_t id = 0;
    uint32_t stage = 0;
    std::vector<TexBind> tex;
    std::vector<SmpBind> smp;
    std::vector<uint32_t> ubo;
    std::vector<SsboBind> ssbo;
    uint32_t push = 0;
    std::vector<uint32_t> vin;
    std::string name;
    bool valid = false;

    ~WebGpuShader() override { DestroyId(id); }
    void setName(const std::string &n) override { name = n; }
};

std::vector<std::string> Split(const std::string &s, char c) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t at = s.find(c, start);
        out.push_back(s.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos)
            break;
        start = at + 1;
    }
    return out;
}

void ParseMeta(WebGpuShader &sh, const std::string &meta) {
    const uint32_t vis = sh.stage == 0 ? VIS_VERTEX : sh.stage == 5 ? VIS_COMPUTE : VIS_FRAGMENT;
    for (const std::string &field : Split(meta, ' ')) {
        const size_t eq = field.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = field.substr(0, eq), value = field.substr(eq + 1);
        if (key == "tex") {
            for (const std::string &e : Split(value, ',')) {
                const auto p = Split(e, ':');  // binding:type:heap set:<s|p>word:<f|u>
                if (p.size() < 4 || p[3].size() < 2)
                    continue;
                TexBind t;
                t.binding = uint32_t(atoi(p[0].c_str()));
                const std::string &type = p[1];
                t.heapSet = uint32_t(atoi(p[2].c_str()));
                t.push = p[3][0] == 'p';
                t.word = uint32_t(atoi(p[3].c_str() + 1));
                const bool sampled = p.size() < 5 || p[4] == "f";
                t.visibility = vis;
                std::string base = type;
                uint32_t kind = sampled ? SK_FLOAT : SK_UNFILTERABLE;
                if (base[0] == 'u') {
                    kind = SK_UINT;
                    base = base.substr(1);
                } else if (base[0] == 'i') {
                    kind = SK_SINT;
                    base = base.substr(1);
                }
                if (base == "texture1D")
                    t.dim = VD_1D;
                else if (base == "texture3D")
                    t.dim = VD_3D;
                else if (base == "textureCube")
                    t.dim = VD_CUBE;
                else if (base == "texture2DArray")
                    t.dim = VD_2D_ARRAY;
                else if (base == "textureCubeArray")
                    t.dim = VD_CUBE_ARRAY;
                else if (base == "texture2DMS")
                    t.dim = VD_2D, t.multisampled = true;
                else if (base == "texture2DMSArray")
                    t.dim = VD_2D_ARRAY, t.multisampled = true;
                else
                    t.dim = VD_2D;
                if (t.multisampled && kind == SK_FLOAT)
                    kind = SK_UNFILTERABLE;
                t.kind = kind;
                sh.tex.push_back(t);
            }
        } else if (key == "smp") {
            for (const std::string &e : Split(value, ',')) {
                const auto p = Split(e, ':');  // binding:heap set:<s|p>word
                if (p.size() < 3 || p[2].size() < 2)
                    continue;
                SmpBind s;
                s.binding = uint32_t(atoi(p[0].c_str()));
                s.heapSet = uint32_t(atoi(p[1].c_str()));
                s.push = p[2][0] == 'p';
                s.word = uint32_t(atoi(p[2].c_str() + 1));
                s.visibility = vis;
                sh.smp.push_back(s);
            }
        } else if (key == "ubo") {
            for (const std::string &e : Split(value, ','))
                sh.ubo.push_back(uint32_t(atoi(e.c_str())));
        } else if (key == "ssbo") {
            for (const std::string &e : Split(value, ',')) {
                SsboBind b;  // <slot><r|w>[@set.binding]
                b.slot = uint32_t(atoi(e.c_str()));
                b.write = e.find('w') != std::string::npos;
                const size_t at = e.find('@');
                if (at != std::string::npos) {
                    b.set = uint32_t(atoi(e.c_str() + at + 1));
                    const size_t dot = e.find('.', at);
                    if (dot != std::string::npos)
                        b.binding = uint32_t(atoi(e.c_str() + dot + 1));
                }
                b.visibility = vis;
                sh.ssbo.push_back(b);
            }
        } else if (key == "push") {
            sh.push = uint32_t(atoi(value.c_str()));
        } else if (key == "vin") {
            for (const std::string &e : Split(value, ','))
                sh.vin.push_back(uint32_t(atoi(e.c_str())));
        }
    }
}

// ---- pipelines --------------------------------------------------------------------------------------------------

struct WebGpuPipelineLayout : RenderPipelineLayout {
    std::vector<uint32_t> pushSizes;
    uint32_t setCount = 0;
};

struct WebGpuPipeline : RenderPipeline {
    uint32_t id = 0;
    bool compute = false;
    uint32_t groupLayouts[4] = {};
    uint32_t groupCount = 0;
    std::vector<TexBind> tex;
    std::vector<SmpBind> smp;
    std::vector<uint32_t> ubo;   // group 1 bindings
    std::vector<SsboBind> ssbo;  // group 2
    uint32_t push = 0;           // group 3 size
    std::vector<uint32_t> slots;  // vertex buffer i <- plume slot slots[i]
    std::vector<uint32_t> strides;
    std::string name;

    ~WebGpuPipeline() override { DestroyId(id); }
    void setName(const std::string &n) override { name = n; }
    RenderPipelineProgram getProgram(const std::string &) const override { return {}; }
};

void MergeStages(WebGpuPipeline &p, const WebGpuShader *s) {
    if (!s)
        return;
    p.tex.insert(p.tex.end(), s->tex.begin(), s->tex.end());
    p.smp.insert(p.smp.end(), s->smp.begin(), s->smp.end());
    for (uint32_t b : s->ubo)
        if (std::find(p.ubo.begin(), p.ubo.end(), b) == p.ubo.end())
            p.ubo.push_back(b);
    for (const SsboBind &b : s->ssbo) {
        auto it = std::find_if(p.ssbo.begin(), p.ssbo.end(), [&](const SsboBind &x) { return x.slot == b.slot; });
        if (it == p.ssbo.end())
            p.ssbo.push_back(b);
        else
            it->visibility |= b.visibility, it->write |= b.write;
    }
    p.push = std::max(p.push, s->push);
}

// The bind group layouts and the pipeline layout of a pipeline's bindings (Global::mutex held).
uint32_t BuildLayoutsLocked(WebGpuPipeline &p) {
    std::sort(p.ubo.begin(), p.ubo.end());
    std::sort(p.ssbo.begin(), p.ssbo.end(), [](const SsboBind &a, const SsboBind &b) { return a.slot < b.slot; });
    std::vector<uint32_t> e0, e1, e2, e3;
    for (const TexBind &t : p.tex) {
        e0.insert(e0.end(), {t.binding, t.visibility, EK_TEXTURE, t.kind, t.dim, t.multisampled ? 1u : 0u});
    }
    for (const SmpBind &s : p.smp)
        e0.insert(e0.end(), {s.binding, s.visibility, EK_SAMPLER, 0, 0, 0});
    const uint32_t graphicsVis = p.compute ? VIS_COMPUTE : (VIS_VERTEX | VIS_FRAGMENT);
    for (uint32_t b : p.ubo)
        e1.insert(e1.end(), {b, graphicsVis, EK_UBO, 1, 0, 0});
    for (const SsboBind &b : p.ssbo)
        e2.insert(e2.end(), {b.slot, b.visibility, b.write ? EK_STORAGE : EK_STORAGE_READ, 0, 0, 0});
    if (p.push)
        e3.insert(e3.end(), {0, graphicsVis, EK_UBO, 1, 0, 0});
    // bindings in order within a layout
    auto sortEntries = [](std::vector<uint32_t> &e) {
        std::vector<std::array<uint32_t, 6>> rows(e.size() / 6);
        for (size_t k = 0; k < rows.size(); k++)
            std::copy(e.begin() + k * 6, e.begin() + k * 6 + 6, rows[k].begin());
        std::sort(rows.begin(), rows.end(), [](auto &a, auto &b) { return a[0] < b[0]; });
        e.clear();
        for (auto &r : rows)
            e.insert(e.end(), r.begin(), r.end());
    };
    sortEntries(e0);
    p.groupCount = p.push ? 4 : !e2.empty() ? 3 : !e1.empty() ? 2 : !e0.empty() ? 1 : 0;
    Cache &c = C();
    if (!c.emptyLayout)
        c.emptyLayout = LayoutLocked({});
    const std::vector<uint32_t> *entries[4] = {&e0, &e1, &e2, &e3};
    for (uint32_t g = 0; g < 4; g++)
        p.groupLayouts[g] = g < p.groupCount ? (entries[g]->empty() ? c.emptyLayout : LayoutLocked(*entries[g])) : 0;
    return PipelineLayoutLocked(p.groupLayouts, p.groupCount);
}

struct WebGpuFramebuffer : RenderFramebuffer {
    uint32_t id = 0;
    uint32_t width = 0, height = 0;
    std::vector<uint32_t> ownViews;
    ~WebGpuFramebuffer() override {
        Global &g = G();
        std::lock_guard<std::mutex> lock(g.mutex);
        for (uint32_t v : ownViews)
            g.destroyLocked(v);
        g.destroyLocked(id);
    }
    uint32_t getWidth() const override { return width; }
    uint32_t getHeight() const override { return height; }
};

struct WebGpuFence : RenderCommandFence {
    int32_t *word = nullptr;
    int32_t target = 0;
    WebGpuFence() {
        word = &g_fenceWords[g_fenceNext.fetch_add(1) % 4096];
        target = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    }
};

struct WebGpuSemaphore : RenderCommandSemaphore {};

struct WebGpuQueryPool : RenderQueryPool {
    std::vector<uint64_t> results;
    explicit WebGpuQueryPool(uint32_t count) : results(count, 0) {}
    void queryResults() override {}
    const uint64_t *getResults() const override { return results.data(); }
    uint32_t getCount() const override { return uint32_t(results.size()); }
};

// ---- command lists ------------------------------------------------------------------------------------------------

struct UploadRange {
    const WebGpuBuffer *buffer;
    uint64_t lo, hi;
};

struct StageState {
    const WebGpuPipelineLayout *layout = nullptr;
    WebGpuDescriptorSet *sets[16] = {};
    uint32_t dynamicOffsets[16] = {};
    uint8_t pushData[256] = {};
    // what was sent
    uint32_t sentGroup[4] = {};
    std::vector<uint32_t> sentOffsets[4];
    bool pushDirty = true;
};

struct WebGpuCommandList : RenderCommandList {
    Words words;
    std::vector<UploadRange> uploads;
    bool open = false;
    StageState gfx, cmp;
    const WebGpuPipeline *pipeline = nullptr;
    const WebGpuPipeline *computePipeline = nullptr;
    uint32_t sentPipeline = 0, sentComputePipeline = 0;
    RenderVertexBufferView vertexViews[16] = {};
    uint32_t sentVertex[8][3] = {};
    RenderIndexBufferView indexView;
    bool indexDirty = false;
    const WebGpuFramebuffer *framebuffer = nullptr;
    bool float32Filterable = false;

    void reset() {
        words.w.clear();
        uploads.clear();
        gfx = StageState();
        cmp = StageState();
        pipeline = computePipeline = nullptr;
        sentPipeline = sentComputePipeline = 0;
        for (auto &v : vertexViews)
            v = RenderVertexBufferView();
        memset(sentVertex, 0, sizeof(sentVertex));
        indexView = RenderIndexBufferView();
        indexDirty = false;
        framebuffer = nullptr;
    }

    void begin() override {
        reset();
        float32Filterable = (G().features & FEAT_FLOAT32_FILTERABLE) != 0;
        if (!open) {
            open = true;
            Global &g = G();
            std::lock_guard<std::mutex> lock(g.mutex);
            g.openLists++;
        }
    }
    void end() override {}

    void upload(const RenderBuffer *b, uint64_t lo, uint64_t hi) {
        const WebGpuBuffer *buf = static_cast<const WebGpuBuffer *>(b);
        if (!buf || !buf->shadow || buf->desc.heapType == RenderHeapType::READBACK || hi <= lo)
            return;
        hi = std::min(hi, buf->desc.size);
        if (lo >= hi)
            return;
        if (!uploads.empty()) {
            UploadRange &last = uploads.back();
            if (last.buffer == buf && lo <= last.hi + 4096 && hi + 4096 >= last.lo) {
                last.lo = std::min(last.lo, lo);
                last.hi = std::max(last.hi, hi);
                return;
            }
        }
        uploads.push_back({buf, lo, hi});
    }

    void barriers(RenderBarrierStages, const RenderBufferBarrier *, uint32_t, const RenderTextureBarrier *, uint32_t) override {
        const size_t at = words.begin(OP_BARRIER);
        words.end(at);
    }

    // ---- binding resolution

    uint32_t readWord(const StageState &st, bool push, uint32_t offset, bool &ok) const {
        ok = false;
        if (push) {
            if (offset + 4 > sizeof(st.pushData))
                return 0;
            uint32_t v;
            memcpy(&v, st.pushData + offset, 4);
            ok = true;
            return v;
        }
        // SharedConstants: set 7's uniform buffer at its dynamic offset
        const WebGpuDescriptorSet *set = st.sets[7];
        if (!set || set->slots.empty() || !set->slots[0].buffer || !set->slots[0].buffer->shadow)
            return 0;
        const WebGpuBuffer *b = set->slots[0].buffer;
        const uint64_t at = uint64_t(st.dynamicOffsets[7]) + offset;
        if (at + 4 > b->desc.size)
            return 0;
        uint32_t v;
        memcpy(&v, b->shadow + at, 4);
        ok = true;
        return v;
    }

    static uint32_t dummyView(const TexBind &t) {
        if (t.kind == SK_UINT)
            return ID_DUMMY_2D_UINT;
        if (t.kind == SK_SINT)
            return ID_DUMMY_2D_SINT;
        switch (t.dim) {
        case VD_CUBE: return ID_DUMMY_CUBE;
        case VD_2D_ARRAY: return ID_DUMMY_2D_ARRAY;
        case VD_3D: return ID_DUMMY_3D;
        default: return t.kind == SK_UNFILTERABLE ? ID_DUMMY_2D_UNFILTERABLE : ID_DUMMY_2D;
        }
    }

    uint32_t resolveTexture(const StageState &st, const TexBind &t) const {
        bool ok;
        const uint32_t word = readWord(st, t.push, t.word, ok);
        if (!ok || t.heapSet >= 16 || t.multisampled)
            return dummyView(t);
        const WebGpuDescriptorSet *set = st.sets[t.heapSet];
        const uint32_t index = word & 0x03FFFFFFu;
        if (!set || index >= set->slots.size())
            return dummyView(t);
        const auto &slot = set->slots[index];
        const WebGpuTexture *tex = slot.view ? slot.view->texture : slot.texture;
        if (!tex)
            return dummyView(t);
        uint32_t aspect = slot.view ? slot.view->aspect : ViewAspect(tex->desc.format, tex->desc.format);
        if (t.kind == SK_UINT && HasStencil(tex->desc.format))
            aspect = 2;
        if (!KindFits(tex->desc.format, aspect, t.kind, float32Filterable)) {
            static std::atomic<int> warned{0};
            if (warned.fetch_add(1) < 8)
                Logf("texture format %u (aspect %u) does not fit a binding of sample kind %u: a dummy",
                     uint32_t(tex->desc.format), aspect, t.kind);
            return dummyView(t);
        }
        if (slot.view && slot.view->dim == t.dim && slot.view->aspect == aspect)
            return slot.view->id;
        const uint32_t v = tex->viewLocked(t.dim, aspect);
        return v ? v : dummyView(t);
    }

    uint32_t resolveSampler(const StageState &st, const SmpBind &s) const {
        bool ok;
        const uint32_t word = readWord(st, s.push, s.word, ok);
        if (!ok || s.heapSet >= 16)
            return ID_DUMMY_SAMPLER;
        const WebGpuDescriptorSet *set = st.sets[s.heapSet];
        if (!set || word >= set->slots.size() || !set->slots[word].sampler || set->slots[word].sampler->comparison)
            return ID_DUMMY_SAMPLER;
        return set->slots[word].sampler->id;
    }

    void sendGroup(StageState &st, uint32_t op, uint32_t group, uint32_t bg, const std::vector<uint32_t> &offsets) {
        if (st.sentGroup[group] == bg && st.sentOffsets[group] == offsets)
            return;
        st.sentGroup[group] = bg;
        st.sentOffsets[group] = offsets;
        const size_t at = words.begin(op);
        words.u(group);
        words.u(bg);
        words.u(uint32_t(offsets.size()));
        for (uint32_t o : offsets)
            words.u(o);
        words.end(at);
    }

    // Bind groups 0-3 of the pipeline from the bound state (Global::mutex held).
    void bindGroupsLocked(StageState &st, const WebGpuPipeline &p, bool compute) {
        const uint32_t setOp = compute ? OP_SET_COMPUTE_BIND_GROUP : OP_SET_BIND_GROUP;
        std::vector<uint32_t> entries;
        static const std::vector<uint32_t> kNoOffsets;
        // group 0: textures and samplers
        if (p.groupCount > 0) {
            entries.clear();
            // entries in binding order: textures and samplers interleave by binding number
            std::vector<std::pair<uint32_t, uint32_t>> rows;
            rows.reserve(p.tex.size() + p.smp.size());
            for (const TexBind &t : p.tex)
                rows.emplace_back(t.binding, resolveTexture(st, t));
            for (const SmpBind &s : p.smp)
                rows.emplace_back(s.binding, resolveSampler(st, s));
            std::sort(rows.begin(), rows.end());
            for (auto &r : rows)
                entries.insert(entries.end(), {r.first, 0, r.second, 0, 0});
            sendGroup(st, setOp, 0, BindGroupLocked(p.groupLayouts[0], entries), kNoOffsets);
        }
        // group 1: the constant buffers (binding b: set 5 + b), dynamic offsets
        if (p.groupCount > 1) {
            entries.clear();
            std::vector<uint32_t> offsets;
            for (uint32_t b : p.ubo) {
                const uint32_t setIndex = 5 + b;
                const WebGpuDescriptorSet *set = setIndex < 16 ? st.sets[setIndex] : nullptr;
                const WebGpuDescriptorSet::Slot *slot = set && !set->slots.empty() ? &set->slots[0] : nullptr;
                if (slot && slot->buffer) {
                    const uint64_t size = slot->size ? slot->size : slot->buffer->desc.size;
                    entries.insert(entries.end(), {b, 1, slot->buffer->id, 0, uint32_t(std::min<uint64_t>(size, 65536))});
                    offsets.push_back(st.dynamicOffsets[setIndex]);
                    upload(slot->buffer, st.dynamicOffsets[setIndex], st.dynamicOffsets[setIndex] + std::min<uint64_t>(size, 65536));
                } else {
                    entries.insert(entries.end(), {b, 1, ID_PUSH_RING, 0, 256});
                    offsets.push_back(0);
                }
            }
            sendGroup(st, setOp, 1, BindGroupLocked(p.groupLayouts[1], entries), offsets);
        }
        // group 2: storage (slot 0 the vertex fetch window, 1 memexport, 2.. from the descriptor sets)
        if (p.groupCount > 2) {
            entries.clear();
            for (const SsboBind &b : p.ssbo) {
                uint32_t id = ID_DUMMY_STORAGE;
                uint64_t size = 256;
                if (b.slot == 0) {
                    bool ok1, ok2, ok3;
                    const uint32_t low = readWord(st, false, 560, ok1);
                    const uint32_t high = readWord(st, false, 564, ok2);
                    const uint32_t bytes = readWord(st, false, 972, ok3);
                    const WebGpuBuffer *buf = findBuffer(high);
                    if (ok1 && ok2 && buf) {
                        id = buf->id;
                        size = buf->desc.size & ~uint64_t(3);
                        upload(buf, low, uint64_t(low) + (ok3 ? bytes : 0));
                    }
                } else if (b.slot >= 2 && b.set < 16 && st.sets[b.set]) {
                    const WebGpuDescriptorSet::Slot *slot = st.sets[b.set]->atBinding(b.binding);
                    if (slot && slot->buffer) {
                        id = slot->buffer->id;
                        size = (slot->size ? slot->size : slot->buffer->desc.size) & ~uint64_t(3);
                        upload(slot->buffer, 0, size);
                    }
                }
                entries.insert(entries.end(), {b.slot, 1, id, 0, uint32_t(std::min<uint64_t>(size, 0xFFFFFFFCull))});
            }
            sendGroup(st, setOp, 2, BindGroupLocked(p.groupLayouts[2], entries), kNoOffsets);
        }
        // group 3: the push constants, through the server's ring
        if (p.groupCount > 3) {
            entries.clear();
            const uint32_t size = (p.push + 15) & ~15u;
            entries.insert(entries.end(), {0, 1, ID_PUSH_RING, 0, size});
            const uint32_t bg = BindGroupLocked(p.groupLayouts[3], entries);
            // every draw sends its push bytes (the server gives them a ring offset)
            const size_t at = words.begin(compute ? OP_SET_COMPUTE_PUSH : OP_SET_PUSH);
            words.u(3);
            words.u(bg);
            words.u(size);
            words.bytes(st.pushData, size);
            words.end(at);
            st.sentGroup[3] = 0;
        }
    }

    static const WebGpuBuffer *findBuffer(uint32_t id);

    bool prepareDraw() {
        if (!pipeline || pipeline->compute || !pipeline->id || !framebuffer) {
            (!pipeline || pipeline->compute ? g_drawStats.noPipeline : !pipeline->id ? g_drawStats.noForm
                                                                                    : g_drawStats.noFramebuffer)++;
            return false;
        }
        g_drawStats.drawn++;
        Global &g = G();
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            bindGroupsLocked(gfx, *pipeline, false);
        }
        if (sentPipeline != pipeline->id) {
            sentPipeline = pipeline->id;
            const size_t at = words.begin(OP_SET_PIPELINE);
            words.u(pipeline->id);
            words.end(at);
        }
        for (size_t i = 0; i < pipeline->slots.size() && i < 8; i++) {
            const RenderVertexBufferView &v = vertexViews[pipeline->slots[i]];
            const WebGpuBuffer *b = static_cast<const WebGpuBuffer *>(v.buffer.ref);
            const uint32_t id = b ? b->id : 0;
            if (!b)
                continue;
            if (sentVertex[i][0] != id || sentVertex[i][1] != uint32_t(v.buffer.offset) || sentVertex[i][2] != v.size) {
                sentVertex[i][0] = id;
                sentVertex[i][1] = uint32_t(v.buffer.offset);
                sentVertex[i][2] = v.size;
                const size_t at = words.begin(OP_SET_VERTEX_BUFFER);
                words.u(uint32_t(i));
                words.u(id);
                words.u64(v.buffer.offset);
                words.u(uint32_t(std::min<uint64_t>(v.size, b->desc.size - std::min(b->desc.size, v.buffer.offset))));
                words.end(at);
                upload(b, v.buffer.offset, v.buffer.offset + v.size);
            }
        }
        if (indexDirty) {
            indexDirty = false;
            const WebGpuBuffer *b = static_cast<const WebGpuBuffer *>(indexView.buffer.ref);
            if (b) {
                const size_t at = words.begin(OP_SET_INDEX_BUFFER);
                words.u(b->id);
                words.u(indexView.format == RenderFormat::R32_UINT ? 1 : 0);
                words.u64(indexView.buffer.offset);
                words.u(uint32_t(std::min<uint64_t>(indexView.size, b->desc.size - std::min(b->desc.size, indexView.buffer.offset))));
                words.end(at);
                upload(b, indexView.buffer.offset, indexView.buffer.offset + indexView.size);
            }
        }
        return true;
    }

    void dispatch(uint32_t x, uint32_t y, uint32_t z) override {
        if (!computePipeline || !computePipeline->id)
            return;
        Global &g = G();
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            bindGroupsLocked(cmp, *computePipeline, true);
        }
        if (sentComputePipeline != computePipeline->id) {
            sentComputePipeline = computePipeline->id;
            const size_t at = words.begin(OP_SET_COMPUTE_PIPELINE);
            words.u(computePipeline->id);
            words.end(at);
        }
        const size_t at = words.begin(OP_DISPATCH);
        words.u(x);
        words.u(y);
        words.u(z);
        words.end(at);
    }
    void traceRays(uint32_t, uint32_t, uint32_t, RenderBufferReference, const RenderShaderBindingGroupsInfo &) override {}

    void drawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override {
        if (!vertexCount || !instanceCount || !prepareDraw())
            return;
        const size_t at = words.begin(OP_DRAW);
        words.u(vertexCount);
        words.u(instanceCount);
        words.u(firstVertex);
        words.u(firstInstance);
        words.end(at);
    }
    void drawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t baseVertex,
                              uint32_t firstInstance) override {
        if (!indexCount || !instanceCount || !prepareDraw())
            return;
        const size_t at = words.begin(OP_DRAW_INDEXED);
        words.u(indexCount);
        words.u(instanceCount);
        words.u(firstIndex);
        words.i(baseVertex);
        words.u(firstInstance);
        words.end(at);
    }

    void setPipeline(const RenderPipeline *p) override {
        const WebGpuPipeline *wp = static_cast<const WebGpuPipeline *>(p);
        if (wp && wp->compute)
            computePipeline = wp;
        else
            pipeline = wp;
    }
    void setComputePipelineLayout(const RenderPipelineLayout *layout) override {
        cmp.layout = static_cast<const WebGpuPipelineLayout *>(layout);
    }
    void setComputePushConstants(uint32_t, const void *data, uint32_t offset, uint32_t size) override {
        if (!size && cmp.layout && !cmp.layout->pushSizes.empty())
            size = cmp.layout->pushSizes[0];
        if (offset < sizeof(cmp.pushData))
            memcpy(cmp.pushData + offset, data, std::min<size_t>(size, sizeof(cmp.pushData) - offset));
    }
    void setComputeDescriptorSet(RenderDescriptorSet *set, uint32_t index) override {
        if (index < 16)
            cmp.sets[index] = static_cast<WebGpuDescriptorSet *>(set);
    }
    void setGraphicsPipelineLayout(const RenderPipelineLayout *layout) override {
        gfx.layout = static_cast<const WebGpuPipelineLayout *>(layout);
    }
    void setGraphicsPushConstants(uint32_t, const void *data, uint32_t offset, uint32_t size) override {
        if (!size && gfx.layout && !gfx.layout->pushSizes.empty())
            size = gfx.layout->pushSizes[0];
        if (offset < sizeof(gfx.pushData))
            memcpy(gfx.pushData + offset, data, std::min<size_t>(size, sizeof(gfx.pushData) - offset));
    }
    void setGraphicsDescriptorSet(RenderDescriptorSet *set, uint32_t index) override {
        if (index < 16) {
            gfx.sets[index] = static_cast<WebGpuDescriptorSet *>(set);
            gfx.dynamicOffsets[index] = 0;
        }
    }
    void setGraphicsDescriptorSetDynamic(RenderDescriptorSet *set, uint32_t index, const uint32_t *offsets, uint32_t count) override {
        if (index < 16) {
            gfx.sets[index] = static_cast<WebGpuDescriptorSet *>(set);
            gfx.dynamicOffsets[index] = count ? offsets[0] : 0;
        }
    }
    void setGraphicsRootDescriptor(RenderBufferReference, uint32_t) override {}
    void setRaytracingPipelineLayout(const RenderPipelineLayout *) override {}
    void setRaytracingPushConstants(uint32_t, const void *, uint32_t, uint32_t) override {}
    void setRaytracingDescriptorSet(RenderDescriptorSet *, uint32_t) override {}

    void setIndexBuffer(const RenderIndexBufferView *view) override {
        if (view)
            indexView = *view;
        else
            indexView = RenderIndexBufferView();
        indexDirty = true;
    }
    void setVertexBuffers(uint32_t startSlot, const RenderVertexBufferView *views, uint32_t count, const RenderInputSlot *) override {
        for (uint32_t k = 0; k < count && startSlot + k < 16; k++)
            vertexViews[startSlot + k] = views ? views[k] : RenderVertexBufferView();
    }
    void setViewports(const RenderViewport *v, uint32_t count) override {
        if (!count)
            return;
        const size_t at = words.begin(OP_SET_VIEWPORT);
        words.f(v[0].x);
        words.f(v[0].y);
        words.f(v[0].width);
        words.f(v[0].height);
        words.f(v[0].minDepth);
        words.f(v[0].maxDepth);
        words.end(at);
    }
    void setScissors(const RenderRect *r, uint32_t count) override {
        if (!count)
            return;
        const size_t at = words.begin(OP_SET_SCISSOR);
        words.i(r[0].left);
        words.i(r[0].top);
        words.i(r[0].right - r[0].left);
        words.i(r[0].bottom - r[0].top);
        words.end(at);
    }
    void setFramebuffer(const RenderFramebuffer *fb) override {
        framebuffer = static_cast<const WebGpuFramebuffer *>(fb);
        const size_t at = words.begin(OP_SET_FRAMEBUFFER);
        words.u(framebuffer ? framebuffer->id : 0);
        words.end(at);
        // a new render pass: everything is bound again in it by the server; vertex buffers and groups stay as sent
    }
    void setDepthBias(float, float, float) override {}
    void setStencilReference(uint32_t reference) override {
        const size_t at = words.begin(OP_SET_STENCIL_REF);
        words.u(reference);
        words.end(at);
    }
    void setBlendConstant(const float rgba[4]) {
        const size_t at = words.begin(OP_SET_BLEND_CONSTANT);
        for (int k = 0; k < 4; k++)
            words.f(rgba[k]);
        words.end(at);
    }
    void clearColor(uint32_t index, RenderColor color, const RenderRect *rects, uint32_t count) override {
        const size_t at = words.begin(OP_CLEAR_COLOR);
        words.u(index);
        for (int k = 0; k < 4; k++)
            words.f(color.rgba[k]);
        words.u(count);
        for (uint32_t k = 0; k < count; k++) {
            words.i(rects[k].left);
            words.i(rects[k].top);
            words.i(rects[k].right);
            words.i(rects[k].bottom);
        }
        words.end(at);
    }
    void clearDepthStencil(bool clearDepth, bool clearStencil, float depth, uint32_t stencil, const RenderRect *rects,
                           uint32_t count) override {
        const size_t at = words.begin(OP_CLEAR_DS);
        words.u((clearDepth ? 1u : 0u) | (clearStencil ? 2u : 0u));
        words.f(depth);
        words.u(stencil);
        words.u(count);
        for (uint32_t k = 0; k < count; k++) {
            words.i(rects[k].left);
            words.i(rects[k].top);
            words.i(rects[k].right);
            words.i(rects[k].bottom);
        }
        words.end(at);
    }

    // ---- copies

    void copyBufferRegion(RenderBufferReference dst, RenderBufferReference src, uint64_t size) override {
        const WebGpuBuffer *d = static_cast<const WebGpuBuffer *>(dst.ref), *s = static_cast<const WebGpuBuffer *>(src.ref);
        if (!d || !s || !size)
            return;
        size = (size + 3) & ~uint64_t(3);
        const size_t at = words.begin(OP_COPY_B2B);
        words.u(d->id);
        words.u64(dst.offset);
        words.u(s->id);
        words.u64(src.offset);
        words.u64(size);
        words.end(at);
        upload(s, src.offset, src.offset + size);
    }
    void copyBuffer(const RenderBuffer *dst, const RenderBuffer *src) override {
        const WebGpuBuffer *d = static_cast<const WebGpuBuffer *>(dst), *s = static_cast<const WebGpuBuffer *>(src);
        if (d && s)
            copyBufferRegion(RenderBufferReference(d, 0), RenderBufferReference(s, 0), std::min(d->desc.size, s->desc.size));
    }

    static uint32_t aspectOf(const WebGpuTexture *t) {
        if (!IsDepthFormat(t->desc.format))
            return 0;
        return 1;  // depth (a depth-stencil texture's copies take one aspect; stencil copies are rare)
    }

    void copyTextureRegion(const RenderTextureCopyLocation &dstLoc, const RenderTextureCopyLocation &srcLoc, uint32_t dstX = 0,
                           uint32_t dstY = 0, uint32_t dstZ = 0, const RenderBox *srcBox = nullptr) override {
        if (dstLoc.type == RenderTextureCopyType::SUBRESOURCE && srcLoc.type == RenderTextureCopyType::PLACED_FOOTPRINT) {
            const WebGpuTexture *t = static_cast<const WebGpuTexture *>(dstLoc.texture);
            const WebGpuBuffer *b = static_cast<const WebGpuBuffer *>(srcLoc.buffer);
            if (!t || !b)
                return;
            const auto &fp = srcLoc.placedFootprint;
            const RenderFormat fmt = fp.format == RenderFormat::UNKNOWN ? t->desc.format : fp.format;
            const uint32_t bw = RenderFormatBlockWidth(fmt);
            const uint32_t bytesPerRow = (fp.rowWidth + bw - 1) / bw * RenderFormatSize(fmt);
            const uint32_t rowsPerImage = (fp.height + bw - 1) / bw;
            uint32_t x = dstX, y = dstY, z = dstZ, w = fp.width, h = fp.height, d = std::max(1u, fp.depth);
            uint64_t offset = fp.offset;
            if (srcBox) {
                offset += uint64_t(srcBox->top / bw) * bytesPerRow + uint64_t(srcBox->left / bw) * RenderFormatSize(fmt) +
                          uint64_t(srcBox->front) * bytesPerRow * rowsPerImage;
                w = srcBox->right - srcBox->left;
                h = srcBox->bottom - srcBox->top;
                d = std::max(1, srcBox->back - srcBox->front);
            }
            w = (w + bw - 1) / bw * bw;
            h = (h + bw - 1) / bw * bw;
            if (t->desc.dimension != RenderTextureDimension::TEXTURE_3D) {
                z = dstLoc.subresource.arrayIndex;
                d = 1;
            }
            const size_t at = words.begin(OP_COPY_B2T);
            words.u(b->id);
            words.u64(offset);
            words.u(bytesPerRow);
            words.u(rowsPerImage);
            words.u(t->id);
            words.u(dstLoc.subresource.mipLevel);
            words.u(x);
            words.u(y);
            words.u(z);
            words.u(w);
            words.u(h);
            words.u(d);
            words.u(aspectOf(t));
            words.end(at);
            upload(b, offset, offset + uint64_t(bytesPerRow) * rowsPerImage * d);
        } else if (dstLoc.type == RenderTextureCopyType::PLACED_FOOTPRINT && srcLoc.type == RenderTextureCopyType::SUBRESOURCE) {
            const WebGpuTexture *t = static_cast<const WebGpuTexture *>(srcLoc.texture);
            const WebGpuBuffer *b = static_cast<const WebGpuBuffer *>(dstLoc.buffer);
            if (!t || !b)
                return;
            const auto &fp = dstLoc.placedFootprint;
            const RenderFormat fmt = fp.format == RenderFormat::UNKNOWN ? t->desc.format : fp.format;
            const uint32_t bw = RenderFormatBlockWidth(fmt);
            const uint32_t bytesPerRow = (fp.rowWidth + bw - 1) / bw * RenderFormatSize(fmt);
            const uint32_t rowsPerImage = (fp.height + bw - 1) / bw;
            uint32_t x = 0, y = 0, z = 0, w = fp.width, h = fp.height, d = std::max(1u, fp.depth);
            if (srcBox) {
                x = srcBox->left;
                y = srcBox->top;
                z = srcBox->front;
                w = srcBox->right - srcBox->left;
                h = srcBox->bottom - srcBox->top;
                d = std::max(1, srcBox->back - srcBox->front);
            }
            if (t->desc.dimension != RenderTextureDimension::TEXTURE_3D) {
                z = srcLoc.subresource.arrayIndex;
                d = 1;
            }
            const size_t at = words.begin(OP_COPY_T2B);
            words.u(t->id);
            words.u(srcLoc.subresource.mipLevel);
            words.u(x);
            words.u(y);
            words.u(z);
            words.u(w);
            words.u(h);
            words.u(d);
            words.u(aspectOf(t));
            words.u(b->id);
            words.u64(fp.offset + uint64_t(dstY / bw) * bytesPerRow + uint64_t(dstX / bw) * RenderFormatSize(fmt));
            words.u(bytesPerRow);
            words.u(rowsPerImage);
            words.end(at);
        } else if (dstLoc.type == RenderTextureCopyType::SUBRESOURCE && srcLoc.type == RenderTextureCopyType::SUBRESOURCE) {
            const WebGpuTexture *d = static_cast<const WebGpuTexture *>(dstLoc.texture);
            const WebGpuTexture *s = static_cast<const WebGpuTexture *>(srcLoc.texture);
            if (!d || !s)
                return;
            const uint32_t mip = srcLoc.subresource.mipLevel;
            uint32_t x = 0, y = 0, z = 0;
            uint32_t w = std::max(1u, s->desc.width >> mip), h = std::max(1u, s->desc.height >> mip);
            uint32_t depth = s->desc.dimension == RenderTextureDimension::TEXTURE_3D ? std::max(1u, s->desc.depth >> mip) : 1;
            if (srcBox) {
                x = srcBox->left;
                y = srcBox->top;
                z = srcBox->front;
                w = srcBox->right - srcBox->left;
                h = srcBox->bottom - srcBox->top;
                depth = std::max(1, srcBox->back - srcBox->front);
            }
            if (s->desc.dimension != RenderTextureDimension::TEXTURE_3D)
                z = srcLoc.subresource.arrayIndex;
            if (d->desc.dimension != RenderTextureDimension::TEXTURE_3D)
                dstZ = dstLoc.subresource.arrayIndex;
            const size_t at = words.begin(OP_COPY_T2T);
            words.u(s->id);
            words.u(mip);
            words.u(x);
            words.u(y);
            words.u(z);
            words.u(d->id);
            words.u(dstLoc.subresource.mipLevel);
            words.u(dstX);
            words.u(dstY);
            words.u(dstZ);
            words.u(w);
            words.u(h);
            words.u(depth);
            words.u(0);
            words.end(at);
        }
    }
    void copyTexture(const RenderTexture *dst, const RenderTexture *src) override {
        const WebGpuTexture *d = static_cast<const WebGpuTexture *>(dst), *s = static_cast<const WebGpuTexture *>(src);
        if (!d || !s)
            return;
        const uint32_t mips = std::min(std::max(1u, d->desc.mipLevels), std::max(1u, s->desc.mipLevels));
        const uint32_t layers = std::min(d->layers, s->layers);
        for (uint32_t m = 0; m < mips; m++)
            for (uint32_t l = 0; l < layers; l++)
                copyTextureRegion(RenderTextureCopyLocation::Subresource(d, m, l), RenderTextureCopyLocation::Subresource(s, m, l));
    }
    void resolveTexture(const RenderTexture *dst, const RenderTexture *src) override { copyTexture(dst, src); }
    void resolveTextureRegion(const RenderTexture *dst, uint32_t dstX, uint32_t dstY, const RenderTexture *src,
                              const RenderRect *rect, RenderResolveMode) override {
        // single-sampled only: a copy
        const WebGpuTexture *s = static_cast<const WebGpuTexture *>(src);
        if (!s)
            return;
        RenderBox box(0, 0, s->desc.width, s->desc.height);
        if (rect)
            box = RenderBox(rect->left, rect->top, rect->right, rect->bottom);
        copyTextureRegion(RenderTextureCopyLocation::Subresource(dst), RenderTextureCopyLocation::Subresource(src), dstX, dstY,
                          0, &box);
    }
    void buildBottomLevelAS(const RenderAccelerationStructure *, RenderBufferReference, const RenderBottomLevelASBuildInfo &) override {}
    void buildTopLevelAS(const RenderAccelerationStructure *, RenderBufferReference, RenderBufferReference,
                         const RenderTopLevelASBuildInfo &) override {}
    void discardTexture(const RenderTexture *) override {}
    void resetQueryPool(const RenderQueryPool *, uint32_t, uint32_t) override {}
    void writeTimestamp(const RenderQueryPool *, uint32_t) override {}

    ~WebGpuCommandList() override {
        if (open) {
            Global &g = G();
            std::lock_guard<std::mutex> lock(g.mutex);
            g.openLists--;
        }
    }
};

// Buffers by id, for the vertex fetch window (the address's high word).
std::mutex g_bufferMapMutex;
std::unordered_map<uint32_t, const WebGpuBuffer *> g_buffers;

const WebGpuBuffer *WebGpuCommandList::findBuffer(uint32_t id) {
    std::lock_guard<std::mutex> lock(g_bufferMapMutex);
    auto it = g_buffers.find(id);
    return it == g_buffers.end() ? nullptr : it->second;
}

struct TrackedBuffer : WebGpuBuffer {
    explicit TrackedBuffer(const RenderBufferDesc &d) : WebGpuBuffer(d) {
        std::lock_guard<std::mutex> lock(g_bufferMapMutex);
        g_buffers[id] = this;
    }
    ~TrackedBuffer() override {
        std::lock_guard<std::mutex> lock(g_bufferMapMutex);
        g_buffers.erase(id);
    }
};

// ---- swap chain ---------------------------------------------------------------------------------------------------

struct WebGpuSwapChain : RenderSwapChain {
    RenderSwapChainDesc desc;
    std::vector<std::unique_ptr<WebGpuTexture>> textures;
    uint32_t width = 0, height = 0;
    uint32_t current = 0;
    int32_t presented = 0;
    bool vsync = true;

    explicit WebGpuSwapChain(const RenderSwapChainDesc &d) : desc(d) { make(); }

    void canvasSize(uint32_t &w, uint32_t &h) const {
        w = uint32_t(std::max(0, LoadStatus(ST_CANVAS_W)));
        h = uint32_t(std::max(0, LoadStatus(ST_CANVAS_H)));
        if (!w || !h)
            w = 1280, h = 720;
    }
    void make() {
        textures.clear();
        canvasSize(width, height);
        const uint32_t count = std::max(2u, desc.textureCount);
        for (uint32_t k = 0; k < count; k++)
            textures.push_back(std::make_unique<WebGpuTexture>(RenderTextureDesc::ColorTarget(width, height, desc.format)));
    }
    bool present(uint32_t index, RenderCommandSemaphore **, uint32_t) override {
        if (index >= textures.size())
            return false;
        Global &g = G();
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            const size_t at = g.stream.begin(OP_PRESENT);
            g.stream.u(textures[index]->id);
            g.stream.end(at);
            g.flushLocked();
        }
        presented++;
        return LoadStatus(ST_STATE) > 0;
    }
    void wait() override {
        // one present ahead of the page's thread at most
        WaitWord(&g_status[ST_PRESENTS], presented - 1);
    }
    bool resize() override {
        make();
        return true;
    }
    bool needsResize() const override {
        uint32_t w, h;
        canvasSize(w, h);
        return w != width || h != height;
    }
    void setVsyncEnabled(bool on) override { vsync = on; }
    bool isVsyncEnabled() const override { return vsync; }
    uint32_t getWidth() const override { return width; }
    uint32_t getHeight() const override { return height; }
    RenderTexture *getTexture(uint32_t index) override { return index < textures.size() ? textures[index].get() : nullptr; }
    uint32_t getTextureCount() const override { return uint32_t(textures.size()); }
    bool acquireTexture(RenderCommandSemaphore *, uint32_t *index) override {
        wait();
        current = (current + 1) % uint32_t(textures.size());
        *index = current;
        return true;
    }
    RenderWindow getWindow() const override { return desc.renderWindow; }
    bool isEmpty() const override { return width == 0 || height == 0; }
    uint32_t getRefreshRate() const override { return 60; }
};

// ---- queue --------------------------------------------------------------------------------------------------------

struct WebGpuCommandQueue : RenderCommandQueue {
    std::unique_ptr<RenderCommandList> createCommandList() override { return std::make_unique<WebGpuCommandList>(); }
    std::unique_ptr<RenderSwapChain> createSwapChain(const RenderSwapChainDesc &desc) override {
        return std::make_unique<WebGpuSwapChain>(desc);
    }
    void executeCommandLists(const RenderCommandList **lists, uint32_t count, RenderCommandSemaphore **, uint32_t,
                             RenderCommandSemaphore **, uint32_t, RenderCommandFence *signalFence) override {
        Global &g = G();
        std::lock_guard<std::mutex> lock(g.mutex);
        ReportDrawStats();
        // the upload buffers' ranges the lists read, first
        for (uint32_t k = 0; k < count; k++) {
            WebGpuCommandList *cl = const_cast<WebGpuCommandList *>(static_cast<const WebGpuCommandList *>(lists[k]));
            std::sort(cl->uploads.begin(), cl->uploads.end(), [](const UploadRange &a, const UploadRange &b) {
                return a.buffer != b.buffer ? a.buffer < b.buffer : a.lo < b.lo;
            });
            const UploadRange *run = nullptr;
            uint64_t lo = 0, hi = 0;
            auto emit = [&]() {
                if (!run)
                    return;
                const uint64_t l = lo & ~uint64_t(3);
                const uint64_t h = std::min((hi + 3) & ~uint64_t(3), (run->buffer->desc.size + 3) & ~uint64_t(3));
                if (h <= l)
                    return;
                const size_t at = g.stream.begin(OP_WRITE_BUFFER);
                g.stream.u(run->buffer->id);
                g.stream.u64(l);
                g.stream.u64(uint64_t(uintptr_t(run->buffer->shadow)) + l);
                g.stream.u(uint32_t(h - l));
                g.stream.end(at);
            };
            for (const UploadRange &r : cl->uploads) {
                if (run && run->buffer == r.buffer && r.lo <= hi + 65536) {
                    hi = std::max(hi, r.hi);
                    continue;
                }
                emit();
                run = &r;
                lo = r.lo;
                hi = r.hi;
            }
            emit();
            cl->uploads.clear();
        }
        WebGpuFence *fence = static_cast<WebGpuFence *>(signalFence);
        const size_t at = g.stream.begin(OP_SUBMIT);
        if (fence) {
            fence->target++;
            g.stream.u64(uint64_t(uintptr_t(fence->word)));
            g.stream.i(fence->target);
        } else {
            g.stream.u64(0);
            g.stream.u(0);
        }
        g.stream.u(count);
        for (uint32_t k = 0; k < count; k++) {
            WebGpuCommandList *cl = const_cast<WebGpuCommandList *>(static_cast<const WebGpuCommandList *>(lists[k]));
            g.stream.u(uint32_t(cl->words.w.size()));
            g.stream.w.insert(g.stream.w.end(), cl->words.w.begin(), cl->words.w.end());
            cl->words.w.clear();
            if (cl->open) {
                cl->open = false;
                g.openLists--;
            }
        }
        g.stream.end(at);
        g.emitDestroysLocked();
        g.flushLocked();
    }
    void waitForCommandFence(RenderCommandFence *fence) override {
        WebGpuFence *f = static_cast<WebGpuFence *>(fence);
        if (!f)
            return;
        G().flush();
        WaitWord(f->word, f->target);
    }
};

// ---- device -------------------------------------------------------------------------------------------------------

struct WebGpuPool : RenderPool {
    std::unique_ptr<RenderBuffer> createBuffer(const RenderBufferDesc &desc) override { return std::make_unique<TrackedBuffer>(desc); }
    std::unique_ptr<RenderTexture> createTexture(const RenderTextureDesc &desc) override { return std::make_unique<WebGpuTexture>(desc); }
};

struct WebGpuDevice : RenderDevice {
    RenderDeviceCapabilities caps;
    RenderDeviceDescription description;

    WebGpuDevice() {
        caps.geometryShader = false;
        caps.descriptorIndexing = true;
        caps.scalarBlockLayout = true;
        caps.bufferDeviceAddress = true;
        caps.samplerMirrorClampToEdge = false;
        caps.maxTextureSize = uint64_t(std::max(4096, LoadStatus(ST_MAX_TEX_2D)));
        caps.triangleFan = false;
        caps.dynamicDepthBias = false;
        caps.queryPools = false;
        caps.uma = false;
        description.name = "WebGPU";
        description.type = RenderDeviceType::UNKNOWN;
    }

    std::unique_ptr<RenderDescriptorSet> createDescriptorSet(const RenderDescriptorSetDesc &desc) override {
        return std::make_unique<WebGpuDescriptorSet>(desc);
    }
    std::unique_ptr<RenderShader> createShader(const void *data, uint64_t size, const char *, RenderShaderFormat format) override {
        auto sh = std::make_unique<WebGpuShader>();
        WebGpuShaderBlob blob;
        if (format != RenderShaderFormat::WGSL || !data || size < sizeof(blob))
            return sh;
        memcpy(&blob, data, sizeof(blob));
        if (blob.magic != WebGpuShaderBlob::Magic || sizeof(blob) + uint64_t(blob.metaSize) + blob.wgslSize > size)
            return sh;
        const char *meta = static_cast<const char *>(data) + sizeof(blob);
        const char *wgsl = meta + blob.metaSize;
        sh->stage = blob.stage;
        if (!blob.wgslSize || blob.stage == 3)
            return sh;  // none (a geometry shader, or one the converter could not make)
        ParseMeta(*sh, std::string(meta, blob.metaSize));
        sh->id = G().newId();
        sh->valid = true;
        Global &g = G();
        std::lock_guard<std::mutex> lock(g.mutex);
        const size_t at = g.stream.begin(OP_CREATE_SHADER);
        g.stream.u(sh->id);
        g.stream.u(sh->stage);
        g.stream.u(blob.wgslSize);
        g.stream.bytes(wgsl, blob.wgslSize);
        g.stream.end(at);
        return sh;
    }
    std::unique_ptr<RenderSampler> createSampler(const RenderSamplerDesc &desc) override {
        return std::make_unique<WebGpuSampler>(desc);
    }
    std::unique_ptr<RenderPipeline> createComputePipeline(const RenderComputePipelineDesc &desc) override {
        auto p = std::make_unique<WebGpuPipeline>();
        p->compute = true;
        const WebGpuShader *cs = static_cast<const WebGpuShader *>(desc.computeShader);
        if (!cs || !cs->valid)
            return p;
        MergeStages(*p, cs);
        for (TexBind &t : p->tex)
            t.visibility = VIS_COMPUTE;
        for (SmpBind &s : p->smp)
            s.visibility = VIS_COMPUTE;
        for (SsboBind &b : p->ssbo)
            b.visibility = VIS_COMPUTE;
        Global &g = G();
        std::lock_guard<std::mutex> lock(g.mutex);
        const uint32_t layout = BuildLayoutsLocked(*p);
        p->id = g.newId();
        const size_t at = g.stream.begin(OP_CREATE_COMPUTE_PIPELINE);
        g.stream.u(p->id);
        g.stream.u(layout);
        g.stream.u(cs->id);
        g.stream.u(desc.specConstantsCount);
        for (uint32_t k = 0; k < desc.specConstantsCount; k++) {
            g.stream.u(desc.specConstants[k].index);
            g.stream.u(desc.specConstants[k].value);
        }
        g.stream.end(at);
        return p;
    }
    std::unique_ptr<RenderPipeline> createGraphicsPipeline(const RenderGraphicsPipelineDesc &desc) override {
        auto p = std::make_unique<WebGpuPipeline>();
        const WebGpuShader *vs = static_cast<const WebGpuShader *>(desc.vertexShader);
        const WebGpuShader *ps = static_cast<const WebGpuShader *>(desc.pixelShader);
        if (ps && !ps->valid) {
            Logf("graphics pipeline with a pixel shader WebGPU cannot run (%s): drawn without one", ps->name.c_str());
            ps = nullptr;
        }
        if (!vs || !vs->valid) {
            Logf("graphics pipeline without a vertex shader WebGPU can run (%s)", vs ? vs->name.c_str() : "none");
            g_drawStats.pipelinesWithoutForm++;
            return p;
        }
        g_drawStats.pipelinesMade++;
        MergeStages(*p, vs);
        MergeStages(*p, ps);
        // vertex buffers: the plume slots the elements use, in order, as WebGPU's 0..; locations as the shader's vin
        std::vector<uint32_t> slots;
        for (uint32_t k = 0; k < desc.inputElementsCount; k++) {
            const RenderInputElement &e = desc.inputElements[k];
            if (std::find(vs->vin.begin(), vs->vin.end(), e.location) == vs->vin.end())
                continue;
            if (std::find(slots.begin(), slots.end(), e.slotIndex) == slots.end())
                slots.push_back(e.slotIndex);
        }
        std::sort(slots.begin(), slots.end());
        if (slots.size() > 8) {
            Logf("pipeline %s: %zu vertex buffers (WebGPU has 8)", vs->name.c_str(), slots.size());
            slots.resize(8);
        }
        p->slots = slots;
        Global &g = G();
        std::lock_guard<std::mutex> lock(g.mutex);
        const uint32_t layout = BuildLayoutsLocked(*p);
        p->id = g.newId();
        Words &w = g.stream;
        const size_t at = w.begin(OP_CREATE_RENDER_PIPELINE);
        w.u(p->id);
        w.u(layout);
        w.u(vs->id);
        w.u(ps ? ps->id : 0);
        w.u(uint32_t(desc.primitiveTopology));
        w.u(uint32_t(desc.cullMode));
        w.u(uint32_t(desc.frontFace));
        w.u(uint32_t(desc.depthTargetFormat));
        w.u((desc.depthEnabled ? 1u : 0u) | (desc.depthWriteEnabled ? 2u : 0u));
        w.u(uint32_t(desc.depthFunction));
        w.u(desc.stencilEnabled ? 1u : 0u);
        w.u(desc.stencilReadMask);
        w.u(desc.stencilWriteMask);
        for (const RenderStencilFaceDesc *face : {&desc.stencilFrontFace, &desc.stencilBackFace}) {
            w.u(uint32_t(face->failOp));
            w.u(uint32_t(face->depthFailOp));
            w.u(uint32_t(face->passOp));
            w.u(uint32_t(face->compareFunction));
        }
        w.i(desc.depthBias);
        w.f(desc.slopeScaledDepthBias);
        w.f(desc.depthBiasClamp);
        w.u(1);  // samples
        w.u(desc.alphaToCoverageEnabled ? 1 : 0);
        w.u(desc.renderTargetCount);
        for (uint32_t k = 0; k < desc.renderTargetCount; k++) {
            const RenderBlendDesc &b = desc.renderTargetBlend[k];
            w.u(uint32_t(desc.renderTargetFormat[k]));
            w.u(b.blendEnabled ? 1 : 0);
            w.u(uint32_t(b.srcBlend));
            w.u(uint32_t(b.dstBlend));
            w.u(uint32_t(b.blendOp));
            w.u(uint32_t(b.srcBlendAlpha));
            w.u(uint32_t(b.dstBlendAlpha));
            w.u(uint32_t(b.blendOpAlpha));
            w.u(b.renderTargetWriteMask);
        }
        w.u(uint32_t(slots.size()));
        for (uint32_t slot : slots) {
            uint32_t stride = 0;
            bool instance = false;
            for (uint32_t k = 0; k < desc.inputSlotsCount; k++)
                if (desc.inputSlots[k].index == slot) {
                    stride = desc.inputSlots[k].stride;
                    instance = desc.inputSlots[k].classification == RenderInputSlotClassification::PER_INSTANCE_DATA;
                }
            p->strides.push_back(stride);
            w.u(stride);
            w.u(instance ? 1 : 0);
            std::vector<const RenderInputElement *> attrs;
            for (uint32_t k = 0; k < desc.inputElementsCount; k++) {
                const RenderInputElement &e = desc.inputElements[k];
                if (e.slotIndex != slot)
                    continue;
                if (std::find(vs->vin.begin(), vs->vin.end(), e.location) == vs->vin.end())
                    continue;
                // one attribute a location
                if (std::any_of(attrs.begin(), attrs.end(), [&](const RenderInputElement *a) { return a->location == e.location; }))
                    continue;
                attrs.push_back(&e);
            }
            w.u(uint32_t(attrs.size()));
            for (const RenderInputElement *e : attrs) {
                w.u(uint32_t(e->format));
                w.u(e->alignedByteOffset);
                w.u(uint32_t(std::find(vs->vin.begin(), vs->vin.end(), e->location) - vs->vin.begin()));
            }
        }
        w.u(desc.specConstantsCount);
        for (uint32_t k = 0; k < desc.specConstantsCount; k++) {
            w.u(desc.specConstants[k].index);
            w.u(desc.specConstants[k].value);
        }
        w.end(at);
        return p;
    }
    std::unique_ptr<RenderPipeline> createRaytracingPipeline(const RenderRaytracingPipelineDesc &, const RenderPipeline *) override {
        return nullptr;
    }
    std::unique_ptr<RenderCommandQueue> createCommandQueue(RenderCommandListType) override {
        return std::make_unique<WebGpuCommandQueue>();
    }
    std::unique_ptr<RenderBuffer> createBuffer(const RenderBufferDesc &desc) override { return std::make_unique<TrackedBuffer>(desc); }
    std::unique_ptr<RenderTexture> createTexture(const RenderTextureDesc &desc) override { return std::make_unique<WebGpuTexture>(desc); }
    std::unique_ptr<RenderAccelerationStructure> createAccelerationStructure(const RenderAccelerationStructureDesc &) override {
        return nullptr;
    }
    std::unique_ptr<RenderPool> createPool(const RenderPoolDesc &) override { return std::make_unique<WebGpuPool>(); }
    std::unique_ptr<RenderPipelineLayout> createPipelineLayout(const RenderPipelineLayoutDesc &desc) override {
        auto l = std::make_unique<WebGpuPipelineLayout>();
        for (uint32_t k = 0; k < desc.pushConstantRangesCount; k++)
            l->pushSizes.push_back(desc.pushConstantRanges[k].size);
        l->setCount = desc.descriptorSetDescsCount;
        return l;
    }
    std::unique_ptr<RenderCommandFence> createCommandFence() override { return std::make_unique<WebGpuFence>(); }
    std::unique_ptr<RenderCommandSemaphore> createCommandSemaphore() override { return std::make_unique<WebGpuSemaphore>(); }
    std::unique_ptr<RenderFramebuffer> createFramebuffer(const RenderFramebufferDesc &desc) override {
        auto fb = std::make_unique<WebGpuFramebuffer>();
        Global &g = G();
        std::lock_guard<std::mutex> lock(g.mutex);
        std::vector<std::pair<uint32_t, uint32_t>> colors;
        auto attachmentView = [&](const RenderTexture *rt, const RenderTextureView *rv, uint32_t &w, uint32_t &h,
                                  RenderFormat &format) -> uint32_t {
            const WebGpuTexture *t = static_cast<const WebGpuTexture *>(rt);
            const WebGpuTextureView *v = static_cast<const WebGpuTextureView *>(rv);
            if (v && !t)
                t = v->texture;
            if (!t)
                return 0;
            format = t->desc.format;
            w = t->desc.width;
            h = t->desc.height;
            // a render attachment: one level, one layer, every aspect
            const uint32_t id = CreateViewLocked(t, t->desc.format, VD_2D, 0, 1, 0, 1, 0);
            fb->ownViews.push_back(id);
            return id;
        };
        uint32_t w = 0, h = 0;
        for (uint32_t k = 0; k < desc.colorAttachmentsCount; k++) {
            RenderFormat f = RenderFormat::UNKNOWN;
            const uint32_t v = attachmentView(desc.colorAttachments ? desc.colorAttachments[k] : nullptr,
                                              desc.colorAttachmentViews ? desc.colorAttachmentViews[k] : nullptr, w, h, f);
            colors.emplace_back(v, uint32_t(f));
        }
        RenderFormat df = RenderFormat::UNKNOWN;
        uint32_t dv = 0;
        if (desc.depthAttachment || desc.depthAttachmentView) {
            uint32_t dw = 0, dh = 0;
            dv = attachmentView(desc.depthAttachment, desc.depthAttachmentView, dw, dh, df);
            if (!w)
                w = dw, h = dh;
        }
        fb->width = w;
        fb->height = h;
        fb->id = g.newId();
        const size_t at = g.stream.begin(OP_CREATE_FRAMEBUFFER);
        g.stream.u(fb->id);
        g.stream.u(uint32_t(colors.size()));
        for (auto &c : colors) {
            g.stream.u(c.first);
            g.stream.u(c.second);
        }
        g.stream.u(dv);
        g.stream.u(uint32_t(df));
        g.stream.u(desc.depthAttachmentReadOnly ? 1 : 0);
        g.stream.u(w);
        g.stream.u(h);
        g.stream.u(1);
        g.stream.end(at);
        return fb;
    }
    std::unique_ptr<RenderQueryPool> createQueryPool(uint32_t count) override { return std::make_unique<WebGpuQueryPool>(count); }
    void setBottomLevelASBuildInfo(RenderBottomLevelASBuildInfo &, const RenderBottomLevelASMesh *, uint32_t, bool, bool) override {}
    void setTopLevelASBuildInfo(RenderTopLevelASBuildInfo &, const RenderTopLevelASInstance *, uint32_t, bool, bool) override {}
    void setShaderBindingTableInfo(RenderShaderBindingTableInfo &, const RenderShaderBindingGroups &, const RenderPipeline *,
                                   RenderDescriptorSet **, uint32_t) override {}
    const RenderDeviceCapabilities &getCapabilities() const override { return caps; }
    const RenderDeviceDescription &getDescription() const override { return description; }
    RenderSampleCounts getSampleCountsSupported(RenderFormat) const override { return RenderSampleCount::COUNT_1; }
    bool beginCapture() override { return false; }
    bool endCapture() override { return false; }
};

struct WebGpuInterface : RenderInterface {
    RenderInterfaceCapabilities caps;
    std::vector<std::string> names{"WebGPU"};
    WebGpuInterface() { caps.shaderFormat = RenderShaderFormat::WGSL; }

    std::unique_ptr<RenderDevice> createDevice(const std::string &) override {
        Global &g = G();
        if (!g.ready) {
            static const char selector[] = "#canvas";
            MAIN_THREAD_ASYNC_EM_ASM({ WebGpuServer.init($0, $1); }, double(uintptr_t(g_status)), double(uintptr_t(selector)));
            WaitNonZero(&g_status[ST_STATE]);
            if (LoadStatus(ST_STATE) < 0) {
                fprintf(stderr, "[webgpu] the browser gave no WebGPU device\n");
                return nullptr;
            }
            g.ready = true;
            g.features = uint32_t(LoadStatus(ST_FEATURES));
            fprintf(stderr, "[webgpu] device ready: features 0x%x, canvas %dx%d, max 2D %d, storage binding %d MB\n",
                    g.features, LoadStatus(ST_CANVAS_W), LoadStatus(ST_CANVAS_H), LoadStatus(ST_MAX_TEX_2D),
                    LoadStatus(ST_MAX_STORAGE_MB));
        }
        return std::make_unique<WebGpuDevice>();
    }
    const std::vector<std::string> &getDeviceNames() const override { return names; }
    const RenderInterfaceCapabilities &getCapabilities() const override { return caps; }
};

} // namespace

std::unique_ptr<RenderInterface> CreateWebGPUInterface() { return std::make_unique<WebGpuInterface>(); }

void WebGPUSetBlendConstant(RenderCommandList *commandList, const float rgba[4]) {
    if (commandList)
        static_cast<WebGpuCommandList *>(commandList)->setBlendConstant(rgba);
}

bool WebGPUPipelineCreated(const RenderPipeline *pipeline) {
    return pipeline && static_cast<const WebGpuPipeline *>(pipeline)->id != 0;
}

bool WebGPUDeviceLost(const RenderDevice *) { return LoadStatus(ST_STATE) == -2; }

void WebGPULog(const char *message) { G().log(message ? message : ""); }

bool WebGPUCanvasSize(uint32_t &width, uint32_t &height) {
    if (LoadStatus(ST_STATE) <= 0)
        return false;
    const int32_t w = LoadStatus(ST_CANVAS_W), h = LoadStatus(ST_CANVAS_H);
    if (w <= 0 || h <= 0)
        return false;
    width = uint32_t(w);
    height = uint32_t(h);
    return true;
}

} // namespace plume
