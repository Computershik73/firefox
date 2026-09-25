/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Pixel shading on a Direct3D 11 device of feature level 9_3 or better.
//
// WebRender cannot run on GPUs that stop at feature level 9_3: its vertex
// shaders read every primitive's data from textures, and 9_3 has no vertex
// texture fetch. SWGL already runs those vertex shaders on the CPU, four
// vertices at a time, and then rasterises and shades on the CPU too -- which
// is where nearly all of software rendering's time goes. With a device
// attached (SwglGpuAttach), a draw whose program has a 9_3 pixel shader
// (GpuProgramInfo, built from the same GLSL by glsl-to-cxx) stops after the
// vertex stage: the quads and their varyings go to the GPU, which rasterises
// and shades them into GPU copies of SWGL's textures.
//
// Everything else stays as it was. Each texture keeps its CPU buffer; a GPU
// copy is made when the GPU first needs it, and the two are kept in step by
// two flags. Before a CPU reader looks at a texture (prepare_texture) a GPU
// copy that is newer is read back; before the GPU uses one, a CPU buffer
// that is newer is uploaded. Programs, blend modes and formats the GPU path
// does not cover simply run on the CPU as before.
//
// Rows: SWGL keeps GL's convention, row 0 of a buffer is GL's y = 0. The GPU
// vertex shaders flip clip-space Y, which makes Direct3D's row 0 the same
// row, so memory layouts match and viewport and scissor rectangles carry
// over unchanged.

#if defined(_WIN32) && SWGL_GPU_SHADERS

#  define SWGL_GPU 1

#  include <d3d11.h>
#  include <unordered_map>
#  include <vector>

namespace swgl_gpu {

// SWGL_GPU_TRACE=1: a line on stderr for every draw, clear, upload and
// readback. For the desktop test harness; never set on a phone.
static bool trace() {
  static int on = -1;
  if (on < 0) on = getenv("SWGL_GPU_TRACE") ? 1 : 0;
  return on == 1;
}

static const int MAX_QUADS_PER_FLUSH = 2048;
static const int FLAT_TEX_WIDTH = 1024;
static const int FLAT_TEX_HEIGHT = 64;

template <typename T>
static inline void safe_release(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

// Bytes of CPU buffers freed because the GPU holds the texture (resident).
static int64_t resident_bytes = 0;
// Bytes of Direct3D textures: SWGL textures' GPU copies, and the caches
// (spare depth buffers, readback staging, feedback copies). On a phone they
// come out of the same memory as everything else.
static int64_t gpu_tex_bytes = 0;
static int64_t gpu_cache_bytes = 0;

static int64_t texel_bytes(DXGI_FORMAT f, int64_t w, int64_t h) {
  int b = f == DXGI_FORMAT_R8_UNORM                ? 1
          : f == DXGI_FORMAT_R8G8_UNORM            ? 2
          : f == DXGI_FORMAT_R32G32B32A32_FLOAT    ? 16
                                                   : 4;
  return w * h * b;
}

static int64_t texture_bytes(ID3D11Texture2D* t) {
  if (!t) return 0;
  D3D11_TEXTURE2D_DESC d;
  t->GetDesc(&d);
  return texel_bytes(d.Format, d.Width, d.Height);
}

}  // namespace swgl_gpu

// The GPU side of one SWGL texture.
struct GpuTex {
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  GLenum internal_format = 0;
  int width = 0;
  int height = 0;
  // Whether the GPU copy holds content the CPU buffer lacks, and the other
  // way round. Both false means they agree, or that neither holds anything
  // anybody still wants.
  bool gpu_newer = false;
  bool cpu_newer = true;
  // Stored as BGRA although SWGL's buffer is one byte a pixel, because the
  // device cannot sample or render the narrower format.
  bool expanded = false;
  // A texture that belongs to someone else -- a compositor tile -- of which
  // this SWGL texture is the rectangle at (ox, oy). It is never recreated,
  // and SWGL keeps no CPU buffer for it unless a CPU draw needs one.
  bool external = false;
  // ... and one that is only read, never drawn into: a video frame decoded
  // on this device (SwglGpuSetTextureSource). Draws may sample it.
  bool source = false;
  int ox = 0;
  int oy = 0;
  // A colour texture whose CPU buffer was freed once the GPU copy was
  // current: on a phone the GPU's memory is the phone's memory, and two
  // copies of every render target and atlas are twice the memory. The CPU
  // buffer comes back (read back) when something on the CPU needs it.
  bool resident = false;
  // The Direct3D texture's own size: larger than SWGL's for an external
  // texture, and for a depth buffer shared with one.
  int tex_w = 0;
  int tex_h = 0;

  // What this texture counts in resident_bytes while resident.
  int64_t resident_size = 0;
  // What its Direct3D texture counts in gpu_tex_bytes (0 for external ones).
  int64_t gpu_size = 0;

  void release() {
    if (resident) {
      swgl_gpu::resident_bytes -= resident_size;
      resident_size = 0;
    }
    swgl_gpu::gpu_tex_bytes -= gpu_size;
    gpu_size = 0;
    swgl_gpu::safe_release(srv);
    swgl_gpu::safe_release(rtv);
    swgl_gpu::safe_release(dsv);
    swgl_gpu::safe_release(tex);
    width = height = 0;
    tex_w = tex_h = 0;
    format = DXGI_FORMAT_UNKNOWN;
    external = false;
    source = false;
    resident = false;
    ox = oy = 0;
  }
  ~GpuTex() { release(); }
};

namespace swgl_gpu {

static void mark_resident(GpuTex& gt, const Texture& t) {
  if (gt.resident) return;
  gt.resident = true;
  gt.resident_size =
      int64_t(t.width) * t.height * (t.internal_format == GL_R8 ? 1 : 4);
  resident_bytes += gt.resident_size;
}

static void unmark_resident(GpuTex& gt) {
  if (!gt.resident) return;
  gt.resident = false;
  resident_bytes -= gt.resident_size;
  gt.resident_size = 0;
}

}  // namespace swgl_gpu

struct GpuBackend {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* dc = nullptr;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_9_1;

  bool r8_ok = false;
  bool rg8_ok = false;
  bool rgba32f_ok = false;

  ID3D11VertexShader* vs[9] = {};
  ID3D11InputLayout* il[9] = {};
  ID3D11VertexShader* clear_vs = nullptr;
  ID3D11PixelShader* clear_ps = nullptr;
  ID3D11PixelShader* copy_ps = nullptr;
  // The CPU draws into this when the GPU places the result, and the quads
  // they drew are kept here (four float4 positions each).
  Texture* scratch = nullptr;
  bool recording = false;
  std::vector<float> recorded;
  ID3D11InputLayout* clear_il = nullptr;

  ID3D11Buffer* vb = nullptr;
  UINT vb_size = 0;
  UINT vb_pos = 0;
  ID3D11Buffer* ib = nullptr;
  ID3D11Buffer* cb = nullptr;
  ID3D11Buffer* clear_cb = nullptr;
  ID3D11Buffer* clear_vb = nullptr;

  ID3D11Texture2D* flat_tex = nullptr;
  ID3D11ShaderResourceView* flat_srv = nullptr;
  // What a sampler with no texture behind it reads, as in GL: (0, 0, 0, 1).
  ID3D11ShaderResourceView* empty_srv = nullptr;

  ID3D11SamplerState* samp_linear = nullptr;
  ID3D11SamplerState* samp_point = nullptr;
  ID3D11RasterizerState* raster = nullptr;
  ID3D11DepthStencilState* depth_off = nullptr;
  ID3D11DepthStencilState* depth_states[2][2] = {};  // [less/lequal][mask]
  ID3D11DepthStencilState* depth_clear = nullptr;
  std::unordered_map<uint64_t, ID3D11BlendState*> blends;
  std::unordered_map<const GpuProgramInfo*, ID3D11PixelShader*> pixel_shaders;
  // Programs whose pixel shader the device refused.
  std::unordered_map<const GpuProgramInfo*, bool> refused;

  std::vector<float> verts;
  std::vector<float> flats;
  std::vector<char> interps;

  // Readback staging textures by (format, width, height).
  std::unordered_map<uint64_t, ID3D11Texture2D*> staging;
  // Copies of render targets that a draw also samples, likewise.
  std::unordered_map<uint64_t,
                     std::pair<ID3D11Texture2D*, ID3D11ShaderResourceView*>>
      feedback;
  // Direct3D binds a depth buffer only with a render target of exactly its
  // size, and compositor tiles come in several: the shared depth texture
  // takes the buffer that fits the tile at hand, the others wait here.
  struct SpareDepth {
    int w, h;
    ID3D11Texture2D* tex;
    ID3D11DepthStencilView* dsv;
  };
  std::vector<SpareDepth> spare_depth;
  // Present when the device runs with the Direct3D debug layer.
  ID3D11InfoQueue* iq = nullptr;

  bool broken = false;

  struct Stats {
    uint32_t gpu_draws = 0;
    uint32_t cpu_draws = 0;
    uint32_t gpu_quads = 0;
    uint32_t uploads = 0;
    uint32_t readbacks = 0;
    uint32_t gpu_clears = 0;
    uint32_t refused_program = 0;
    uint32_t refused_state = 0;
    uint32_t depth_lost = 0;
    uint32_t cpu_freed = 0;
    uint32_t cpu_restored = 0;
  } stats;

