// Head cosmetics for the native renderer: the Tylenol bottle.
//
// A dense host-built 3D bottle (lathe body, wrap-around 4096x1440 label,
// ridged child-resistant cap) worn over the head of every skater wearing the
// marker tiara: locked to the head bone, upright, label facing behind the
// skater. (The spin/bob/contrail motion code is kept but disabled.) Drawn
// inside the scene pass with the scene's smoothed view_proj, depth-tested
// against the world; visual only. Wearers come from
// FrameScene::cosmetic_anchors (skate3_native_scene.cpp).

#include "skate3_native_scene.h"

#include "generated/skate3_init.h"

#include <array>
#include <atomic>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

#include "native/skate3_native_diag.h"
#include "native/skate3_native_entity.h"
#include "native/skate3_native_guest_read.h"
#include "native/skate3_native_lw.h"
#include "native/skate3_native_palette.h"
// Offline-compiled SPIR-V for the native shaders (compiled from the HLSL
// sources with DXC): the Vulkan RHI backend consumes these blobs; the D3D12
// backend runtime-compiles the embedded HLSL as before.
#include "native/shaders/spirv/skate3_native_shaders_spirv.h"

#if (defined(REX_HAS_D3D12) && REX_HAS_D3D12) || (defined(REX_HAS_VULKAN) && REX_HAS_VULKAN)
#include <rex/graphics/native_rhi.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#endif
#include "skate3_native_scene_gpu_internal.h"

#if (defined(REX_HAS_D3D12) && REX_HAS_D3D12) || (defined(REX_HAS_VULKAN) && REX_HAS_VULKAN)

