#include "skate3_native_debug_dialog.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

#include "skate3_native_scene.h"
#include "net/skate3_net_system.h"

REXCVAR_DEFINE_BOOL(skate3_native_render_mode_indicator, false, "Skate 3",
                    "Small top-right corner readout of which renderer produced the "
                    "last presented frame: NATIVE (native scene renderer) or "
                    "EMULATED (Xenos GPU emulation; menus/loading yields, F5 off). "
                    "Shows automatically while the native scene renderer is "
                    "switched off, regardless of this setting.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_net_hud, false, "Skate 3",
                    "Debug online overlay: the top-left player list with scores, "
                    "ping and current trick. Off by default (Accessibility "
                    "settings toggle it); the game-mode scoreboard is separate.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Hook-layer master (boot-time), shown read-only.
REXCVAR_DECLARE(bool, skate3_native_render);
// Hot-reload feature gates (skate3_native_scene.cpp).
REXCVAR_DECLARE(bool, skate3_native_render_scene);
REXCVAR_DECLARE(bool, skate3_native_render_scene_lightmaps);
REXCVAR_DECLARE(bool, skate3_native_render_scene_macro);
REXCVAR_DECLARE(bool, skate3_native_render_scene_decals);
REXCVAR_DECLARE(bool, skate3_native_render_scene_transparents);
REXCVAR_DECLARE(bool, skate3_native_render_scene_shadows);
REXCVAR_DECLARE(bool, skate3_native_render_scene_backface_cull);
REXCVAR_DECLARE(bool, skate3_native_render_scene_2d);
REXCVAR_DECLARE(int32_t, skate3_stance);
REXCVAR_DECLARE(bool, skate3_native_render_scene_splines);
REXCVAR_DECLARE(bool, skate3_native_render_scene_quadlists);
REXCVAR_DECLARE(bool, skate3_native_render_scene_world_items);
REXCVAR_DECLARE(bool, skate3_native_render_scene_dynamic_items);
REXCVAR_DECLARE(bool, skate3_native_render_scene_lw_fade);
REXCVAR_DECLARE(bool, skate3_native_render_scene_lw_identity);
REXCVAR_DECLARE(bool, skate3_native_render_scene_lw_gap_fill);
REXCVAR_DECLARE(bool, skate3_native_render_scene_lw_palette);
REXCVAR_DECLARE(bool, skate3_native_render_scene_retain_offscreen);
REXCVAR_DECLARE(bool, skate3_native_render_scene_tex_revalidate);
REXCVAR_DECLARE(bool, skate3_native_render_scene_mesh_revalidate);
REXCVAR_DECLARE(bool, skate3_native_render_scene_tex_mips);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_debug);
// Image quality (hot; skate3_native_scene.cpp).
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_msaa);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_shadow_tile);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_shadow_static_size);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ssao_full_res);
REXCVAR_DECLARE(bool, skate3_native_render_scene_hdr_packed);
// HDR post-effect stack (hot; skate3_native_scene.cpp).
REXCVAR_DECLARE(bool, skate3_native_render_scene_hdr);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_hdr_debug);
REXCVAR_DECLARE(bool, skate3_native_render_scene_bloom);
REXCVAR_DECLARE(double, skate3_native_render_scene_bloom_threshold);
REXCVAR_DECLARE(double, skate3_native_render_scene_bloom_knee);
REXCVAR_DECLARE(double, skate3_native_render_scene_bloom_intensity);
REXCVAR_DECLARE(bool, skate3_native_render_scene_shafts);
REXCVAR_DECLARE(double, skate3_native_render_scene_shafts_intensity);
REXCVAR_DECLARE(double, skate3_native_render_scene_shafts_reach);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_shafts_steps);
REXCVAR_DECLARE(bool, skate3_native_render_scene_haze);
REXCVAR_DECLARE(double, skate3_native_render_scene_haze_intensity);
REXCVAR_DECLARE(double, skate3_native_render_scene_haze_density);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ssao);
REXCVAR_DECLARE(double, skate3_native_render_scene_ssao_radius);
REXCVAR_DECLARE(double, skate3_native_render_scene_ssao_intensity);
REXCVAR_DECLARE(double, skate3_native_render_scene_ssao_luma_protect);
REXCVAR_DECLARE(bool, skate3_native_render_scene_shadow_static_casters);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_static_strength);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_static_radius);
REXCVAR_DECLARE(bool, skate3_native_render_scene_shadow_pcss);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_pcss_sun_deg);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_pcss_max_m);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_static_bias);
REXCVAR_DECLARE(bool, skate3_native_render_scene_sun_override);
REXCVAR_DECLARE(double, skate3_native_render_scene_sun_azimuth);
REXCVAR_DECLARE(double, skate3_native_render_scene_sun_elevation);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_refl_mode);
REXCVAR_DECLARE(double, skate3_native_render_scene_refl_lod);
REXCVAR_DECLARE(double, skate3_native_render_scene_refl_bias_x);
REXCVAR_DECLARE(double, skate3_native_render_scene_refl_bias_y);
REXCVAR_DECLARE(bool, skate3_native_render_scene_refl_bias_auto);
// Smoothness / pacing.
REXCVAR_DECLARE(bool, skate3_native_render_scene_smooth_camera);
REXCVAR_DECLARE(double, skate3_native_render_scene_smooth_camera_filter_ms);
REXCVAR_DECLARE(bool, skate3_native_render_scene_sort_opaque);
REXCVAR_DECLARE(double, skate3_guest_fps_cap);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_synthetic_pan);
REXCVAR_DECLARE(double, skate3_native_render_scene_synthetic_pan_rate);
REXCVAR_DECLARE(double, skate3_native_render_scene_synthetic_pan_amp);
// SDK: emulated-draw suppression while the native output is active.
REXCVAR_DECLARE(bool, native_render_suppress_emulated_draws);
REXCVAR_DECLARE(bool, skate3_native_render_scene_showcase);
REXCVAR_DECLARE(double, skate3_native_render_scene_showcase_hold);
REXCVAR_DECLARE(double, skate3_native_render_scene_showcase_wipe);
REXCVAR_DECLARE(std::string, skate3_native_render_scene_showcase_order);
REXCVAR_DECLARE(bool, skate3_native_render_scene_freecam);
REXCVAR_DECLARE(double, skate3_native_render_scene_freecam_speed);
REXCVAR_DECLARE(double, skate3_native_render_scene_freecam_look_speed);
REXCVAR_DECLARE(bool, skate3_native_render_scene_freecam_capture_input);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ssr);
// Draw distance (hot; skate3_draw_distance.cpp).
REXCVAR_DECLARE(double, skate3_draw_distance_scale);
REXCVAR_DECLARE(double, skate3_lod_distance_scale);
REXCVAR_DECLARE(double, skate3_draw_distance_stream_probe);