  ~GpuBackend() {
    using swgl_gpu::safe_release;
    delete scratch;
    safe_release(copy_ps);
    for (auto& v : vs) safe_release(v);
    for (auto& l : il) safe_release(l);
    safe_release(clear_vs);
    safe_release(clear_ps);
    safe_release(clear_il);
    safe_release(vb);
    safe_release(ib);
    safe_release(cb);
    safe_release(clear_cb);
    safe_release(clear_vb);
    safe_release(flat_srv);
    safe_release(flat_tex);
    safe_release(empty_srv);
    safe_release(samp_linear);
    safe_release(samp_point);
    safe_release(raster);
    safe_release(depth_off);
    for (auto& a : depth_states)
      for (auto& d : a) safe_release(d);
    safe_release(depth_clear);
    for (auto& b : blends) b.second->Release();
    for (auto& p : pixel_shaders) p.second->Release();
    for (auto& s : staging) s.second->Release();
    for (auto& f : feedback) {
      f.second.first->Release();
      f.second.second->Release();
    }
    for (auto& d : spare_depth) {
      d.dsv->Release();
      d.tex->Release();
    }
    safe_release(iq);
    safe_release(dc);
    safe_release(dev);
  }
};

namespace swgl_gpu {

// With the Direct3D debug layer (SWGL_GPU_DEBUG=1 for a device of SWGL's
// own), prints what it found wrong since the last call: devices differ in
// what they forgive, and a phone's driver forgives less.
static void debug_check(GpuBackend* g, const char* where) {
  if (!g || !g->iq) return;
  UINT64 n = g->iq->GetNumStoredMessages();
  for (UINT64 i = 0; i < n; i++) {
    SIZE_T len = 0;
    if (FAILED(g->iq->GetMessage(i, nullptr, &len)) || !len) continue;
    std::vector<char> buf(len);
    D3D11_MESSAGE* m = (D3D11_MESSAGE*)buf.data();
    if (FAILED(g->iq->GetMessage(i, m, &len))) continue;
    if (m->Severity > D3D11_MESSAGE_SEVERITY_WARNING) continue;
    fprintf(stderr, "gpu: d3d %s before %s: %.*s\n",
            m->Severity == D3D11_MESSAGE_SEVERITY_WARNING ? "warning" : "ERROR",
            where, int(m->DescriptionByteLength), m->pDescription);
  }
  g->iq->ClearStoredMessages();
}

static bool init_backend(GpuBackend* g) {
  HRESULT hr;
  g->level = g->dev->GetFeatureLevel();

  UINT support = 0;
  g->r8_ok = SUCCEEDED(g->dev->CheckFormatSupport(DXGI_FORMAT_R8_UNORM,
                                                  &support)) &&
             (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) &&
             (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET) &&
             (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE);
  support = 0;
  g->rg8_ok = SUCCEEDED(g->dev->CheckFormatSupport(DXGI_FORMAT_R8G8_UNORM,
                                                   &support)) &&
              (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) &&
              (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE);
  support = 0;
  g->rgba32f_ok =
      SUCCEEDED(g->dev->CheckFormatSupport(DXGI_FORMAT_R32G32B32A32_FLOAT,
                                           &support)) &&
      (support & D3D11_FORMAT_SUPPORT_TEXTURE2D);

  const unsigned char* vs_code[9] = {nullptr,      swgl_gpu_vs1, swgl_gpu_vs2,
                                     swgl_gpu_vs3, swgl_gpu_vs4, swgl_gpu_vs5,
                                     swgl_gpu_vs6, swgl_gpu_vs7, swgl_gpu_vs8};
  const size_t vs_size[9] = {0,
                             sizeof(swgl_gpu_vs1),
                             sizeof(swgl_gpu_vs2),
                             sizeof(swgl_gpu_vs3),
                             sizeof(swgl_gpu_vs4),
                             sizeof(swgl_gpu_vs5),
                             sizeof(swgl_gpu_vs6),
                             sizeof(swgl_gpu_vs7),
                             sizeof(swgl_gpu_vs8)};
  for (int slots = 1; slots <= 8; slots++) {
    hr = g->dev->CreateVertexShader(vs_code[slots], vs_size[slots], nullptr,
                                    &g->vs[slots]);
    if (FAILED(hr)) return false;
    D3D11_INPUT_ELEMENT_DESC desc[9];
    desc[0] = {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
               D3D11_INPUT_PER_VERTEX_DATA, 0};
    for (int i = 0; i < slots; i++) {
      desc[i + 1] = {"TEXCOORD", UINT(i), DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                     UINT(16 * (i + 1)), D3D11_INPUT_PER_VERTEX_DATA, 0};
    }
    hr = g->dev->CreateInputLayout(desc, slots + 1, vs_code[slots],
                                   vs_size[slots], &g->il[slots]);
    if (FAILED(hr)) return false;
  }
  hr = g->dev->CreateVertexShader(swgl_gpu_clear_vs, sizeof(swgl_gpu_clear_vs),
                                  nullptr, &g->clear_vs);
  if (FAILED(hr)) return false;
  hr = g->dev->CreatePixelShader(swgl_gpu_copy_ps, sizeof(swgl_gpu_copy_ps),
                                 nullptr, &g->copy_ps);
  if (FAILED(hr)) return false;
  hr = g->dev->CreatePixelShader(swgl_gpu_clear_ps, sizeof(swgl_gpu_clear_ps),
                                 nullptr, &g->clear_ps);
  if (FAILED(hr)) return false;
  {
    D3D11_INPUT_ELEMENT_DESC desc = {"POSITION", 0,
                                     DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
                                     D3D11_INPUT_PER_VERTEX_DATA, 0};
    hr = g->dev->CreateInputLayout(&desc, 1, swgl_gpu_clear_vs,
                                   sizeof(swgl_gpu_clear_vs), &g->clear_il);
    if (FAILED(hr)) return false;
  }

  // Vertex ring: enough for a flush of the widest layout.
  g->vb_size = MAX_QUADS_PER_FLUSH * 4 * 16 * 9;
  {
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = g->vb_size;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = g->dev->CreateBuffer(&bd, nullptr, &g->vb);
    if (FAILED(hr)) return false;
  }
  // SWGL's vertex stage leaves a quad's corners in the order they go round
  // the polygon (load_attrib swaps WebRender's third and fourth vertices), so
  // the quad is the fan 0 1 2, 0 2 3.
  {
    std::vector<uint16_t> idx(MAX_QUADS_PER_FLUSH * 6);
    for (int q = 0; q < MAX_QUADS_PER_FLUSH; q++) {
      uint16_t b = uint16_t(q * 4);
      uint16_t* o = &idx[q * 6];
      o[0] = b;
      o[1] = b + 1;
      o[2] = b + 2;
      o[3] = b;
      o[4] = b + 2;
      o[5] = b + 3;
    }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = UINT(idx.size() * sizeof(uint16_t));
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = {idx.data(), 0, 0};
    hr = g->dev->CreateBuffer(&bd, &init, &g->ib);
    if (FAILED(hr)) return false;
  }
  {
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16 * 20;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = g->dev->CreateBuffer(&bd, nullptr, &g->cb);
    if (FAILED(hr)) return false;
    bd.ByteWidth = 16;
    hr = g->dev->CreateBuffer(&bd, nullptr, &g->clear_cb);
    if (FAILED(hr)) return false;
  }
  {
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16 * 4;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    hr = g->dev->CreateBuffer(&bd, nullptr, &g->clear_vb);
    if (FAILED(hr)) return false;
  }
  if (g->rgba32f_ok) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = FLAT_TEX_WIDTH;
    td.Height = FLAT_TEX_HEIGHT;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = g->dev->CreateTexture2D(&td, nullptr, &g->flat_tex);
    if (SUCCEEDED(hr)) {
      hr = g->dev->CreateShaderResourceView(g->flat_tex, nullptr, &g->flat_srv);
    }
    if (FAILED(hr)) {
      safe_release(g->flat_tex);
      safe_release(g->flat_srv);
    }
  }
  {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 1;
    td.Height = 1;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    uint32_t black = 0xFF000000;
    D3D11_SUBRESOURCE_DATA init = {&black, 4, 4};
    ID3D11Texture2D* t = nullptr;
    hr = g->dev->CreateTexture2D(&td, &init, &t);
    if (FAILED(hr)) return false;
    hr = g->dev->CreateShaderResourceView(t, nullptr, &g->empty_srv);
    t->Release();
    if (FAILED(hr)) return false;
  }
  {
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    hr = g->dev->CreateSamplerState(&sd, &g->samp_linear);
    if (FAILED(hr)) return false;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    hr = g->dev->CreateSamplerState(&sd, &g->samp_point);
    if (FAILED(hr)) return false;
  }
  {
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    hr = g->dev->CreateRasterizerState(&rd, &g->raster);
    if (FAILED(hr)) return false;
  }
  {
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    hr = g->dev->CreateDepthStencilState(&dd, &g->depth_off);
    if (FAILED(hr)) return false;
    for (int f = 0; f < 2; f++) {
      for (int m = 0; m < 2; m++) {
        dd.DepthEnable = TRUE;
        dd.DepthFunc =
            f ? D3D11_COMPARISON_LESS_EQUAL : D3D11_COMPARISON_LESS;
        dd.DepthWriteMask =
            m ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        hr = g->dev->CreateDepthStencilState(&dd, &g->depth_states[f][m]);
        if (FAILED(hr)) return false;
      }
    }
    // Writes the quad's depth everywhere: a scissored depth clear.
    dd.DepthEnable = TRUE;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    hr = g->dev->CreateDepthStencilState(&dd, &g->depth_clear);
    if (FAILED(hr)) return false;
  }
  return true;
}

static inline bool gl_format_on_gpu(GpuBackend* g, GLenum internal_format,
                                    DXGI_FORMAT* format, bool* expanded) {
  *expanded = false;
  switch (internal_format) {
    case GL_RGBA8:
      *format = DXGI_FORMAT_B8G8R8A8_UNORM;
      return true;
    case GL_R8:
      if (g->r8_ok) {
        *format = DXGI_FORMAT_R8_UNORM;
      } else {
        *format = DXGI_FORMAT_B8G8R8A8_UNORM;
        *expanded = true;
      }
      return true;
    case GL_RG8:
      // The chroma plane of NV12 video.
      *format = DXGI_FORMAT_R8G8_UNORM;
      return g->rg8_ok;
    case GL_RGBA32F:
    case GL_RGBA32I:
      *format = DXGI_FORMAT_R32G32B32A32_FLOAT;
      return g->rgba32f_ok;
    case GL_DEPTH_COMPONENT24:
      *format = DXGI_FORMAT_D24_UNORM_S8_UINT;
      return true;
    default:
      return false;
  }
}

// Whether a texture of this format can be the GPU's render target.
static inline bool renderable(GLenum internal_format) {
  return internal_format == GL_RGBA8 || internal_format == GL_R8;
}

// Makes sure t has a GPU copy of the right size and format. Returns false if
// its format has no place on this device.
static bool ensure_resource(GpuBackend* g, Texture& t, int min_w = 0,
                            int min_h = 0) {
  if (!t.gpu) {
    t.gpu = new GpuTex;
  }
  GpuTex& gt = *t.gpu;
  if (gt.external) {
    return gt.tex != nullptr;
  }
  int want_w = std::max(t.width, min_w);
  int want_h = std::max(t.height, min_h);
  bool depth = t.internal_format == GL_DEPTH_COMPONENT24;
  // Sized for a render target: exactly its size (see SpareDepth).
  bool exact = depth && (min_w > 0 || min_h > 0);
  if (gt.tex && gt.width == t.width && gt.height == t.height &&
      gt.internal_format == t.internal_format &&
      (exact ? gt.tex_w == want_w && gt.tex_h == want_h
             : gt.tex_w >= want_w && gt.tex_h >= want_h)) {
    return true;
  }
  // A texture that lives only on the GPU stays so on a new resource.
  bool keep_resident = gt.resident && !t.buf;
  if (depth && gt.tex && gt.dsv && gt.internal_format == t.internal_format) {
    if (g->spare_depth.size() >= 4) {
      gpu_cache_bytes -= texture_bytes(g->spare_depth.front().tex);
      g->spare_depth.front().dsv->Release();
      g->spare_depth.front().tex->Release();
      g->spare_depth.erase(g->spare_depth.begin());
    }
    g->spare_depth.push_back({gt.tex_w, gt.tex_h, gt.tex, gt.dsv});
    gpu_tex_bytes -= gt.gpu_size;
    gpu_cache_bytes += gt.gpu_size;
    gt.gpu_size = 0;
    gt.tex = nullptr;
    gt.dsv = nullptr;
  }
  gt.release();
  if (keep_resident) mark_resident(gt, t);
  if (t.width <= 0 || t.height <= 0) return false;
  DXGI_FORMAT format;
  bool expanded;
  if (!gl_format_on_gpu(g, t.internal_format, &format, &expanded)) {
    return false;
  }
  if (depth) {
    for (size_t i = 0; i < g->spare_depth.size(); i++) {
      auto& d = g->spare_depth[i];
      if (d.w != want_w || d.h != want_h) continue;
      gt.tex = d.tex;
      gt.dsv = d.dsv;
      gt.gpu_size = texture_bytes(d.tex);
      gpu_cache_bytes -= gt.gpu_size;
      gpu_tex_bytes += gt.gpu_size;
      g->spare_depth.erase(g->spare_depth.begin() + i);
      gt.format = format;
      gt.internal_format = t.internal_format;
      gt.width = t.width;
      gt.height = t.height;
      gt.tex_w = want_w;
      gt.tex_h = want_h;
      gt.expanded = expanded;
      // What it held belongs to another tile.
      gt.gpu_newer = false;
      gt.cpu_newer = true;
      return true;
    }
  }
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = want_w;
  td.Height = want_h;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = format;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  if (t.internal_format == GL_DEPTH_COMPONENT24) {
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  } else if (renderable(t.internal_format)) {
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  } else {
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  }
  if (FAILED(g->dev->CreateTexture2D(&td, nullptr, &gt.tex))) {
    gt.release();
    return false;
  }
  HRESULT hr = S_OK;
  if (td.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
    hr = g->dev->CreateShaderResourceView(gt.tex, nullptr, &gt.srv);
  }
  if (SUCCEEDED(hr) && (td.BindFlags & D3D11_BIND_RENDER_TARGET)) {
    hr = g->dev->CreateRenderTargetView(gt.tex, nullptr, &gt.rtv);
  }
  if (SUCCEEDED(hr) && (td.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
    hr = g->dev->CreateDepthStencilView(gt.tex, nullptr, &gt.dsv);
  }
  if (FAILED(hr)) {
    gt.release();
    return false;
  }
  gt.format = format;
  gt.internal_format = t.internal_format;
  gt.width = t.width;
  gt.height = t.height;
  gt.tex_w = want_w;
  gt.tex_h = want_h;
  gt.expanded = expanded;
  gt.gpu_size = texel_bytes(format, want_w, want_h);
  gpu_tex_bytes += gt.gpu_size;
  // A new GPU copy holds nothing yet: whatever the CPU has is newer -- and
  // if the CPU has nothing (resident), neither holds anything.
  gt.gpu_newer = false;
  gt.cpu_newer = !gt.resident;
  return true;
}

// Writes rows [y0, y1) of the CPU buffer into the GPU copy.
static void upload_rows(GpuBackend* g, Texture& t, int x0, int y0, int x1,
                        int y1) {
  GpuTex& gt = *t.gpu;
  if (!t.buf || x1 <= x0 || y1 <= y0) return;
  D3D11_BOX box = {UINT(x0 + gt.ox), UINT(y0 + gt.oy), 0, UINT(x1 + gt.ox),
                   UINT(y1 + gt.oy), 1};
  const char* src = t.sample_ptr(x0, y0);
  int w = x1 - x0;
  int h = y1 - y0;
  if (t.internal_format == GL_DEPTH_COMPONENT24) {
    // Depth runs flattened: the flat form is a D24S8 texel exactly, depth in
    // the low 24 bits and a zero count where the stencil goes.
    std::vector<uint32_t> tmp(size_t(w) * h);
    for (int y = 0; y < h; y++) {
      DepthRun* row = (DepthRun*)t.sample_ptr(0, y0 + y);
      flatten_depth_runs(row, t.width);
      memcpy(&tmp[size_t(y) * w], row + x0, size_t(w) * 4);
    }
    g->dc->UpdateSubresource(gt.tex, 0, &box, tmp.data(), w * 4, 0);
  } else if (gt.expanded) {
    std::vector<uint8_t> tmp(size_t(w) * h * 4);
    for (int y = 0; y < h; y++) {
      copy_red_to_bgra8(&tmp[size_t(y) * w * 4],
                        (const uint8_t*)t.sample_ptr(x0, y0 + y), w);
    }
    g->dc->UpdateSubresource(gt.tex, 0, &box, tmp.data(), w * 4, 0);
  } else if (t.internal_format == GL_RGBA32I) {
    std::vector<float> tmp(size_t(w) * h * 4);
    for (int y = 0; y < h; y++) {
      const int32_t* s = (const int32_t*)t.sample_ptr(x0, y0 + y);
      for (int i = 0; i < w * 4; i++) tmp[size_t(y) * w * 4 + i] = float(s[i]);
    }
    g->dc->UpdateSubresource(gt.tex, 0, &box, tmp.data(), w * 16, 0);
  } else {
    g->dc->UpdateSubresource(gt.tex, 0, &box, src, UINT(t.stride()), 0);
  }
  g->stats.uploads++;
  if (trace()) {
    int nonzero = 0;
    if (t.internal_format == GL_RGBA8) {
      for (int y = y0; y < y1; y++) {
        const uint32_t* row = (const uint32_t*)t.sample_ptr(0, y);
        for (int x = x0; x < x1; x++) nonzero += row[x] != 0;
      }
    }
    fprintf(stderr,
            "gpu: upload %dx%d of %dx%d fmt %x, %d pixels not zero, "
            "delay_clear %d\n",
            w, h, t.width, t.height, t.internal_format, nonzero,
            t.delay_clear);
  }
}

static ID3D11Texture2D* get_staging(GpuBackend* g, GpuTex& gt) {
  uint64_t key = (uint64_t(gt.format) << 48) | (uint64_t(gt.tex_w) << 24) |
                 uint64_t(gt.tex_h);
  auto it = g->staging.find(key);
  if (it != g->staging.end()) return it->second;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = gt.tex_w;
  td.Height = gt.tex_h;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = gt.format;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* s = nullptr;
  if (FAILED(g->dev->CreateTexture2D(&td, nullptr, &s))) return nullptr;
  // A staging texture is as large as a whole render target and readbacks
  // are rare: keep two.
  if (g->staging.size() >= 2) {
    for (auto& e : g->staging) {
      gpu_cache_bytes -= texture_bytes(e.second);
      e.second->Release();
    }
    g->staging.clear();
  }
  gpu_cache_bytes += texture_bytes(s);
  g->staging[key] = s;
  return s;
}

// Copies the GPU copy back into the CPU buffer.
static void readback(GpuBackend* g, Texture& t) {
  debug_check(g, "readback");
  GpuTex& gt = *t.gpu;
  if (!t.buf || !gt.tex) return;
  if (t.internal_format == GL_DEPTH_COMPONENT24) {
    // Feature level 9 copies no depth buffer anywhere. The CPU keeps what
    // it had; a CPU draw testing against depth the GPU wrote will be wrong,
    // which is why everything drawn with depth has a GPU shader.
    g->stats.depth_lost++;
    if (trace()) fprintf(stderr, "gpu: depth cannot come back to the CPU\n");
    return;
  }
  ID3D11Texture2D* s = get_staging(g, gt);
  if (!s) {
    g->broken = true;
    return;
  }
  g->dc->CopyResource(s, gt.tex);
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(g->dc->Map(s, 0, D3D11_MAP_READ, 0, &m))) {
    g->broken = true;
    return;
  }
  for (int y = 0; y < t.height; y++) {
    const uint8_t* src = (const uint8_t*)m.pData +
                         size_t(y + gt.oy) * m.RowPitch +
                         size_t(gt.ox) * (gt.format == DXGI_FORMAT_R8_UNORM ? 1 : 4);
    uint8_t* dst = (uint8_t*)t.sample_ptr(0, y);
    if (t.internal_format == GL_DEPTH_COMPONENT24) {
      const uint32_t* s32 = (const uint32_t*)src;
      uint32_t* d32 = (uint32_t*)dst;
      for (int x = 0; x < t.width; x++) d32[x] = s32[x] & 0xFFFFFF;
    } else if (gt.expanded) {
      for (int x = 0; x < t.width; x++) dst[x] = src[x * 4 + 2];
    } else {
      memcpy(dst, src, size_t(t.width) * t.bpp());
    }
  }
  g->dc->Unmap(s, 0);
  if (t.internal_format == GL_DEPTH_COMPONENT24) {
    t.set_cleared(true);
  }
  g->stats.readbacks++;
  if (trace()) {
    fprintf(stderr, "gpu: readback %dx%d fmt %x\n", t.width, t.height,
            t.internal_format);
  }
}

// Before the GPU reads or draws into t: make the GPU copy current. A CPU
// buffer still waiting on a delayed clear is cleared on the GPU instead of
// being cleared and uploaded.
static bool to_gpu(GpuBackend* g, Texture& t) {
  if (!ensure_resource(g, t)) return false;
  GpuTex& gt = *t.gpu;
  if (gt.cpu_newer) {
    if (t.delay_clear && gt.rtv) {
      uint32_t c = t.clear_val;
      float color[4];
      if (t.internal_format == GL_R8) {
        color[0] = (c & 0xFF) / 255.0f;
        color[1] = color[2] = 0.0f;
        color[3] = 1.0f;
      } else {
        // clear_val is BGRA in memory order.
        color[0] = ((c >> 16) & 0xFF) / 255.0f;
        color[1] = ((c >> 8) & 0xFF) / 255.0f;
        color[2] = (c & 0xFF) / 255.0f;
        color[3] = ((c >> 24) & 0xFF) / 255.0f;
      }
      if (t.delay_clear >= t.height) {
        g->dc->ClearRenderTargetView(gt.rtv, color);
        t.disable_delayed_clear();
        g->stats.gpu_clears++;
      } else {
        // Partly cleared rows: settle them on the CPU, then upload.
        prepare_texture(t);
        upload_rows(g, t, 0, 0, t.width, t.height);
      }
    } else if (t.internal_format == GL_DEPTH_COMPONENT24) {
      // Direct3D takes no UpdateSubresource into a depth buffer, and reads
      // none back at 9_3. What SWGL holds is almost always one clear value,
      // which is a clear here too; anything else keeps the draw on the CPU.
      if (t.cleared() && t.buf) {
        uint32_t first = 0;
        bool uniform = true;
        for (int y = 0; y < t.height && uniform; y++) {
          DepthRun* row = (DepthRun*)t.sample_ptr(0, y);
          flatten_depth_runs(row, t.width);
          const uint32_t* v = (const uint32_t*)row;
          if (y == 0) first = v[0] & 0xFFFFFF;
          for (int x = 0; x < t.width; x++) {
            if ((v[x] & 0xFFFFFF) != first) {
              uniform = false;
              break;
            }
          }
        }
        if (!uniform) return false;
        g->dc->ClearDepthStencilView(gt.dsv, D3D11_CLEAR_DEPTH,
                                     float(first) / float(0xFFFFFF), 0);
        g->stats.gpu_clears++;
      }
    } else {
      upload_rows(g, t, 0, 0, t.width, t.height);
    }
    gt.cpu_newer = false;
  }
  return true;
}

static D3D11_BLEND gl_blend_factor(GLenum f, bool* ok) {
  switch (f) {
    case GL_ZERO:
      return D3D11_BLEND_ZERO;
    case GL_ONE:
      return D3D11_BLEND_ONE;
    case GL_SRC_COLOR:
      return D3D11_BLEND_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR:
      return D3D11_BLEND_INV_SRC_COLOR;
    case GL_SRC_ALPHA:
      return D3D11_BLEND_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA:
      return D3D11_BLEND_INV_SRC_ALPHA;
    case GL_DST_ALPHA:
      return D3D11_BLEND_DEST_ALPHA;
    case GL_ONE_MINUS_DST_ALPHA:
      return D3D11_BLEND_INV_DEST_ALPHA;
    case GL_DST_COLOR:
      return D3D11_BLEND_DEST_COLOR;
    case GL_ONE_MINUS_DST_COLOR:
      return D3D11_BLEND_INV_DEST_COLOR;
    case GL_CONSTANT_COLOR:
      return D3D11_BLEND_BLEND_FACTOR;
    case GL_ONE_MINUS_CONSTANT_COLOR:
      return D3D11_BLEND_INV_BLEND_FACTOR;
    default:
      *ok = false;
      return D3D11_BLEND_ONE;
  }
}

// Alpha factors may not name colours in Direct3D.
static D3D11_BLEND alpha_factor(D3D11_BLEND b) {
  switch (b) {
    case D3D11_BLEND_SRC_COLOR:
      return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR:
      return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR:
      return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR:
      return D3D11_BLEND_INV_DEST_ALPHA;
    default:
      return b;
  }
}

static ID3D11BlendState* blend_state(GpuBackend* g) {
  bool ok = true;
  uint64_t key = 0;
  D3D11_BLEND_DESC bd = {};
  D3D11_RENDER_TARGET_BLEND_DESC& rt = bd.RenderTarget[0];
  rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (ctx->blend) {
    GLenum eq = ctx->blend_equation;
    D3D11_BLEND_OP op;
    switch (eq) {
      case GL_FUNC_ADD:
        op = D3D11_BLEND_OP_ADD;
        break;
      case GL_MIN:
        op = D3D11_BLEND_OP_MIN;
        break;
      case GL_MAX:
        op = D3D11_BLEND_OP_MAX;
        break;
      default:
        // KHR advanced blends and SWGL's own: the CPU does those.
        return nullptr;
    }
    rt.BlendEnable = TRUE;
    rt.BlendOp = op;
    rt.BlendOpAlpha = op;
    if (op == D3D11_BLEND_OP_ADD) {
      rt.SrcBlend = gl_blend_factor(ctx->blendfunc_srgb, &ok);
      rt.DestBlend = gl_blend_factor(ctx->blendfunc_drgb, &ok);
      rt.SrcBlendAlpha = alpha_factor(gl_blend_factor(ctx->blendfunc_sa, &ok));
      rt.DestBlendAlpha = alpha_factor(gl_blend_factor(ctx->blendfunc_da, &ok));
    } else {
      rt.SrcBlend = rt.DestBlend = rt.SrcBlendAlpha = rt.DestBlendAlpha =
          D3D11_BLEND_ONE;
    }
    if (!ok) return nullptr;
    key = 1 | (uint64_t(rt.BlendOp) << 4) | (uint64_t(rt.SrcBlend) << 8) |
          (uint64_t(rt.DestBlend) << 16) | (uint64_t(rt.SrcBlendAlpha) << 24) |
          (uint64_t(rt.DestBlendAlpha) << 32);
  }
  auto it = g->blends.find(key);
  if (it != g->blends.end()) return it->second;
  ID3D11BlendState* s = nullptr;
  if (FAILED(g->dev->CreateBlendState(&bd, &s))) return nullptr;
  g->blends[key] = s;
  return s;
}

// A copy of a texture that is also the draw's target, for reading from.
static ID3D11ShaderResourceView* feedback_copy(GpuBackend* g, GpuTex& gt) {
  uint64_t key = (uint64_t(gt.format) << 48) | (uint64_t(gt.width) << 24) |
                 uint64_t(gt.height);
  auto it = g->feedback.find(key);
  if (it == g->feedback.end()) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = gt.width;
    td.Height = gt.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = gt.format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* t = nullptr;
    ID3D11ShaderResourceView* v = nullptr;
    if (FAILED(g->dev->CreateTexture2D(&td, nullptr, &t))) return nullptr;
    if (FAILED(g->dev->CreateShaderResourceView(t, nullptr, &v))) {
      t->Release();
      return nullptr;
    }
    // One or two sizes recur; do not hoard.
    if (g->feedback.size() >= 2) {
      for (auto& e : g->feedback) {
        gpu_cache_bytes -= texture_bytes(e.second.first);
        e.second.first->Release();
        e.second.second->Release();
      }
      g->feedback.clear();
    }
    gpu_cache_bytes += texture_bytes(t);
    it = g->feedback.emplace(key, std::make_pair(t, v)).first;
  }
  g->dc->CopyResource(it->second.first, gt.tex);
  if (trace()) {
    fprintf(stderr, "gpu: feedback copy %dx%d\n", gt.width, gt.height);
  }
  return it->second.second;
}

