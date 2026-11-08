#include <core/log/log.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/display.h>
#include <domain/gfx/opacity_micromap.h>
#include <domain/gfx/requirements.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/flora_motion.h>
#include <systems/renderer/rt_capacity.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/view_set.h>

#include <algorithm>
#include <iterator>

namespace engine::renderer {

ENGINE_LOG_CATEGORY_DEFINE(log_renderer, "renderer");

namespace {
tunables::Bool shadow_texel_lod{
    "renderer.shadow.texel_lod", true,
    "The sun's cascades also accept a transition by their own texels (R103); false draws every "
    "cascade from the picture's cut alone"};
tunables::Float shadow_texel_error{
    "renderer.shadow.texel_error", static_cast<f64>(k_shadow_texel_error_texels), 0.0, 64.0,
    "The world error a cascade accepts, in its own texels (R103; a measurement's knob)"};
tunables::Float shadow_texel_cover{
    "renderer.shadow.texel_cover", static_cast<f64>(k_shadow_texel_cover_texels), 0.0, 64.0,
    "The coverage error a cascade accepts, in its own texels (R103; a measurement's knob)"};
tunables::Float shadow_opaque_coverage{
    "renderer.shadow.opaque_coverage", static_cast<f64>(k_shadow_opaque_coverage), 0.0, 1.0,
    "With ray-traced shadows, an alpha-tested cluster whose baked coverage is at least this "
    "fraction is solid to a shadow ray, without its mask (R109); 0 is off"};
}  // namespace

const char* raster_name(RasterMode mode) noexcept {
  switch (mode) {
    case RasterMode::Direct: return "direct";
    case RasterMode::Hardware: return "hw";
    case RasterMode::Software: return "sw";
    case RasterMode::Auto: return "auto";
    case RasterMode::Vertex: return "vertex";
    case RasterMode::RayTrace: return "rt";
  }
  return "?";
}

bool parse_raster_mode(std::string_view text, RasterMode& out) noexcept {
  if (text == "direct") {
    out = RasterMode::Direct;
  } else if (text == "hw") {
    out = RasterMode::Hardware;
  } else if (text == "sw") {
    out = RasterMode::Software;
  } else if (text == "auto") {
    out = RasterMode::Auto;
  } else if (text == "vertex") {
    out = RasterMode::Vertex;
  } else if (text == "rt") {
    out = RasterMode::RayTrace;
  } else {
    return false;
  }
  return true;
}

const char* shadow_name(ShadowMode mode) noexcept {
  switch (mode) {
    case ShadowMode::Auto: return "auto";
    case ShadowMode::Off: return "off";
    case ShadowMode::RayTraced: return "rt";
    case ShadowMode::Cascaded: return "csm";
    case ShadowMode::Hybrid: return "hybrid";
  }
  return "?";
}

bool parse_shadow_mode(std::string_view text, ShadowMode& out) noexcept {
  if (text == "off") {
    out = ShadowMode::Off;
  } else if (text == "rt") {
    out = ShadowMode::RayTraced;
  } else if (text == "csm") {
    out = ShadowMode::Cascaded;
  } else if (text == "auto") {
    out = ShadowMode::Auto;
  } else if (text == "hybrid") {
    out = ShadowMode::Hybrid;
  } else {
    return false;
  }
  return true;
}

const char* water_quality_name(WaterQuality quality) noexcept {
  return quality == WaterQuality::Low ? "low" : "high";
}

bool parse_water_quality(std::string_view text, WaterQuality& out) noexcept {
  if (text == "low") {
    out = WaterQuality::Low;
  } else if (text == "high") {
    out = WaterQuality::High;
  } else {
    return false;
  }
  return true;
}

const char* resolved_shadow_name(const ResolvedSettings& resolved) noexcept {
  if (resolved.hybrid) return "hybrid";
  return resolved.shadows ? "rt" : (resolved.csm ? "csm" : "off");
}

const char* deform_name(const RenderSettings& settings) noexcept {
  if (!settings.deform) return "none";
  switch (settings.deform_kind) {
    case gfx::k_deform_wave: return "wave";
    case gfx::k_deform_lattice: return "lattice";
    default: return "identity";
  }
}

bool parse_deform_mode(std::string_view text, bool& deform, u32& kind) noexcept {
  if (text == "none") {
    deform = false;
  } else if (text == "identity") {
    deform = true;
    kind = gfx::k_deform_identity;
  } else if (text == "wave") {
    deform = true;
    kind = gfx::k_deform_wave;
  } else if (text == "lattice") {
    deform = true;
    kind = gfx::k_deform_lattice;
  } else {
    return false;
  }
  return true;
}

const char* view_mode_name(u32 mode) noexcept {
  switch (mode) {
    case 0: return "id";
    case 1: return "tri";
    case 2: return "depth";
    case 3: return "shaded";
    case 4: return "normals";
    case 5: return "uv";
    case 6: return "shadow";
    case 7: return "albedo";
    case 8: return "occlusion";
    case 9: return "detail";
    case 10: return "ramp";
    case 11: return "sun-specular";
    case 12: return "sun-specular-legacy";
    default: return "?";
  }
}

bool parse_view_mode(std::string_view text, u32& out) noexcept {
  if (text == "id") {
    out = 0;
  } else if (text == "tri") {
    out = 1;
  } else if (text == "depth") {
    out = 2;
  } else if (text == "shaded") {
    out = 3;
  } else if (text == "normals") {
    out = 4;
  } else if (text == "uv") {
    out = 5;
  } else if (text == "shadow") {
    out = 6;
  } else if (text == "albedo") {
    out = 7;
  } else if (text == "occlusion") {
    out = 8;
  } else if (text == "detail") {
    out = 9;  // the sand's detail as data (gfx::ResolveMode::GroundDetail)
  } else if (text == "ramp") {
    out = 10;  // the output encode's test pattern (gfx::ResolveMode::Ramp, E39)
  } else if (text == "sun-specular") {
    out = 11;
  } else if (text == "sun-specular-legacy") {
    out = 12;
  } else {
    return false;
  }
  return true;
}

const char* view_layout_name(ViewLayout layout) noexcept {
  switch (layout) {
    case ViewLayout::Single: return "single";
    case ViewLayout::Surround3: return "surround3";
    case ViewLayout::Panini: return "panini";
  }
  return "?";
}

bool parse_view_layout(std::string_view text, ViewLayout& out) noexcept {
  if (text == "single") {
    out = ViewLayout::Single;
  } else if (text == "surround3") {
    out = ViewLayout::Surround3;
  } else if (text == "panini") {
    out = ViewLayout::Panini;
  } else {
    return false;
  }
  return true;
}

namespace {

// The HDR output's display, for a run that names none (E39; `display_levels`). 0 is "not set":
// what the display reports, then the defaults.
tunables::Float display_peak{"renderer.display.peak_nits", 0.0, 0.0, 10000.0,
                             "The peak luminance an HDR picture rolls off towards, nits (0: what "
                             "the display reports, else 1000)"};
// The foliage LOD's two numbers (RenderSettings::foliage_px and lod_fade; R91).
tunables::Float lod_foliage_px{
    "renderer.lod.foliage_px", static_cast<f64>(geometry::k_default_foliage_px), 0.0, 1000.0,
    "The pixels a coverage error (a leaf card's) may project to before "
    "the finer foliage is drawn instead (renderer.md, The foliage threshold)"};
tunables::Float lod_fade{"renderer.lod.fade", 0.1, 0.0, 0.5,
                         "The cross-fade's half band at a foliage switch, a fraction of the "
                         "threshold; 0 switches at once"};
// R118, the occlusion term of the LOD threshold (renderer.md, "Occlusion-aware LOD"): k, the most a
// wholly hidden transition's thresholds are multiplied by is 1 + k; the term's footprint's mip
// bias; and two measurement switches, never defaults — k cycled over a flight, and the debug
// counters.
tunables::Float lod_occluded_k{
    "renderer.lod.occluded_scale", static_cast<f64>(k_lod_occluded_scale), 0.0, 64.0,
    "The LOD thresholds of a transition the Hi-Z shows hidden by a share h are multiplied by "
    "1 + k h, so a crown behind nearer leaves is drawn coarser; 0 is off (renderer.md, "
    "Occlusion-aware LOD)"};
tunables::Int lod_occluded_bias{
    "renderer.lod.occluded_bias", static_cast<i64>(k_lod_occluded_bias), 0, 4,
    "How many mips finer than the occlusion test's the term reads a transition's hidden share at; "
    "each is up to four times the reads"};
tunables::Bool lod_occluded_foliage_only{
    "renderer.lod.occluded_foliage_only", true,
    "The occlusion term moves the foliage (coverage) threshold alone, so a trunk or the ground "
    "keeps its geometric threshold whatever stands in front of it; false moves both"};
tunables::Int lod_occluded_cycle{
    "renderer.lod.occluded_cycle", 0, 0, 1 << 20,
    "Measurement only: n > 0 holds k at 0, k/2 and k for n frames each in turn (frame f takes "
    "(f / n) % 3), so one flight measures the three"};
tunables::Bool lod_occluded_histogram{
    "renderer.lod.occluded_histogram", false,
    "Measurement only: count the drawn pairs' hidden shares and what the occlusion term changed, "
    "one log line a frame, building the Hi-Z where nothing else does"};
}  // namespace

f32 lod_occluded_scale_tunable(u64 frame_index) noexcept {
  const f32 k = static_cast<f32>(lod_occluded_k.get());
  const i64 cycle = lod_occluded_cycle.get();
  if (cycle <= 0) return k;
  return k * 0.5f * static_cast<f32>((frame_index / static_cast<u64>(cycle)) % 3);
}
bool lod_occluded_active_tunable() noexcept {
  return lod_occluded_k.get() > 0.0 || lod_occluded_histogram.get();
}
u32 lod_occluded_bias_tunable() noexcept { return static_cast<u32>(lod_occluded_bias.get()); }
bool lod_occluded_histogram_tunable() noexcept { return lod_occluded_histogram.get(); }
bool lod_occluded_foliage_only_tunable() noexcept { return lod_occluded_foliage_only.get(); }

namespace {
tunables::Float display_paper_white{"renderer.display.paper_white_nits", 0.0, 0.0, 10000.0,
                                    "The luminance an HDR picture shows a diffuse white at, nits "
                                    "(0: Windows' SDR white level for the display, else 200)"};
// A measurement switch, never a default (R102): what the alpha-tested casters' masks cost the
// shadow rays, read off the same frame with every caster opaque.
tunables::Bool shadow_opaque_rays{
    "renderer.shadow.opaque_rays", false,
    "Measurement only: the shadow rays treat every caster as opaque, so an alpha-tested leaf "
    "casts its whole card (renderer.md, Rays near, maps far)"};
// A measurement switch, never a default (R115): the resolve ends its work after the named stage,
// so its milliseconds split by stage (renderer.md, "Where the resolve's time goes"). The choices
// are indexed by `gfx::ResolveStop`, and `cycle`, one past them, steps through every stage a frame
// at a time — frame f draws stage f % k_resolve_stops — so one flight measures the whole curve.
constexpr const char* k_resolve_stop_choices[] = {"none",          "decode",   "reconstruct",
                                                  "material",      "lighting", "sun_shadow",
                                                  "light_shadows", "sky",      "cycle"};
static_assert(std::size(k_resolve_stop_choices) == gfx::k_resolve_stops + 1);
tunables::Enum<gfx::ResolveStop> resolve_stop_after{
    "renderer.resolve.stop_after", gfx::ResolveStop::None, k_resolve_stop_choices,
    "Measurement only: the resolve ends each pixel's work after this stage and writes what it has "
    "(none: the picture; cycle: frame f stops after stage f % 8; renderer.md, Where the resolve's "
    "time goes)"};
// R126: a shading rate by region (renderer.md, "A shading rate by region"). The sides' default is
// a rule rather than a number, so a surround gets its outer thirds and every other frame nothing.
tunables::Float resolve_half_rate_sides{
    "renderer.resolve.half_rate_sides", -1.0, -1.0, 0.5,
    "The share of the frame's width at each side whose sun ray is asked once per 2x2 block of one "
    "pair; negative: a third on a frame wider than 2.5:1, none otherwise (renderer.md, A shading "
    "rate by region)"};
tunables::Bool resolve_half_rate_centre{
    "renderer.resolve.half_rate_centre", false,
    "The centre's sun ray at half rate too, so the whole frame's (renderer.md, A shading rate by "
    "region)"};
// A measurement switch, never a default (R126): the sides by the frame, so one flight under
// `renderer.resolve.stop_after=cycle` measures every stage at every share.
tunables::Bool resolve_half_rate_sweep{
    "renderer.resolve.half_rate_sweep", false,
    "Measurement only: frame f takes the sides' share (f / 8) % 3 - none, as set, a half "
    "(renderer.md, A shading rate by region)"};

// R115: a light whose contribution is this small a fraction of the rest of the pixel's light is not
// tested for its shadow (renderer.md, "A light too faint to test"; Ward 1991). 2^-12 is a
// thirty-sixth of the finest code step of the 8-bit and the PQ encodes (0.87% and 0.90%), so six
// lights skipped at the bound move a pixel a sixth of a step.
tunables::Float shadow_light_tolerance{
    "renderer.shadow.light_tolerance", 1.0 / 4096.0, 0.0, 1.0,
    "A traced light whose unshadowed contribution is at most this fraction of the pixel's light "
    "from the sun, the ambient and the emission takes the sun ray's answer instead of a ray of its "
    "own; 0 traces every light (renderer.md, A light too faint to test)"};
// R97: a normal map's variance chain widens the GGX roughness (renderer.md, "Normal filtering").
tunables::Bool material_normal_filter{
    "renderer.material.normal_filter", true,
    "Upload a built normal map's normal-variance chain and widen the GGX roughness by the normal "
    "variation a pixel no longer resolves; false draws every material as before R97 (read at "
    "load)"};
tunables::Float shadow_rest_texels{
    "renderer.shadow.rest_texels", static_cast<f64>(k_shadow_rest_texels), 0.0, 64.0,
    "A cascade past the nearest draws a deformed instance at rest when its reach from the rest "
    "pose is under this many of its texels (R103); 0 poses every cascade"};
// R115 stage 2: the measurement sweep over the sun ray's rules (renderer.md, "Where the resolve's
// time goes").
tunables::Bool shadow_sweep{
    "renderer.shadow.sweep", false,
    "Measurement only: frame f takes the sun ray's rule (f / 8) % 6 — as set, opaque, the proxy "
    "at 0.5, 0.3, 0.2, as set counting the sun ray's work (renderer.md, Where the resolve's time "
    "goes; the proxy's table is baked at load)"};
// R120: opacity micromaps on the alpha-tested clusters' structures (renderer.md, "Opacity
// micromaps"). The budget's default is from the v25 forest's bake (forest-resolve-2026-11-01.md,
// stage 4).
tunables::Bool shadow_micromaps{
    "renderer.shadow.micromaps", true,
    "Bake opacity micromaps for the alpha-tested clusters and attach them to the ray tracing "
    "chain's structures where the device has VK_EXT_opacity_micromap; the picture is the same "
    "at the same chain capacity, and the device's default chain budget grows by their slot cost "
    "so the capacity is the same too. On since R120 batch 53: with the shadow rays' two-state "
    "answer at level 6 the v25 walk's GPU frame went 41.3 to 34.2 ms at 11520x2160, for 715 MB "
    "resident and a bake of about 5 s at load; false is the old picture and cost. Not with the "
    "opaque shadow proxy on (R120; renderer.md, Opacity micromaps; read at load)"};
tunables::Bool shadow_micromap_2state{
    "renderer.shadow.micromap_2state", true,
    "With opacity micromaps on, the shadow rays read them two-state "
    "(RAY_FLAG_FORCE_OMM_2_STATE): an unknown micro-triangle is decided by its centre sample in "
    "the traversal and never reaches the mask loop, for a shadow edge at most a micro-triangle "
    "wide; primary rays keep the four states (R120 stage 3; renderer.md, Opacity micromaps; read "
    "at load)"};
tunables::Int micromap_mib{
    "renderer.rt.micromap_mib", 1024, 0, 65536,
    "The opacity micromaps' bake budget in MiB, states, indices and triangle "
    "records together: a level is dropped until it fits (0: no budget)"};
tunables::Float micromap_texels{
    "renderer.rt.micromap_texels", static_cast<f64>(gfx::k_micromap_target_texels), 0.01, 4096.0,
    "The opacity micromaps' level per triangle: the smallest whose micro-triangles cover at most "
    "this many of the mask's level-0 texels (then the level cap)"};
tunables::Int micromap_level{
    "renderer.rt.micromap_level", 6, 0, 12,
    "The opacity micromaps' subdivision level cap (each level is four times "
    "the micro-triangles; at most the device's)"};
// R69 stage 1: indirect light from the world-space cache (renderer.md, "Indirect light: the
// world-space cache"; docs/experiments/gi-cache-2026-11-07.md has the numbers the defaults rest
// on).
constexpr const char* k_gi_mode_choices[] = {"off", "sky", "bounce"};
tunables::Enum<GiMode> gi_mode{
    "renderer.gi.mode", GiMode::Off, k_gi_mode_choices,
    "Indirect light: off, the sky's harmonics as before; sky, a world-space cache fed by rays "
    "against the frame's structures, read in their place; bounce, a ray that hits takes the hit's "
    "light too, the sun on its albedo and the cache at its cell. Only with traced shadows "
    "(renderer.md, Indirect light: the world-space "
    "cache; read at load)"};
// 32 MiB is a million cells, about thirty times what a 3840x2160 frame of the forest touches at
// the default scale, so a walk's recent past stays warm as the eye turns back.
tunables::Int gi_cache_mib{
    "renderer.gi.cache_mib", 32, 1, 4096,
    "The world-space cache's size in MiB, made once at load: 32 bytes a cell, a power of two of "
    "cells and a 4-byte key each, and a 64-byte header (renderer.md, Indirect light: the "
    "world-space cache; read at load)"};
// Two million rays: R127's primary trace on the forest is 15.4 ms for 24.9 million at full width,
// so about 1.2 ms of traversal if these cost what those do; one pixel in every stride is sampled.
tunables::Int gi_rays_per_frame{
    "renderer.gi.rays_per_frame", 2000000, 1, 1 << 28,
    "The cache's sample pass: rays a frame over every view, half of them a trickle every cell "
    "keeps and the rest for young cells (renderer.md, Indirect light: the world-space cache; read "
    "at load)"};
// A cell about ten pixels wide at 3840x2160 and 90 degrees (a pixel is 4.1e-4 radians), rounded up
// to the level: 25 mm to 6 m from the eye, 10 cm at 20 m, 40 cm at 100 m, 6.4 m past 1.6 km.
tunables::Float gi_cell_scale{
    "renderer.gi.cell_scale", 0.004, 0.0001, 1.0,
    "The cache's cell size per metre of distance from the eye, rounded up to a level of 25 mm "
    "times a power of two up to 6.4 m (renderer.md, Indirect light: the world-space cache; read "
    "at load)"};
// Rounded rocks and trunks drew the cells a block at a time where the dominant axis of their
// normals changed (R69 stage 2): the read takes the next face's cells too near a face's edge.
tunables::Bool gi_face_blend{
    "renderer.gi.face_blend", true,
    "The cache's read blends the next face's cells where the normal is near a face's edge, so a "
    "rounded surface does not step between faces (renderer.md, Indirect light: the world-space "
    "cache; read at load)"};
// Stage 2's levers (R69; renderer.md, "Indirect light: the world-space cache", "Stage 2"), each a
// knob so it can be judged against the stage before it: 1, 0, false and false are stage 1's read.
tunables::Int gi_confident{
    "renderer.gi.confident", 32, 1, 255,
    "The samples at which a cache cell is read whole; below it the read blends the cell with the "
    "next coarser level's by samples over this (1: any sample reads whole, as stage 1) "
    "(renderer.md, "
    "Indirect light: the world-space cache; read at load)"};
tunables::Int gi_evict_age{
    "renderer.gi.evict_age", 16, 0, 255,
    "Frames without a sample after which a cache cell whose bucket turned an insert away is freed "
    "(0: only by the 240-frame age, as stage 1) (renderer.md, Indirect light: the world-space "
    "cache; read at load)"};
tunables::Bool gi_young_rays{
    "renderer.gi.young_rays", true,
    "Half the cache's rays are a trickle every cell keeps and half go to pixels whose cell is "
    "young "
    "(false: every ray a stride pixel's, as stage 1) (renderer.md, Indirect light: the world-space "
    "cache; read at load)"};
tunables::Bool gi_hit_fallback{
    "renderer.gi.hit_fallback", true,
    "A bounce's hit reads up to three coarser cache levels, then the sky's harmonics times the "
    "ray's "
    "openness (false: two levels, then nothing, as stage 1) (renderer.md, Indirect light: the "
    "world-space cache; read at load)"};
tunables::Float gi_far_lift{
    "renderer.gi.far_lift", 0.0, 0.0, 1.0,
    "How far a coarse cache cell's read (1.6 m and up, past 400 m) is lifted towards the sky's "
    "harmonics, a third a level past the fifth: a crown's lit and shaded leaves the eye cannot "
    "resolve there (renderer.md, Indirect light: the world-space cache; read at load)"};
// One log line a frame of the cache's counts (R69 stage 2): how a walk fills it is read from them.
tunables::Bool gi_log{
    "renderer.gi.log", false,
    "Log the world-space cache's counts every frame: rays, samples and young ones, dropped "
    "inserts, cells held and young, cells freed (renderer.md, Indirect light: the world-space "
    "cache; read at load)"};
tunables::Bool shadow_hybrid_fit{
    "renderer.shadow.hybrid_fit", false,
    "Under the hybrid, fit the cascades from the ray distance out rather than from the near plane "
    "(R103; renderer.md, Rays near, maps far)"};

}  // namespace

bool shadow_opaque_rays_tunable() noexcept { return shadow_opaque_rays.get(); }
f32 shadow_light_tolerance_tunable() noexcept {
  return static_cast<f32>(shadow_light_tolerance.get());
}
f32 half_rate_sides_tunable(u64 frame_index) noexcept {
  const f32 as_set = static_cast<f32>(resolve_half_rate_sides.get());
  if (!resolve_half_rate_sweep.get()) return as_set;
  switch ((frame_index / gfx::k_resolve_stops) % k_half_rate_sweeps) {
    case 0: return 0.0f;
    case 1: return as_set;
    default: return 0.5f;
  }
}
bool half_rate_centre_tunable() noexcept { return resolve_half_rate_centre.get(); }

u32 resolve_stop_tunable(u64 frame_index) noexcept {
  const u32 choice = static_cast<u32>(resolve_stop_after.get());
  if (choice < gfx::k_resolve_stops) return choice;
  return static_cast<u32>(frame_index % gfx::k_resolve_stops);
}
ShadowRayRule shadow_ray_rule(u64 frame_index, f32 opaque_coverage) noexcept {
  ShadowRayRule rule;
  rule.opaque = shadow_opaque_rays.get();
  rule.opaque_coverage = opaque_coverage;
  if (!shadow_sweep.get()) return rule;
  switch ((frame_index / gfx::k_resolve_stops) % k_shadow_sweeps) {
    case 1:
      rule = ShadowRayRule{};
      rule.opaque = true;
      break;
    case 2:
      rule = ShadowRayRule{};
      rule.opaque_coverage = 0.5f;
      break;
    case 3:
      rule = ShadowRayRule{};
      rule.opaque_coverage = 0.3f;
      break;
    case 4:
      rule = ShadowRayRule{};
      rule.opaque_coverage = 0.2f;
      break;
    case 5: rule.count = true; break;
    default: break;  // the frame as set
  }
  return rule;
}
bool normal_filter_tunable() noexcept { return material_normal_filter.get(); }
f32 shadow_rest_texels_tunable() noexcept { return static_cast<f32>(shadow_rest_texels.get()); }
bool shadow_hybrid_fit_tunable() noexcept { return shadow_hybrid_fit.get(); }

DisplayLevels display_levels(const RenderSettings& settings) {
  DisplayLevels out;
  const auto pick = [](f32 setting, f64 tunable, f32 reported, f32 fallback, f32& value,
                       const char*& source) {
    if (setting > 0.0f) {
      value = setting;
      source = "setting";
    } else if (tunable > 0.0) {
      value = static_cast<f32>(tunable);
      source = "tunable";
    } else if (reported > 0.0f) {
      value = reported;
      source = "display";
    } else {
      value = fallback;
      source = "default";
    }
  };
  pick(settings.peak_nits, display_peak.get(), settings.display_peak_nits, gfx::k_default_peak_nits,
       out.peak_nits, out.peak_source);
  pick(settings.paper_white_nits, display_paper_white.get(), settings.display_paper_white_nits,
       gfx::k_default_paper_white_nits, out.paper_white_nits, out.paper_white_source);
  return out;
}

// The order here is the order the decisions depend on each other in, and changing it changes
// pictures: the mesh-shader fallback decides which path runs, the path decides whether shadows
// can be traced at all, and shadows decide whether two-pass occlusion culling may run (the
// acceleration structures are built from one visible list, and pass 2's survivors are in a
// second one). Everything after that only ever forces a switch *on*, never off.
void authored_rules(const RenderSettings& settings, u32 height, f32 fov_y,
                    AuthoredStreaming& out) noexcept {
  const f32 tan_half = std::tan(0.5f * fov_y);
  if (height > 0 && tan_half > 0.0f) out.proj_scale = static_cast<f32>(height) * 0.5f / tan_half;
  out.threshold_px = settings.lod_px;
  out.foliage_px =
      settings.foliage_px >= 0.0f ? settings.foliage_px : static_cast<f32>(lod_foliage_px.get());
  const f32 fade = settings.lod_fade >= 0.0f ? settings.lod_fade : static_cast<f32>(lod_fade.get());
  out.fade = fade > 0.5f ? 0.5f : fade;
  out.far_forest = settings.far_forest;
  // A form sways only where the trees do (renderer.md, "Streamed trees sway"), and a streamed
  // (paged) scene's never do.
  out.sway_m =
      settings.flora_motion && !settings.stream && flora_motion_tunable() && flora_tail_tunable()
          ? flora_sway_m_tunable()
          : 0.0f;
}

void resolve_settings(const RenderSettings& requested, const gfx::DeviceFeatures& features,
                      const SceneData* scene, ResolvedSettings& out, const PairAccount* account) {
  out = ResolvedSettings{};
  RenderSettings s = requested;
  if (s.foliage_px < 0.0f) s.foliage_px = static_cast<f32>(lod_foliage_px.get());
  if (s.lod_fade < 0.0f) s.lod_fade = static_cast<f32>(lod_fade.get());
  if (s.lod_fade > 0.5f) s.lod_fade = 0.5f;
  if (!s.cull && s.raster == RasterMode::Auto) s.raster = RasterMode::Hardware;
  if (!features.mesh_shader && (s.raster == RasterMode::Direct ||
                                s.raster == RasterMode::Hardware || s.raster == RasterMode::Auto)) {
    s.raster = RasterMode::Vertex;  // the baseline tier
  }
  // **A scene whose instances come and go between frames** — a streamed world's — is traced and
  // has the deformed-vertex pool since 2026-10-12: a block of the tail writes its own slots of the
  // chain's per-instance tables, and the pool's per-frame suballocation walks the visible list,
  // which a hole never reaches (GpuScene, renderer.md "Instances that come and go"); since
  // 2026-10-18 its loaded instances may be skinned or morphed, since the deform table is laid out
  // for them once and their slots never move. What it still has not is a deformed *tail*:
  // `--deform` deforms every instance and a tail instance has no entry in the pool's table, so it
  // is off with a warning; a skinned or morphed tail instance is refused by name when it is handed
  // over (`GpuScene::set_dynamic_instances`), and cluster templates are off below.
  const bool dynamic = scene != nullptr && scene->dynamic;
  if (dynamic && s.deform) {
    s.deform = false;
    ENGINE_LOG_WARN(log_renderer, "deformation off",
                    log::field("reason", "a scene whose instances come and go draws them rigid"));
  }
  // Final admission waits for GpuScene's terrain reservations. Compact records name only
  // eligible pairs; the per-frame capacity bounds the actual geometry index.
  if (account != nullptr) out.pairs = *account;
  const u32 index_limit = features.cluster_max_geometry_index;
  const bool index_known =
      account != nullptr && index_limit != 0 && features.cluster_acceleration_structure;
  const u32 index_views = view_count_of(s.views);
  out.direct = s.raster == RasterMode::Direct;
  out.vertex_path = s.raster == RasterMode::Vertex;
  out.ray_path = s.raster == RasterMode::RayTrace;

  // Ray-traced shadows want the same per-frame structures --raster rt builds, so they need the
  // same device and the same cull output. The resolve is where they are shaded, which rules out
  // the direct path, and they are built from one visible list, which rules out the modes that
  // produce more than one: the software split's second list, and occlusion culling's second
  // pass.
  const bool shadow_device = features.cluster_acceleration_structure && features.ray_query;
  const bool shadow_mode = out.ray_path || s.raster == RasterMode::Hardware || out.vertex_path;
  const bool wants_rays = s.shadows == ShadowMode::RayTraced || s.shadows == ShadowMode::Auto ||
                          s.shadows == ShadowMode::Hybrid;
  out.shadows = wants_rays && shadow_device && shadow_mode;
  // **Cascaded shadow maps** are the baseline tier's shadow (04 §4.4, the 2026-09-23 direction
  // note). They are drawn by the cull pass and the rasterizers from the light and read by the
  // resolve, so they need a path that rasterizes the picture into the visibility buffer and
  // resolves it: hw (the mesh path, or the vertex path it fell back to) and vertex. The ray path
  // traces its picture and has its own shadows, and the direct, sw and auto paths are refused for
  // the reason rt is: `auto` resolves to maps exactly where it cannot trace.
  const bool map_mode = s.raster == RasterMode::Hardware || out.vertex_path;
  // **Rays near, maps far** (renderer.md, "Rays near, maps far"; R102): both, on a path that
  // rasterizes its picture and a device that traces. The ray path draws no maps, so the hybrid is
  // its rays alone; a device that cannot trace has the maps. Asked for only: `auto` stays the rays
  // where the device traces, because on the forest it was measured against the hybrid's frame cost
  // two to three times the rays' (the cascades it draws beside them), and the page says when that
  // changes.
  const bool hybrid_asked = s.shadows == ShadowMode::Hybrid;
  out.hybrid = hybrid_asked && out.shadows && map_mode;
  const bool maps_instead = (hybrid_asked || s.shadows == ShadowMode::Auto) && !out.shadows;
  out.csm = map_mode && (s.shadows == ShadowMode::Cascaded || out.hybrid || maps_instead);
  if (out.hybrid) {
    if (!(s.shadow_ray_distance_m >= 0.0f)) s.shadow_ray_distance_m = 0.0f;
    if (!(s.shadow_ray_band_m >= 0.0f)) s.shadow_ray_band_m = 0.0f;
  }
  // An explicit `rt` on a device that cannot trace is not a warning here: `check_availability`
  // refuses it, and the host prints the device's own verdict (`unavailable_reason`).
  if (s.shadows == ShadowMode::RayTraced && !out.shadows && shadow_device) {
    ENGINE_LOG_WARN(log_renderer, "ray-traced shadows off",
                    log::field("reason", "the direct, sw, and auto paths do not build them"));
  } else if (s.shadows == ShadowMode::Hybrid && !out.hybrid) {
    ENGINE_LOG_WARN(log_renderer, "shadows not hybrid",
                    log::field("reason", out.shadows ? "the ray path draws no maps: rays alone"
                                         : out.csm   ? "no cluster acceleration structures or "
                                                       "ray queries: maps alone"
                                                     : "the direct, sw, and auto paths draw "
                                                       "neither"));
  } else if (s.shadows == ShadowMode::Auto && out.csm && !out.hybrid) {
    ENGINE_LOG_INFO(log_renderer, "cascaded shadow maps instead of ray-traced shadows",
                    log::field("reason", dynamic ? "a scene whose instances come and go"
                                                 : "no cluster acceleration structures or ray "
                                                   "queries"));
  } else if (s.shadows == ShadowMode::Cascaded && !out.csm) {
    ENGINE_LOG_WARN(log_renderer, "cascaded shadow maps off",
                    log::field("reason", "the direct, sw, auto, and rt paths do not draw them"));
  }
  if (out.csm) {
    const u32 cascades = s.shadow_cascades;
    s.shadow_cascades = cascades < 1                            ? 1u
                        : cascades > gfx::k_max_shadow_cascades ? gfx::k_max_shadow_cascades
                                                                : cascades;
    // A power of two keeps the atlas's texel grid exact in the resolve's arithmetic; the size
    // is clamped to what one atlas row of four cascades can hold on any device.
    u32 map = 64;
    while (map < s.shadow_map && map < k_max_shadow_map)
      map <<= 1;
    if (map != s.shadow_map) {
      ENGINE_LOG_INFO(log_renderer, "shadow map size rounded", log::field("asked", s.shadow_map),
                      log::field("texels", map));
    }
    s.shadow_map = map;
    if (!(s.shadow_distance >= 0.0f)) s.shadow_distance = 0.0f;
    out.shadow_cascades = s.shadow_cascades;
    out.shadow_texel_error = static_cast<f32>(shadow_texel_error.get());
    out.shadow_texel_cover = static_cast<f32>(shadow_texel_cover.get());
    out.shadow_texel_lod =
        s.shadow_texel_lod && shadow_texel_lod.get() && out.shadow_texel_error > 0.0f;
  }
  out.rt_chain = out.ray_path || out.shadows;
  // The opaque shadow proxy is the shadow rays' (R109): read once, here, so a scene bakes its
  // clusters' coverage when it is made and every frame of it reads the same threshold.
  out.shadow_opaque_coverage = out.shadows ? static_cast<f32>(shadow_opaque_coverage.get()) : 0.0f;
  // The proxy's table, baked at load, which the measurement sweep's proxy rules read too (R115).
  out.shadow_coverage_table =
      out.shadows && (out.shadow_opaque_coverage > 0.0f || shadow_sweep.get());
  // Opacity micromaps (R120), read once: a scene bakes them when it is made. Not under the opaque
  // proxy: it makes a cluster solid in the loop, and the traversal never hands the loop a
  // micromap's transparent micro-triangle to make solid.
  out.shadow_micromaps = out.rt_chain && features.cluster_acceleration_structure &&
                         features.opacity_micromap && shadow_micromaps.get() &&
                         out.shadow_opaque_coverage <= 0.0f;
  out.micromap_2state = out.shadow_micromaps && shadow_micromap_2state.get();
  out.micromap_level = static_cast<u32>(micromap_level.get());
  out.micromap_budget_bytes = static_cast<u64>(micromap_mib.get()) << 20;
  out.micromap_texels = static_cast<f32>(micromap_texels.get());
  // Indirect light (R69): read once, here, so the cache is made at load and never in a frame. It
  // traces the shadow rays' query, so it needs them.
  out.gi_mode = out.shadows && !out.direct ? gi_mode.get() : GiMode::Off;
  out.gi_cache_bytes = static_cast<u64>(gi_cache_mib.get()) << 20;
  out.gi_rays_per_frame = static_cast<u32>(gi_rays_per_frame.get());
  out.gi_cell_scale = static_cast<f32>(gi_cell_scale.get());
  out.gi_log = gi_log.get();
  out.gi_face_blend = gi_face_blend.get();
  out.gi_confident = static_cast<u32>(gi_confident.get());
  out.gi_evict_age = static_cast<u32>(gi_evict_age.get());
  out.gi_young_rays = gi_young_rays.get();
  out.gi_hit_fallback = gi_hit_fallback.get();
  out.gi_far_lift = static_cast<f32>(gi_far_lift.get());
  // More than one view needs the cull pass: every view's draw reads its own run of the visible
  // list, and there is no run without one. The direct path draws one view and one only, because
  // it writes color rather than a visibility buffer and has nowhere to put a second rectangle.
  out.view_count = view_count_of(s.views);
  if (out.direct && s.views != ViewLayout::Single) {
    s.views = ViewLayout::Single;
    out.view_count = 1;
    ENGINE_LOG_WARN(
        log_renderer, "one view only on the direct path",
        log::field("reason", "it draws straight to color and has no visibility buffer"));
  }
  if (s.views != ViewLayout::Single && !s.cull) {
    s.cull = true;
    ENGINE_LOG_WARN(log_renderer, "culling forced on with more than one view");
  }
  out.occlusion = s.occlusion && s.cull && !out.shadows &&
                  (s.raster == RasterMode::Hardware || out.vertex_path);
  if (out.shadows && s.occlusion && !out.ray_path) {
    ENGINE_LOG_INFO(log_renderer, "two-pass occlusion culling off with ray-traced shadows",
                    log::field("reason", "the structures are built from one visible list"));
  }
  if (out.rt_chain && !s.cull) {
    s.cull = true;  // the ray tracing geometry is built from the cull output
    ENGINE_LOG_WARN(log_renderer, "culling forced on", log::field("raster", raster_name(s.raster)),
                    log::field("shadows", out.shadows));
  }
  if (out.csm && !s.cull) {
    s.cull = true;  // every cascade is the cull pass's output, run from the light
    ENGINE_LOG_WARN(log_renderer, "culling forced on with cascaded shadow maps");
  }
  // The deformed-vertex pool pass runs when the settings deform every instance *or* when the
  // scene has a skinned one, and those are the same pass: skinning is `deform.slang`'s third
  // kind, not a path beside it. Deciding it here rather than at each use is what keeps
  // renderer.md's "every device- and scene-driven override lives in one function" true.
  // **A moving terrain is a deformed one** (renderer.md, "The dunes in time-lapse"): a terrain
  // whose ground provider moves with game time (the dunes; `terrain_moves`, from the registry)
  // under a time-lapse draws its heights out of the pool, blended between two evaluated fields.
  // A scene whose instances come and go has them too since 2026-10-12: the levels are its load's
  // prefix and the tail's blocks follow them. A rate that may change while
  // it runs (`time_rate_live`) needs the levels at zero too; a scene that cannot have them is only
  // warned about when something asked for motion now.
  // **The world's tiles** (renderer.md, "The ground from the world's tiles") are terrain levels
  // too: asked for by the settings or by the scene's `world` block, they need the levels at any
  // rate, and they take the rings' place.
  const bool tiles_asked =
      s.terrain_tiles || (scene != nullptr && scene->world.enabled && scene->world.ground);
  const bool millimetres = scene != nullptr && terrain_has_millimetres(scene->terrain);
  // Streamed terrain is a layout, independent of time (ADR-0057). The native source and its
  // supported filters are checked when MovingGround builds the layout through the registry.
  out.terrain_levels = scene != nullptr && scene->terrain.enabled && scene->terrain_mesh != ~0u &&
                       ((tiles_asked && millimetres) ||
                        (terrain_moves(scene->terrain) && (s.time_rate > 0.0 || s.terrain_rings ||
                                                           s.time_rate_live || tiles_asked)));
  // The rings and the tiles are more meshes in the scene, laid out once beside its own: not for a
  // streamed scene, whose clusters live in pages, and not with cluster templates, which are built
  // once per mesh from a rest pose a slot does not have.
  const bool paged = s.stream && scene != nullptr && scene->paged();
  // **The world's tiles go wherever the camera does, so their ground must be right there**
  // (ADR-0053; renderer.md, "The ground from the world's tiles"): a provider that says its heights
  // are right wherever a cell reaches (`scene_gen::k_ground_millimetres`, the dunes). The waves
  // are the stand-in ground of a scene that names no generator, float arithmetic of absolute
  // metres drawn as the scene's grid round the origin, and are refused here rather than given
  // millimetre entries (scene_gen.md, "Far from the origin", says why that is the line).
  out.terrain_tiles = out.terrain_levels && tiles_asked && !paged && millimetres;
  if (tiles_asked && !out.terrain_tiles && scene != nullptr) {
    ENGINE_LOG_WARN(
        log_renderer, "the ground is not drawn from the world's tiles",
        log::field("reason", paged          ? "a scene streamed in pages"
                             : !millimetres ? "its terrain's ground provider has no millimetre "
                                              "entries: a tile past 16 km would stand on the "
                                              "heights of a float32 metre"
                                            : "the scene has no terrain mesh"));
  }
  out.terrain_rings = out.terrain_levels && s.terrain_rings && !paged && !out.terrain_tiles &&
                      terrain_has_rings(scene->terrain);
  if (s.terrain_rings && out.terrain_tiles) {
    s.terrain_rings = false;
    ENGINE_LOG_WARN(log_renderer, "terrain rings ignored",
                    log::field("reason", "the ground is drawn from the world's tiles"));
  }
  if ((out.terrain_rings || out.terrain_tiles) && s.rt_templates) {
    s.rt_templates = false;
    ENGINE_LOG_WARN(log_renderer, out.terrain_tiles
                                      ? "cluster templates ignored with world tiles"
                                      : "cluster templates ignored with terrain rings");
  }
  // **Trees sway** (renderer.md, "The flora stage"): a scene whose meshes carry the motion contract
  // draws them through the pool's flora stage — unless streaming was asked for, which indexes the
  // vertices by page and wins, since asking for it is deliberate and swaying is the default.
  bool any_motion = false;
  if (scene != nullptr) {
    for (const SourceMesh& source : scene->sources)
      any_motion = any_motion || source.has_motion();
  }
  out.flora_motion = any_motion && s.flora_motion && flora_motion_tunable() && !s.stream;
  if (any_motion && s.flora_motion && s.stream) {
    ENGINE_LOG_WARN(log_renderer, "trees drawn rigid",
                    log::field("reason", "a streamed scene indexes its vertices by page"));
  }
  out.deform_pass = s.deform || out.terrain_levels || out.flora_motion ||
                    (scene != nullptr && (scene->skinned() || scene->morphed()));
  if (out.deform_pass && !s.cull) {
    s.cull = true;  // the pool pass walks the cull's visible list, which is the point
    ENGINE_LOG_WARN(log_renderer, "culling forced on with a deformed scene");
  }
  if (s.rt_templates && !out.rt_chain) {
    s.rt_templates = false;
    ENGINE_LOG_WARN(log_renderer, "cluster templates ignored without ray tracing");
  }
  // Cluster templates are built once from the load's clusters; a tail's blocks are not laid out
  // with them yet, so a scene whose instances come and go builds its clusters' structures from the
  // records every frame (renderer.md, "Instances that come and go").
  if (s.rt_templates && dynamic) {
    s.rt_templates = false;
    ENGINE_LOG_WARN(log_renderer, "cluster templates ignored",
                    log::field("reason", "a scene whose instances come and go"));
  }
  if (scene != nullptr) {
    if (scene->instances.size() > 1 && !s.cull) {
      s.cull = true;  // a scene draws through the cull pass; there is no direct draw of one
      ENGINE_LOG_WARN(log_renderer, "culling forced on with more than one instance");
    }
    // A mesh laid out in streaming pages is ordered coarse to fine, so its leaves are the *last*
    // clusters rather than the first and the direct draw's "clusters 0..leaf_count" would draw
    // the root. The cull pass tests every cluster on its own and does not care which end they
    // are at.
    if (!s.cull && !scene->lod.lod.empty() && scene->lod.lod[0].level != 0) {
      s.cull = true;
      ENGINE_LOG_WARN(log_renderer, "culling forced on for a paged mesh: its leaves are not first");
    }
  }
  // ---- geometry streaming (04 §4.3 step 3, §4.9) ----------------------------------------------
  //
  // Streaming is refused rather than half-applied, and each of the four refusals is a statement
  // about what the cut means. Without a page table there is nothing to stream. Without the cull
  // pass there is no cut, and the drawing rule's fallback clause *is* the cut. A **deformed or
  // skinned** instance reads the deformed-vertex pool at a scene-wide vertex index, and a streamed
  // scene's vertices live at slot-relative ones, so the two index spaces disagree — joining them is
  // a change to the pool's allocation, which is E25's open suballocation item and not this one.
  // And **cluster templates** are built once at load from every cluster's float positions, which a
  // streamed scene does not hold: a template has to be built when its page arrives and dropped when
  // it leaves, which is a second residency problem with its own budget.
  if (s.stream) {
    const char* refused = nullptr;
    if (scene != nullptr && !scene->paged()) {
      refused = "the scene carries no page table; load it with SceneDesc::stream";
    } else if (out.deform_pass) {
      refused = "a deformed or skinned instance indexes the vertex pool scene-wide";
    }
    if (refused != nullptr) {
      s.stream = false;
      ENGINE_LOG_WARN(log_renderer, "geometry streaming off", log::field("reason", refused));
    } else {
      if (!s.cull) {
        s.cull = true;
        ENGINE_LOG_WARN(log_renderer, "culling forced on with geometry streaming",
                        log::field("reason", "the drawing rule falls back to the LOD cut"));
      }
      if (s.rt_templates) {
        s.rt_templates = false;
        ENGINE_LOG_WARN(
            log_renderer, "cluster templates off with geometry streaming",
            log::field("reason", "a template is built from a page's positions when it arrives"));
      }
    }
  }
  out.stream = s.stream;
  // **Shadow casters.** The cone test drops a cluster that faces away from the *camera*, and the
  // shadow rays trace what the cull pass kept, so without this a card seen from behind casts no
  // shadow and a lit box seen from its dark side loses part of its own (docs/subsystems/
  // geometry.md, "Normal cones"). The pass keeps those clusters instead, in a run no rasterizer
  // reads, and the chain builds them non-opaque so the primary rays pass through them. Only where
  // it means something: shadows traced, the cone test on, and build records rather than template
  // instantiations — an instantiated cluster takes its flags from its template, which is opaque,
  // so a caster could not be kept out of a primary ray. Templates are for deforming meshes, whose
  // instances are never cone-tested at all, so what they give up is a rigid instance's casters.
  out.casters = out.shadows && s.cone && s.shadow_casters && !s.rt_templates;
  // Drawn and caster entries partition each view's pairs. Past the compact bound, drop
  // casters first; the capacity counts any remaining drops without refusing the scene.
  if (out.casters && index_known) {
    const u64 caster_index =
        rt_max_geometry_index(out.pairs.frame_pairs(out.ray_path), index_views, true);
    if (caster_index > index_limit) {
      out.caster_index_fallback = true;
      out.casters = false;
      s.shadow_casters = false;
      ENGINE_LOG_WARN(log_renderer, "shadow casters off",
                      log::field("reason",
                                 "the scene's pairs in every view need more cluster "
                                 "geometry indices than the device has"),
                      log::field("pairs", scene->pair_count), log::field("views", index_views),
                      log::field("geometry_index", caster_index),
                      log::field("device_limit", index_limit));
    }
  }
  // **Static structures for rigid forms** (renderer.md, "Static structures for rigid forms"). A
  // streamed world's ring-1-and-beyond forms are rigid and, drawn at their frontier, a fixed cut:
  // their structures are built once and referenced per instance, and the frame builds the rest.
  // It needs the cut (a form's frontier is a cut the cull selects), and build records, since a
  // template instantiation is the per-frame path for a deforming mesh.
  // **The caster horizon**: the cascades' bound for the rays' casters, when there is one.
  // **The hybrid's casters end where its rays do** (R102): a surface past the ray distance and the
  // band asks the maps, so a caster farther than that from the eye is in no structure. A caster
  // nearer can still be met by a ray from a near receiver, which is the horizon's one approximation
  // (renderer.md, "Rays near, maps far").
  const bool distance_horizon =
      (s.rt_caster_horizon || rt_caster_horizon_tunable()) && s.shadow_distance > 0.0f;
  out.caster_horizon = out.casters && (distance_horizon || out.hybrid);
  if (out.caster_horizon) {
    f32 horizon = distance_horizon ? s.shadow_distance : 0.0f;
    if (out.hybrid) {
      // Never 0, which the cull reads as no horizon: a hybrid whose rays reach nowhere keeps none.
      const f32 reach = std::max(s.shadow_ray_distance_m + s.shadow_ray_band_m, 1.0e-3f);
      horizon = horizon > 0.0f ? std::min(horizon, reach) : reach;
    }
    out.caster_horizon_m = horizon;
  }
  out.static_forms = out.rt_chain && s.cull && s.rt_static_forms && rt_static_forms_tunable() &&
                     !s.rt_templates && !out.stream;
  // **A morphed mesh is not streamed**, and the reason is said out loud rather than discovered as
  // a wrong picture. A page's payload is the cluster's positions, attributes, triangles and
  // bindings; the morph stream is keyed by cluster too, but its slice directory indexes a
  // scene-wide delta array whose offsets a page copy would have to patch the way it patches
  // `ClusterDesc::vertex_offset`. That is the same work and belongs with it. Until then the whole
  // stream is resident (`ClusterFileReader`'s resident list says so), and a scene that asks for
  // both gets the one that draws the right picture.
  if (out.stream && scene != nullptr && scene->morphed()) {
    out.stream = false;
    ENGINE_LOG_WARN(log_renderer, "streaming refused",
                    log::field("reason", "the scene has morph channels, which are not paged yet"));
  }
  // **The vertex path draws a culled cut indexed** (cluster_vertex_indexed.slang): one draw of the
  // cut's own triangles, whose indices let the vertex cache share a cluster's vertices. Its
  // fragment stage reads SV_PrimitiveID, which a vertex pipeline only has with geometryShader, and
  // an index carries the visible slot above the local vertex, which past 65,536 clusters in a run
  // needs fullDrawIndexUint32. Without either, or without the cull pass to allocate from, the path
  // draws every cluster's whole capacity instead — the same picture, one vertex invocation per
  // corner of every triangle it could hold (gfx.md, "Baseline tier").
  out.vertex_indexed =
      out.vertex_path && s.cull && features.geometry_shader && features.full_draw_index_uint32;
  out.settings = s;
  out.settings.stream = out.stream;
}

u64 rt_max_geometry_index(u64 pair_count, u32 views, bool casters) noexcept {
  const u64 drawn = pair_count * (views > 0 ? views : 1u);
  if (drawn == 0) return 0;
  (void)casters;  // drawn and caster entries partition the same pairs
  return drawn - 1;
}

u32 flora_tail_device_budget_mib(u64 device_local_bytes) noexcept {
  const u64 share = device_local_bytes / k_flora_tail_device_share / (u64{1} << 20);
  if (share < k_flora_tail_floor_mib) return k_flora_tail_floor_mib;
  if (share > k_flora_tail_ceiling_mib) return k_flora_tail_ceiling_mib;
  return static_cast<u32>(share);
}

FloraTailBudget resolve_flora_tail_budget(u32 tunable_mib, u64 device_local_bytes) noexcept {
  if (tunable_mib != 0) return {tunable_mib, false};
  return {flora_tail_device_budget_mib(device_local_bytes), true};
}

RenderAvailability check_availability(const ResolvedSettings& resolved,
                                      const gfx::DeviceFeatures& features) noexcept {
  if (!features.buffer_int64_atomics) return RenderAvailability::NoVisibilityBuffer;
  // An explicit request the device cannot meet is refused rather than dropped. `--raster rt`
  // always was; `--shadows rt` is too, because a picture without the shadows somebody asked for
  // reads as a shadowing bug, and the adapter report already says the device cannot trace. `auto`
  // is the request that degrades quietly. The same flag on a *path* that builds no structures
  // (direct, sw, auto) is a settings conflict rather than a device one, and stays a warning in
  // `resolve_settings`.
  const bool can_trace = features.cluster_acceleration_structure && features.ray_query;
  if (!can_trace && (resolved.ray_path || resolved.settings.shadows == ShadowMode::RayTraced)) {
    return RenderAvailability::NoAccelerationStructures;
  }
  return RenderAvailability::Ok;
}

std::string unavailable_reason(RenderAvailability availability, const gfx::Device& device) {
  std::string text(device.adapter().name);
  if (availability == RenderAvailability::NoAccelerationStructures) {
    const std::string sentence = gfx::ray_tracing_degradation(device.caps());
    if (!sentence.empty()) return text + " cannot trace rays: " + sentence;
  }
  return text + " " + availability_message(availability);
}

const char* availability_message(RenderAvailability availability) noexcept {
  switch (availability) {
    case RenderAvailability::Ok: return "";
    case RenderAvailability::NoVisibilityBuffer: return "has no 64-bit buffer atomics";
    case RenderAvailability::NoAccelerationStructures:
      return "has no cluster acceleration structures or ray queries";
  }
  return "";
}

}  // namespace engine::renderer