namespace skate3 {
namespace {

bool CvarCheckbox(const char* label, bool value, const char* help = nullptr) {
  bool v = value;
  ImGui::Checkbox(label, &v);
  if (help != nullptr && ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", help);
  }
  return v;
}

// Float-cvar slider (the cvars are hot-reload doubles; edits apply next
// frame). Returns the possibly-changed value for REXCVAR_SET.
double CvarSlider(const char* label, double value, float lo, float hi,
                  const char* fmt, const char* help = nullptr) {
  float v = float(value);
  if (ImGui::SliderFloat(label, &v, lo, hi, fmt)) {
    value = double(v);
  }
  if (help != nullptr && (ImGui::IsItemHovered() || ImGui::IsItemActive())) {
    ImGui::SetTooltip("%s", help);
  }
  return value;
}

// Combo over a fixed value list (for quality steps that are not a
// continuum: sample counts, map sizes). Returns the possibly-changed value;
// out-of-list current values snap to the nearest entry's label without
// writing back until the user picks one.
int32_t ValueCombo(const char* label, int32_t value, const int32_t* values,
                   const char* const* labels, int count,
                   const char* help = nullptr) {
  int index = 0;
  for (int i = 1; i < count; ++i) {
    const int32_t di = values[i] > value ? values[i] - value : value - values[i];
    const int32_t db = values[index] > value ? values[index] - value
                                             : value - values[index];
    if (di < db) {
      index = i;
    }
  }
  if (ImGui::Combo(label, &index, labels, count)) {
    value = values[index];
  }
  if (help != nullptr && ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", help);
  }
  return value;
}

// ---- Max quality (photo mode) ---------------------------------------------
// One switch that pushes every quality knob to its maximum for
// capture/recording (showcase runs, promo footage, screenshots) and
// restores the previous values when switched off. Session-scoped: nothing
// is persisted. Aesthetic calibrations (bloom/shaft/haze strengths, SSAO
// tuning) keep their values: this maximizes fidelity, not effect strength.
// SSR stays as-is (experimental, image-quality issues) and quad-list
// particles stay off (no sprite textures).

struct MaxQualityState {
  bool active = false;
  // Saved values, restored on switch-off.
  int32_t msaa;
  int32_t shadow_tile;
  int32_t static_size;
  int32_t shafts_steps;
  bool ssao_full_res;
  bool hdr_packed;
  bool hdr;
  bool bloom;
  bool shafts;
  bool ssao;
  bool shadows;
  bool static_casters;
  bool pcss;
  double draw_scale;
  double lod_scale;
  double stream_probe;
};

MaxQualityState s_max_quality;

void SetMaxQuality(bool enable) {
  MaxQualityState& s = s_max_quality;
  if (enable == s.active) {
    return;
  }
  if (enable) {
    s.msaa = REXCVAR_GET(skate3_native_render_scene_msaa);
    s.shadow_tile = REXCVAR_GET(skate3_native_render_scene_shadow_tile);
    s.static_size = REXCVAR_GET(skate3_native_render_scene_shadow_static_size);
    s.shafts_steps = REXCVAR_GET(skate3_native_render_scene_shafts_steps);
    s.ssao_full_res = REXCVAR_GET(skate3_native_render_scene_ssao_full_res);
    s.hdr_packed = REXCVAR_GET(skate3_native_render_scene_hdr_packed);
    s.hdr = REXCVAR_GET(skate3_native_render_scene_hdr);
    s.bloom = REXCVAR_GET(skate3_native_render_scene_bloom);
    s.shafts = REXCVAR_GET(skate3_native_render_scene_shafts);
    s.ssao = REXCVAR_GET(skate3_native_render_scene_ssao);
    s.shadows = REXCVAR_GET(skate3_native_render_scene_shadows);
    s.static_casters =
        REXCVAR_GET(skate3_native_render_scene_shadow_static_casters);
    s.pcss = REXCVAR_GET(skate3_native_render_scene_shadow_pcss);
    s.draw_scale = REXCVAR_GET(skate3_draw_distance_scale);
    s.lod_scale = REXCVAR_GET(skate3_lod_distance_scale);
    s.stream_probe = REXCVAR_GET(skate3_draw_distance_stream_probe);

    REXCVAR_SET(skate3_native_render_scene_msaa, 8);
    REXCVAR_SET(skate3_native_render_scene_shadow_tile, 4096);
    REXCVAR_SET(skate3_native_render_scene_shadow_static_size, 8192);
    REXCVAR_SET(skate3_native_render_scene_shafts_steps, 64);
    REXCVAR_SET(skate3_native_render_scene_ssao_full_res, true);
    REXCVAR_SET(skate3_native_render_scene_hdr_packed, false);  // RGBA16F
    REXCVAR_SET(skate3_native_render_scene_hdr, true);
    REXCVAR_SET(skate3_native_render_scene_bloom, true);
    REXCVAR_SET(skate3_native_render_scene_shafts, true);
    REXCVAR_SET(skate3_native_render_scene_ssao, true);
    REXCVAR_SET(skate3_native_render_scene_shadows, true);
    REXCVAR_SET(skate3_native_render_scene_shadow_static_casters, true);
    REXCVAR_SET(skate3_native_render_scene_shadow_pcss, true);
    // The largest steps the settings menu offers (tested territory).
    REXCVAR_SET(skate3_draw_distance_scale, 5.0);
    REXCVAR_SET(skate3_lod_distance_scale, 5.0);
    REXCVAR_SET(skate3_draw_distance_stream_probe, 300.0);
  } else {
    REXCVAR_SET(skate3_native_render_scene_msaa, s.msaa);
    REXCVAR_SET(skate3_native_render_scene_shadow_tile, s.shadow_tile);
    REXCVAR_SET(skate3_native_render_scene_shadow_static_size, s.static_size);
    REXCVAR_SET(skate3_native_render_scene_shafts_steps, s.shafts_steps);
    REXCVAR_SET(skate3_native_render_scene_ssao_full_res, s.ssao_full_res);
    REXCVAR_SET(skate3_native_render_scene_hdr_packed, s.hdr_packed);
    REXCVAR_SET(skate3_native_render_scene_hdr, s.hdr);
    REXCVAR_SET(skate3_native_render_scene_bloom, s.bloom);
    REXCVAR_SET(skate3_native_render_scene_shafts, s.shafts);
    REXCVAR_SET(skate3_native_render_scene_ssao, s.ssao);
    REXCVAR_SET(skate3_native_render_scene_shadows, s.shadows);
    REXCVAR_SET(skate3_native_render_scene_shadow_static_casters,
                s.static_casters);
    REXCVAR_SET(skate3_native_render_scene_shadow_pcss, s.pcss);
    REXCVAR_SET(skate3_draw_distance_scale, s.draw_scale);
    REXCVAR_SET(skate3_lod_distance_scale, s.lod_scale);
    REXCVAR_SET(skate3_draw_distance_stream_probe, s.stream_probe);
  }
  s.active = enable;
}

REXCVAR_DEFINE_INT32(
    skate3_native_render_scene_maxq_cycle, 0, "Skate 3",
    "DEBUG: auto-toggle MAX QUALITY every N native-scene frames (0 = off). "
    "Stress-tests the hot pipeline/target/shadow-map rebuild path; pair "
    "with Vulkan validation layers when hunting stale bindings.")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

void DrawMaxQualityToggle() {
  bool on = s_max_quality.active;
  if (ImGui::Checkbox("MAX QUALITY (photo mode)", &on)) {
    SetMaxQuality(on);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Push every quality setting to its maximum for recording: MSAA 8x,\n"
        "4096 shadow tiles, 8192 static sun map, full-res SSAO, RGBA16F\n"
        "HDR, max shaft steps, 5x draw/LOD distance, 300 m streaming, all\n"
        "quality features on. GPU-heavy; previous values restore when\n"
        "unchecked. Applies live (a one-frame pipeline rebuild).");
  }
  if (s_max_quality.active) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "ACTIVE");
  }
}

// ---- Showcase setup window ------------------------------------------------
// Editor for skate3_native_render_scene_showcase_order: one row per build-up
// layer, reorderable, each optionally joined to the previous row's wipe. The
// cvar string is the single source of truth ("tok,tok+tok,-tok": ',' starts
// a new wipe, '+' joins, '-' disables in place); the rows parse from it
// every frame and any edit writes it straight back, so the sequencer, this
// window and hand edits in config all stay consistent.

bool s_showcase_setup_open = false;

struct ShowcaseRow {
  const skate3::native_scene::ShowcaseLayer* layer;
  bool enabled;
  bool joined;
};

std::vector<ShowcaseRow> ParseShowcaseRows(const std::string& order) {
  using skate3::native_scene::kShowcaseLayers;
  std::vector<ShowcaseRow> rows;
  std::string tok;
  bool cur_joined = false;  // the separator before the accumulating token
  const auto flush = [&]() {
    while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) {
      tok.erase(tok.begin());
    }
    while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) {
      tok.pop_back();
    }
    bool enabled = true;
    if (!tok.empty() && tok[0] == '-') {
      enabled = false;
      tok.erase(tok.begin());
    }
    for (char& ch : tok) {
      if (ch >= 'A' && ch <= 'Z') {
        ch = char(ch + 32);
      }
    }
    for (const auto& layer : kShowcaseLayers) {
      if (tok != layer.token) {
        continue;
      }
      bool present = false;
      for (const auto& row : rows) {
        present = present || row.layer == &layer;
      }
      if (!present) {
        rows.push_back({&layer, enabled, !rows.empty() && cur_joined});
      }
      break;
    }
    tok.clear();
  };
  for (char ch : order) {
    if (ch == ',' || ch == '+') {
      flush();
      cur_joined = ch == '+';
    } else {
      tok += ch;
    }
  }
  flush();
  // Layers missing from the string append disabled, so they can always be
  // re-enabled from the window.
  for (const auto& layer : kShowcaseLayers) {
    bool present = false;
    for (const auto& row : rows) {
      present = present || row.layer == &layer;
    }
    if (!present) {
      rows.push_back({&layer, false, false});
    }
  }
  return rows;
}

std::string BuildShowcaseOrder(const std::vector<ShowcaseRow>& rows) {
  std::string out;
  for (size_t k = 0; k < rows.size(); ++k) {
    if (k > 0) {
      out += rows[k].joined ? '+' : ',';
    }
    if (!rows[k].enabled) {
      out += '-';
    }
    out += rows[k].layer->token;
  }
  return out;
}