static ID3D11PixelShader* pixel_shader(GpuBackend* g,
                                       const GpuProgramInfo* info) {
  auto it = g->pixel_shaders.find(info);
  if (it != g->pixel_shaders.end()) return it->second;
  if (g->refused.count(info)) return nullptr;
  ID3D11PixelShader* ps = nullptr;
  if (FAILED(g->dev->CreatePixelShader(info->ps, info->ps_size, nullptr, &ps))) {
    g->refused[info] = true;
    return nullptr;
  }
  g->pixel_shaders[info] = ps;
  return ps;
}

static void set_target(GpuBackend* g, Texture& colortex, Texture* depthtex) {
  ID3D11RenderTargetView* rtv = colortex.gpu->rtv;
  ID3D11DepthStencilView* dsv = depthtex ? depthtex->gpu->dsv : nullptr;
  // Nothing may be bound for reading while it is being drawn into.
  ID3D11ShaderResourceView* none[16] = {};
  g->dc->PSSetShaderResources(0, 16, none);
  g->dc->OMSetRenderTargets(1, &rtv, dsv);
  // SWGL subtracts the texture's offset after the viewport transform.
  D3D11_VIEWPORT vp;
  vp.TopLeftX = float(ctx->viewport.x0 - colortex.offset.x + colortex.gpu->ox);
  vp.TopLeftY = float(ctx->viewport.y0 - colortex.offset.y + colortex.gpu->oy);
  vp.Width = float(ctx->viewport.width());
  vp.Height = float(ctx->viewport.height());
  vp.MinDepth = 0.0f;
  vp.MaxDepth = 1.0f;
  g->dc->RSSetViewports(1, &vp);
  IntRect sc = ctx->apply_scissor(colortex);
  D3D11_RECT r = {sc.x0 + colortex.gpu->ox, sc.y0 + colortex.gpu->oy,
                  sc.x1 + colortex.gpu->ox, sc.y1 + colortex.gpu->oy};
  g->dc->RSSetScissorRects(1, &r);
  g->dc->RSSetState(g->raster);
}

}  // namespace swgl_gpu

