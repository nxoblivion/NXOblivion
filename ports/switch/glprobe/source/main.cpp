// glprobe: OpenGL (Mesa nouveau via EGL) capability probe for the Switch GL backend plan.
// Logs to sdmc:/glprobe.txt, stdout and svcOutputDebugString (shows in Eden's log as Debug.Emulated).
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include <switch.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glad/glad.h>

#include "spirv_glsl.hpp"

#include "all_blobs.inc"

// ----------------------------------------------------------------------------------------------
static FILE* g_log = nullptr;

static void L(const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  size_t n = strlen(buf);
  if (g_log) {
    fwrite(buf, 1, n, g_log);
    fputc('\n', g_log);
    fflush(g_log);
  }
  printf("%s\n", buf);
  svcOutputDebugString(buf, n);
}

// long text (shader source / logs): file only + truncated debug string
static void LText(const char* tag, const std::string& s, size_t maxLines = 60) {
  L("--- %s (%zu bytes)", tag, s.size());
  size_t pos = 0, line = 0;
  while (pos < s.size() && line < maxLines) {
    size_t e = s.find('\n', pos);
    if (e == std::string::npos) e = s.size();
    std::string l = s.substr(pos, e - pos);
    if (l.size() > 400) l.resize(400);
    L("| %s", l.c_str());
    pos = e + 1;
    ++line;
  }
  if (pos < s.size()) L("| ... (truncated)");
  L("--- end %s", tag);
}

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  L("[%s] %s", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

// ----------------------------------------------------------------------------------------------
static EGLDisplay s_display;
static EGLContext s_context;
static EGLSurface s_surface;

static bool InitEgl(NWindow* win) {
  s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!s_display) {
    L("eglGetDisplay failed %x", eglGetError());
    return false;
  }
  EGLint maj = 0, min = 0;
  eglInitialize(s_display, &maj, &min);
  L("EGL %d.%d  vendor=%s  version=%s", maj, min, eglQueryString(s_display, EGL_VENDOR),
    eglQueryString(s_display, EGL_VERSION));
  L("EGL client APIs: %s", eglQueryString(s_display, EGL_CLIENT_APIS));
  L("EGL extensions: %s", eglQueryString(s_display, EGL_EXTENSIONS));
  if (eglBindAPI(EGL_OPENGL_API) == EGL_FALSE) {
    L("eglBindAPI(OPENGL) failed %x", eglGetError());
    return false;
  }
  static const EGLint fb[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                              EGL_BLUE_SIZE,       8,               EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24,
                              EGL_STENCIL_SIZE,    8,               EGL_NONE};
  EGLConfig config;
  EGLint num = 0;
  eglChooseConfig(s_display, fb, &config, 1, &num);
  if (!num) {
    L("no EGL config %x", eglGetError());
    return false;
  }
  s_surface = eglCreateWindowSurface(s_display, config, win, nullptr);
  if (!s_surface) {
    L("eglCreateWindowSurface failed %x", eglGetError());
    return false;
  }
  // Try the highest core profile version first.
  static const int vers[][2] = {{4, 6}, {4, 5}, {4, 4}, {4, 3}};
  for (auto& v : vers) {
    EGLint attr[] = {EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
                     EGL_CONTEXT_MAJOR_VERSION_KHR,       v[0],
                     EGL_CONTEXT_MINOR_VERSION_KHR,       v[1],
                     EGL_NONE};
    s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, attr);
    L("eglCreateContext core %d.%d -> %s (err %x)", v[0], v[1], s_context ? "OK" : "fail", s_context ? 0 : eglGetError());
    if (s_context) break;
  }
  if (!s_context) return false;
  return eglMakeCurrent(s_display, s_surface, s_surface, s_context) == EGL_TRUE;
}

static void ShutdownEgl() {
  if (s_display) {
    eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (s_context) eglDestroyContext(s_display, s_context);
    if (s_surface) eglDestroySurface(s_display, s_surface);
    eglTerminate(s_display);
  }
}

// ----------------------------------------------------------------------------------------------
static std::set<std::string> g_ext;

static bool HasExt(const char* e) { return g_ext.count(e) != 0; }