// Mirrors the sequencer's availability rules (TickShowcase): the material
// looks need nothing; shadow/post layers need their feature (post gates
// live in the HDR tonemap).
bool ShowcaseLayerAvailable(uint32_t bit) {
  const bool hdr_on = REXCVAR_GET(skate3_native_render_scene_hdr);
  switch (bit) {
    case 8u:
      return REXCVAR_GET(skate3_native_render_scene_shadows);
    case 16u:
      return hdr_on && REXCVAR_GET(skate3_native_render_scene_ssao);
    case 32u:
      return hdr_on && REXCVAR_GET(skate3_native_render_scene_ssr);
    case 64u:
      return hdr_on && (REXCVAR_GET(skate3_native_render_scene_shafts) ||
                        REXCVAR_GET(skate3_native_render_scene_haze));
    case 128u:
      return hdr_on && REXCVAR_GET(skate3_native_render_scene_bloom);
    default:
      return true;
  }
}

// Drone-camera controls (same cvars everywhere the section is embedded;
// edits apply live).
void DrawFreecamControls() {
  ImGui::SeparatorText("Drone camera");
  bool freecam = REXCVAR_GET(skate3_native_render_scene_freecam);
  if (ImGui::Checkbox("Free-fly drone cam (End)", &freecam)) {
    REXCVAR_SET(skate3_native_render_scene_freecam, freecam);
  }
  ImGui::SameLine();
  bool capture = REXCVAR_GET(skate3_native_render_scene_freecam_capture_input);
  if (ImGui::Checkbox("capture game input", &capture)) {
    REXCVAR_SET(skate3_native_render_scene_freecam_capture_input, capture);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "While flying, keep keyboard/controller input away from the game so\n"
        "the fly keys don't also steer the skater.");
  }
  REXCVAR_SET(skate3_native_render_scene_freecam_speed,
              CvarSlider("fly speed (m/s)",
                         REXCVAR_GET(skate3_native_render_scene_freecam_speed),
                         0.5f, 60.0f, "%.1f",
                         "Base fly speed; Shift = 4x, Ctrl = 0.2x"));
  REXCVAR_SET(
      skate3_native_render_scene_freecam_look_speed,
      CvarSlider("look speed (deg/s)",
                 REXCVAR_GET(skate3_native_render_scene_freecam_look_speed),
                 20.0f, 240.0f, "%.0f", "Arrow-key look rate"));
  ImGui::TextDisabled(
      "WASD fly, E/Space up, Q/C down, arrows or right-mouse drag look,\n"
      "Z/X zoom, Shift fast, Ctrl slow.");
}