namespace swgl_gpu {

// Frees t's CPU buffer when the GPU copy is current (see GpuTex::resident).
static void drop_cpu_copy(GpuBackend* g, Texture& t) {
  if (!t.gpu || !t.gpu->tex || t.gpu->external || t.gpu->resident) return;
  if (!t.buf || !t.should_free() || t.locked || t.delay_clear) return;
  if (t.internal_format != GL_RGBA8 && t.internal_format != GL_R8) return;
  if (t.gpu->cpu_newer || &t == g->scratch) return;
  if (t.gpu->width != t.width || t.gpu->height != t.height) return;
  t.cleanup();
  mark_resident(*t.gpu, t);
  t.gpu->gpu_newer = true;
  g->stats.cpu_freed++;
  if (trace()) {
    fprintf(stderr, "gpu: CPU copy of %dx%d fmt %x freed\n", t.width,
            t.height, t.internal_format);
  }
}

}  // namespace swgl_gpu

// TexSubImage2D into a texture that lives only on the GPU: straight there,
// with no CPU buffer. False if it cannot, and the caller brings the CPU
// buffer back.
static bool gpu_upload_direct(Texture& t, int x, int y, int w, int h,
                              const void* data, int row_bytes) {
  using namespace swgl_gpu;
  GpuBackend* g = ctx ? ctx->gpu : nullptr;
  if (!g || g->broken || !t.gpu || !t.gpu->resident || t.buf || w <= 0 ||
      h <= 0) {
    return false;
  }
  if (!ensure_resource(g, t)) return false;
  GpuTex& gt = *t.gpu;
  gt.gpu_newer = true;
  D3D11_BOX box = {UINT(x), UINT(y), 0, UINT(x + w), UINT(y + h), 1};
  if (gt.expanded) {
    std::vector<uint8_t> tmp(size_t(w) * h * 4);
    for (int row = 0; row < h; row++) {
      copy_red_to_bgra8(&tmp[size_t(row) * w * 4],
                        (const uint8_t*)data + size_t(row) * row_bytes, w);
    }
    g->dc->UpdateSubresource(gt.tex, 0, &box, tmp.data(), w * 4, 0);
  } else {
    g->dc->UpdateSubresource(gt.tex, 0, &box, data, UINT(row_bytes), 0);
  }
  g->stats.uploads++;
  if (trace()) {
    fprintf(stderr, "gpu: upload %dx%d of %dx%d fmt %x straight to the GPU\n",
            w, h, t.width, t.height, t.internal_format);
  }
  return true;
}

// Called from prepare_texture: before the CPU reads t, bring back what only
// the GPU has.
static void gpu_sync_to_cpu(Texture& t) {
  if (!t.gpu || !ctx || !ctx->gpu || ctx->gpu_defer_sync) return;
  if (!t.buf && (t.gpu->external || t.gpu->resident)) {
    // The CPU wants a texture that so far lived only on the GPU.
    if (!t.allocate()) return;
    if (t.gpu->resident) {
      swgl_gpu::unmark_resident(*t.gpu);
      ctx->gpu->stats.cpu_restored++;
    }
  }
  if (!t.gpu->gpu_newer) return;
  swgl_gpu::readback(ctx->gpu, t);
  t.gpu->gpu_newer = false;
}

// The CPU has just written t.
static void gpu_cpu_wrote(Texture& t) {
  if (t.gpu) {
    t.gpu->cpu_newer = true;
    t.gpu->gpu_newer = false;
  }
}

// The CPU has just written a rectangle of t, which the GPU copy otherwise
// agrees with: send just that.
static void gpu_cpu_wrote_rect(Texture& t, int x0, int y0, int x1, int y1) {
  if (!t.gpu) return;
  if (ctx && ctx->gpu && t.gpu->tex && !t.gpu->cpu_newer && !t.gpu->gpu_newer &&
      t.gpu->width == t.width && t.gpu->height == t.height) {
    swgl_gpu::upload_rows(ctx->gpu, t, x0, y0, x1, y1);
  } else {
    gpu_cpu_wrote(t);
  }
}

// Storage was (re)specified: whatever the GPU had no longer applies.
static void gpu_storage_changed(Texture& t) {
  if (t.gpu && t.gpu->external) {
    // It held someone else's Direct3D texture; that is over now.
    t.gpu->release();
  }
  if (t.gpu) {
    t.gpu->gpu_newer = false;
    t.gpu->cpu_newer = true;
    swgl_gpu::unmark_resident(*t.gpu);
  }
}

// With a device attached, a colour texture SWGL allocates itself is born on
// the GPU (resident): no CPU buffer until the CPU needs one, and nothing to
// upload, its content being undefined until drawn, cleared or uploaded.
// Called by set_tex_storage in place of allocating; false if it cannot.
static bool gpu_storage_on_gpu(Texture& t) {
  GpuBackend* g = ctx ? ctx->gpu : nullptr;
  if (!g || g->broken || !t.should_free() || t.locked) return false;
  if (t.internal_format != GL_RGBA8 && t.internal_format != GL_R8) return false;
  if (t.width <= 0 || t.height <= 0 || &t == g->scratch) return false;
  if (t.buf) t.cleanup();
  if (!t.gpu) t.gpu = new GpuTex;
  swgl_gpu::unmark_resident(*t.gpu);
  swgl_gpu::mark_resident(*t.gpu, t);
  t.gpu->gpu_newer = false;
  t.gpu->cpu_newer = false;
  return true;
}

static void gpu_release_texture(Texture& t) {
  delete t.gpu;
  t.gpu = nullptr;
}

// The content is no longer wanted (glInvalidateFramebuffer): neither copy
// needs to be sent to the other.
static void gpu_invalidate(Texture& t) {
  if (t.gpu) {
    t.gpu->gpu_newer = false;
    t.gpu->cpu_newer = false;
  }
}

static int gpu_max_texture_size() {
  GpuBackend* g = ctx ? ctx->gpu : nullptr;
  if (!g) return 0;
  switch (g->level) {
    case D3D_FEATURE_LEVEL_9_1:
    case D3D_FEATURE_LEVEL_9_2:
      return 2048;
    case D3D_FEATURE_LEVEL_9_3:
      return 4096;
    case D3D_FEATURE_LEVEL_10_0:
    case D3D_FEATURE_LEVEL_10_1:
      return 8192;
    default:
      return 16384;
  }
}

static void gpu_count_cpu_draw() {
  if (ctx->gpu) {
    ctx->gpu->stats.cpu_draws++;
    if (swgl_gpu::trace()) {
      ProgramImpl* impl = ctx->programs[ctx->current_program].impl;
      fprintf(stderr, "gpu: CPU draw %s\n", impl ? impl->get_name() : "?");
    }
  }
}

Context::~Context() { delete gpu; }