static GLuint CompileStage(GLenum type, const char* src, std::string* log) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  GLint len = 0;
  glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
  if (log) {
    log->assign(len > 1 ? len : 0, '\0');
    if (len > 1) glGetShaderInfoLog(s, len, nullptr, log->data());
  }
  if (!ok) {
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static GLuint LinkProgram(std::initializer_list<GLuint> shaders, std::string* log) {
  GLuint p = glCreateProgram();
  for (GLuint s : shaders) glAttachShader(p, s);
  glLinkProgram(p);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  GLint len = 0;
  glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
  if (log) {
    log->assign(len > 1 ? len : 0, '\0');
    if (len > 1) glGetProgramInfoLog(p, len, nullptr, log->data());
  }
  for (GLuint s : shaders) glDetachShader(p, s);
  if (!ok) {
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

static GLint GetI(GLenum e) {
  GLint v = -1;
  glGetIntegerv(e, &v);
  return v;
}
static GLint64 GetI64(GLenum e) {
  GLint64 v = -1;
  glGetInteger64v(e, &v);
  return v;
}
static GLint GetIi(GLenum e, GLuint i) {
  GLint v = -1;
  glGetIntegeri_v(e, i, &v);
  return v;
}

static void QueryInfo(int* glslVersion) {
  L("GL_VENDOR   : %s", glGetString(GL_VENDOR));
  L("GL_RENDERER : %s", glGetString(GL_RENDERER));
  L("GL_VERSION  : %s", glGetString(GL_VERSION));
  const char* sl = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
  L("GLSL        : %s", sl);
  int ma = 4, mi = 3;
  if (sl) sscanf(sl, "%d.%d", &ma, &mi);
  *glslVersion = ma * 100 + mi;
  L("context     : GL %d.%d, profile mask 0x%x, flags 0x%x", GetI(GL_MAJOR_VERSION), GetI(GL_MINOR_VERSION),
    GetI(GL_CONTEXT_PROFILE_MASK), GetI(GL_CONTEXT_FLAGS));

  GLint n = GetI(GL_NUM_EXTENSIONS);
  for (GLint i = 0; i < n; ++i) g_ext.insert((const char*)glGetStringi(GL_EXTENSIONS, i));
  L("=== %d extensions ===", n);
  for (auto& e : g_ext) L("  %s", e.c_str());

  static const char* kKey[] = {
      "GL_ARB_gl_spirv",
      "GL_ARB_spirv_extensions",
      "GL_ARB_compute_shader",
      "GL_ARB_shader_storage_buffer_object",
      "GL_ARB_shader_image_load_store",
      "GL_ARB_buffer_storage",
      "GL_ARB_texture_view",
      "GL_ARB_fragment_shader_interlock",
      "GL_NV_fragment_shader_interlock",
      "GL_INTEL_fragment_shader_ordering",
      "GL_ARB_clip_control",
      "GL_ARB_sparse_buffer",
      "GL_ARB_sparse_texture",
      "GL_ARB_get_program_binary",
      "GL_ARB_texture_storage_multisample",
      "GL_ARB_texture_cube_map_array",
      "GL_ARB_copy_image",
      "GL_ARB_clear_texture",
      "GL_ARB_get_texture_sub_image",
      "GL_ARB_texture_barrier",
      "GL_ARB_framebuffer_no_attachments",
      "GL_ARB_vertex_attrib_binding",
      "GL_ARB_draw_indirect",
      "GL_ARB_multi_draw_indirect",
      "GL_ARB_indirect_parameters",
      "GL_ARB_base_instance",
      "GL_ARB_draw_elements_base_vertex",
      "GL_ARB_provoking_vertex",
      "GL_ARB_depth_clamp",
      "GL_ARB_depth_buffer_float",
      "GL_ARB_shader_viewport_layer_array",
      "GL_ARB_cull_distance",
      "GL_ARB_derivative_control",
      "GL_ARB_gpu_shader5",
      "GL_ARB_gpu_shader_fp64",
      "GL_ARB_shader_bit_encoding",
      "GL_ARB_shading_language_packing",
      "GL_ARB_shader_image_size",
      "GL_ARB_shader_atomic_counters",
      "GL_ARB_shader_texture_lod",
      "GL_ARB_sample_shading",
      "GL_ARB_sync",
      "GL_ARB_direct_state_access",
      "GL_ARB_enhanced_layouts",
      "GL_ARB_explicit_uniform_location",
      "GL_ARB_texture_buffer_range",
      "GL_ARB_polygon_offset_clamp",
      "GL_ARB_conservative_depth",
      "GL_ARB_texture_compression_bptc",
      "GL_ARB_texture_compression_rgtc",
      "GL_EXT_texture_compression_s3tc",
      "GL_EXT_texture_sRGB",
      "GL_EXT_texture_filter_anisotropic",
      "GL_ARB_texture_filter_anisotropic",
      "GL_EXT_texture_swizzle",
      "GL_ARB_texture_swizzle",
      "GL_ARB_ES3_compatibility",
      "GL_ARB_ES3_1_compatibility",
      "GL_ARB_invalidate_subdata",
      "GL_ARB_shader_stencil_export",
      "GL_ARB_stencil_texturing",
      "GL_ARB_seamless_cube_map",
      "GL_ARB_seamless_cubemap_per_texture",
      "GL_ARB_sampler_objects",
      "GL_ARB_blend_func_extended",
      "GL_ARB_viewport_array",
      "GL_ARB_transform_feedback3",
      "GL_ARB_pipeline_statistics_query",
      "GL_ARB_occlusion_query2",
      "GL_ARB_query_buffer_object",
      "GL_ARB_robust_buffer_access_behavior",
      "GL_KHR_debug",
      "GL_ARB_debug_output",
      "GL_ARB_parallel_shader_compile",
      "GL_KHR_parallel_shader_compile",
  };
  L("=== key extensions ===");
  for (const char* k : kKey) L("  %-44s %s", k, HasExt(k) ? "YES" : "no");

  if (HasExt("GL_ARB_spirv_extensions")) {
    GLint ns = GetI(0x9554 /*GL_NUM_SPIR_V_EXTENSIONS*/);
    L("SPIR-V extensions (%d):", ns);
    for (GLint i = 0; i < ns; ++i) L("  %s", (const char*)glGetStringi(0x9553 /*GL_SPIR_V_EXTENSIONS*/, i));
  }
  GLint nfmt = GetI(GL_NUM_SHADER_BINARY_FORMATS);
  L("GL_NUM_SHADER_BINARY_FORMATS = %d", nfmt);
  if (nfmt > 0 && nfmt < 32) {
    std::vector<GLint> f(nfmt);
    glGetIntegerv(GL_SHADER_BINARY_FORMATS, f.data());
    for (GLint x : f) L("  shader binary format 0x%x", x);
  }
  GLint nprog = GetI(GL_NUM_PROGRAM_BINARY_FORMATS);
  L("GL_NUM_PROGRAM_BINARY_FORMATS = %d", nprog);

  L("=== limits ===");
  L("MAX_TEXTURE_SIZE                 %d", GetI(GL_MAX_TEXTURE_SIZE));
  L("MAX_3D_TEXTURE_SIZE              %d", GetI(GL_MAX_3D_TEXTURE_SIZE));
  L("MAX_CUBE_MAP_TEXTURE_SIZE        %d", GetI(GL_MAX_CUBE_MAP_TEXTURE_SIZE));
  L("MAX_ARRAY_TEXTURE_LAYERS         %d", GetI(GL_MAX_ARRAY_TEXTURE_LAYERS));
  L("MAX_TEXTURE_BUFFER_SIZE          %d", GetI(GL_MAX_TEXTURE_BUFFER_SIZE));
  L("MAX_RENDERBUFFER_SIZE            %d", GetI(GL_MAX_RENDERBUFFER_SIZE));
  L("MAX_COLOR_ATTACHMENTS            %d", GetI(GL_MAX_COLOR_ATTACHMENTS));
  L("MAX_DRAW_BUFFERS                 %d", GetI(GL_MAX_DRAW_BUFFERS));
  L("MAX_SAMPLES                      %d", GetI(GL_MAX_SAMPLES));
  L("MAX_TEXTURE_IMAGE_UNITS          %d", GetI(GL_MAX_TEXTURE_IMAGE_UNITS));
  L("MAX_COMBINED_TEXTURE_IMAGE_UNITS %d", GetI(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS));
  L("MAX_IMAGE_UNITS                  %d", GetI(GL_MAX_IMAGE_UNITS));
  L("MAX_VERTEX_ATTRIBS               %d", GetI(GL_MAX_VERTEX_ATTRIBS));
  L("MAX_UNIFORM_BLOCK_SIZE           %d", GetI(GL_MAX_UNIFORM_BLOCK_SIZE));
  L("MAX_UNIFORM_BUFFER_BINDINGS      %d", GetI(GL_MAX_UNIFORM_BUFFER_BINDINGS));
  L("MAX_SHADER_STORAGE_BLOCK_SIZE    %lld", (long long)GetI64(GL_MAX_SHADER_STORAGE_BLOCK_SIZE));
  L("MAX_SHADER_STORAGE_BUFFER_BINDINGS %d", GetI(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS));
  L("MAX_COMPUTE_SHADER_STORAGE_BLOCKS %d", GetI(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS));
  L("MAX_COMPUTE_UNIFORM_BLOCKS       %d", GetI(GL_MAX_COMPUTE_UNIFORM_BLOCKS));
  L("MAX_COMPUTE_SHARED_MEMORY_SIZE   %d", GetI(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE));
  L("MAX_COMPUTE_WORK_GROUP_INVOCATIONS %d", GetI(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS));
  L("MAX_COMPUTE_WORK_GROUP_COUNT     %d x %d x %d", GetIi(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0),
    GetIi(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 1), GetIi(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 2));
  L("MAX_COMPUTE_WORK_GROUP_SIZE      %d x %d x %d", GetIi(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 0),
    GetIi(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 1), GetIi(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 2));
  L("SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT %d", GetI(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT));
  L("UNIFORM_BUFFER_OFFSET_ALIGNMENT  %d", GetI(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT));
  L("MAX_VIEWPORTS                    %d", GetI(GL_MAX_VIEWPORTS));
  L("MAX_FRAMEBUFFER_WIDTH/HEIGHT     %d / %d", GetI(GL_MAX_FRAMEBUFFER_WIDTH), GetI(GL_MAX_FRAMEBUFFER_HEIGHT));
  L("MAX_VERTEX_SHADER_STORAGE_BLOCKS %d", GetI(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS));
  L("MAX_FRAGMENT_SHADER_STORAGE_BLOCKS %d", GetI(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS));
  L("MAX_VERTEX_UNIFORM_COMPONENTS    %d", GetI(GL_MAX_VERTEX_UNIFORM_COMPONENTS));
  L("MAX_FRAGMENT_UNIFORM_COMPONENTS  %d", GetI(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS));
  if (HasExt("GL_EXT_texture_filter_anisotropic") || HasExt("GL_ARB_texture_filter_anisotropic")) {
    GLfloat a = 0;
    glGetFloatv(0x84FF /*GL_MAX_TEXTURE_MAX_ANISOTROPY*/, &a);
    L("MAX_TEXTURE_MAX_ANISOTROPY       %g", a);
  }
}

// ----------------------------------------------------------------------------------------------
// Clear + triangle + compute self tests
static const char* kTriVs = R"(#version 430 core
const vec2 p[3] = vec2[3](vec2(-0.6,-0.6), vec2(0.6,-0.6), vec2(0.0,0.7));
void main(){ gl_Position = vec4(p[gl_VertexID], 0.0, 1.0); })";
static const char* kTriFs = R"(#version 430 core
layout(location=0) out vec4 o;
void main(){ o = vec4(1.0, 0.55, 0.0, 1.0); })";
static const char* kCompCs = R"(#version 430 core
layout(local_size_x = 16) in;
layout(std430, binding = 0) buffer Out { uint v[]; };
void main(){ uint i = gl_GlobalInvocationID.x; v[i] = i * 3u + 1u; })";