#include "native/cosmetics/tylenol_label_png.h"
#include "skate3_native_shaders.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "../third_party/rexglue-sdk/thirdparty/tracy/profiler/src/stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace skate3::native_scene {
namespace {

// ---- Shape (meters, bottle-local; y up, origin at the bottle's middle) ----
constexpr double kBodyR = 0.165;
constexpr double kPivotY = 0.31;  // mid-height of the 0.625 m bottle
// Uniform scale of the bottle mesh: 1.5x the volume (cube root of 1.5).
constexpr double kScale = 1.1447;
// Bottle center above the head center: base (-kPivotY * kScale) lands at the
// chin, so the whole head is inside the bottle and only the neck shows below it.
constexpr float kHoverY = float(kPivotY * kScale - 0.10);
constexpr double kPi = 3.14159265358979323846;

// Motion.
constexpr double kSpinSeconds = 4.5;  // steady barber-pole pace
constexpr double kSpinSign = 1.0;
constexpr double kBobSeconds = 1.8;
constexpr double kBobMeters = 0.0;  // head-locked: no bob
constexpr double kWobbleDeg = 0.0;  // head-locked: no tilt
constexpr double kFollowRate = 0.0;  // 0 = locked to the head (no follow lag)
constexpr double kSnapDistance = 6.0; // teleports snap instead of gliding

// Contrail (off: a static bottle has no spin for it to trail).
constexpr bool kContrail = false;
constexpr double kTrailLife = 1.2;
constexpr size_t kTrailMaxSamples = 320;
constexpr size_t kMaxWearers = 8;
constexpr uint32_t kTrailRegions = 8;
constexpr uint32_t kTrailVertexBytes = 28;
constexpr uint32_t kTrailRegionBytes =
    uint32_t(kMaxWearers * kTrailMaxSamples) * 4 * kTrailVertexBytes;

struct TrailSample {
  double t;
  float top[3], bot[3], edge[3];
};

struct Wearer {
  float anchor[3] = {};
  float back[2] = {0.0f, -1.0f};  // world X/Z behind the skater's head
  bool seen = false;
  std::deque<TrailSample> trail;
};

struct CosmeticState {
  bool failed = false;
  nrhi::Pipeline* pso_bottle = nullptr;
  nrhi::Pipeline* pso_trail = nullptr;
  bool pso_hdr = false;
  uint32_t pso_msaa = 0;
  nrhi::Format pso_format = nrhi::Format::kUnknown;
  nrhi::Buffer* bottle_vb = nullptr;
  uint32_t bottle_vertices = 0;
  nrhi::Texture* label = nullptr;
  nrhi::TextureView* label_srv = nullptr;
  nrhi::Buffer* trail_vb = nullptr;
  uint8_t* trail_cpu = nullptr;
  // Motion (shared clock; one contrail per wearer).
  std::chrono::steady_clock::time_point start;
  std::chrono::steady_clock::time_point last;
  double sim_t = 0.0;
  std::vector<Wearer> wearers;
};
CosmeticState g_cos;

nrhi::Buffer* MakeUploadBuffer(nrhi::Device* device, size_t size, nrhi::BufferBindClass bind) {
  nrhi::BufferDesc desc;
  desc.size = size;
  desc.heap = nrhi::HeapKind::kUpload;
  desc.bind_class = bind;
  return device->CreateBuffer(desc);
}

// ---- Mesh -----------------------------------------------------------------
// Parametric surfaces sampled on a (ns x nt) grid, t wrapping around the
// bottle axis. Normals come from central differences on the grid and are
// oriented away from the bottle's center (the shape is convex), so every
// surface gets smooth, facet-free shading.
using Surface = void (*)(double s, double t, double p[3], double uv[2]);

void AddSurface(std::vector<float>& out, int ns, int nt, Surface f, float mat) {
  const int cols = nt + 1, rows = ns + 1;
  std::vector<double> P(size_t(rows) * cols * 3), UV(size_t(rows) * cols * 2), N(size_t(rows) * cols * 3);
  for (int i = 0; i < rows; ++i) {
    for (int j = 0; j < cols; ++j) {
      f(double(i) / ns, double(j) / nt, &P[(size_t(i) * cols + j) * 3], &UV[(size_t(i) * cols + j) * 2]);
    }
  }
  auto at = [&](int i, int j) -> const double* {
    i = std::clamp(i, 0, ns);
    j = ((j % nt) + nt) % nt;
    return &P[(size_t(i) * cols + j) * 3];
  };
  for (int i = 0; i < rows; ++i) {
    for (int j = 0; j < cols; ++j) {
      const double* s0 = at(i - 1, j), *s1 = at(i + 1, j);
      const double* t0 = at(i, j - 1), *t1 = at(i, j + 1);
      double ds[3] = {s1[0] - s0[0], s1[1] - s0[1], s1[2] - s0[2]};
      double dt[3] = {t1[0] - t0[0], t1[1] - t0[1], t1[2] - t0[2]};
      double n[3] = {ds[1] * dt[2] - ds[2] * dt[1], ds[2] * dt[0] - ds[0] * dt[2], ds[0] * dt[1] - ds[1] * dt[0]};
      double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      const double* p = &P[(size_t(i) * cols + j) * 3];
      double out_hint[3] = {p[0], p[1] - kPivotY, p[2]};
      if (len < 1e-12) {  // pole (disc center): straight along the axis
        n[0] = 0.0; n[1] = out_hint[1] < 0 ? -1.0 : 1.0; n[2] = 0.0; len = 1.0;
      }
      if (n[0] * out_hint[0] + n[1] * out_hint[1] + n[2] * out_hint[2] < 0) len = -len;
      double* dn = &N[(size_t(i) * cols + j) * 3];
      dn[0] = n[0] / len; dn[1] = n[1] / len; dn[2] = n[2] / len;
    }
  }
  auto emit = [&](int i, int j) {
    const size_t k = size_t(i) * cols + j;
    out.push_back(float(P[k * 3]));
    out.push_back(float(P[k * 3 + 1] - kPivotY));
    out.push_back(float(P[k * 3 + 2]));
    out.push_back(float(N[k * 3]));
    out.push_back(float(N[k * 3 + 1]));
    out.push_back(float(N[k * 3 + 2]));
    out.push_back(float(UV[k * 2]));
    out.push_back(float(UV[k * 2 + 1]));
    out.push_back(mat);
  };
  for (int i = 0; i < ns; ++i) {
    for (int j = 0; j < nt; ++j) {
      emit(i, j); emit(i + 1, j); emit(i, j + 1);
      emit(i, j + 1); emit(i + 1, j); emit(i + 1, j + 1);
    }
  }
}

void Around(double r, double y, double t, double p[3]) {
  const double a = t * 2.0 * kPi;
  p[0] = r * std::sin(a);
  p[1] = y;
  p[2] = r * std::cos(a);
}

// Bottle body profile, s: 0 = bottom center .. 1 = neck lip.
void BodyProfile(double s, double* r, double* y) {
  constexpr double kCorner = 0.028, kShoulderTop = 0.465, kNeckR = 0.106, kNeckTop = 0.49;
  constexpr double kSideTop = 0.40;
  // Segment arc lengths for an even-ish vertex distribution.
  const double lb = kBodyR - kCorner, lc = kCorner * kPi / 2.0, ls = kSideTop - kCorner;
  const double lsh = 0.105, ln = kNeckTop - kShoulderTop;
  const double total = lb + lc + ls + lsh + ln;
  double d = s * total;
  if (d <= lb) { *r = d; *y = 0.0; return; }
  d -= lb;
  if (d <= lc) {
    const double a = d / kCorner;
    *r = (kBodyR - kCorner) + std::sin(a) * kCorner;
    *y = kCorner - std::cos(a) * kCorner;
    return;
  }
  d -= lc;
  if (d <= ls) { *r = kBodyR; *y = kCorner + d; return; }
  d -= ls;
  if (d <= lsh) {
    const double u = d / lsh;  // quarter-ellipse shoulder into the neck
    *r = kNeckR + (kBodyR - kNeckR) * std::cos(u * kPi / 2.0);
    *y = kSideTop + (kShoulderTop - kSideTop) * std::sin(u * kPi / 2.0);
    return;
  }
  d -= lsh;
  *r = kNeckR;
  *y = kShoulderTop + std::min(d, ln);
}

void BodySurface(double s, double t, double p[3], double uv[2]) {
  double r, y;
  BodyProfile(s, &r, &y);
  Around(r, y, t, p);
  uv[0] = t; uv[1] = s;
}

// Label sleeve, just proud of the body, 0.365 m tall (matches the texture's
// 4096:1440 aspect on the 1.05 m circumference).
void LabelSurface(double s, double t, double p[3], double uv[2]) {
  Around(kBodyR + 0.0022, 0.395 - s * 0.365, t, p);
  uv[0] = t; uv[1] = s;
}

// Ridged cap side: 64 grip ridges.
constexpr double kCapR = 0.119, kCapBottom = 0.488, kCapTop = 0.622;
double CapRadius(double t) {
  const double c = std::cos(t * 2.0 * kPi * 64.0);
  return kCapR * (1.0 + 0.022 * c * c * c * c);
}
void CapSideSurface(double s, double t, double p[3], double uv[2]) {
  // s runs down from the rounded top edge to the bottom rim.
  constexpr double kRound = 0.009;
  double r = CapRadius(t), y;
  const double top_len = kRound * kPi / 2.0, side_len = (kCapTop - kRound) - kCapBottom;
  double d = s * (top_len + side_len);
  if (d < top_len) {
    const double a = d / kRound;
    r = (r - kRound) + std::sin(a) * kRound;
    y = (kCapTop - kRound) + std::cos(a) * kRound;
  } else {
    y = (kCapTop - kRound) - (d - top_len);
  }
  Around(r, y, t, p);
  uv[0] = t; uv[1] = s;
}
void CapTopSurface(double s, double t, double p[3], double uv[2]) {
  Around(s * (kCapR - 0.009), kCapTop, t, p);  // red disc
  uv[0] = t; uv[1] = s;
}
void CapRingSurface(double s, double t, double p[3], double uv[2]) {
  Around(0.058 + s * 0.016, kCapTop + 0.0012, t, p);  // white push-down ring
  uv[0] = t; uv[1] = s;
}

bool BuildBottle(const NativeGuestOutputRenderContext& context) {
  std::vector<float> v;
  v.reserve(9 * 1400000);
  AddSurface(v, 360, 512, BodySurface, 0.0f);
  AddSurface(v, 48, 1024, LabelSurface, 1.0f);
  AddSurface(v, 96, 1024, CapSideSurface, 2.0f);
  AddSurface(v, 48, 512, CapTopSurface, 3.0f);
  AddSurface(v, 8, 512, CapRingSurface, 4.0f);
  const size_t bytes = v.size() * sizeof(float);
  g_cos.bottle_vb = MakeUploadBuffer(context.device, bytes, nrhi::BufferBindClass::kVertexIndex);
  if (g_cos.bottle_vb == nullptr) return false;
  std::memcpy(context.device->Map(g_cos.bottle_vb), v.data(), bytes);
  context.device->Unmap(g_cos.bottle_vb);
  g_cos.bottle_vertices = uint32_t(v.size() / 9);
  REXLOG_INFO("[cosmetics] Tylenol bottle mesh: {} triangles ({} MB)", g_cos.bottle_vertices / 3,
              bytes / (1024 * 1024));
  return true;
}

bool UploadLabel(const NativeGuestOutputRenderContext& context) {
  int w = 0, h = 0, comp = 0;
  stbi_uc* rgba = stbi_load_from_memory(cosmetics::kTylenolLabelPng,
                                        int(cosmetics::kTylenolLabelPngSize), &w, &h, &comp, 4);
  if (rgba == nullptr) {
    REXLOG_WARN("[cosmetics] label decode failed");
    return false;
  }
  // Full box-filtered mip chain for crisp text at any distance/angle.
  std::vector<std::vector<uint8_t>> mips;
  mips.emplace_back(rgba, rgba + size_t(w) * h * 4);
  stbi_image_free(rgba);
  std::vector<std::pair<uint32_t, uint32_t>> dims{{uint32_t(w), uint32_t(h)}};
  while (dims.back().first > 1 || dims.back().second > 1) {
    const auto [pw, ph] = dims.back();
    const uint32_t nw = std::max(pw / 2, 1u), nh = std::max(ph / 2, 1u);
    std::vector<uint8_t> m(size_t(nw) * nh * 4);
    const std::vector<uint8_t>& src = mips.back();
    for (uint32_t y = 0; y < nh; ++y) {
      for (uint32_t x = 0; x < nw; ++x) {
        for (int c = 0; c < 4; ++c) {
          uint32_t sum = 0;
          for (uint32_t dy = 0; dy < 2; ++dy) {
            for (uint32_t dx = 0; dx < 2; ++dx) {
              const uint32_t sx = std::min(x * 2 + dx, pw - 1), sy = std::min(y * 2 + dy, ph - 1);
              sum += src[(size_t(sy) * pw + sx) * 4 + c];
            }
          }
          m[(size_t(y) * nw + x) * 4 + c] = uint8_t((sum + 2) / 4);
        }
      }
    }
    mips.push_back(std::move(m));
    dims.push_back({nw, nh});
  }
  const uint32_t mip_count = uint32_t(mips.size());
  constexpr uint64_t kPlace = 512;
  std::vector<uint64_t> offsets(mip_count);
  std::vector<uint32_t> pitches(mip_count);
  uint64_t size = 0;
  for (uint32_t m = 0; m < mip_count; ++m) {
    pitches[m] = (dims[m].first * 4u + (nrhi::kRowPitchAlignment - 1u)) & ~(nrhi::kRowPitchAlignment - 1u);
    offsets[m] = (size + (kPlace - 1)) & ~(kPlace - 1);
    size = offsets[m] + uint64_t(pitches[m]) * dims[m].second;
  }
  nrhi::Device* device = context.device;
  nrhi::TextureDesc desc;
  desc.kind = nrhi::TextureKind::k2D;
  desc.width = uint32_t(w);
  desc.height = uint32_t(h);
  desc.mip_levels = mip_count;
  desc.format = nrhi::Format::kR8G8B8A8_UNORM;
  desc.initial_state = nrhi::ResourceState::kCopyDest;
  g_cos.label = device->CreateTexture(desc);
  nrhi::Buffer* up = MakeUploadBuffer(device, size, nrhi::BufferBindClass::kCopySrc);
  if (g_cos.label == nullptr || up == nullptr) return false;
  uint8_t* map = static_cast<uint8_t*>(device->Map(up));
  for (uint32_t m = 0; m < mip_count; ++m) {
    for (uint32_t y = 0; y < dims[m].second; ++y) {
      std::memcpy(map + offsets[m] + uint64_t(y) * pitches[m], &mips[m][size_t(y) * dims[m].first * 4],
                  size_t(dims[m].first) * 4);
    }
  }
  device->Unmap(up);
  for (uint32_t m = 0; m < mip_count; ++m) {
    context.cmd->CopyBufferToTexture(g_cos.label, m, 0, up, offsets[m], pitches[m], dims[m].first,
                                     dims[m].second, 1);
  }
  context.cmd->Barrier(g_cos.label, nrhi::ResourceState::kCopyDest,
                       nrhi::ResourceState::kPixelShaderResource);
  device->DestroyDeferred(up);
  nrhi::TextureViewDesc vd;
  vd.mip_levels = mip_count;
  g_cos.label_srv = device->CreateTextureView(g_cos.label, vd);
  return g_cos.label_srv != nullptr;
}

bool EnsurePipelines(const NativeGuestOutputRenderContext& context) {
  const nrhi::Format fmt = g_r.hdr_active ? g_r.hdr_scene_format : context.guest_output->format();
  if (g_cos.pso_bottle != nullptr && g_cos.pso_hdr == g_r.hdr_active &&
      g_cos.pso_msaa == g_r.msaa && g_cos.pso_format == fmt) {
    return true;
  }
  nrhi::Device* device = context.device;
  for (nrhi::Pipeline** p : {&g_cos.pso_bottle, &g_cos.pso_trail}) {
    if (*p != nullptr) {
      device->DestroyDeferred(*p);
      *p = nullptr;
    }
  }
  nrhi::ShaderMacro defs[2] = {{"HDR", "1"}, {nullptr, nullptr}};
  const nrhi::ShaderMacro* macros = g_r.hdr_active ? defs : nullptr;
  const char* variant = g_r.hdr_active ? "HDR=1" : "";
  nrhi::Shader* vs_b = device->CreateShader(MakeShaderDesc(
      nrhi::ShaderStage::kVertex, "cosmetic.hlsl", kCosmeticShaderSource, "vs_bottle", nullptr, ""));
  nrhi::Shader* ps_b = device->CreateShader(MakeShaderDesc(
      nrhi::ShaderStage::kPixel, "cosmetic.hlsl", kCosmeticShaderSource, "ps_bottle", macros, variant));
  nrhi::Shader* vs_t = device->CreateShader(MakeShaderDesc(
      nrhi::ShaderStage::kVertex, "cosmetic.hlsl", kCosmeticShaderSource, "vs_trail", nullptr, ""));
  nrhi::Shader* ps_t = device->CreateShader(MakeShaderDesc(
      nrhi::ShaderStage::kPixel, "cosmetic.hlsl", kCosmeticShaderSource, "ps_trail", macros, variant));
  bool ok = vs_b && ps_b && vs_t && ps_t;
  if (ok) {
    static constexpr nrhi::InputElementDesc kBottleInput[4] = {
        {"POSITION", 0, 0, nrhi::Format::kR32G32B32_FLOAT, 0},
        {"NORMAL", 0, 1, nrhi::Format::kR32G32B32_FLOAT, 12},
        {"TEXCOORD", 0, 2, nrhi::Format::kR32G32_FLOAT, 24},
        {"TEXCOORD", 1, 3, nrhi::Format::kR32_FLOAT, 32}};
    nrhi::GraphicsPipelineDesc pd;
    pd.layout = g_r.layout;
    pd.vs = vs_b;
    pd.ps = ps_b;
    pd.input_elements = kBottleInput;
    pd.input_element_count = 4;
    pd.vertex_stride = 36;
    pd.cull = nrhi::CullMode::kNone;  // normals are authored outward; depth hides the far side
    pd.depth.test_enable = true;
    pd.depth.write_enable = true;
    pd.depth.func = nrhi::CompareFunc::kLessEqual;
    pd.rtv_format = fmt;
    pd.dsv_format = nrhi::Format::kD32_FLOAT;
    pd.sample_count = g_r.msaa;
    g_cos.pso_bottle = device->CreateGraphicsPipeline(pd);

    static constexpr nrhi::InputElementDesc kTrailInput[2] = {
        {"POSITION", 0, 0, nrhi::Format::kR32G32B32_FLOAT, 0},
        {"COLOR", 0, 1, nrhi::Format::kR32G32B32A32_FLOAT, 12}};
    nrhi::GraphicsPipelineDesc td;
    td.layout = g_r.layout;
    td.vs = vs_t;
    td.ps = ps_t;
    td.input_elements = kTrailInput;
    td.input_element_count = 2;
    td.vertex_stride = kTrailVertexBytes;
    td.cull = nrhi::CullMode::kNone;
    td.depth.test_enable = true;
    td.depth.write_enable = false;
    td.depth.func = nrhi::CompareFunc::kLessEqual;
    td.rtv_format = fmt;
    td.dsv_format = nrhi::Format::kD32_FLOAT;
    td.sample_count = g_r.msaa;
    td.blend.enable = true;
    td.blend.src = nrhi::BlendFactor::kSrcAlpha;
    td.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
    td.blend.src_alpha = nrhi::BlendFactor::kOne;
    td.blend.dst_alpha = nrhi::BlendFactor::kInvSrcAlpha;
    g_cos.pso_trail = device->CreateGraphicsPipeline(td);
    ok = g_cos.pso_bottle != nullptr && g_cos.pso_trail != nullptr;
  }
  for (nrhi::Shader* s : {vs_b, ps_b, vs_t, ps_t}) {
    if (s != nullptr) device->DestroyDeferred(s);
  }
  if (!ok) {
    REXLOG_WARN("[cosmetics] pipeline creation failed; bottle disabled");
    return false;
  }
  g_cos.pso_hdr = g_r.hdr_active;
  g_cos.pso_msaa = g_r.msaa;
  g_cos.pso_format = fmt;
  return true;
}

bool EnsureResources(const NativeGuestOutputRenderContext& context) {
  if (g_cos.failed) return false;
  if (g_cos.bottle_vb == nullptr && !BuildBottle(context)) {
    g_cos.failed = true;
  } else if (g_cos.label_srv == nullptr && !UploadLabel(context)) {
    g_cos.failed = true;
  } else if (g_cos.trail_vb == nullptr) {
    g_cos.trail_vb = MakeUploadBuffer(context.device, size_t(kTrailRegionBytes) * kTrailRegions,
                                      nrhi::BufferBindClass::kVertexIndex);
    g_cos.trail_cpu = g_cos.trail_vb ? static_cast<uint8_t*>(context.device->Map(g_cos.trail_vb)) : nullptr;
    if (g_cos.trail_cpu == nullptr) g_cos.failed = true;
  }
  if (!g_cos.failed && !EnsurePipelines(context)) g_cos.failed = true;
  return !g_cos.failed;
}

struct Mat34 {
  double m[3][4];
  void Apply(double x, double y, double z, float out[3]) const {
    for (int r = 0; r < 3; ++r) out[r] = float(m[r][0] * x + m[r][1] * y + m[r][2] * z + m[r][3]);
  }
};

// world = T * Rwobble * Ryaw. Static (no spin): the label's front panel
// (bottle-local -Z) turns to face `back`, away from the skater's face.
Mat34 BottleTransform(const float pos[3], const float back[2], double t) {
  // Ry * (0,0,-1) = (-sin, 0, -cos) == back
  // + pi: tuned in game (the label reads from behind the skater).
  const double spin = std::atan2(-double(back[0]), -double(back[1])) + kPi;
  const double wob = kWobbleDeg * kPi / 180.0;
  const double ax = std::sin(t * 1.3) * wob * 0.6, az = std::cos(t * 1.3) * wob;
  const double cx = std::cos(ax), sx = std::sin(ax), cz = std::cos(az), sz = std::sin(az);
  const double cy = std::cos(spin), sy = std::sin(spin);
  // Rx * Rz
  double rxz[3][3] = {{cz, -sz, 0.0}, {cx * sz, cx * cz, -sx}, {sx * sz, sx * cz, cx}};
  double ry[3][3] = {{cy, 0.0, sy}, {0.0, 1.0, 0.0}, {-sy, 0.0, cy}};
  Mat34 out{};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      out.m[r][c] =
          (rxz[r][0] * ry[0][c] + rxz[r][1] * ry[1][c] + rxz[r][2] * ry[2][c]) * kScale;
    }
    out.m[r][3] = pos[r];
  }
  return out;
}