// A clear of t, with the colour in SWGL's memory order (BGRA for RGBA8), or
// a depth value. Done on the GPU when the GPU copy is what counts; returns
// false to let the CPU do it.
static bool gpu_clear(Texture& t, const IntRect& rect, uint32_t color,
                      bool is_depth, float depth) {
  using namespace swgl_gpu;
  GpuBackend* g = ctx ? ctx->gpu : nullptr;
  if (!g || g->broken) return false;
  debug_check(g, "clear");
  if (is_depth) {
    // Depth lives on the GPU whenever there is one: Direct3D can neither
    // upload a depth buffer nor read one back, so a clear on the CPU would
    // strand the draws that follow there.
    // Sized for the render target it is cleared for: a compositor tile's
    // texture is larger than the SWGL texture that is its valid rectangle.
    int min_w = 0, min_h = 0;
    Framebuffer* fb = get_framebuffer(GL_DRAW_FRAMEBUFFER);
    if (fb && fb->depth_attachment &&
        &ctx->textures[fb->depth_attachment] == &t && fb->color_attachment) {
      Texture& ct = ctx->textures[fb->color_attachment];
      if (ct.gpu && ct.gpu->tex) {
        min_w = ct.gpu->tex_w;
        min_h = ct.gpu->tex_h;
      }
    }
    if (!ensure_resource(g, t, min_w, min_h) || !t.gpu->dsv) return false;
  } else {
    // Colour only when the GPU is where this texture lives now; otherwise
    // the CPU clear is free (usually delayed) and the upload happens anyway.
    if (t.gpu && t.gpu->resident && !t.buf && !ensure_resource(g, t)) {
      return false;
    }
    if (!t.gpu || !t.gpu->tex) return false;
    if (!t.gpu->gpu_newer && t.gpu->cpu_newer) return false;
    if (!t.gpu->external &&
        (t.gpu->width != t.width || t.gpu->height != t.height)) {
      return false;
    }
    if (!t.gpu->rtv) return false;
  }
  GpuTex& gt = *t.gpu;
  IntRect r = IntRect(rect).intersection(t.offset_bounds()) - t.offset;
  bool full = r.x0 <= 0 && r.y0 <= 0 && r.x1 >= t.width && r.y1 >= t.height;
  float c[4] = {0, 0, 0, 0};
  if (!is_depth) {
    if (t.internal_format == GL_R8) {
      c[0] = (color & 0xFF) / 255.0f;
      c[3] = 1.0f;
    } else {
      c[0] = ((color >> 16) & 0xFF) / 255.0f;
      c[1] = ((color >> 8) & 0xFF) / 255.0f;
      c[2] = (color & 0xFF) / 255.0f;
      c[3] = ((color >> 24) & 0xFF) / 255.0f;
    }
  }
  if (full) {
    if (is_depth) {
      g->dc->ClearDepthStencilView(gt.dsv, D3D11_CLEAR_DEPTH, depth, 0);
    } else {
      g->dc->ClearRenderTargetView(gt.rtv, c);
    }
  } else if (r.x1 > r.x0 && r.y1 > r.y0) {
    // A quad through the clear shader, scissored to the rectangle: colour
    // writes it, depth is written at the quad's own depth.
    ID3D11ShaderResourceView* none[16] = {};
    g->dc->PSSetShaderResources(0, 16, none);
    if (is_depth) {
      g->dc->OMSetRenderTargets(0, nullptr, gt.dsv);
    } else {
      g->dc->OMSetRenderTargets(1, &gt.rtv, nullptr);
    }
    D3D11_VIEWPORT vp = {0, 0, float(gt.tex_w), float(gt.tex_h), 0, 1};
    g->dc->RSSetViewports(1, &vp);
    D3D11_RECT sr = {r.x0 + gt.ox, r.y0 + gt.oy, r.x1 + gt.ox, r.y1 + gt.oy};
    g->dc->RSSetScissorRects(1, &sr);
    g->dc->RSSetState(g->raster);
    float z = is_depth ? depth * 2.0f - 1.0f : 0.0f;
    float quad[16] = {-1, -1, z, 1, 1, -1, z, 1, 1, 1, z, 1, -1, 1, z, 1};
    g->dc->UpdateSubresource(g->clear_vb, 0, nullptr, quad, 0, 0);
    g->dc->UpdateSubresource(g->clear_cb, 0, nullptr, c, 0, 0);
    UINT stride = 16, offset = 0;
    g->dc->IASetInputLayout(g->clear_il);
    g->dc->IASetVertexBuffers(0, 1, &g->clear_vb, &stride, &offset);
    g->dc->IASetIndexBuffer(g->ib, DXGI_FORMAT_R16_UINT, 0);
    g->dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g->dc->VSSetShader(g->clear_vs, nullptr, 0);
    g->dc->PSSetShader(g->clear_ps, nullptr, 0);
    g->dc->PSSetConstantBuffers(0, 1, &g->clear_cb);
    uint64_t key = is_depth ? 2 : 0;
    ID3D11BlendState* bs = nullptr;
    auto it = g->blends.find(key);
    if (it != g->blends.end()) {
      bs = it->second;
    } else {
      D3D11_BLEND_DESC bd = {};
      bd.RenderTarget[0].RenderTargetWriteMask =
          is_depth ? 0 : D3D11_COLOR_WRITE_ENABLE_ALL;
      if (SUCCEEDED(g->dev->CreateBlendState(&bd, &bs))) {
        g->blends[key] = bs;
      }
    }
    g->dc->OMSetBlendState(bs, nullptr, 0xFFFFFFFF);
    g->dc->OMSetDepthStencilState(is_depth ? g->depth_clear : g->depth_off, 0);
    g->dc->DrawIndexed(6, 0, 0);
  }
  // Not t.set_cleared(): that flag speaks for SWGL's CPU depth runs, which the
  // caller clears next -- marking them cleared first would have it fill runs
  // in a buffer that holds none yet.
  t.disable_delayed_clear();
  gt.gpu_newer = true;
  gt.cpu_newer = false;
  g->stats.gpu_clears++;
  if (trace()) {
    fprintf(stderr, "gpu: clear %s %dx%d %s %08x\n",
            is_depth ? "depth" : "color", t.width, t.height,
            full ? "full" : "part", color);
  }
  drop_cpu_copy(g, t);
  return true;
}

// Whether the current draw can run on the GPU: its program has a pixel
// shader the device takes, the blend mode and target format have Direct3D
// equivalents, and every texture it samples can be put on the GPU.
// Counts, and with SWGL_GPU_TRACE names, why a draw stays on the CPU.
static bool refuse(GpuBackend* g, bool program, const char* why) {
  if (program) {
    g->stats.refused_program++;
  } else {
    g->stats.refused_state++;
  }
  if (swgl_gpu::trace()) {
    ProgramImpl* impl = ctx->programs[ctx->current_program].impl;
    fprintf(stderr, "gpu: CPU for %s: %s\n", impl ? impl->get_name() : "?", why);
  }
  return false;
}

static bool gpu_check(Texture& colortex, Texture** sampled,
                      ID3D11BlendState** bs_out, ID3D11PixelShader** ps_out) {
  using namespace swgl_gpu;
  GpuBackend* g = ctx->gpu;
  if (!g || g->broken) return false;
  Program& prog = ctx->programs[ctx->current_program];
  ProgramImpl* impl = prog.gpu_impl;
  const GpuProgramInfo* info = impl ? impl->gpu_info() : nullptr;
  if (!info || !gpu_vertex_shader) {
    return refuse(g, true, "no GPU variant");
  }
  // SWGL_GPU_CPU=<substring>: keep matching programs on the CPU, for
  // telling a GPU shader's fault from the rest when testing.
  static const char* keep_on_cpu = getenv("SWGL_GPU_CPU");
  if (keep_on_cpu && strstr(info->name, keep_on_cpu)) {
    return refuse(g, true, "SWGL_GPU_CPU");
  }
  if (info->texture_floats > 0 && !g->flat_srv) {
    return refuse(g, true, "no float texture for flat data");
  }
  if (!renderable(colortex.internal_format)) {
    return refuse(g, false, "target format");
  }
  ID3D11BlendState* bs = blend_state(g);
  *bs_out = bs;
  if (!bs) {
    return refuse(g, false, "blend mode");
  }
  ID3D11PixelShader* ps = pixel_shader(g, info);
  *ps_out = ps;
  if (!ps) {
    return refuse(g, true, "pixel shader refused");
  }
  // The textures the pixel shader samples.
  for (int i = 0; i < info->num_samplers && i < 15; i++) {
    int unit = impl->gpu_sampler_unit(i);
    if (unit < 0 || unit >= int(MAX_TEXTURE_UNITS)) {
      return refuse(g, false, "sampler unit");
    }
    GLuint id = info->sampler_rect[i]
                    ? ctx->texture_units[unit].texture_rectangle_binding
                    : ctx->texture_units[unit].texture_2d_binding;
    Texture& st = ctx->textures[id];
    DXGI_FORMAT f;
    bool e;
    if (!id || (!st.buf && !st.gpu && st.width <= 0)) {
      // Nothing bound: WebRender leaves the clip mask unit empty when there is
      // no mask, and the shader does not look at it then.
      sampled[i] = nullptr;
      continue;
    }
    if ((!st.buf && !(st.gpu && st.gpu->resident)) ||
        !gl_format_on_gpu(g, st.internal_format, &f, &e) ||
        st.internal_format == GL_DEPTH_COMPONENT24 ||
        (st.gpu && st.gpu->external && !st.gpu->source)) {
      return refuse(g, false, "sampled texture");
    }
    sampled[i] = &st;
  }

  return true;
}

static bool gpu_can_draw(Texture& colortex) {
  Texture* sampled[16] = {};
  ID3D11BlendState* bs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  return gpu_check(colortex, sampled, &bs, &ps);
}

// A copy of a rectangle between two textures of one format, on the GPU,
// when the source's GPU copy is current: the backdrop copies and texture
// cache moves then need no readback. Rectangles are in texture space and of
// one size. Returns false to leave it to the CPU.
static bool gpu_copy(Texture& src, const IntRect& src_rect, Texture& dst,
                     const IntRect& dst_rect) {
  using namespace swgl_gpu;
  GpuBackend* g = ctx ? ctx->gpu : nullptr;
  debug_check(g, "copy");
  if (!g || g->broken || &src == &dst || !src.gpu || !src.gpu->tex) {
    return false;
  }
  GpuTex& sg = *src.gpu;
  if (sg.cpu_newer || sg.width != src.width || sg.height != src.height) {
    return false;
  }
  if (src.internal_format != dst.internal_format ||
      !renderable(src.internal_format)) {
    return false;
  }
  int dx = dst_rect.x0 - src_rect.x0;
  int dy = dst_rect.y0 - src_rect.y0;
  IntRect s = IntRect(src_rect).intersection(src.bounds());
  s = s.intersection(dst.bounds() - IntPoint(dx, dy));
  if (s.is_empty()) return true;
  if (!to_gpu(g, dst) || dst.gpu->expanded != sg.expanded) return false;
  // A compositor tile is the rectangle at (ox, oy) of its texture.
  D3D11_BOX box = {UINT(s.x0 + sg.ox), UINT(s.y0 + sg.oy), 0,
                   UINT(s.x1 + sg.ox), UINT(s.y1 + sg.oy), 1};
  g->dc->CopySubresourceRegion(dst.gpu->tex, 0, UINT(s.x0 + dx + dst.gpu->ox),
                               UINT(s.y0 + dy + dst.gpu->oy), 0, sg.tex, 0,
                               &box);
  dst.gpu->gpu_newer = true;
  dst.gpu->cpu_newer = false;
  if (trace()) {
    fprintf(stderr, "gpu: copy %dx%d\n", s.width(), s.height());
  }
  drop_cpu_copy(g, dst);
  return true;
}

static void gpu_trace_draw(Texture& colortex, int instances) {
  if (!ctx->gpu || !swgl_gpu::trace()) return;
  ProgramImpl* impl = ctx->programs[ctx->current_program].impl;
  fprintf(stderr, "gpu: > %s x%d into %dx%d depth %d blend %d\n",
          impl ? impl->get_name() : "?", instances, colortex.width,
          colortex.height, ctx->depthtest, ctx->blend);
  fflush(stderr);
}