static GLuint g_triProg, g_vao;
static float g_clear[4] = {0.05f, 0.25f, 0.55f, 1.0f};  // blue

static void SetupTriangle() {
  std::string log;
  GLuint vs = CompileStage(GL_VERTEX_SHADER, kTriVs, &log);
  if (!log.empty()) LText("tri vs log", log);
  GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kTriFs, &log);
  if (!log.empty()) LText("tri fs log", log);
  Check(vs && fs, "triangle shaders compile");
  g_triProg = LinkProgram({vs, fs}, &log);
  if (!log.empty()) LText("tri link log", log);
  Check(g_triProg != 0, "triangle program links");
  glGenVertexArrays(1, &g_vao);
}

static void DrawFrame(int w, int h) {
  glViewport(0, 0, w, h);
  glClearColor(g_clear[0], g_clear[1], g_clear[2], g_clear[3]);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glUseProgram(g_triProg);
  glBindVertexArray(g_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void TestRender(int w, int h) {
  DrawFrame(w, h);
  unsigned char px[4] = {0};
  glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
  L("center pixel (triangle) = %u,%u,%u,%u (expect ~255,140,0,255)", px[0], px[1], px[2], px[3]);
  Check(px[0] > 240 && px[1] > 120 && px[1] < 160 && px[2] < 10, "triangle drawn (center pixel readback)");
  unsigned char c[4] = {0};
  glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
  L("corner pixel (clear)     = %u,%u,%u,%u (expect ~13,64,140,255)", c[0], c[1], c[2], c[3]);
  Check(abs(c[0] - 13) < 3 && abs(c[1] - 64) < 3 && abs(c[2] - 140) < 3, "clear colour (corner readback)");
  Check(glGetError() == GL_NO_ERROR, "no GL error after render");
}

static void TestCompute() {
  std::string log;
  GLuint cs = CompileStage(GL_COMPUTE_SHADER, kCompCs, &log);
  if (!log.empty()) LText("compute log", log);
  Check(cs != 0, "trivial compute shader compiles");
  if (!cs) return;
  GLuint prog = LinkProgram({cs}, &log);
  if (!log.empty()) LText("compute link log", log);
  Check(prog != 0, "trivial compute program links");
  if (!prog) return;
  const int N = 64;
  GLuint buf;
  glGenBuffers(1, &buf);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
  std::vector<uint32_t> zero(N, 0xDEADBEEFu);
  glBufferData(GL_SHADER_STORAGE_BUFFER, N * 4, zero.data(), GL_DYNAMIC_COPY);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buf);
  glUseProgram(prog);
  glDispatchCompute(N / 16, 1, 1);
  glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
  const uint32_t* m = (const uint32_t*)glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, N * 4, GL_MAP_READ_BIT);
  bool ok = m != nullptr;
  if (m) {
    for (int i = 0; i < N; ++i)
      if (m[i] != (uint32_t)(i * 3 + 1)) {
        ok = false;
        L("  mismatch at %d: %08x", i, m[i]);
        break;
      }
    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
  }
  Check(ok, "compute wrote SSBO and results verified (v[i]=i*3+1, 64 values)");
  glDeleteBuffers(1, &buf);
  glDeleteProgram(prog);
  glDeleteShader(cs);
  glUseProgram(0);
}