void DrawShowcaseSetupWindow(bool* p_open) {
  using skate3::native_scene::kShowcaseLayers;
  ImGui::SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Showcase Setup", p_open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }
  const bool running = REXCVAR_GET(skate3_native_render_scene_showcase);
  if (ImGui::Button(running ? "Cancel showcase" : "Start showcase (Ctrl+Shift+B)",
                    ImVec2(-1.0f, 0.0f))) {
    REXCVAR_SET(skate3_native_render_scene_showcase, !running);
  }
  REXCVAR_SET(skate3_native_render_scene_showcase_hold,
              CvarSlider("stage hold (s)",
                         REXCVAR_GET(skate3_native_render_scene_showcase_hold),
                         0.0f, 10.0f, "%.1f",
                         "Seconds each layer holds fullscreen between wipes"));
  REXCVAR_SET(skate3_native_render_scene_showcase_wipe,
              CvarSlider("wipe duration (s)",
                         REXCVAR_GET(skate3_native_render_scene_showcase_wipe),
                         0.5f, 10.0f, "%.1f",
                         "Seconds each split wipe takes to cross the screen"));

  ImGui::SeparatorText("Layer order");
  ImGui::TextDisabled(
      "Every run starts from clay geometry. \"join\" reveals a layer\n"
      "inside the previous layer's wipe instead of its own.");
  std::vector<ShowcaseRow> rows =
      ParseShowcaseRows(REXCVAR_GET(skate3_native_render_scene_showcase_order));
  bool changed = false;
  for (int k = 0; k < int(rows.size()); ++k) {
    ImGui::PushID(k);
    bool en = rows[k].enabled;
    if (ImGui::Checkbox("##on", &en)) {
      rows[k].enabled = en;
      changed = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(k == 0);
    if (ImGui::ArrowButton("up", ImGuiDir_Up) && k > 0) {
      std::swap(rows[k - 1], rows[k]);
      changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(k + 1 == int(rows.size()));
    if (ImGui::ArrowButton("down", ImGuiDir_Down) && k + 1 < int(rows.size())) {
      std::swap(rows[k], rows[k + 1]);
      changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(k == 0);
    bool joined = k > 0 && rows[k].joined;
    if (ImGui::Checkbox("join", &joined)) {
      rows[k].joined = joined;
      changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ShowcaseLayerAvailable(rows[k].layer->bit)) {
      ImGui::TextUnformatted(rows[k].layer->label);
    } else {
      ImGui::TextDisabled("%s (feature off)", rows[k].layer->label);
    }
    ImGui::PopID();
  }
  if (changed) {
    if (!rows.empty()) {
      rows[0].joined = false;
    }
    REXCVAR_SET(skate3_native_render_scene_showcase_order,
                BuildShowcaseOrder(rows));
  }
  if (ImGui::Button("Reset to default order")) {
    REXCVAR_SET(skate3_native_render_scene_showcase_order,
                std::string(skate3::native_scene::kShowcaseOrderDefault));
  }

  // Live preview of the resulting run, mirrors the sequencer's step
  // builder (cumulative layer masks; materials subsumes albedo/lighting;
  // a final full-render step covers whatever the list leaves out).
  ImGui::SeparatorText("Run preview");
  uint32_t avail_mask = 0;
  for (const auto& layer : kShowcaseLayers) {
    if (ShowcaseLayerAvailable(layer.bit)) {
      avail_mask |= layer.bit;
    }
  }
  int step_no = 1;
  ImGui::Text("%d. clay geometry", step_no++);
  uint32_t cum = 0;
  size_t k = 0;
  while (k < rows.size()) {
    uint32_t add = 0;
    std::string label;
    size_t j = k;
    do {
      const ShowcaseRow& row = rows[j];
      if (row.enabled && (avail_mask & row.layer->bit) != 0 &&
          (cum & row.layer->bit) == 0 && (add & row.layer->bit) == 0) {
        add |= row.layer->bit;
        if (!label.empty()) {
          label += " & ";
        }
        label += row.layer->label;
      }
      ++j;
    } while (j < rows.size() && rows[j].joined);
    if (add != 0) {
      cum |= add;
      ImGui::Text("%d. + %s", step_no++, label.c_str());
    }
    k = j;
  }
  const bool covered = (cum & 4u) != 0 && ((avail_mask & ~cum) & 0xF8u) == 0u;
  if (!covered) {
    ImGui::Text("%d. full render", step_no++);
  }

  DrawFreecamControls();
  ImGui::End();
}

// ---- F12 menu sections ----------------------------------------------------
// One CollapsingHeader per intent group. The groups the menu is actually
// opened for (capture tools, image quality) default open; tuning and
// diagnostics stay collapsed until needed.

void DrawShowcaseCaptureSection() {
  DrawMaxQualityToggle();
  {
    const bool running = REXCVAR_GET(skate3_native_render_scene_showcase);
    if (ImGui::Button(running ? "Cancel showcase" : "Start showcase (Ctrl+Shift+B)")) {
      REXCVAR_SET(skate3_native_render_scene_showcase, !running);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Strip the frame to clay geometry, then rebuild it layer by\n"
          "layer, each layer revealed by a live vertical split wiping\n"
          "across the screen. Layer order, grouping and pacing are edited\n"
          "in the setup window.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Setup...")) {
      s_showcase_setup_open = true;
    }
  }
  DrawFreecamControls();
}

void DrawImageQualitySection() {
  {
    static const int32_t kMsaaValues[] = {1, 2, 4, 8};
    static const char* const kMsaaLabels[] = {"Off (1x)", "2x", "4x", "8x"};
    REXCVAR_SET(skate3_native_render_scene_msaa,
                ValueCombo("MSAA", REXCVAR_GET(skate3_native_render_scene_msaa),
                           kMsaaValues, kMsaaLabels, 4,
                           "Scene multisampling. Distant thin geometry "
                           "(railings, wires) shimmers without it. Applies "
                           "live (pipeline rebuild)."));
  }
  {
    static const int32_t kTileValues[] = {0, 512, 1024, 2048, 4096};
    static const char* const kTileLabels[] = {"Auto (render scale)", "512",
                                              "1024", "2048", "4096"};
    REXCVAR_SET(
        skate3_native_render_scene_shadow_tile,
        ValueCombo("shadow tile size",
                   REXCVAR_GET(skate3_native_render_scene_shadow_tile),
                   kTileValues, kTileLabels, 5,
                   "Dynamic-shadow cascade tile resolution. Auto matches the "
                   "emulated renderer's crispness at the current Resolution "
                   "Scale; 512 = the softer original-console look."));
  }
  {
    static const int32_t kStaticValues[] = {1024, 2048, 4096, 8192};
    static const char* const kStaticLabels[] = {"1024", "2048", "4096",
                                                "8192"};
    REXCVAR_SET(
        skate3_native_render_scene_shadow_static_size,
        ValueCombo("static sun map size",
                   REXCVAR_GET(skate3_native_render_scene_shadow_static_size),
                   kStaticValues, kStaticLabels, 4,
                   "Static sun-shadow map resolution per cascade tile (the "
                   "map is three tiles)."));
  }
  REXCVAR_SET(skate3_native_render_scene_ssao_full_res,
              CvarCheckbox("Full-res SSAO",
                           REXCVAR_GET(skate3_native_render_scene_ssao_full_res),
                           "Evaluate SSAO at full output resolution instead "
                           "of half. ~4x the AO cost for slightly sharper "
                           "contact shadows."));
  REXCVAR_SET(skate3_native_render_scene_hdr_packed,
              CvarCheckbox("Packed HDR format (R11G11B10)",
                           REXCVAR_GET(skate3_native_render_scene_hdr_packed),
                           "Halves scene-pass color bandwidth vs RGBA16F at "
                           "slightly lower precision in very dark "
                           "gradients."));
}

void DrawLightingPostSection() {
  REXCVAR_SET(skate3_native_render_scene_hdr,
              CvarCheckbox("HDR intermediate",
                           REXCVAR_GET(skate3_native_render_scene_hdr),
                           "Float scene target + single host tonemap, the basis for "
                           "bloom, shafts and haze. Off = the classic in-material "
                           "tonemap (parity A/B)."));
  REXCVAR_SET(skate3_native_render_scene_bloom,
              CvarCheckbox("Bloom",
                           REXCVAR_GET(skate3_native_render_scene_bloom),
                           "Downsample/upsample pyramid driven by pre-tonemap "
                           "brightness (night lamps, neon, sun glare)."));
  REXCVAR_SET(skate3_native_render_scene_bloom_threshold,
              CvarSlider("bloom threshold",
                         REXCVAR_GET(skate3_native_render_scene_bloom_threshold),
                         0.0f, 1.5f, "%.2f",
                         "Pre-tonemap onset (1.0 = the tone curve's saturation "
                         "point). Below ~0.7 the sunlit day frame starts feeding "
                         "the pyramid and veils in glow."));
  REXCVAR_SET(skate3_native_render_scene_bloom_knee,
              CvarSlider("bloom knee",
                         REXCVAR_GET(skate3_native_render_scene_bloom_knee),
                         0.0f, 0.5f, "%.2f"));
  REXCVAR_SET(skate3_native_render_scene_bloom_intensity,
              CvarSlider("bloom intensity",
                         REXCVAR_GET(skate3_native_render_scene_bloom_intensity),
                         0.0f, 0.3f, "%.3f"));
  REXCVAR_SET(skate3_native_render_scene_shafts,
              CvarCheckbox("Volumetric sun shafts",
                           REXCVAR_GET(skate3_native_render_scene_shafts),
                           "Shadow-marched air: shadowed portions of each view ray "
                           "proportionally dim the light seen through them (dark "
                           "crepuscular shafts from buildings/trees/underpasses). "
                           "Fully lit air is untouched."));
  REXCVAR_SET(skate3_native_render_scene_shafts_intensity,
              CvarSlider("shaft intensity",
                         REXCVAR_GET(skate3_native_render_scene_shafts_intensity),
                         0.0f, 1.5f, "%.2f",
                         "How strongly fully shadowed air dims the scene behind it "
                         "(scaled by the forward-scatter phase toward the sun)."));
  REXCVAR_SET(skate3_native_render_scene_shafts_reach,
              CvarSlider("shaft reach (world units)",
                         REXCVAR_GET(skate3_native_render_scene_shafts_reach),
                         5.0f, 300.0f, "%.0f",
                         "How far in front of the camera the air is sampled. Longer "
                         "= more distant shadow volumes, coarser sampling."));
  {
    int steps = REXCVAR_GET(skate3_native_render_scene_shafts_steps);
    if (ImGui::SliderInt("shaft steps", &steps, 8, 64)) {
      REXCVAR_SET(skate3_native_render_scene_shafts_steps, steps);
    }
  }
  REXCVAR_SET(skate3_native_render_scene_haze,
              CvarCheckbox("Directional haze",
                           REXCVAR_GET(skate3_native_render_scene_haze),
                           "Fog-tinted sun scattering added with view distance, "
                           "strongest toward the sun. Off by default: the game's "
                           "authored sky/fog already carry the base atmosphere."));
  REXCVAR_SET(skate3_native_render_scene_haze_intensity,
              CvarSlider("haze intensity",
                         REXCVAR_GET(skate3_native_render_scene_haze_intensity),
                         0.0f, 0.5f, "%.3f"));
  REXCVAR_SET(skate3_native_render_scene_haze_density,
              CvarSlider("haze density (1/unit)",
                         REXCVAR_GET(skate3_native_render_scene_haze_density),
                         0.0f, 0.02f, "%.4f",
                         "How quickly the scattering saturates with distance "
                         "(0.005 reaches ~63% at 200 units)."));
  REXCVAR_SET(skate3_native_render_scene_ssao,
              CvarCheckbox("SSAO (GTAO)",
                           REXCVAR_GET(skate3_native_render_scene_ssao),
                           "Screen-space ambient occlusion: contact shading under "
                           "ledges, rails, vehicles, feet."));
  REXCVAR_SET(skate3_native_render_scene_ssao_radius,
              CvarSlider("SSAO radius (world units)",
                         REXCVAR_GET(skate3_native_render_scene_ssao_radius),
                         0.1f, 4.0f, "%.2f"));
  REXCVAR_SET(skate3_native_render_scene_ssao_intensity,
              CvarSlider("SSAO intensity",
                         REXCVAR_GET(skate3_native_render_scene_ssao_intensity),
                         0.0f, 3.0f, "%.2f",
                         "Exponent on the visibility term (1 = physical, >1 = "
                         "accentuated)."));
  REXCVAR_SET(skate3_native_render_scene_ssao_luma_protect,
              CvarSlider("SSAO sunlit protection",
                         REXCVAR_GET(skate3_native_render_scene_ssao_luma_protect),
                         0.0f, 3.0f, "%.2f",
                         "How strongly bright (sun-lit) surfaces resist SSAO "
                         "darkening (ambient-only approximation)."));
  {
    const bool was_on = REXCVAR_GET(skate3_native_render_scene_sun_override);
    const bool now_on =
        CvarCheckbox("Sun override (lighting lab)", was_on,
                     "Move the sun with the sliders below: dynamic CSM "
                     "shadows, the static world-shadow map, shadow receivers "
                     "and the volumetric shafts all follow. Baked lightmap "
                     "shade and the sky dome's painted sun stay put (game "
                     "content).");
    if (now_on && !was_on) {
      // Seed the sliders from the captured sun so enabling the override
      // starts at the true position instead of jumping.
      float sun[3];
      skate3::native_scene::GetCapturedSunDir(sun);
      const float kRad = 57.29577951f;
      REXCVAR_SET(skate3_native_render_scene_sun_azimuth,
                  double(std::fmod(std::atan2(sun[0], sun[2]) * kRad + 360.0f,
                                   360.0f)));
      REXCVAR_SET(
          skate3_native_render_scene_sun_elevation,
          double(std::clamp(std::asin(std::clamp(sun[1], -1.0f, 1.0f)) * kRad,
                            2.0f, 88.0f)));
    }
    REXCVAR_SET(skate3_native_render_scene_sun_override, now_on);
  }
  REXCVAR_SET(skate3_native_render_scene_sun_azimuth,
              CvarSlider("sun azimuth (deg)",
                         REXCVAR_GET(skate3_native_render_scene_sun_azimuth),
                         0.0f, 360.0f, "%.0f"));
  REXCVAR_SET(skate3_native_render_scene_sun_elevation,
              CvarSlider("sun elevation (deg)",
                         REXCVAR_GET(skate3_native_render_scene_sun_elevation),
                         2.0f, 88.0f, "%.0f",
                         "Low elevations give long shadows and the most visible "
                         "volumetric shafts."));
}

void DrawShadowsSection() {
  REXCVAR_SET(skate3_native_render_scene_shadows,
              CvarCheckbox("Dynamic shadows",
                           REXCVAR_GET(skate3_native_render_scene_shadows),
                           "Native CSM: skater/NPC/prop shadows onto the world"));
  REXCVAR_SET(skate3_native_render_scene_shadow_static_casters,
              CvarCheckbox(
                  "Static sun shadows",
                  REXCVAR_GET(skate3_native_render_scene_shadow_static_casters),
                  "Native static sun-shadow map: buildings, trees, rails and "
                  "placed props cast live shadows from a dedicated "
                  "sun-aligned depth map: shade on characters and props, "
                  "and shadows that follow a moved sun. The game's own "
                  "dynamic shadow cascades are untouched."));
  REXCVAR_SET(
      skate3_native_render_scene_shadow_static_strength,
      CvarSlider(
          "static shadow strength",
          REXCVAR_GET(skate3_native_render_scene_shadow_static_strength),
          0.0f, 1.0f, "%.2f",
          "How dark static-geometry shadows get (characters/props always "
          "cast at full strength). Lower if live static shade fights the "
          "baked lighting."));
  REXCVAR_SET(
      skate3_native_render_scene_shadow_static_radius,
      CvarSlider(
          "static shadow radius (m)",
          REXCVAR_GET(skate3_native_render_scene_shadow_static_radius), 40.0f,
          600.0f, "%.0f",
          "Far-cascade half-extent of the static sun-shadow map (mid and "
          "inner cascades cover 1/2 and 1/6 at 2x/6x density). Larger "
          "reaches farther at lower far-cascade density."));
  REXCVAR_SET(skate3_native_render_scene_shadow_pcss,
              CvarCheckbox(
                  "Soft shadows (PCSS)",
                  REXCVAR_GET(skate3_native_render_scene_shadow_pcss),
                  "Contact-hardening filter: crisp where a shadow touches "
                  "its caster, progressively softer with caster height."));
  REXCVAR_SET(skate3_native_render_scene_shadow_pcss_sun_deg,
              CvarSlider(
                  "sun angular size (deg)",
                  REXCVAR_GET(skate3_native_render_scene_shadow_pcss_sun_deg),
                  0.1f, 8.0f, "%.1f",
                  "Penumbra growth per meter of caster height (the real sun "
                  "is ~0.53 deg)."));
  REXCVAR_SET(skate3_native_render_scene_shadow_pcss_max_m,
              CvarSlider(
                  "max penumbra (m)",
                  REXCVAR_GET(skate3_native_render_scene_shadow_pcss_max_m),
                  0.05f, 5.0f, "%.2f"));
  REXCVAR_SET(
      skate3_native_render_scene_shadow_static_bias,
      CvarSlider("static receive bias (m)",
                 REXCVAR_GET(skate3_native_render_scene_shadow_static_bias),
                 0.0f, 0.5f, "%.3f",
                 "Raise if static geometry shows self-shadow acne (stipple "
                 "on sunlit ground/walls); lower if static shadows visibly "
                 "detach from their casters."));
}

void DrawWorldShadingSection() {
  REXCVAR_SET(skate3_native_render_scene_lightmaps,
              CvarCheckbox("Lightmaps", REXCVAR_GET(skate3_native_render_scene_lightmaps),
                           "Baked lighting atlas sample x2 on world materials"));
  REXCVAR_SET(skate3_native_render_scene_macro,
              CvarCheckbox("Macro overlay", REXCVAR_GET(skate3_native_render_scene_macro),
                           "Large-scale grime/crack multiply (ground/wall weathering)"));
  REXCVAR_SET(skate3_native_render_scene_decals,
              CvarCheckbox("Decal art composite",
                           REXCVAR_GET(skate3_native_render_scene_decals),
                           "Graffiti/paint art lerped over environment.decal sections"));
  REXCVAR_SET(skate3_native_render_scene_transparents,
              CvarCheckbox("Transparent sub-pass",
                           REXCVAR_GET(skate3_native_render_scene_transparents),
                           "environment.transparent items (mist sheets, glass, fences)"));
  REXCVAR_SET(skate3_native_render_scene_backface_cull,
              CvarCheckbox("Backface cull (game parity)",
                           REXCVAR_GET(skate3_native_render_scene_backface_cull),
                           "World env materials cull FRONT like the game's material "
                           "XMLs; off = legacy cull-none (shows interior faces)"));
}

void DrawSceneContentSection() {
  REXCVAR_SET(skate3_native_render_scene_world_items,
              CvarCheckbox("World items", REXCVAR_GET(skate3_native_render_scene_world_items),
                           "Static geometry from the world sort lists"));
  REXCVAR_SET(skate3_native_render_scene_dynamic_items,
              CvarCheckbox("Dynamic items",
                           REXCVAR_GET(skate3_native_render_scene_dynamic_items),
                           "Characters, movable props, cloth (RenderMesh/world-path "
                           "captures)"));
  REXCVAR_SET(
      skate3_native_render_scene_lw_fade,
      CvarCheckbox("LW entity fade (store)",
                   REXCVAR_GET(skate3_native_render_scene_lw_fade),
                   "Serve NPC/traffic fade alpha from the LivingWorld entity "
                   "itself (per-instance store) instead of the per-draw "
                   "captured constant row; fixes opaque mid-air spawns, "
                   "missing fade-ins and clone alpha blinks"));
  REXCVAR_SET(
      skate3_native_render_scene_lw_identity,
      CvarCheckbox("LW entity identity (pose rings)",
                   REXCVAR_GET(skate3_native_render_scene_lw_identity),
                   "Key NPC/traffic pose-smoothing rings by the game's own "
                   "per-instance MeshContext instead of (mesh, occurrence) "
                   "pairing; clone reshuffles can no longer mispair "
                   "(teleport/slide class)"));
  REXCVAR_SET(
      skate3_native_render_scene_lw_gap_fill,
      CvarCheckbox("LW gap fill (1-2 frame republish)",
                   REXCVAR_GET(skate3_native_render_scene_lw_gap_fill),
                   "Republish a live NPC whose MeshContext skipped this "
                   "frame's submit records (the 1-3 frame publish GAPs that "
                   "read as blinks/small teleports)"));
  REXCVAR_SET(
      skate3_native_render_scene_lw_palette,
      CvarCheckbox("LW authoritative caster palettes",
                   REXCVAR_GET(skate3_native_render_scene_lw_palette),
                   "Replace GUESSED ortho caster-bank palettes on "
                   "edge-of-view vehicles with the entity's own packed "
                   "palette from the pack writer (the mangle/transform "
                   "class)"));
  REXCVAR_SET(skate3_native_render_scene_quadlists,
              CvarCheckbox("Quad-list particles",
                           REXCVAR_GET(skate3_native_render_scene_quadlists),
                           "Non-indexed quad-list captures (particle systems; off by "
                           "default: render as white squares without sprite textures)"));
  REXCVAR_SET(
      skate3_native_render_scene_retain_offscreen,
      CvarCheckbox("Retain off-screen statics",
                   REXCVAR_GET(skate3_native_render_scene_retain_offscreen),
                   "Keep recently seen statics drawn while the game view-culls "
                   "them: the smoothed render camera trails the guest pose, so "
                   "without this world geometry visibly tears down right at the "
                   "screen edges during pans/traversal"));
  REXCVAR_SET(skate3_native_render_scene_2d,
              CvarCheckbox("2D / HUD replay", REXCVAR_GET(skate3_native_render_scene_2d),
                           "APT/Flash HUD + glyph text + SimpleDraw icons"));
  REXCVAR_SET(skate3_native_render_scene_splines,
              CvarCheckbox("Neon splines", REXCVAR_GET(skate3_native_render_scene_splines),
                           "Waypoint arrows / marker beams"));
}

void DrawPacingSection() {
  REXCVAR_SET(skate3_native_render_scene_smooth_camera,
              CvarCheckbox("Smooth camera + entity poses",
                           REXCVAR_GET(skate3_native_render_scene_smooth_camera),
                           "The guest updates its camera/entities on its own ~200 Hz "
                           "sim tick; raw poses judder at high render rates. Re-times "
                           "them on the host clock (1 kHz camera sampler + pose "
                           "interpolation, a few ms of camera latency)."));
  {
    float w = float(REXCVAR_GET(skate3_native_render_scene_smooth_camera_filter_ms));
    if (ImGui::SliderFloat("camera filter (ms, 0 = off)", &w, 0.0f, 100.0f, "%.0f")) {
      REXCVAR_SET(skate3_native_render_scene_smooth_camera_filter_ms, double(w));
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Boxcar average over the camera pose signal. The game's camera "
          "advances in 60 Hz-quantized lumps at high fps (the measured cause "
          "of stick-pan stutter); 50 ms = three 60 Hz periods cancels it "
          "exactly for ~25 ms extra camera latency. 0 reverts to raw "
          "interpolation.");
    }
  }
  REXCVAR_SET(skate3_native_render_scene_sort_opaque,
              CvarCheckbox("Front-to-back opaque sort",
                           REXCVAR_GET(skate3_native_render_scene_sort_opaque),
                           "Early-z rejects occluded pixels before the material shading"));
  {
    int cap = int(REXCVAR_GET(skate3_guest_fps_cap));
    if (ImGui::InputInt("Guest fps cap (0 = off)", &cap, 10, 30)) {
      if (cap < 0) cap = 0;
      if (cap > 1000) cap = 1000;
      REXCVAR_SET(skate3_guest_fps_cap, double(cap));
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Pace guest frames on an even beat. Set a few fps below the display "
          "refresh (with G-Sync/VRR this is what makes motion read as smooth; "
          "uncapped, the guest's irregular frame times drive the refresh "
          "directly).");
    }
  }
}