static void gpu_record_quad(const vec4& pos, int nump) {
  GpuBackend* g = ctx ? ctx->gpu : nullptr;
  if (!g || !g->recording) return;
  for (int n = 0; n < 4; n++) {
    int l = nump < 4 && n == 3 ? 2 : n;
    g->recorded.push_back(pos.x[l]);
    g->recorded.push_back(pos.y[l]);
    g->recorded.push_back(pos.z[l]);
    g->recorded.push_back(pos.w[l]);
  }
}

// Whether the CPU draw about to happen into colortex should be shaded into a
// scratch texture and placed by the GPU; if so, the scratch texture to draw
// into, with the target's size, format and offset.
static Texture* gpu_cpu_composite_begin(Texture& colortex) {
  using namespace swgl_gpu;
  GpuBackend* g = ctx ? ctx->gpu : nullptr;
  if (!g || g->broken || !colortex.gpu || !colortex.gpu->tex) return nullptr;
  if (!colortex.gpu->gpu_newer && !colortex.gpu->external) return nullptr;
  if (!renderable(colortex.internal_format) || !blend_state(g)) return nullptr;
  if (!g->scratch) g->scratch = new Texture;
  Texture& s = *g->scratch;
  if (s.width != colortex.width || s.height != colortex.height ||
      s.internal_format != colortex.internal_format || !s.buf) {
    set_tex_storage(s, colortex.internal_format, colortex.width,
                    colortex.height);
    if (!s.buf) return nullptr;
    memset(s.buf, 0, s.stride() * s.height);
  }
  s.offset = colortex.offset;
  g->recorded.clear();
  g->recording = true;
  return &s;
}

static void gpu_cpu_composite_end(Texture& colortex, Texture& depthtex) {
  using namespace swgl_gpu;
  GpuBackend* g = ctx->gpu;
  g->recording = false;
  Texture& s = *g->scratch;
  size_t quads = g->recorded.size() / 16;
  if (!quads) return;
  // Where the quads land, in texture space, as SWGL places them.
  float vx = float(ctx->viewport.width()) * 0.5f;
  float vy = float(ctx->viewport.height()) * 0.5f;
  float ox = float(ctx->viewport.x0 - colortex.offset.x);
  float oy = float(ctx->viewport.y0 - colortex.offset.y);
  float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
  std::vector<float>& verts = g->verts;
  verts.resize(quads * 4 * 8);
  for (size_t i = 0; i < quads * 4; i++) {
    const float* p = &g->recorded[i * 4];
    float w = p[3] != 0.0f ? 1.0f / p[3] : 0.0f;
    float sx = (p[0] * w + 1) * vx + ox;
    float sy = (p[1] * w + 1) * vy + oy;
    bx0 = std::min(bx0, sx);
    by0 = std::min(by0, sy);
    bx1 = std::max(bx1, sx);
    by1 = std::max(by1, sy);
    float* o = &verts[i * 8];
    o[0] = p[0];
    o[1] = p[1];
    o[2] = p[2];
    o[3] = p[3];
    o[4] = sx / float(s.width) * p[3];
    o[5] = sy / float(s.height) * p[3];
    o[6] = 0.0f;
    o[7] = p[3];
  }
  IntRect bb = IntRect{int(floorf(bx0)) - 1, int(floorf(by0)) - 1,
                       int(ceilf(bx1)) + 1, int(ceilf(by1)) + 1}
                   .intersection(s.bounds());
  if (bb.is_empty()) return;
  if (!ensure_resource(g, s)) return;
  upload_rows(g, s, bb.x0, bb.y0, bb.x1, bb.y1);
  s.gpu->cpu_newer = false;
  // Leave the scratch clean for the next draw.
  for (int y = bb.y0; y < bb.y1; y++) {
    memset(s.sample_ptr(bb.x0, y), 0, size_t(bb.width()) * s.bpp());
  }

  if (!to_gpu(g, colortex)) return;
  bool use_depth = ctx->depthtest && depthtex.buf &&
                   depthtex.width == colortex.width &&
                   depthtex.height == colortex.height &&
                   ensure_resource(g, depthtex, colortex.gpu->tex_w,
                                   colortex.gpu->tex_h) &&
                   to_gpu(g, depthtex);
  set_target(g, colortex, use_depth ? &depthtex : nullptr);
  float blend_factor[4] = {ctx->blendcolor[2] / 255.0f,
                           ctx->blendcolor[1] / 255.0f,
                           ctx->blendcolor[0] / 255.0f,
                           ctx->blendcolor[3] / 255.0f};
  g->dc->OMSetBlendState(blend_state(g), blend_factor, 0xFFFFFFFF);
  g->dc->OMSetDepthStencilState(
      use_depth ? g->depth_states[ctx->depthfunc == GL_LEQUAL ? 1 : 0]
                                 [ctx->depthmask ? 1 : 0]
                : g->depth_off,
      0);
  g->dc->IASetInputLayout(g->il[1]);
  g->dc->IASetIndexBuffer(g->ib, DXGI_FORMAT_R16_UINT, 0);
  g->dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  g->dc->VSSetShader(g->vs[1], nullptr, 0);
  g->dc->PSSetShader(g->copy_ps, nullptr, 0);
  g->dc->PSSetShaderResources(0, 1, &s.gpu->srv);
  g->dc->PSSetSamplers(0, 1, &g->samp_point);
  const UINT vstride = 32;
  for (size_t first = 0; first < quads; first += MAX_QUADS_PER_FLUSH) {
    size_t n = std::min(quads - first, size_t(MAX_QUADS_PER_FLUSH));
    UINT bytes = UINT(n) * 4 * vstride;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    UINT pos = (g->vb_pos + vstride - 1) / vstride * vstride;
    if (pos + bytes > g->vb_size) {
      pos = 0;
      mode = D3D11_MAP_WRITE_DISCARD;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g->dc->Map(g->vb, 0, mode, 0, &m))) {
      g->broken = true;
      return;
    }
    memcpy((char*)m.pData + pos, &verts[first * 4 * 8], bytes);
    g->dc->Unmap(g->vb, 0);
    g->vb_pos = pos + bytes;
    UINT zero = 0;
    g->dc->IASetVertexBuffers(0, 1, &g->vb, &vstride, &zero);
    g->dc->DrawIndexed(UINT(n) * 6, 0, INT(pos / vstride));
  }
  ID3D11ShaderResourceView* none = nullptr;
  g->dc->PSSetShaderResources(0, 1, &none);
  colortex.gpu->gpu_newer = true;
  colortex.gpu->cpu_newer = false;
  if (use_depth && ctx->depthmask) {
    depthtex.gpu->gpu_newer = true;
    depthtex.gpu->cpu_newer = false;
  }
  if (trace()) {
    fprintf(stderr, "gpu: CPU-shaded %zu quads placed on the GPU (%dx%d)\n",
            quads, bb.width(), bb.height());
  }
}