// ----------------------------------------------------------------------------------------------
// SPIRV-Cross
static bool CrossCompile(const Blob& b, int glsl, std::string* out, std::string* err) {
  try {
    std::vector<uint32_t> words(b.data, b.data + b.bytes / 4);
    spirv_cross::CompilerGLSL comp(std::move(words));
    spirv_cross::CompilerGLSL::Options o;
    o.version = glsl;
    o.es = false;
    o.vulkan_semantics = false;
    o.separate_shader_objects = true;
    o.enable_420pack_extension = false;
    o.emit_push_constant_as_uniform_buffer = true;  // push constants -> UBO at a reserved binding
    comp.set_common_options(o);
    auto res = comp.get_shader_resources();
    // GL has a single binding namespace per resource type; the Vulkan set/binding pair is flattened.
    auto remap = [&](spirv_cross::SmallVector<spirv_cross::Resource>& v) {
      for (auto& r : v) {
        uint32_t set = comp.get_decoration(r.id, spv::DecorationDescriptorSet);
        uint32_t bind = comp.get_decoration(r.id, spv::DecorationBinding);
        comp.unset_decoration(r.id, spv::DecorationDescriptorSet);
        comp.set_decoration(r.id, spv::DecorationBinding, set * 8 + bind);
      }
    };
    // Vulkan separate images/samplers -> GL combined texture units (one unit per image+sampler pair).
    if (!res.separate_images.empty()) {
      comp.build_dummy_sampler_for_combined_images();
      comp.build_combined_image_samplers();
      uint32_t unit = 0;
      for (auto& c : comp.get_combined_image_samplers()) {
        comp.set_decoration(c.combined_id, spv::DecorationBinding, unit++);
        comp.unset_decoration(c.combined_id, spv::DecorationDescriptorSet);
      }
      res.separate_images.clear();
      res.separate_samplers.clear();
    }
    for (auto& pc : res.push_constant_buffers) comp.set_decoration(pc.id, spv::DecorationBinding, 15);
    remap(res.uniform_buffers);
    remap(res.storage_buffers);
    remap(res.sampled_images);
    remap(res.separate_images);
    remap(res.separate_samplers);
    remap(res.storage_images);
    *out = comp.compile();
    // gl_PerVertex with gl_CullDistance needs the extension enabled in GLSL 4.30.
    if (out->find("gl_CullDistance") != std::string::npos) {
      size_t nl = out->find('\n');
      out->insert(nl + 1, "#extension GL_ARB_cull_distance : require\n");
    }
    return true;
  } catch (const std::exception& e) {
    *err = e.what();
    return false;
  }
}

