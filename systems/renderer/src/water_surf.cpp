// A body's height function, and where its train breaks for a host's ears (scene.h; S100, S113;
// docs/subsystems/water.md, "The mesh" and "The surf"; world_sound.md, "Running water and the
// surf").

#include <domain/gfx/water_flux.h>
#include <domain/gfx/water_swash.h>
#include <domain/gfx/wind.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_wind.h>

#include <algorithm>
#include <cmath>

namespace engine::renderer {

// **A body's height function** (S100; water_swash.h, `WaterSurface`; water.md, "The mesh"): the
// body's waves (an open body's spectrum, else the material's four sines), the wind's peak wave and
// its run-up, the tide, and the arrival field over the depth map's grid — the depth decoded from
// the map's 16 bits as the GPU's image is, the eikonal's arrival time beside it.
void water_surface_of(const SceneWater& water, const WaterSwashNumbers& n, gfx::WaterSurface& out,
                      Vector<f32>& field) {
  const SceneWaterDepth& map = water.depth;
  out = gfx::WaterSurface{};
  out.origin = map.origin;
  out.texel_m = map.texel_m;
  out.size = map.size;
  const f64 period = gfx::swash_period(n.wave.period_s);
  const f64 two_pi = 6.283185307179586;
  out.height_m = static_cast<f32>(n.wave.height_m);
  out.period_s = static_cast<f32>(period);
  out.k0 = period > 0.0 ? static_cast<f32>((two_pi / period) * (two_pi / period) / gfx::k_swash_g)
                        : 0.0f;
  out.run_up_m = static_cast<f32>(n.run_up);
  out.slope = static_cast<f32>(n.slope);
  out.tide_m = n.tide;
  const f64 l0 = gfx::k_swash_g * period * period / two_pi;
  out.along_m = static_cast<f32>(std::max(2.0 * l0, 4.0));
  const scene::WaterBody& body = water.body;
  if (body.open) {
    out.flags |= gfx::k_water_surface_open;
    out.fade_m = static_cast<f32>(std::clamp(0.5 * l0, 2.0, 30.0));
  }
  out.amplitude = water.shading.wave_amplitude_m;
  out.wavelength = water.shading.wave_length_m;
  out.chop = water.shading.chop;
  out.seed = water.shading.seed;
  out.direction = water.shading.direction;
  out.wind = water.shading.direction;
  out.mesh_far_m = map.mesh_far_m;
  out.mesh_near_m = map.mesh_near_m;
  out.mesh_grade = map.mesh_grade;
  if (water.waves.size() == gfx::k_water_waves) {
    out.flags |= gfx::k_water_surface_spectrum;
    for (u32 i = 0; i < gfx::k_water_waves; ++i)
      out.waves[i] = water.waves[i];
  }
  // S102 (water_swash.h, water_flux.h): the train comes from a direction, each texel carries its
  // own stretch of shore's slope, and since the second pass the field is built in the water's own
  // header — with a second train beside the first (the sea's own second component, or the wind's
  // peak wave beside a swell) and the energy flux's fields, from which a place takes its height.
  out.swell = static_cast<f32>(n.wind_k0);
  out.reach_scale = static_cast<f32>(gfx::swash_reach_scale(n.wave.height_m, n.wave.period_s));
  out.reach_cap =
      static_cast<f32>(gfx::swash_reach_cap(n.wave.height_m, static_cast<f64>(map.bank_max)));
  // W136-2: a varying wind's two trains span its seas (scene_wind.h, `water_cross_of_span`).
  const gfx::WaterCross cross =
      n.wind_k0 > 0.0 ? gfx::water_cross_of_swell(n.wave.height_m, period, n.wind_wave.height_m,
                                                  n.wind_wave.period_s, water.shading.direction)
      : water.scene_wind && water.grid.valid()
          ? water_cross_of_span(n.wave.height_m, period, n.direction)
          : gfx::water_cross_of_sea(n.wave.height_m, period, n.direction, water.shading.seed);
  gfx::WaterFieldDesc desc;
  desc.texels = map.texels.data();
  desc.size = map.size;
  desc.texel_m = static_cast<f64>(map.texel_m);
  desc.depth_max = static_cast<f64>(map.depth_max);
  desc.bank_max = static_cast<f64>(map.bank_max);
  desc.period_s = period;
  desc.direction = n.direction;
  desc.fetch_m = body.open ? 0.0 : n.fetch_m;
  desc.wind = out.wind;
  gfx::water_surface_field(desc, cross, out, field);
}

void water_surf_records(const SceneWater& water, f64 cell_m, Vector<SurfRecord>& out) {
  out.clear();
  const SceneWaterDepth& map = water.depth;
  if (water.channel || map.size < 2 || !(cell_m > 0.0)) return;
  const WaterSwashNumbers n = water_swash_numbers(water);
  // The GPU scene's own test (a body it lifts): a wind's train or an open body's tide; and a train
  // to break, which a tide alone is not.
  if (!(n.run_up > 0.0) || !(n.wave.height_m > 0.0)) return;
  gfx::WaterSurface surface;
  Vector<f32> field;
  water_surface_of(water, n, surface, field);
  const u32 size = map.size;
  const f64 texel = static_cast<f64>(map.texel_m);
  const f64 h0 = static_cast<f64>(surface.height_m);
  const f64 period = static_cast<f64>(surface.period_s);
  if (!(h0 > 0.0) || !(period > 0.0) ||
      field.size() < u64{gfx::k_water_field_channels} * size * size)
    return;
  constexpr f64 k_rho = 998.2;
  constexpr f64 k_g = 9.81;
  const f64 flux0 = k_rho * k_g * k_g * h0 * h0 * period / (64.0 * 3.14159265358979323846);
  const u32 per = std::max(1u, static_cast<u32>(cell_m / texel));
  const u32 cells = (size + per - 1) / per;
  const auto texel_at = [&](u32 i, u32 j) noexcept {
    return field.data() + u64{gfx::k_water_field_channels} * (u64{j} * size + i);
  };
  for (u32 cj = 0; cj < cells; ++cj) {
    for (u32 ci = 0; ci < cells; ++ci) {
      // The outermost texel of the cell where the wave that reaches it stands at the limit.
      f64 best_d = 0.0;
      u32 bi = ~0u, bj = ~0u;
      for (u32 j = cj * per; j < std::min(size, (cj + 1) * per); ++j) {
        for (u32 i = ci * per; i < std::min(size, (ci + 1) * per); ++i) {
          const f32* t = texel_at(i, j);
          const f64 d = static_cast<f64>(t[0]);
          if (!(d > 0.0)) continue;
          const f64 inc = t[5] > 0.0f ? static_cast<f64>(t[5]) : 1.0;
          if (h0 * inc < gfx::k_water_breaking_gamma * d || d <= best_d) continue;
          best_d = d;
          bi = i;
          bj = j;
        }
      }
      if (bi == ~0u) continue;
      const f32* t = texel_at(bi, bj);
      const f64 inc = t[5] > 0.0f ? static_cast<f64>(t[5]) : 1.0;
      // The shore runs across the way the train arrives: the arrival's gradient by its neighbours.
      const u32 i0 = bi > 0 ? bi - 1 : bi, i1 = bi + 1 < size ? bi + 1 : bi;
      const u32 j0 = bj > 0 ? bj - 1 : bj, j1 = bj + 1 < size ? bj + 1 : bj;
      const f64 gx = static_cast<f64>(texel_at(i1, bj)[1] - texel_at(i0, bj)[1]);
      const f64 gz = static_cast<f64>(texel_at(bi, j1)[1] - texel_at(bi, j0)[1]);
      const f64 gl = std::sqrt(gx * gx + gz * gz);
      SurfRecord r;
      r.at = Vec2{static_cast<f32>(static_cast<f64>(map.origin.x) + bi * texel),
                  static_cast<f32>(static_cast<f64>(map.origin.y) + bj * texel)};
      r.along = gl > 1e-9 ? Vec2{static_cast<f32>(-gz / gl), static_cast<f32>(gx / gl)}
                          : Vec2{1.0f, 0.0f};
      r.length_m = static_cast<f32>(per * texel);
      r.power_w_m = static_cast<f32>(flux0 * inc * inc);
      r.height_m = static_cast<f32>(gfx::k_water_breaking_gamma * best_d);
      const f64 slope = t[3] > 0.0f ? static_cast<f64>(t[3]) : static_cast<f64>(surface.slope);
      r.surf_similarity =
          static_cast<f32>(gfx::surf_similarity(slope, static_cast<f64>(r.height_m), period));
      r.period_s = static_cast<f32>(period);
      r.arrival_s = t[1];
      out.push_back(r);
    }
  }
}

}  // namespace engine::renderer