// Draws the current program's instances on the GPU. Returns false, having
// changed nothing, when this draw has to run on the CPU instead.
template <typename INDEX>
static bool gpu_draw_elements(GLsizei count, GLsizei instancecount,
                              size_t offset, VertexArray& v, Texture& colortex,
                              Texture& depthtex) {
  using namespace swgl_gpu;
  GpuBackend* g = ctx->gpu;
  debug_check(g, "draw");
  Program& prog = ctx->programs[ctx->current_program];
  ProgramImpl* impl = prog.gpu_impl;
  const GpuProgramInfo* info = impl ? impl->gpu_info() : nullptr;
  VertexShaderImpl* vs = gpu_vertex_shader;
  Texture* sampled[16] = {};
  ID3D11BlendState* bs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  if (!gpu_check(colortex, sampled, &bs, &ps)) return false;

  Buffer& indices_buf = ctx->buffers[v.element_array_buffer_binding];
  if (!indices_buf.buf || offset >= indices_buf.size) return true;

  // Some pixel shaders unroll a loop whose trip count comes with the quad (a
  // blur's support): ask of every quad whether it fits before committing.
  if (info->has_accept) {
    size_t is = std::max(impl->interpolants_size(), sizeof(float));
    if (g->interps.size() < is * 4) g->interps.resize(is * 4);
    INDEX* ix = (INDEX*)(indices_buf.buf + offset);
    GLsizei n = min(count, (GLsizei)((indices_buf.size - offset) / sizeof(INDEX)));
    for (GLsizei instance = 0; instance < instancecount; instance++) {
      for (GLsizei i = 0; i + 3 <= n; i += 3) {
        bool quad = i + 6 <= n && ix[i + 5] == ix[i] + 3;
        vs->load_attribs(v.attribs, ix[i], instance, quad ? 4 : 3);
        vs->run_primitive(g->interps.data(), is);
        if (!impl->gpu_accept(info->loop_limit)) {
          return refuse(g, false, "a loop longer than the pixel shader's");
        }
        if (quad) i += 3;
      }
    }
  }

  // Everything checks out: from here on the draw is the GPU's.
  if (!to_gpu(g, colortex)) return refuse(g, false, "target on the GPU");
  bool use_depth = ctx->depthtest && depthtex.buf;
  if (use_depth) {
    // Direct3D wants the depth buffer exactly as large as the render
    // target's texture, which for a compositor tile is the whole tile.
    if (depthtex.width != colortex.width ||
        depthtex.height != colortex.height ||
        !ensure_resource(g, depthtex, colortex.gpu->tex_w,
                         colortex.gpu->tex_h) ||
        !to_gpu(g, depthtex)) {
      return refuse(g, false, "depth on the GPU");
    }
  }
  for (int i = 0; i < info->num_samplers; i++) {
    if (sampled[i] && !to_gpu(g, *sampled[i])) {
      return refuse(g, false, "sampled on the GPU");
    }
  }

  set_target(g, colortex, use_depth ? &depthtex : nullptr);
  const int slots = info->slots;
  const UINT vstride = UINT(16 * (1 + slots));
  g->dc->IASetInputLayout(g->il[slots]);
  g->dc->IASetIndexBuffer(g->ib, DXGI_FORMAT_R16_UINT, 0);
  g->dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  g->dc->VSSetShader(g->vs[slots], nullptr, 0);
  g->dc->PSSetShader(ps, nullptr, 0);
  float blend_factor[4] = {0, 0, 0, 0};
  {
    // blendcolor is kept as 16-bit BGRA-order values; D3D wants RGBA.
    blend_factor[0] = ctx->blendcolor[2] / 255.0f;
    blend_factor[1] = ctx->blendcolor[1] / 255.0f;
    blend_factor[2] = ctx->blendcolor[0] / 255.0f;
    blend_factor[3] = ctx->blendcolor[3] / 255.0f;
  }
  g->dc->OMSetBlendState(bs, blend_factor, 0xFFFFFFFF);
  g->dc->OMSetDepthStencilState(
      use_depth ? g->depth_states[ctx->depthfunc == GL_LEQUAL ? 1 : 0]
                                 [ctx->depthmask ? 1 : 0]
                : g->depth_off,
      0);

  // Textures, samplers and their sizes.
  float cbdata[20][4] = {};
  ID3D11ShaderResourceView* srvs[16] = {};
  ID3D11SamplerState* samps[16] = {};
  for (int i = 0; i < info->num_samplers; i++) {
    if (!sampled[i]) {
      srvs[i] = g->empty_srv;
      samps[i] = g->samp_point;
      cbdata[i][0] = cbdata[i][1] = cbdata[i][2] = cbdata[i][3] = 1.0f;
      continue;
    }
    Texture& st = *sampled[i];
    srvs[i] = st.gpu->srv;
    if (&st == &colortex) {
      // WebRender reads one part of a render target while drawing into
      // another (a backdrop copy, an atlas); SWGL allows it, Direct3D unbinds
      // the one while the other is bound. The read goes to a copy.
      srvs[i] = feedback_copy(g, *st.gpu);
      if (!srvs[i]) return false;
    }
    bool linear = st.mag_filter == GL_LINEAR &&
                  st.internal_format != GL_RGBA32F &&
                  st.internal_format != GL_RGBA32I;
    samps[i] = linear ? g->samp_linear : g->samp_point;
    cbdata[i][0] = float(st.width);
    cbdata[i][1] = float(st.height);
    cbdata[i][2] = 1.0f / float(st.width);
    cbdata[i][3] = 1.0f / float(st.height);
  }
  int nres = info->num_samplers;
  if (info->texture_floats > 0) {
    srvs[nres] = g->flat_srv;
    samps[nres] = g->samp_point;
    cbdata[nres][0] = float(FLAT_TEX_WIDTH);
    cbdata[nres][1] = float(FLAT_TEX_HEIGHT);
    cbdata[nres][2] = 1.0f / FLAT_TEX_WIDTH;
    cbdata[nres][3] = 1.0f / FLAT_TEX_HEIGHT;
    nres++;
  }
  g->dc->UpdateSubresource(g->cb, 0, nullptr, cbdata, 0, 0);
  g->dc->PSSetConstantBuffers(0, 1, &g->cb);
  if (nres) {
    g->dc->PSSetShaderResources(0, nres, srvs);
    g->dc->PSSetSamplers(0, nres, samps);
  }

  // The vertex stage, on the CPU as always.
  size_t istride = impl->interpolants_size();
  if (istride < sizeof(float)) istride = sizeof(float);
  if (g->interps.size() < istride * 4) g->interps.resize(istride * 4);
  const int texels = (info->texture_floats + 3) / 4;
  const int per_row = texels ? FLAT_TEX_WIDTH / texels : 0;
  const int max_quads =
      texels ? std::min(MAX_QUADS_PER_FLUSH, per_row * FLAT_TEX_HEIGHT)
             : MAX_QUADS_PER_FLUSH;
  g->verts.resize(size_t(max_quads) * 4 * (4 + slots * 4));
  if (texels) g->flats.resize(size_t(FLAT_TEX_WIDTH) * FLAT_TEX_HEIGHT * 4);
  float packed[4 * 32];
  float flat_vals[64];
  int quads = 0;
  int flat_rows_used = 0;

  auto flush = [&]() {
    if (!quads) return;
    UINT bytes = UINT(quads) * 4 * vstride;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    UINT pos = (g->vb_pos + vstride - 1) / vstride * vstride;
    if (pos + bytes > g->vb_size) {
      pos = 0;
      mode = D3D11_MAP_WRITE_DISCARD;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g->dc->Map(g->vb, 0, mode, 0, &m))) {
      g->broken = true;
      quads = 0;
      return;
    }
    memcpy((char*)m.pData + pos, g->verts.data(), bytes);
    g->dc->Unmap(g->vb, 0);
    g->vb_pos = pos + bytes;
    if (texels && flat_rows_used) {
      D3D11_BOX box = {0, 0, 0, UINT(FLAT_TEX_WIDTH), UINT(flat_rows_used), 1};
      g->dc->UpdateSubresource(g->flat_tex, 0, &box, g->flats.data(),
                               FLAT_TEX_WIDTH * 16, 0);
    }
    UINT zero = 0;
    g->dc->IASetVertexBuffers(0, 1, &g->vb, &vstride, &zero);
    g->dc->DrawIndexed(UINT(quads) * 6, 0, INT(pos / vstride));
    g->stats.gpu_quads += quads;
    quads = 0;
    flat_rows_used = 0;
  };

  auto emit = [&](int nump) {
    char* ib = g->interps.data();
    vs->run_primitive(ib, istride);
    vec4 pos = vs->gl_Position;
    if (nump < 4) {
      // A lone triangle: its fourth vertex repeats the third, which makes
      // the quad's second triangle empty.
      pos.x[3] = pos.x[2];
      pos.y[3] = pos.y[2];
      pos.z[3] = pos.z[2];
      pos.w[3] = pos.w[2];
      memcpy(ib + 3 * istride, ib + 2 * istride, istride);
    }
    impl->gpu_pack(ib, istride, packed, flat_vals);
    if (trace() && quads == 0) {
      for (int n = 0; n < 4; n++) {
        fprintf(stderr,
                "gpu:   v%d pos %.2f %.2f %.3f %.3f  t0 %.4f %.4f %.4f %.4f\n",
                n, pos.x[n], pos.y[n], pos.z[n], pos.w[n], packed[n * 32],
                packed[n * 32 + 1], packed[n * 32 + 2], packed[n * 32 + 3]);
      }
    }
    float* out = &g->verts[size_t(quads) * 4 * (4 + slots * 4)];
    float fu = 0, fv = 0;
    if (texels) {
      int q = quads;
      int row = q / per_row;
      int col = (q % per_row) * texels;
      memcpy(&g->flats[(size_t(row) * FLAT_TEX_WIDTH + col) * 4], flat_vals,
             size_t(info->texture_floats) * sizeof(float));
      fu = (col + 0.5f) / FLAT_TEX_WIDTH;
      fv = (row + 0.5f) / FLAT_TEX_HEIGHT;
      if (row + 1 > flat_rows_used) flat_rows_used = row + 1;
    }
    for (int n = 0; n < 4; n++) {
      float* o = out + n * (4 + slots * 4);
      o[0] = pos.x[n];
      o[1] = pos.y[n];
      o[2] = pos.z[n];
      o[3] = pos.w[n];
      float* p = &packed[n * 32];
      if (info->flat_coord_offset >= 0) {
        p[info->flat_coord_offset] = fu;
        p[info->flat_coord_offset + 1] = fv;
      }
      if (info->fragcoord_offset >= 0) {
        float w = pos.w[n] != 0.0f ? 1.0f / pos.w[n] : 0.0f;
        p[info->fragcoord_offset] =
            (pos.x[n] * w + 1) * 0.5f * ctx->viewport.width() +
            ctx->viewport.x0 - colortex.offset.x;
        p[info->fragcoord_offset + 1] =
            (pos.y[n] * w + 1) * 0.5f * ctx->viewport.height() +
            ctx->viewport.y0 - colortex.offset.y;
      }
      memcpy(o + 4, p, size_t(slots) * 16);
    }
    if (++quads >= max_quads) flush();
  };

  assert((offset & (sizeof(INDEX) - 1)) == 0);
  INDEX* indices = (INDEX*)(indices_buf.buf + offset);
  count = min(count, (GLsizei)((indices_buf.size - offset) / sizeof(INDEX)));
  if (count == 6 && indices[1] == indices[0] + 1 &&
      indices[2] == indices[0] + 2 && indices[5] == indices[0] + 3) {
    vs->load_attribs(v.attribs, indices[0], 0, 4);
    emit(4);
    for (GLsizei instance = 1; instance < instancecount; instance++) {
      vs->load_attribs(v.attribs, indices[0], instance, 0);
      emit(4);
    }
  } else {
    for (GLsizei instance = 0; instance < instancecount; instance++) {
      for (GLsizei i = 0; i + 3 <= count; i += 3) {
        if (indices[i + 1] != indices[i] + 1 ||
            indices[i + 2] != indices[i] + 2) {
          continue;
        }
        if (i + 6 <= count && indices[i + 5] == indices[i] + 3) {
          vs->load_attribs(v.attribs, indices[i], instance, 4);
          emit(4);
          i += 3;
        } else {
          vs->load_attribs(v.attribs, indices[i], instance, 3);
          emit(3);
        }
      }
    }
  }
  flush();

  colortex.gpu->gpu_newer = true;
  colortex.gpu->cpu_newer = false;
  if (use_depth && ctx->depthmask) {
    depthtex.gpu->gpu_newer = true;
    depthtex.gpu->cpu_newer = false;
  }
  g->stats.gpu_draws++;
  if (trace()) {
    fprintf(stderr,
            "gpu: draw %s x%d into %dx%d fmt %x%s blend %d/%x,%x depth %d%d\n",
            info->name, instancecount, colortex.width, colortex.height,
            colortex.internal_format, colortex.offset.x ? " offset" : "",
            ctx->blend, ctx->blendfunc_srgb, ctx->blendfunc_drgb, use_depth,
            ctx->depthmask);
    for (int i = 0; i < info->num_samplers; i++) {
      if (!sampled[i]) {
        fprintf(stderr, "gpu:   sampler %d: none\n", i);
        continue;
      }
      fprintf(stderr, "gpu:   sampler %d: %dx%d fmt %x filter %x\n", i,
              sampled[i]->width, sampled[i]->height,
              sampled[i]->internal_format, sampled[i]->mag_filter);
    }
  }
  // The GPU holds these now: their CPU copies are only memory.
  drop_cpu_copy(g, colortex);
  for (int i = 0; i < info->num_samplers; i++) {
    if (sampled[i]) drop_cpu_copy(g, *sampled[i]);
  }
  return true;
}

// Called by ResolveFramebuffer for a texture that lives in someone else's
// Direct3D texture: rather than bring its content to the CPU, send anything
// the CPU drew there to the GPU. Returns true if it handled the texture.
// Whether t is a texture held only on the GPU as someone else's.
static bool gpu_resolve_external_pending(Texture& t) {
  return t.gpu && (t.gpu->external || t.gpu->resident);
}

static bool gpu_resolve_external(Texture& t) {
  using namespace swgl_gpu;
  if (!t.gpu || !(t.gpu->external || t.gpu->resident)) return false;
  GpuBackend* g = ctx ? ctx->gpu : nullptr;
  if (g && !g->broken && t.gpu->external) {
    to_gpu(g, t);
  }
  return true;
}

