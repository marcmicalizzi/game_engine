// The sky in the renderer (sky.h; docs/subsystems/renderer.md, "The sky"): the scene's sky provider
// found by name, the tables' buffers and pipelines, the star table's cells, and the frame's block.
#include <domain/scene_gen/scene_gen.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/sky.h>
#include <systems/renderer/view_set.h>

#include <algorithm>
#include <cmath>
#include <renderer_log.h>
#include <shaders/sky_luts.spv.h>

namespace engine::renderer {

namespace {

// Read once a frame (renderer.md, "Exposure"). The rule's constants and what an owner changes them
// by; engine-view's keys add to the compensation for a run.
tunables::Float exposure_compensation{
    "renderer.sky.exposure_compensation_ev", 0.0, -16.0, 16.0,
    "Stops added to every sky's exposure (positive brighter), on top of the scene's own "
    "exposure_compensation_ev; engine-view's - and = keys step a run's"};
// The knee and the slope were chosen by eye on the erg's captures
// (docs/experiments/sky-2026-09-30.md): full compensation above the light of a sun about 20 degrees
// up puts noon and a late afternoon where a photograph would, and three quarters of a stop for each
// stop below it gives a sunset about a stop under a photograph's, a civil twilight about two and a
// half, and a moonlit night four and a half.
tunables::Float exposure_knee{"renderer.sky.exposure_knee_lux", 40000.0, 1.0e-4, 1.0e6,
                              "The illuminance (lux) below which a sky's exposure stops "
                              "compensating fully for the dark: about a sun 20 degrees up"};
tunables::Float exposure_slope{"renderer.sky.exposure_slope", 0.75, 0.0, 1.0,
                               "The share of each stop of darkness below renderer.sky."
                               "exposure_knee_lux the exposure still compensates: 1 is a "
                               "photograph's, 0 holds it at the knee's"};
tunables::Float shoulder{"renderer.sky.shoulder", static_cast<f64>(gfx::k_sky_shoulder), 0.1, 1.0,
                         "Where a sky's display shoulder starts, as an exposed value: below it a "
                         "channel is shown as it is, above it rolls off towards white (1: a clip)"};

Vec4 vec4(Vec3 v, f32 w) noexcept { return Vec4{v.x, v.y, v.z, w}; }

}  // namespace

f64 sky_exposure_compensation_tunable() { return exposure_compensation.get(); }
f64 sky_exposure_knee_tunable() { return exposure_knee.get(); }
f64 sky_exposure_slope_tunable() { return exposure_slope.get(); }

u32 sky_star_cell(Vec3 d) noexcept {
  const f32 ax = std::fabs(d.x);
  const f32 ay = std::fabs(d.y);
  const f32 az = std::fabs(d.z);
  u32 face = 0;
  f32 u = 0.0f;
  f32 v = 0.0f;
  if (ax >= ay && ax >= az) {
    face = d.x > 0.0f ? 0u : 1u;
    u = d.y / ax;
    v = d.z / ax;
  } else if (ay >= az) {
    face = d.y > 0.0f ? 2u : 3u;
    u = d.x / ay;
    v = d.z / ay;
  } else {
    face = d.z > 0.0f ? 4u : 5u;
    u = d.x / az;
    v = d.y / az;
  }
  const f32 n = static_cast<f32>(gfx::k_sky_star_face_cells);
  const u32 last = gfx::k_sky_star_face_cells - 1;
  const u32 cx = std::min(static_cast<u32>(std::max((u * 0.5f + 0.5f) * n, 0.0f)), last);
  const u32 cy = std::min(static_cast<u32>(std::max((v * 0.5f + 0.5f) * n, 0.0f)), last);
  return (face * gfx::k_sky_star_face_cells + cy) * gfx::k_sky_star_face_cells + cx;
}

void build_star_cells(std::span<const scene_gen::Star> stars, Vector<u32>& cells,
                      Vector<u32>& index) {
  // Every cell a star's spot can reach: its own, and the cells of points round it at the spot's
  // reach and half of it. A cell is 2.8 degrees at a face's middle and the reach 0.3, so a spot
  // touches at most the cells of a corner, and these samples find each.
  constexpr u32 k_ring = 12;
  Vector<u32> counts(gfx::k_sky_star_cells, 0u);
  Vector<u32> pairs;  // (cell, star), flattened
  pairs.reserve(static_cast<u32>(stars.size()) * 4);
  for (u32 s = 0; s < stars.size(); ++s) {
    const Vec3 d = normalize(stars[s].direction);
    const Vec3 helper = std::fabs(d.z) < 0.9f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 t = normalize(cross(helper, d));
    const Vec3 b = cross(d, t);
    u32 found[2 * k_ring + 1];
    u32 count = 0;
    auto add = [&](Vec3 p) {
      const u32 cell = sky_star_cell(p);
      for (u32 i = 0; i < count; ++i)
        if (found[i] == cell) return;
      found[count++] = cell;
    };
    add(d);
    for (u32 ring = 1; ring <= 2; ++ring) {
      const f32 reach = gfx::k_sky_star_reach * 0.5f * static_cast<f32>(ring);
      for (u32 k = 0; k < k_ring; ++k) {
        const f32 a = 6.2831853f * static_cast<f32>(k) / static_cast<f32>(k_ring);
        add(normalize(d + (t * std::cos(a) + b * std::sin(a)) * reach));
      }
    }
    for (u32 i = 0; i < count; ++i) {
      ++counts[found[i]];
      pairs.push_back(found[i]);
      pairs.push_back(s);
    }
  }
  cells.assign(gfx::k_sky_star_cells + 1, 0u);
  for (u32 c = 0; c < gfx::k_sky_star_cells; ++c)
    cells[c + 1] = cells[c] + counts[c];
  index.assign(cells[gfx::k_sky_star_cells], 0u);
  Vector<u32> cursor(cells.begin(), cells.end() - 1);
  // In star order within a cell, brightest first, so the table reads the same whichever machine
  // built it.
  for (u32 p = 0; p + 1 < pairs.size(); p += 2)
    index[cursor[pairs[p]]++] = pairs[p + 1];
}

Vec3 star_irradiance(const scene_gen::Star& star) noexcept {
  const f32 e = static_cast<f32>(std::pow(10.0, -0.4 * (static_cast<f64>(star.magnitude) + 26.74)));
  return star.color * e;
}

void SkyPass::destroy() noexcept {
  if (device_ != nullptr) {
    const gfx::Device& device = *device_;
    for (gfx::BufferResource* b : {&transmittance, &multiscatter, &sky_view, &aerial, &frame,
                                   &star_list, &star_cells, &star_index}) {
      if (b->buffer.valid()) gfx::destroy_buffer(device, *b);
      *b = gfx::BufferResource{};
    }
    for (gfx::ComputePipeline* p : {&transmittance_pipeline, &multiscatter_pipeline, &view_pipeline,
                                    &aerial_pipeline, &frame_pipeline}) {
      if (p->pipeline.valid()) gfx::destroy_compute_pipeline(device, *p);
      *p = gfx::ComputePipeline{};
    }
  }
  provider_.reset();
  device_ = nullptr;
  star_count_ = 0;
  tables_built_ = false;
}

bool SkyPass::create(const gfx::Device& device, const SceneData& scene, gfx::ShaderLibrary& shaders,
                     std::string* error) {
  destroy();
  if (!scene.sky.has_value()) return true;
  entry_ = *scene.sky;
  const std::string_view name = scene_gen::sky_provider_name(entry_);
  const scene_gen::SkyProviderDesc* desc = scene_gen::GeneratorRegistry::global().find_sky(name);
  if (desc == nullptr) {
    if (error != nullptr)
      *error = "the scene " + scene_gen::GeneratorRegistry::global().unknown_sky(name);
    return false;
  }
  std::string why;
  if (!desc->make(entry_, provider_, &why)) {
    if (error != nullptr) *error = "the scene's sky: " + why;
    return false;
  }
  ground_albedo_ = scene.ground_albedo;

  constexpr gfx::BufferUsage k_address =
      gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress;
  constexpr u64 k_texel = 4 * sizeof(f32);
  const u64 sizes[5] = {
      k_texel * gfx::k_sky_transmittance_width * gfx::k_sky_transmittance_height,
      k_texel * gfx::k_sky_multiscatter_size * gfx::k_sky_multiscatter_size,
      k_texel * gfx::k_sky_view_width * gfx::k_sky_view_height,
      k_texel * gfx::k_sky_aerial_width * gfx::k_sky_aerial_height * gfx::k_sky_aerial_depth,
      sizeof(gfx::SkyFrame)};
  gfx::BufferResource* targets[5] = {&transmittance, &multiscatter, &sky_view, &aerial, &frame};
  for (u32 i = 0; i < 5; ++i) {
    // The frame's sums are copied into the renderer's statistics block after the frame, so that
    // one is a transfer source too.
    const gfx::BufferUsage usage = i == 4 ? k_address | gfx::BufferUsage::TransferSrc : k_address;
    if (!gfx::create_buffer(device, sizes[i], usage, false, *targets[i], error)) {
      destroy();
      return false;
    }
  }
  device_ = &device;

  // The star table: each star's celestial direction and irradiance, and the cells that list them.
  // A sky without stars still gets a one-star table and empty cells, so every address is valid.
  const std::span<const scene_gen::Star> stars = provider_.stars();
  star_count_ = static_cast<u32>(stars.size());
  Vector<Vec4> list;
  list.reserve(2 * std::max(star_count_, 1u));
  for (const scene_gen::Star& s : stars) {
    list.push_back(vec4(normalize(s.direction), s.magnitude));
    list.push_back(vec4(star_irradiance(s), 0.0f));
  }
  if (list.empty()) {
    list.push_back(Vec4{0.0f, 0.0f, 1.0f, 0.0f});
    list.push_back(Vec4{});
  }
  Vector<u32> cells;
  Vector<u32> index;
  build_star_cells(stars, cells, index);
  if (index.empty()) index.push_back(0u);
  if (!gfx::upload_buffer(device, list.data(), u64{list.size()} * sizeof(Vec4),
                          gfx::BufferUsage::Storage, star_list, error) ||
      !gfx::upload_buffer(device, cells.data(), u64{cells.size()} * sizeof(u32),
                          gfx::BufferUsage::Storage, star_cells, error) ||
      !gfx::upload_buffer(device, index.data(), u64{index.size()} * sizeof(u32),
                          gfx::BufferUsage::Storage, star_index, error)) {
    destroy();
    return false;
  }

  shaders.add_embedded("sky_luts", shaders::k_sky_luts_spirv, shaders::k_sky_luts_spirv_size);
  const gfx::Shader* luts = shaders.get("sky_luts", error);
  if (luts == nullptr ||
      !gfx::create_compute_pipeline(device, luts->module, "transmittance_main", {}, sizeof(u64),
                                    transmittance_pipeline, error) ||
      !gfx::create_compute_pipeline(device, luts->module, "multiscatter_main", {}, sizeof(u64),
                                    multiscatter_pipeline, error) ||
      !gfx::create_compute_pipeline(device, luts->module, "sky_view_main", {}, sizeof(u64),
                                    view_pipeline, error) ||
      !gfx::create_compute_pipeline(device, luts->module, "aerial_main", {}, sizeof(u64),
                                    aerial_pipeline, error) ||
      !gfx::create_compute_pipeline(device, luts->module, "sky_frame_main", {}, sizeof(u64),
                                    frame_pipeline, error)) {
    destroy();
    return false;
  }
  ENGINE_LOG_INFO(log_renderer, "sky", log::field("provider", std::string(name)),
                  log::field("latitude_deg", static_cast<f64>(entry_.latitude_deg)),
                  log::field("day_of_year", static_cast<f64>(entry_.day_of_year)),
                  log::field("turbidity", static_cast<f64>(entry_.turbidity)),
                  log::field("stars", star_count_), log::field("star_cell_entries", index.size()));
  return true;
}

void SkyPass::evaluate(f64 time_s, FrameSky& out) const noexcept {
  out = FrameSky{};
  if (!active()) return;
  out.active = true;
  out.time_s = time_s;
  provider_.state(time_s, out.state);
  out.moon = entry_.moon;
  out.stars = entry_.stars && star_count_ > 0;
  out.moon_key = out.moon && out.state.sun.y <= 0.0f && out.state.moon.y > 0.0f;
}

void SkyPass::fill(const FrameSky& sky, const ViewSet& views, const Camera& camera,
                   const ExposureRequest& exposure, gfx::SkyParams& out) const noexcept {
  out = gfx::SkyParams{};
  const scene_gen::Atmosphere& air = provider_.atmosphere();
  out.rayleigh = vec4(air.rayleigh_scattering, air.rayleigh_height_km);
  out.mie_scattering = vec4(air.mie_scattering, air.mie_height_km);
  out.mie_extinction = vec4(air.mie_extinction, air.mie_g);
  out.ozone = vec4(air.ozone_absorption, air.ozone_peak_km);
  out.radii = Vec4{air.bottom_radius_km, air.top_radius_km, air.ozone_half_width_km, 0.0f};
  // The air sees the region, the ambient term the ground round the point: the region is the scene
  // ground's colour at the brightness the sky entry gives it (sky.h, `SkyParams::ground_albedo`).
  const f32 luminance =
      0.2126f * ground_albedo_.x + 0.7152f * ground_albedo_.y + 0.0722f * ground_albedo_.z;
  const Vec3 hue = luminance > 1e-4f ? ground_albedo_ * (1.0f / luminance) : Vec3{1.0f, 1.0f, 1.0f};
  out.ground_albedo = vec4(hue * entry_.ground_albedo, 0.0f);
  out.local_albedo = vec4(ground_albedo_, 0.0f);
  out.night = vec4(air.night_glow, 0.0f);
  const scene_gen::SkyState& s = sky.state;
  out.sun = vec4(normalize(s.sun), s.sun_radius);
  out.sun_illuminance = Vec4{s.sun_illuminance, s.sun_illuminance, s.sun_illuminance, 0.0f};
  out.moon = vec4(normalize(s.moon), s.moon_radius);
  out.moon_illuminance =
      Vec4{s.moon_illuminance, s.moon_illuminance, s.moon_illuminance, s.moon_albedo};
  // The eye's height in the tables: the world's y over the planet's ground, which is the world's
  // zero, and never below the tables' least nor above the top of the air.
  const f32 top = air.top_radius_km - air.bottom_radius_km;
  const f32 altitude =
      std::clamp(camera.position.y * 0.001f, gfx::k_sky_min_altitude_km, top - 0.001f);
  out.camera = vec4(camera.position, altitude);
  out.celestial_x = vec4(s.celestial_x, 0.0f);
  out.celestial_y = vec4(s.celestial_y, 0.0f);
  out.celestial_z = vec4(s.celestial_z, 0.0f);
  const bool fixed = exposure.fixed || entry_.exposure == scene::SkyExposure::Fixed;
  const f32 ev = exposure.fixed ? exposure.ev100
                 : fixed        ? entry_.exposure_ev100
                                : entry_.exposure_compensation_ev + exposure.compensation_ev +
                                      static_cast<f32>(exposure_compensation.get());
  out.exposure =
      Vec4{fixed ? gfx::k_sky_exposure_fixed : gfx::k_sky_exposure_auto, ev,
           static_cast<f32>(exposure_knee.get()), static_cast<f32>(exposure_slope.get())};
  out.stars = Vec4{static_cast<f32>(star_count_), 1.0f, 0.0f, 0.0f};
  out.shoulder = static_cast<f32>(shoulder.get());
  out.transmittance = transmittance.address;
  out.multiscatter = multiscatter.address;
  out.sky_view = sky_view.address;
  out.aerial = aerial.address;
  out.frame = frame.address;
  out.star_list = star_list.address;
  out.star_cells = star_cells.address;
  out.star_index = star_index.address;
  // The stars are drawn by day too. Skipping them while the sun is up would move a handful of
  // pixels by a level and save under half of what they cost the resolve at 11520 x 2160; the rest
  // is their code's presence in it (docs/experiments/sky-2026-09-30.md, "What it costs").
  out.flags =
      gfx::k_sky_sun_disc | (sky.moon ? gfx::k_sky_moon : 0u) | (sky.stars ? gfx::k_sky_stars : 0u);
  out.view_count = std::min(views.size(), gfx::k_sky_max_views);
  for (u32 v = 0; v < out.view_count; ++v) {
    const View& view = views[v];
    out.views[v].inv_view_proj = inverse(view.view_proj);
    // Radians a pixel spans at the view's centre: the reciprocal of the projection's scale in
    // pixels, cot(fov / 2) * height / 2.
    out.views[v].pixel =
        Vec4{view.proj_scale > 0.0f ? 1.0f / view.proj_scale : 1.0e-3f, 0.0f, 0.0f, 0.0f};
  }
}

}  // namespace engine::renderer