void DrawDiagnosticsSection() {
  {
    int mode = REXCVAR_GET(skate3_native_render_scene_debug);
    const char* kModes[] = {"0: normal", "1: clear only", "2: solid colors",
                            "3: first 20 items", "4: no depth"};
    if (ImGui::Combo("debug mode", &mode, kModes, 5)) {
      REXCVAR_SET(skate3_native_render_scene_debug, mode);
    }
  }
  {
    int dbg = REXCVAR_GET(skate3_native_render_scene_hdr_debug);
    const char* kHdrDbg[] = {"0: off",           "1: bloom term",
                             "2: raw pre-tonemap", "3: AO plane",
                             "4: shaft plane",   "5: haze term"};
    if (ImGui::Combo("HDR debug view", &dbg, kHdrDbg, 6)) {
      REXCVAR_SET(skate3_native_render_scene_hdr_debug, dbg);
    }
  }

  ImGui::SeparatorText("Reflective glass isolation (env fam 5/6/13)");
  {
    int mode = REXCVAR_GET(skate3_native_render_scene_refl_mode);
    const char* kReflModes[] = {"0: normal",
                                "1: cube reflection OFF",
                                "2: cube at absolute LOD (slider)",
                                "3: flat normal (no normal map)",
                                "4: visualize cube sample only",
                                "5: body only (no spec, no cube)",
                                "6: normal-map LOD bias (slider)",
                                "7: visualize lightmap sample",
                                "8: visualize lightmap UV (frac x16)",
                                "9: lightmap resolve status (blue = missing)"};
    if (ImGui::Combo("refl mode", &mode, kReflModes, 10)) {
      REXCVAR_SET(skate3_native_render_scene_refl_mode, mode);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Live isolation of the glass-facade reflection term. Flip F5 to "
          "compare against the emulated frame at each setting:\n"
          "1 removes the cube term; if the artifact survives, it is NOT "
          "the reflection.\n2 + slider finds which mip level (if any) "
          "matches the emulated look.\n3 tests the normal-map perturbation."
          "\n4 shows exactly what the reflection vector samples.\n5 leaves "
          "only diffuse x lightmap.");
    }
    float lod = float(REXCVAR_GET(skate3_native_render_scene_refl_lod));
    if (ImGui::SliderFloat("refl LOD / extra bias", &lod, -4.0f, 12.0f, "%.1f")) {
      REXCVAR_SET(skate3_native_render_scene_refl_lod, double(lod));
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Mode 2: the absolute cube mip level (0 = sharpest; ~9 = the "
          "face average).\nOther modes: extra LOD bias added to the "
          "automatic 640p-parity bias.");
    }
    REXCVAR_SET(skate3_native_render_scene_refl_bias_auto,
                CvarCheckbox("auto normal tilt (derive from material)",
                             REXCVAR_GET(skate3_native_render_scene_refl_bias_auto),
                             "Compute the constant tilt from each material's own "
                             "detail texture (hardware-exact BC1 decode), the "
                             "principled source of the hand-tuned reference "
                             "values. Uncheck to drive the sliders below "
                             "instead."));
    float bx = float(REXCVAR_GET(skate3_native_render_scene_refl_bias_x));
    if (ImGui::SliderFloat("normal tilt X (horizontal)", &bx, -0.2f, 0.2f, "%.3f")) {
      REXCVAR_SET(skate3_native_render_scene_refl_bias_x, double(bx));
    }
    float by = float(REXCVAR_GET(skate3_native_render_scene_refl_bias_y));
    if (ImGui::SliderFloat("normal tilt Y (vertical)", &by, -0.2f, 0.2f, "%.3f")) {
      REXCVAR_SET(skate3_native_render_scene_refl_bias_y, double(by));
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
      ImGui::SetTooltip(
          "Constant normal tilt on the glass, applied in EVERY mode; "
          "rotates all reflections. Drag until the reflected content sits "
          "where the emulated frame puts it (F5 to compare). Defaults "
          "(0.028, 0.012) = the exact detail-map constant. 0.01 here is "
          "roughly 1 degree of reflection rotation.");
    }
  }

  ImGui::SeparatorText("Judder isolation");
  {
    int mode = REXCVAR_GET(skate3_native_render_scene_synthetic_pan);
    const char* kPanModes[] = {"0: off", "1: time-based (build clock)",
                               "2: fixed step per frame", "3: through smoother"};
    if (ImGui::Combo("Synthetic pan (P)", &mode, kPanModes, 4)) {
      REXCVAR_SET(skate3_native_render_scene_synthetic_pan, mode);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Judder isolation probe: replaces the camera with a host-generated "
          "constant-rate 360-degree pan. AFTER ENGAGING, SWEEP THE REAL CAMERA "
          "AROUND ONCE WITH THE STICK; the game only submits what its own "
          "frustum sees; the sweep fills a static-world union so the full "
          "circle is populated.\n1 smooth = camera path is fine; 1 judders = "
          "frame pacing / present / display, not the camera.\n2 = constant "
          "angle per FRAME (smooth only if displayed frames are evenly spaced "
          ", the complement of 1).\n3 = feeds synthetic ~200 Hz samples "
          "through the camera smoother and logs reconstruction error "
          "(synthetic-pan: lines).");
    }
    float rate = float(REXCVAR_GET(skate3_native_render_scene_synthetic_pan_rate));
    if (ImGui::SliderFloat("pan rate (deg/s)", &rate, 5.0f, 720.0f, "%.0f")) {
      REXCVAR_SET(skate3_native_render_scene_synthetic_pan_rate, double(rate));
    }
    float amp = float(REXCVAR_GET(skate3_native_render_scene_synthetic_pan_amp));
    if (ImGui::SliderFloat("pan amplitude (0 = full spin)", &amp, 0.0f, 60.0f, "%.0f")) {
      REXCVAR_SET(skate3_native_render_scene_synthetic_pan_amp, double(amp));
    }
    if (ImGui::Button("Record camera signal (8 s)")) {
      skate3::native_scene::RecordCameraSignal(8.0);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Click, then IMMEDIATELY pan the camera with the right stick at a "
          "steady rate for 8 seconds (synthetic pan should be OFF). Records "
          "every distinct guest camera pose with 1 kHz-sampler timestamps + "
          "the smoothed output, to logs\\cam_signal_<ts>.csv; offline "
          "analysis shows whether the game's own camera signal is jerky at "
          "the source.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Record bone signal (6 s)")) {
      skate3::native_scene::RecordBoneSignal(6.0);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Click, then skate at a steady speed for 6 seconds. Records every "
          "raw entity pose (bone palettes with timestamps) + the rendered "
          "interpolation output to logs\\bone_signal_<ts>.bin, so wheel-vs-"
          "deck lag/orbit can be measured and candidate fixes replayed "
          "offline against real data.");
    }
  }
}