extern "C" {

// Makes texture `texid` the rectangle at (x, y), `width` by `height`, of a
// Direct3D texture that belongs to the caller (an ID3D11Texture2D* created
// with render target and shader resource binding) -- a compositor tile. GPU
// draws go straight into it and it is never read back; SWGL allocates a CPU
// buffer for it only if a CPU draw needs one. The current content of that
// rectangle counts: nothing is uploaded over it.
bool SwglGpuSetTextureTarget(GLuint texid, void* d3d_texture, GLsizei width,
                             GLsizei height, GLint x, GLint y) {
  using namespace swgl_gpu;
  GpuBackend* g = ctx ? ctx->gpu : nullptr;
  if (!g || g->broken || !d3d_texture) return false;
  debug_check(g, "tile target");
  Texture& t = ctx->textures[texid];
  if (t.width != width || t.height != height ||
      t.internal_format != GL_RGBA8 || !t.should_free()) {
    t.cleanup();
    t.set_should_free(true);
    t.internal_format = GL_RGBA8;
    t.width = width;
    t.height = height;
  }
  t.offset = IntPoint(0, 0);
  t.disable_delayed_clear();
  if (!t.gpu) t.gpu = new GpuTex;
  GpuTex& gt = *t.gpu;
  ID3D11Texture2D* tex = (ID3D11Texture2D*)d3d_texture;
  if (gt.tex != tex) {
    gt.release();
    D3D11_TEXTURE2D_DESC td;
    tex->GetDesc(&td);
    tex->AddRef();
    gt.tex = tex;
    if (FAILED(g->dev->CreateRenderTargetView(tex, nullptr, &gt.rtv))) {
      gt.release();
      return false;
    }
    g->dev->CreateShaderResourceView(tex, nullptr, &gt.srv);
    gt.format = td.Format;
    gt.tex_w = td.Width;
    gt.tex_h = td.Height;
  }
  gt.external = true;
  gt.internal_format = GL_RGBA8;
  gt.width = width;
  gt.height = height;
  gt.ox = x;
  gt.oy = y;
  gt.expanded = false;
  gt.gpu_newer = true;
  gt.cpu_newer = false;
  return true;
}

// Makes texture `texid` a Direct3D texture on the attached device that
// belongs to the caller -- a decoded video frame -- `width` by `height` BGRA,
// which GPU draws sample where it is: no copy to memory and back. A CPU draw
// that needs it reads it back. False if it is not on this device or not of
// that size and format, and the caller hands over memory instead.
bool SwglGpuSetTextureSource(GLuint texid, void* d3d_texture, GLsizei width,
                             GLsizei height) {
  using namespace swgl_gpu;
  GpuBackend* g = ctx ? ctx->gpu : nullptr;
  if (!g || g->broken || !d3d_texture || width <= 0 || height <= 0) {
    return false;
  }
  ID3D11Texture2D* tex = (ID3D11Texture2D*)d3d_texture;
  D3D11_TEXTURE2D_DESC td;
  tex->GetDesc(&td);
  if (td.Width != UINT(width) || td.Height != UINT(height) ||
      td.ArraySize != 1 ||
      (td.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
       td.Format != DXGI_FORMAT_B8G8R8X8_UNORM) ||
      !(td.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
    return false;
  }
  ID3D11Device* dev = nullptr;
  tex->GetDevice(&dev);
  bool same = dev == g->dev;
  if (dev) dev->Release();
  if (!same) return false;
  debug_check(g, "texture source");
  Texture& t = ctx->textures[texid];
  if (t.width != width || t.height != height ||
      t.internal_format != GL_RGBA8 || !t.should_free()) {
    t.cleanup();
    t.set_should_free(true);
    t.internal_format = GL_RGBA8;
    t.width = width;
    t.height = height;
  } else if (t.buf) {
    t.cleanup();
  }
  t.offset = IntPoint(0, 0);
  t.disable_delayed_clear();
  if (!t.gpu) t.gpu = new GpuTex;
  GpuTex& gt = *t.gpu;
  if (gt.tex != tex || !gt.source) {
    gt.release();
    tex->AddRef();
    gt.tex = tex;
    if (FAILED(g->dev->CreateShaderResourceView(tex, nullptr, &gt.srv))) {
      gt.release();
      return false;
    }
    gt.format = td.Format;
    gt.tex_w = td.Width;
    gt.tex_h = td.Height;
  }
  gt.external = true;
  gt.source = true;
  gt.internal_format = GL_RGBA8;
  gt.width = width;
  gt.height = height;
  gt.ox = gt.oy = 0;
  gt.expanded = false;
  gt.gpu_newer = true;
  gt.cpu_newer = false;
  return true;
}

// Attaches a Direct3D 11 device to the current SWGL context; from then on
// draws that can run on it do. The device must belong to the thread that
// uses the context. Returns false, attaching nothing, if the device cannot
// take the shaders.
// One line about the attached device, for the log: its feature level and
// what it can do that decides which draws stay on the CPU.
void SwglGpuDescribe(char* out, size_t size) {
  GpuBackend* g = ctx ? ctx->gpu : nullptr;
  if (!g) {
    snprintf(out, size, "no device");
    return;
  }
  int programs = 0;
  for (auto* p : ctx->programs) {
    if (p && p->gpu_impl && p->gpu_impl->gpu_info()) programs++;
  }
  snprintf(out, size,
           "feature level %x, R8 targets %s, RG8 textures %s (NV12 video), "
           "float textures %s (flat data and gradients %s), max texture %d",
           unsigned(g->level), g->r8_ok ? "yes" : "no (BGRA instead)",
           g->rg8_ok ? "yes" : "no",
           g->rgba32f_ok ? "yes" : "NO", g->rgba32f_ok ? "on the GPU" : "on the CPU",
           gpu_max_texture_size());
  (void)programs;
}

bool SwglGpuAttach(void* device) {
  if (!ctx) return false;
  delete ctx->gpu;
  ctx->gpu = nullptr;
  if (!device) return true;
  GpuBackend* g = new GpuBackend;
  g->dev = (ID3D11Device*)device;
  g->dev->AddRef();
  g->dev->GetImmediateContext(&g->dc);
  g->dev->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)&g->iq);
  if (!swgl_gpu::init_backend(g)) {
    delete g;
    return false;
  }
  ctx->gpu = g;
  return true;
}

// Makes a device of its own on the default adapter, capped at the given
// feature level (0x9300 for 9_3), and attaches it: for testing on machines
// whose GPU is better than the one being imitated. `warp` asks for Microsoft's
// software rasteriser instead.
bool SwglGpuAttachNewDevice(int max_level, bool warp) {
  typedef HRESULT(WINAPI * CreateFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE,
                                     UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                     ID3D11Device**, D3D_FEATURE_LEVEL*,
                                     ID3D11DeviceContext**);
  HMODULE lib = LoadLibraryW(L"d3d11.dll");
  if (!lib) return false;
  CreateFn create = (CreateFn)GetProcAddress(lib, "D3D11CreateDevice");
  if (!create) return false;
  const D3D_FEATURE_LEVEL all[] = {
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,
      D3D_FEATURE_LEVEL_9_1};
  int first = 0;
  while (first < 6 && int(all[first]) > max_level) first++;
  ID3D11Device* dev = nullptr;
  const char* debug = getenv("SWGL_GPU_DEBUG");
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  if (debug && *debug && *debug != '0') flags |= D3D11_CREATE_DEVICE_DEBUG;
  HRESULT hr = create(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
                      nullptr, flags, all + first,
                      UINT(7 - first), D3D11_SDK_VERSION, &dev, nullptr,
                      nullptr);
  if (FAILED(hr) || !dev) return false;
  bool ok = SwglGpuAttach(dev);
  dev->Release();
  return ok;
}

// For tests of the compositor-tile path: a tile texture on the attached
// device, as a compositor would make one, and a way to read it back.
void* SwglGpuTestCreateTile(int width, int height) {
  GpuBackend* g = ctx ? ctx->gpu : nullptr;
  if (!g) return nullptr;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = width;
  td.Height = height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D* tex = nullptr;
  if (FAILED(g->dev->CreateTexture2D(&td, nullptr, &tex))) return nullptr;
  return tex;
}

void SwglGpuTestReleaseTile(void* tex) {
  if (tex) ((ID3D11Texture2D*)tex)->Release();
}

bool SwglGpuTestReadTile(void* tex, int x, int y, int width, int height,
                         void* out, int stride) {
  GpuBackend* g = ctx ? ctx->gpu : nullptr;
  if (!g || !tex || width <= 0 || height <= 0) return false;
  swgl_gpu::debug_check(g, "tile readback");
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = width;
  td.Height = height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* st = nullptr;
  if (FAILED(g->dev->CreateTexture2D(&td, nullptr, &st))) return false;
  D3D11_BOX box = {UINT(x), UINT(y), 0, UINT(x + width), UINT(y + height), 1};
  g->dc->CopySubresourceRegion(st, 0, 0, 0, 0, (ID3D11Texture2D*)tex, 0, &box);
  D3D11_MAPPED_SUBRESOURCE m;
  bool ok = SUCCEEDED(g->dc->Map(st, 0, D3D11_MAP_READ, 0, &m));
  if (ok) {
    for (int row = 0; row < height; row++) {
      memcpy((char*)out + size_t(row) * stride,
             (const char*)m.pData + size_t(row) * m.RowPitch, size_t(width) * 4);
    }
    g->dc->Unmap(st, 0);
  }
  st->Release();
  return ok;
}

// Counters since the last call, for the log: GPU draws, CPU draws, quads,
// uploads, readbacks, GPU clears, draws refused for the program, refused for
// state. Zeroes if no device is attached.
void SwglGpuStats(uint32_t* out8) {
  GpuBackend* g = ctx ? ctx->gpu : nullptr;
  swgl_gpu::debug_check(g, "stats");
  if (!g) {
    memset(out8, 0, 8 * sizeof(uint32_t));
    return;
  }
  out8[0] = g->stats.gpu_draws;
  out8[1] = g->stats.cpu_draws;
  out8[2] = g->stats.gpu_quads;
  out8[3] = g->stats.uploads;
  out8[4] = g->stats.readbacks;
  out8[5] = g->stats.gpu_clears;
  out8[6] = g->stats.refused_program;
  out8[7] = g->stats.refused_state;
  g->stats = GpuBackend::Stats();
}

// Since the last SwglGpuStats: CPU copies freed and brought back; and the
// megabytes of CPU memory the GPU-only textures save now.
// Then the megabytes of Direct3D textures: SWGL textures' GPU copies, and
// SWGL's caches.
void SwglGpuMemoryStats(uint32_t* out5) {
  GpuBackend* g = ctx ? ctx->gpu : nullptr;
  if (!g) {
    memset(out5, 0, 5 * sizeof(uint32_t));
    return;
  }
  out5[0] = g->stats.cpu_freed;
  out5[1] = g->stats.cpu_restored;
  out5[2] = uint32_t(std::max<int64_t>(swgl_gpu::resident_bytes, 0) >> 20);
  out5[3] = uint32_t(std::max<int64_t>(swgl_gpu::gpu_tex_bytes, 0) >> 20);
  out5[4] = uint32_t(std::max<int64_t>(swgl_gpu::gpu_cache_bytes, 0) >> 20);
}

}  // extern "C"

#else  // no GPU path on this platform

struct GpuTex {};
struct GpuBackend {};
static inline void gpu_sync_to_cpu(Texture&) {}
static inline void gpu_cpu_wrote(Texture&) {}
static inline void gpu_cpu_wrote_rect(Texture&, int, int, int, int) {}
static inline void gpu_storage_changed(Texture&) {}
static inline void gpu_release_texture(Texture&) {}
static inline bool gpu_clear(Texture&, const IntRect&, uint32_t, bool, float) {
  return false;
}
static inline void gpu_invalidate(Texture&) {}
static inline bool gpu_copy(Texture&, const IntRect&, Texture&, const IntRect&) {
  return false;
}
static inline bool gpu_resolve_external(Texture&) { return false; }
static inline void gpu_record_quad(const vec4&, int) {}
static inline void gpu_trace_draw(Texture&, int) {}
static inline Texture* gpu_cpu_composite_begin(Texture&) { return nullptr; }
static inline void gpu_cpu_composite_end(Texture&, Texture&) {}
static inline bool gpu_resolve_external_pending(Texture&) { return false; }
static inline bool gpu_upload_direct(Texture&, int, int, int, int,
                                     const void*, int) {
  return false;
}
static inline bool gpu_storage_on_gpu(Texture&) { return false; }
static inline void gpu_count_cpu_draw() {}
static inline int gpu_max_texture_size() { return 0; }
static inline bool gpu_can_draw(Texture&) { return false; }
template <typename INDEX>
static inline bool gpu_draw_elements(GLsizei, GLsizei, size_t, VertexArray&,
                                     Texture&, Texture&) {
  return false;
}
Context::~Context() { delete gpu; }

extern "C" {
bool SwglGpuAttach(void*) { return false; }
void SwglGpuDescribe(char* out, size_t size) {
  if (size) out[0] = 0;
}
bool SwglGpuSetTextureTarget(GLuint, void*, GLsizei, GLsizei, GLint, GLint) {
  return false;
}
bool SwglGpuSetTextureSource(GLuint, void*, GLsizei, GLsizei) {
  return false;
}
bool SwglGpuAttachNewDevice(int, bool) { return false; }
void* SwglGpuTestCreateTile(int, int) { return nullptr; }
void SwglGpuTestReleaseTile(void*) {}
bool SwglGpuTestReadTile(void*, int, int, int, int, void*, int) {
  return false;
}
void SwglGpuStats(uint32_t* out8) { memset(out8, 0, 8 * sizeof(uint32_t)); }
void SwglGpuMemoryStats(uint32_t* out5) {
  memset(out5, 0, 5 * sizeof(uint32_t));
}
}

#endif