static GLenum StageOf(const std::string& n) {
  if (n.size() > 3 && n.compare(n.size() - 3, 3, "_vs") == 0) return GL_VERTEX_SHADER;
  if (n.size() > 3 && n.compare(n.size() - 3, 3, "_ps") == 0) return GL_FRAGMENT_SHADER;
  return GL_COMPUTE_SHADER;
}

typedef void(APIENTRY* PFNSPEC)(GLuint, const GLchar*, GLuint, const GLuint*, const GLuint*);

static void TestSpirvCross(int glslVersion) {
  L("=== SPIRV-Cross sweep: %zu SDK blobs -> GLSL %d ===", sizeof(kBlobs) / sizeof(kBlobs[0]), glslVersion);
  int nCross = 0, nComp = 0, nLink = 0, total = 0;
  std::vector<std::string> failures;
  for (const Blob& b : kBlobs) {
    ++total;
    std::string glsl, err;
    if (!CrossCompile(b, glslVersion, &glsl, &err)) {
      failures.push_back(std::string(b.name) + ": SPIRV-Cross error: " + err);
      continue;
    }
    ++nCross;
    bool detailed = (strcmp(b.name, "texture_load_32bpb_cs") == 0);
    if (detailed) LText("GLSL for texture_load_32bpb_cs", glsl, 80);
    std::string log;
    GLenum st = StageOf(b.name);
    GLuint sh = CompileStage(st, glsl.c_str(), &log);
    if (detailed && !log.empty()) LText("compile log texture_load_32bpb_cs", log);
    if (!sh) {
      failures.push_back(std::string(b.name) + ": glCompileShader failed: " + log.substr(0, 300));
      LText((std::string("GLSL of failing ") + b.name).c_str(), glsl, 40);
      continue;
    }
    ++nComp;
    if (st == GL_COMPUTE_SHADER) {
      GLuint p = LinkProgram({sh}, &log);
      if (p) {
        ++nLink;
        glDeleteProgram(p);
      } else {
        failures.push_back(std::string(b.name) + ": link failed: " + log.substr(0, 300));
      }
    }
    glDeleteShader(sh);
  }
  L("SPIRV-Cross: %d/%d blobs cross-compiled, %d/%d GL-compiled, %d compute programs linked", nCross, total, nComp, total,
    nLink);
  for (auto& f : failures) L("  FAIL %s", f.c_str());
  // explicit headline test on a real SDK compute shader
  for (const Blob& b : kBlobs) {
    if (strcmp(b.name, "texture_load_32bpb_cs") != 0) continue;
    std::string glsl, err, log;
    bool x = CrossCompile(b, glslVersion, &glsl, &err);
    GLuint sh = x ? CompileStage(GL_COMPUTE_SHADER, glsl.c_str(), &log) : 0;
    GLuint p = sh ? LinkProgram({sh}, &log) : 0;
    Check(p != 0, "SDK texture_load_32bpb_cs: SPIR-V -> GLSL (SPIRV-Cross) -> glCompileShader -> glLinkProgram");
    if (p) glDeleteProgram(p);
    if (sh) glDeleteShader(sh);
  }
}