void TrailColor(double age, float rgba[4], float gain) {
  auto smooth = [](double e0, double e1, double x) {
    const double k = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return k * k * (3.0 - 2.0 * k);
  };
  constexpr double red[3] = {0.95, 0.06, 0.10}, pink[3] = {1.0, 0.78, 0.84}, white[3] = {1.0, 1.0, 1.0};
  const double a = smooth(0.26, 0.40, age), b = smooth(0.60, 0.73, age);
  for (int c = 0; c < 3; ++c) {
    const double rp = red[c] + (pink[c] - red[c]) * a;
    rgba[c] = float(rp + (white[c] - rp) * b);
  }
  rgba[3] = float(std::pow(std::max(0.0, 1.0 - age), 0.9) * gain);
}

}  // namespace

void RenderHeadCosmetics(const NativeGuestOutputRenderContext& context, nrhi::Cmd* cmd,
                         const FrameScene& scene, uint64_t frame_number) {
  const auto now = std::chrono::steady_clock::now();
  if (scene.cosmetic_anchors.empty()) {
    g_cos.wearers.clear();
    return;
  }
  if (!EnsureResources(context)) return;

  if (g_cos.start.time_since_epoch().count() == 0) {
    g_cos.start = now;
    g_cos.last = now;
  }
  const double dt = std::clamp(std::chrono::duration<double>(now - g_cos.last).count(), 0.0, 0.1);
  g_cos.last = now;
  g_cos.sim_t += dt;
  const double t = g_cos.sim_t;

  // Match this frame's anchors to last frame's wearers (nearest within 1.5 m)
  // so each bottle keeps its own contrail; wearers not seen are dropped.
  for (Wearer& w : g_cos.wearers) w.seen = false;
  for (const auto& a : scene.cosmetic_anchors) {
    Wearer* best = nullptr;
    double best_d = 1.5 * 1.5;
    for (Wearer& w : g_cos.wearers) {
      if (w.seen) continue;
      const double d = (w.anchor[0] - a[0]) * (w.anchor[0] - a[0]) +
                       (w.anchor[1] - a[1]) * (w.anchor[1] - a[1]) +
                       (w.anchor[2] - a[2]) * (w.anchor[2] - a[2]);
      if (d < best_d) {
        best_d = d;
        best = &w;
      }
    }
    if (best == nullptr) {
      if (g_cos.wearers.size() >= kMaxWearers) continue;
      g_cos.wearers.emplace_back();
      best = &g_cos.wearers.back();
    }
    best->seen = true;
    std::memcpy(best->anchor, a.data(), sizeof(best->anchor));
    best->back[0] = a[3];
    best->back[1] = a[4];
  }
  g_cos.wearers.erase(std::remove_if(g_cos.wearers.begin(), g_cos.wearers.end(),
                                     [](const Wearer& w) { return !w.seen; }),
                      g_cos.wearers.end());

  const double kl = std::sqrt(0.35 * 0.35 + 0.85 * 0.85 + 0.40 * 0.40);
  const uint32_t region = uint32_t(frame_number % kTrailRegions) * kTrailRegionBytes;
  uint8_t* dst = g_cos.trail_cpu + region;
  uint32_t used = 0;
  struct Strip {
    uint32_t offset, count;
  };
  std::vector<Strip> strips;
  auto put = [&](const float p[3], const float c[4]) {
    std::memcpy(dst, p, 12);
    std::memcpy(dst + 12, c, 16);
    dst += kTrailVertexBytes;
  };

  for (Wearer& w : g_cos.wearers) {
    const float pos[3] = {w.anchor[0], w.anchor[1] + kHoverY, w.anchor[2]};
    const Mat34 xf = BottleTransform(pos, w.back, t);

    float consts[40];
    std::memcpy(consts, scene.view_proj, sizeof(float) * 16);
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 4; ++c) consts[16 + r * 4 + c] = float(xf.m[r][c]);
    }
    consts[28] = scene.cam_pos[0];
    consts[29] = scene.cam_pos[1];
    consts[30] = scene.cam_pos[2];
    consts[31] = float(std::fmod(t, 3600.0));
    consts[32] = float(0.35 / kl);
    consts[33] = float(0.85 / kl);
    consts[34] = float(0.40 / kl);
    consts[35] = 1.0f;
    consts[36] = 0.95f;  // rim glow: Tylenol red, slightly pink
    consts[37] = 0.20f;
    consts[38] = 0.30f;
    consts[39] = 0.55f;
    cmd->SetPipeline(g_cos.pso_bottle);
    cmd->SetRootConstants(0, 40, consts, 0);
    cmd->SetTexture(1, g_cos.label_srv);
    cmd->SetVertexBuffer(g_cos.bottle_vb, 0, g_cos.bottle_vertices * 36, 36);
    cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
    cmd->Draw(g_cos.bottle_vertices, 0);

    if (!kContrail) continue;
    if (dt > 0.0) {
      TrailSample s{};
      s.t = t;
      xf.Apply(0.12, 0.33, 0.0, s.edge);
      for (int k = 0; k < 3; ++k) s.edge[k] -= pos[k];
      w.trail.push_back(s);
    }
    while (!w.trail.empty() &&
           (w.trail.size() > kTrailMaxSamples || t - w.trail.front().t > kTrailLife)) {
      w.trail.pop_front();
    }
    const size_t n = w.trail.size();
    if (n < 2 || used + uint32_t(n) * 2 * kTrailVertexBytes > kTrailRegionBytes) continue;
    auto edge = [&](const TrailSample& s, int k) { return double(s.edge[k]) + pos[k]; };
    // Filament: a 4.4 cm band tracing the cap edge, widened across the view.
    strips.push_back({used, uint32_t(n) * 2});
    for (size_t i = 0; i < n; ++i) {
      const size_t k = n - 1 - i;
      const TrailSample& s = w.trail[k];
      const TrailSample& ahead = w.trail[std::min(k + 1, n - 1)];
      const TrailSample& behind = w.trail[k > 0 ? k - 1 : 0];
      const double tan[3] = {ahead.edge[0] - behind.edge[0], ahead.edge[1] - behind.edge[1],
                             ahead.edge[2] - behind.edge[2]};
      const double e[3] = {edge(s, 0), edge(s, 1), edge(s, 2)};
      const double view[3] = {scene.cam_pos[0] - e[0], scene.cam_pos[1] - e[1],
                              scene.cam_pos[2] - e[2]};
      double side[3] = {tan[1] * view[2] - tan[2] * view[1], tan[2] * view[0] - tan[0] * view[2],
                        tan[0] * view[1] - tan[1] * view[0]};
      double len = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
      if (len < 1e-9) {
        side[0] = 0.0;
        side[1] = 1.0;
        side[2] = 0.0;
        len = 1.0;
      }
      const double hw = 0.022 / len;
      const float a[3] = {float(e[0] + side[0] * hw), float(e[1] + side[1] * hw),
                          float(e[2] + side[2] * hw)};
      const float b[3] = {float(e[0] - side[0] * hw), float(e[1] - side[1] * hw),
                          float(e[2] - side[2] * hw)};
      float c[4];
      TrailColor((t - s.t) / kTrailLife, c, 1.0f);
      put(a, c);
      put(b, c);
    }
    used += uint32_t(n) * 2 * kTrailVertexBytes;
  }

  if (strips.empty()) return;
  cmd->SetPipeline(g_cos.pso_trail);
  cmd->SetRootConstants(0, 16, scene.view_proj, 0);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
  for (const Strip& s : strips) {
    cmd->SetVertexBuffer(g_cos.trail_vb, region + s.offset, s.count * kTrailVertexBytes,
                         kTrailVertexBytes);
    cmd->Draw(s.count, 0);
  }
}

}  // namespace skate3::native_scene

#endif  // REX_HAS_D3D12 || REX_HAS_VULKAN