void DrawCachesSection() {
  REXCVAR_SET(
      skate3_native_render_scene_tex_revalidate,
      CvarCheckbox("Texture payload revalidation",
                   REXCVAR_GET(skate3_native_render_scene_tex_revalidate),
                   "Re-fingerprint cached texture payloads every 16 frames, re-decode "
                   "on change (added to heal late-composed lightmap pages)"));
  REXCVAR_SET(skate3_native_render_scene_mesh_revalidate,
              CvarCheckbox("Mesh payload revalidation",
                           REXCVAR_GET(skate3_native_render_scene_mesh_revalidate),
                           "Re-decode cached meshes when the guest payload fingerprint "
                           "changes (streaming arena reuse / CPU-animated buffers)"));
  REXCVAR_SET(skate3_native_render_scene_tex_mips,
              CvarCheckbox("Texture mip chains",
                           REXCVAR_GET(skate3_native_render_scene_tex_mips),
                           "Upload guest mip chains (off = mip 0 only). Flush the "
                           "texture cache after toggling."));
  if (ImGui::Button("Flush texture cache")) {
    skate3::native_scene::FlushTextureCache();
  }
  ImGui::SameLine();
  if (ImGui::Button("Flush mesh cache")) {
    skate3::native_scene::FlushMeshCache();
  }
}

}  // namespace

void MaxQualityAutoCycle(uint64_t frame_number) {
  const int32_t period = REXCVAR_GET(skate3_native_render_scene_maxq_cycle);
  if (period <= 0) {
    return;
  }
  static uint64_t next_frame = 0;
  if (next_frame == 0) {
    next_frame = frame_number + uint64_t(period);
    return;
  }
  if (frame_number >= next_frame) {
    next_frame = frame_number + uint64_t(period);
    SetMaxQuality(!s_max_quality.active);
    REXLOG_INFO("native-scene: max-quality auto-cycle -> {}",
                s_max_quality.active ? "ON" : "OFF");
  }
}