static void TestGlSpirv() {
  L("=== ARB_gl_spirv direct path ===");
  bool adv = HasExt("GL_ARB_gl_spirv");
  L("GL_ARB_gl_spirv advertised: %s", adv ? "YES" : "no");
  PFNSPEC spec = (PFNSPEC)eglGetProcAddress("glSpecializeShader");
  if (!spec) spec = (PFNSPEC)eglGetProcAddress("glSpecializeShaderARB");
  L("glSpecializeShader pointer: %s", spec ? "non-null" : "null");
  int ok = 0, total = 0;
  for (const Blob& b : kBlobs) {
    if (StageOf(b.name) != GL_COMPUTE_SHADER) continue;
    if (strcmp(b.name, "texture_load_32bpb_cs") != 0 && strcmp(b.name, "resolve_full_32bpp_cs") != 0 &&
        strcmp(b.name, "host_depth_store_1xmsaa_cs") != 0)
      continue;
    ++total;
    if (!adv || !spec) {
      L("  %s: skipped (extension/entry point missing)", b.name);
      continue;
    }
    while (glGetError() != GL_NO_ERROR) {}
    GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
    glShaderBinary(1, &sh, 0x9551 /*GL_SHADER_BINARY_FORMAT_SPIR_V*/, b.data, (GLsizei)b.bytes);
    GLenum e1 = glGetError();
    spec(sh, "main", 0, nullptr, nullptr);
    GLenum e2 = glGetError();
    GLint st = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &st);
    GLint len = 0;
    glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
    std::string log(len > 1 ? len : 0, '\0');
    if (len > 1) glGetShaderInfoLog(sh, len, nullptr, log.data());
    L("  %s: glShaderBinary err=0x%x, glSpecializeShader err=0x%x, COMPILE_STATUS=%d %s", b.name, e1, e2, st,
      log.substr(0, 300).c_str());
    if (st) ++ok;
    glDeleteShader(sh);
  }
  L("ARB_gl_spirv direct: %d/%d sampled blobs accepted", ok, total);
}