void NativeDebugDialog::Show() {
  visible_ = true;
  SetDrawActive(true);
}

void NativeDebugDialog::Hide() {
  if (!visible_) {
    return;
  }
  visible_ = false;
  SetDrawActive(false);
}

void NativeDebugDialog::Toggle() {
  if (visible_) {
    Hide();
  } else {
    Show();
  }
}

void NativeDebugDialog::OnDraw(ImGuiIO& io) {
  (void)io;
  if (!visible_) {
    return;
  }
  bool open = visible_;
  ImGui::SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Native Render Debug (F12)", &open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    if (!open) {
      Hide();
    }
    return;
  }

  // Renderer master switches stay visible above the sections: they gate
  // everything below and are the first thing checked when bisecting.
  if (!REXCVAR_GET(skate3_native_render)) {
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                       "skate3_native_render hook layer is OFF (boot-time)");
  }
  {
    const bool v = CvarCheckbox("Native scene renderer (F5)",
                                REXCVAR_GET(skate3_native_render_scene),
                                "Full native/emulated switch, same as F5");
    if (v != REXCVAR_GET(skate3_native_render_scene)) {
      skate3::native_scene::ToggleSceneEnabled();
    }
  }
  REXCVAR_SET(native_render_suppress_emulated_draws,
              CvarCheckbox("Suppress emulated draws",
                           REXCVAR_GET(native_render_suppress_emulated_draws),
                           "Skip emulated GPU work for framebuffer-sized passes while "
                           "native output is active (perf). Small-surface passes "
                           "(lightmap page composition) always run."));

  if (ImGui::CollapsingHeader("Showcase & capture",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    DrawShowcaseCaptureSection();
  }
  if (ImGui::CollapsingHeader("Image quality",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    DrawImageQualitySection();
  }
  if (ImGui::CollapsingHeader("Lighting & post")) {
    DrawLightingPostSection();
  }
  if (ImGui::CollapsingHeader("Shadows")) {
    DrawShadowsSection();
  }
  if (ImGui::CollapsingHeader("World shading")) {
    DrawWorldShadingSection();
  }
  if (ImGui::CollapsingHeader("Scene content & overlays")) {
    DrawSceneContentSection();
  }
  if (ImGui::CollapsingHeader("Smoothness & pacing")) {
    DrawPacingSection();
  }
  if (ImGui::CollapsingHeader("Diagnostics")) {
    DrawDiagnosticsSection();
  }
  if (ImGui::CollapsingHeader("Caches")) {
    DrawCachesSection();
  }

  ImGui::End();
  if (s_showcase_setup_open) {
    DrawShowcaseSetupWindow(&s_showcase_setup_open);
  }
  if (!open) {
    Hide();
  }
}

void RenderModeIndicator::OnDraw(ImGuiIO& io) {
  // Online net HUD + game-mode scoreboard render WHENEVER online play is active,
  // independent of the render-mode-indicator debug cvar (they self-gate on
  // system.active()). Kept above the indicator's cvar early-return so every
  // instance shows the net UI without needing skate3_native_render_mode_indicator
  // set in its config (that gate previously hid the scoreboard on peers whose
  // config didn't enable it).
  DrawNetOverlay(io);

  // Force-show while the native scene renderer is switched off (F5, the
  // settings Renderer row, or the boot-time hook-layer master) or has hit an
  // unrecoverable failure and permanently yields to the emulated output:
  // running emulated is a degraded state the player should be able to see
  // even with the indicator cvar off. Natural per-frame yields while the
  // scene renderer is enabled (menus/loading) do not trigger this.
  const bool scene_off = !REXCVAR_GET(skate3_native_render) ||
                         !REXCVAR_GET(skate3_native_render_scene) ||
                         skate3::native_scene::SceneFailed();
  if (!REXCVAR_GET(skate3_native_render_mode_indicator) && !scene_off) {
    return;
  }
  // Pre-runtime (installer wizards) no guest frame exists yet - there is no
  // renderer to indicate.
  if (rex::ui::Presenter* presenter = imgui_drawer()->presenter()) {
    const rex::ui::Presenter::GuestOutputPaintRect rect =
        presenter->GetLastGuestOutputPaintRect();
    if (rect.width <= 0 || rect.height <= 0) {
      return;
    }
  }
  const bool native = rex::graphics::IsNativeGuestOutputActive();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 10.0f, 10.0f), ImGuiCond_Always,
                          ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.35f);
  ImGui::Begin("##render_mode_indicator", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
  ImGui::TextColored(native ? ImVec4(0.35f, 1.0f, 0.45f, 1.0f)
                            : ImVec4(1.0f, 0.65f, 0.25f, 1.0f),
                     "%s", native ? "NATIVE" : "EMULATED");
  ImGui::End();
}

// [S.K.A.T.E.] Bottom-left "MATCH THIS" plate for the SET trick during Match.
// James's call: drop the flick-path circle/arrow, show the trick NAME only.
// Reads-clean at a glance, no stance mirror to get wrong, and works for every
// trick (grabs/grinds are excluded upstream so a matcher only ever sees flip
// families here anyway).
void RenderModeIndicator::DrawSetTrickHowTo(ImGuiIO& io, const std::string& trick) {
  // Name-only plate at bottom-left: black fill, thick white border, small
  // "MATCH THIS" caption over the trick name in a larger font. Big enough to
  // read at a glance without occluding gameplay.
  const float scale = std::clamp(io.DisplaySize.y / 1080.0f, 0.7f, 1.6f);
  const ImVec2 pad(20.0f * scale, 12.0f * scale);
  const float caption_size = 18.0f * scale;
  const float name_size = 34.0f * scale;
  const char* caption = "MATCH THIS";

  // Auto-size the panel to fit the trick name, with a minimum width so short
  // names still look like a plate rather than a chip.
  const float name_w = ImGui::GetFont()->CalcTextSizeA(name_size, FLT_MAX,
                                                        0.0f, trick.c_str()).x;
  const float caption_w = ImGui::GetFont()->CalcTextSizeA(caption_size, FLT_MAX,
                                                          0.0f, caption).x;
  const float content_w = std::max(name_w, caption_w);
  const float panel_w = std::max(240.0f * scale, content_w + pad.x * 2.0f);
  const float panel_h = pad.y * 2.0f + caption_size + 6.0f * scale + name_size;

  ImGui::SetNextWindowPos(ImVec2(24.0f * scale, io.DisplaySize.y - 24.0f * scale),
                          ImGuiCond_Always, ImVec2(0.0f, 1.0f));
  ImGui::SetNextWindowSize(ImVec2(panel_w, panel_h), ImGuiCond_Always);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.78f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.95f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
  ImGui::Begin("##skate3_trick_howto", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos();
  ImFont* font = ImGui::GetFont();
  const ImU32 kCaption = IM_COL32(200, 220, 255, 210);
  const ImU32 kName = IM_COL32(255, 240, 100, 250);   // gold, matches YOUR TURN.

  const float caption_x = wp.x + panel_w * 0.5f - caption_w * 0.5f;
  const float caption_y = wp.y + pad.y;
  dl->AddText(font, caption_size, ImVec2(caption_x, caption_y), kCaption, caption);
  const float name_x = wp.x + panel_w * 0.5f - name_w * 0.5f;
  const float name_y = caption_y + caption_size + 6.0f * scale;
  dl->AddText(font, name_size, ImVec2(name_x, name_y), kName, trick.c_str());

  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
}

// [online play] Toggleable on-screen net diagnostics: session state, local id,
// and each connected remote peer with its interpolated position and RTT. Only
// visible while online play is active, so single-player is unaffected. This is
// how a two-peer session is verified without reading log files, and stands in
// for the not-yet-rendered remote skater mesh.
void RenderModeIndicator::DrawNetOverlay(ImGuiIO& io) {
  namespace net = skate3::net;
  auto& system = net::Skate3Net();
  if (!system.active()) {
    return;
  }
  const net::NetStatus st = system.Status();

  const char* mode = st.mode == net::SessionMode::kHost      ? "HOST"
                     : st.mode == net::SessionMode::kClient  ? "CLIENT"
                                                             : "OFF";
  const char* state = st.state == net::SessionState::kConnected    ? "connected"
                      : st.state == net::SessionState::kConnecting  ? "connecting"
                      : st.state == net::SessionState::kFailed      ? "failed"
                      : st.state == net::SessionState::kDisconnected ? "disconnected"
                                                                     : "idle";

  // Debug net HUD (player list / scores / ping / trick): OFF by default,
  // toggled from Accessibility settings. The game-mode scoreboard below is
  // separate and shows only while a mode is active.
  if (REXCVAR_GET(skate3_net_hud)) {
  ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always, ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.4f);
  ImGui::Begin("##skate3_net_overlay", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
  ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                     "NET %s  %s  id=%u  peers=%u", mode, state,
                     static_cast<unsigned>(st.local_id), st.peer_count);
  if (!st.last_error.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "err: %s", st.last_error.c_str());
  }
  // Local player's live score + current trick (game-mode foundation).
  {
    const std::string lt = system.LocalTrick();
    ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f), "  YOU  score=%u  trick=%s",
                       static_cast<unsigned>(system.LocalScore()),
                       lt.empty() ? "-" : lt.c_str());
  }
  const std::vector<net::RemoteSkaterView> remotes =
      system.SampleRemoteSkaters(system.NowMs());
  if (remotes.empty()) {
    ImGui::TextDisabled("  (no remote players)");
  }
  for (const net::RemoteSkaterView& r : remotes) {
    if (r.valid) {
      ImGui::Text("  #%u %s  score=%u  trick=%s  %ums",
                  static_cast<unsigned>(r.id),
                  r.name.empty() ? "?" : r.name.c_str(),
                  static_cast<unsigned>(r.state.score),
                  r.current_trick.empty() ? "-" : r.current_trick.c_str(),
                  r.rtt_ms);
    } else {
      ImGui::Text("  #%u %s  (waiting for data)",
                  static_cast<unsigned>(r.id), r.name.empty() ? "?" : r.name.c_str());
    }
  }
  ImGui::End();
  }  // if (skate3_net_hud)

  // --- Online game-mode scoreboard (Milestone D) -----------------------------
  // Styled to echo base Skate 3's session board: dark warm panel, tan/orange
  // header + "SESSION BEST" rows. Only shows while a round is running.
  const net::GameModeView gv = system.GetGameModeView();
  if (gv.active) {
    const ImVec4 kTan(0.86f, 0.72f, 0.47f, 1.0f);
    const ImVec4 kOrange(0.96f, 0.60f, 0.16f, 1.0f);
    const ImVec4 kWhite(0.93f, 0.92f, 0.89f, 1.0f);
    const ImVec4 kGold(1.0f, 0.84f, 0.22f, 1.0f);
    const ImVec4 kDim(0.62f, 0.60f, 0.56f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.09f, 0.08f, 0.065f, 0.86f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.55f, 0.42f, 0.20f, 0.85f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 10.0f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12.0f, 12.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(290.0f, 0.0f), ImGuiCond_Always);
    ImGui::Begin("##skate3_mode_board", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    const uint32_t secs = (gv.remaining_ms + 999u) / 1000u;
    if (gv.phase == net::GamePhase::kCountdown) {
      ImGui::TextColored(kOrange, "SPOT BATTLE");
      ImGui::SetWindowFontScale(2.4f);
      ImGui::TextColored(kWhite, "  %u", secs == 0u ? 1u : secs);
      ImGui::SetWindowFontScale(1.0f);
      ImGui::TextColored(kDim, "get ready...");
    } else if (gv.phase == net::GamePhase::kActive) {
      ImGui::TextColored(kOrange, "SPOT BATTLE");
      ImGui::SameLine();
      ImGui::TextColored(kTan, "  %u:%02u", secs / 60u, secs % 60u);
    } else {
      ImGui::TextColored(kGold, "RESULTS");
    }
    ImGui::Separator();
    ImGui::TextColored(kTan, "SESSION BEST");
    int rank = 1;
    for (const net::GameModeEntry& e : gv.entries) {
      const bool win = gv.phase == net::GamePhase::kResults &&
                       e.id == gv.winner && e.best > 0;
      const ImVec4 col = win ? kGold : kWhite;
      ImGui::TextColored(col, "%d. %s", rank++, e.name.c_str());
      ImGui::SameLine(ImGui::GetWindowWidth() - 96.0f);
      ImGui::TextColored(win ? kGold : kTan, "%9u", static_cast<unsigned>(e.best));
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    // Centered WINNER popup during results (auto-closes when the ~5s results
    // phase ends and the round returns to idle).
    if (gv.phase == net::GamePhase::kResults) {
      const char* wname = nullptr;
      uint32_t wbest = 0;
      for (const net::GameModeEntry& e : gv.entries) {
        if (e.id == gv.winner) { wname = e.name.c_str(); wbest = e.best; break; }
      }
      ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.08f, 0.06f, 0.93f));
      ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.90f, 0.70f, 0.22f, 0.95f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.5f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(30.0f, 22.0f));
      ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.42f),
                              ImGuiCond_Always, ImVec2(0.5f, 0.5f));
      ImGui::Begin("##skate3_winner_popup", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
      ImGui::SetWindowFontScale(1.5f);
      ImGui::TextColored(kOrange, "SPOT BATTLE");
      ImGui::SetWindowFontScale(2.6f);
      if (wname != nullptr && wbest > 0) {
        ImGui::TextColored(kGold, "%s WINS!", wname);
      } else {
        ImGui::TextColored(kGold, "NO SCORE");
      }
      ImGui::SetWindowFontScale(1.25f);
      if (wname != nullptr && wbest > 0) {
        ImGui::TextColored(kWhite, "best line  %u", static_cast<unsigned>(wbest));
      }
      ImGui::SetWindowFontScale(1.0f);
      ImGui::End();
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor(2);
    }
  }

  // --- S.K.A.T.E. board ------------------------------------------------------
  const net::SkateView sv = system.GetSkateView();
  if (sv.active) {
    const ImVec4 kOrange(1.0f, 0.55f, 0.15f, 1.0f);
    const ImVec4 kGold(1.0f, 0.85f, 0.3f, 1.0f);
    const ImVec4 kWhite(0.95f, 0.95f, 0.95f, 1.0f);
    const ImVec4 kDim(0.5f, 0.5f, 0.5f, 1.0f);
    const unsigned secs = (sv.remaining_ms + 999) / 1000;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12.0f, 12.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##skate3_skate_board", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextColored(kOrange, "S.K.A.T.E.   round %u/%u",
                       static_cast<unsigned>(sv.round),
                       static_cast<unsigned>(sv.rounds));
    switch (sv.phase) {
      case net::SkatePhase::kCountdown:
        ImGui::TextColored(kGold, "starting in %u...", secs); break;
      case net::SkatePhase::kSet:
        ImGui::TextColored(kWhite, "SET a trick   %us", secs); break;
      case net::SkatePhase::kMatch:
        ImGui::TextColored(kWhite, "MATCH:  %s   %us",
                           sv.set_trick.empty() ? "?" : sv.set_trick.c_str(), secs);
        break;
      case net::SkatePhase::kAnnounce:
        ImGui::TextColored(kGold, "%s",
                           sv.message.empty() ? "..." : sv.message.c_str());
        break;
      case net::SkatePhase::kResults:
        ImGui::TextColored(kGold, "GAME OVER"); break;
      default: break;
    }
    ImGui::Separator();
    for (const net::SkateLetterRow& p : sv.players) {
      const char* L = "SKATE";
      std::string spelled;
      for (int i = 0; i < 5; ++i) spelled += (i < p.letters) ? L[i] : '-';
      const ImVec4 col = (p.id == sv.current) ? kGold
                         : (p.letters >= 5)   ? kDim
                                              : kWhite;
      ImGui::TextColored(col, "%-14s %s%s", p.name.c_str(), spelled.c_str(),
                         p.id == sv.current ? "  <" : "");
    }
    ImGui::End();

    // Center-left announce beat (paced result message): big text in a box with
    // a thick white outline and a black fill.
    auto centered_box = [&](const char* text, const ImVec4& color, float scale) {
      ImGui::SetNextWindowPos(
          ImVec2(io.DisplaySize.x * 0.04f, io.DisplaySize.y * 0.44f),
          ImGuiCond_Always, ImVec2(0.0f, 0.5f));
      ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.82f));
      ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 4.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 11.0f));
      ImGui::Begin("##skate3_turn_msg", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
      ImGui::SetWindowFontScale(scale);
      ImGui::TextColored(color, "%s", text);
      ImGui::SetWindowFontScale(1.0f);
      ImGui::End();
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor(2);
    };
    if (sv.phase == net::SkatePhase::kAnnounce && !sv.message.empty()) {
      centered_box(sv.message.c_str(), kGold, 1.5f);
    } else if ((sv.your_turn || sv.youre_next) &&
               (sv.phase == net::SkatePhase::kSet ||
                sv.phase == net::SkatePhase::kMatch)) {
      centered_box(sv.your_turn ? "YOUR TURN" : "YOU'RE NEXT",
                   sv.your_turn ? kGold : kWhite, 1.5f);
    }

    // Bottom-left "how-to" panel for the SET trick during Match phase: a
    // circle with the right-stick flick path drawn on it, plus the trick
    // name. Mirrors L/R by this viewer's stance (skate3_stance cvar) so
    // goofy and regular players see the diagram flipped correctly.
    if (sv.phase == net::SkatePhase::kMatch && !sv.set_trick.empty()) {
      DrawSetTrickHowTo(io, sv.set_trick);
    }
  }
}

}  // namespace skate3