// ----------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
  g_log = fopen("sdmc:/glprobe.txt", "w");
  L("glprobe starting (log %s)", g_log ? "sdmc:/glprobe.txt" : "UNAVAILABLE");
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  NWindow* win = nwindowGetDefault();
  {
    u32 w = 0, h = 0;
    nwindowGetDimensions(win, &w, &h);
    L("default NWindow dimensions %ux%u", w, h);
  }
  if (!InitEgl(win)) {
    L("EGL init FAILED");
    if (g_log) fclose(g_log);
    return 1;
  }
  if (!gladLoadGLLoader((GLADloadproc)eglGetProcAddress)) {
    L("gladLoadGLLoader failed");
    return 1;
  }
  int glslVersion = 430;
  QueryInfo(&glslVersion);

  EGLint sw = 0, sh = 0;
  eglQuerySurface(s_display, s_surface, EGL_WIDTH, &sw);
  eglQuerySurface(s_display, s_surface, EGL_HEIGHT, &sh);
  if (sw <= 0 || sh <= 0) {
    u32 ww = 1280, hh = 720;
    nwindowGetDimensions(win, &ww, &hh);
    sw = ww;
    sh = hh;
    L("eglQuerySurface gave 0x0, using NWindow dims");
  }
  L("EGL surface %dx%d", sw, sh);

  SetupTriangle();
  TestRender(sw, sh);
  TestCompute();
  TestGlSpirv();
  TestSpirvCross(glslVersion);

  L("=== SUMMARY: %d check failure(s) ===", g_fail);
  L("Drawing the test frame until the process is killed (or + pressed)...");

  u64 t0 = armGetSystemTick();
  int frame = 0;
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    DrawFrame(sw, sh);
    eglSwapBuffers(s_display, s_surface);
    ++frame;
    if (frame == 120) L("rendered 120 frames, %.2f s", (double)(armGetSystemTick() - t0) / armGetSystemTickFreq());
    if (frame == 121) L("PROBE_DONE");
  }
  ShutdownEgl();
  if (g_log) fclose(g_log);
  return 0;
}
