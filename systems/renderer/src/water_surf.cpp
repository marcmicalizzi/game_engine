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

// **The surf's records: where the field loses wave energy** (S113; scene.h, `SurfRecord`). The
// field's `kept` is the square root of the flux breaking left over the unbroken flux
// (water_flux.h), and its fall along the train's ray is what Battjes and Janssen's bore takes there
// — at the ratio of each wave of the group to 0.78 of the depth the picture draws white water by
// (`water_bore_foam`). A texel's loss is the sea's deep-water flux times `incident` squared times
// what `kept` squared fell from its upwind neighbours (the arrival's gradient weighting the two
// axes, as the flux's own donor-cell scheme does) — over a plane beach a row's losses telescope to
// the flux in less the flux out — and a cell's power is its texels' sum over the cell's width.
void water_surf_records(const SceneWater& water, const gfx::WaterSurface& surface,
                        std::span<const f32> field, f64 cell_m, Vector<SurfRecord>& out) {
  out.clear();
  const SceneWaterDepth& map = water.depth;
  if (water.channel || map.size < 2 || !(cell_m > 0.0)) return;
  const u32 size = map.size;
  const f64 texel = static_cast<f64>(map.texel_m);
  const f64 h0 = static_cast<f64>(surface.height_m);
  const f64 period = static_cast<f64>(surface.period_s);
  // A train to break (a wind's or an open body's run-up; a tide alone is not one).
  if (!(surface.run_up_m > 0.0f) || !(h0 > 0.0) || !(period > 0.0) || !(texel > 0.0) ||
      field.size() < u64{gfx::k_water_field_channels} * size * size)
    return;
  constexpr f64 k_rho = 998.2;
  constexpr f64 k_g = 9.81;
  const f64 flux0 = k_rho * k_g * k_g * h0 * h0 * period / (64.0 * 3.14159265358979323846);
  const f64 k0 = static_cast<f64>(surface.k0);
  const u32 per = std::max(1u, static_cast<u32>(cell_m / texel));
  const u32 cells = (size + per - 1) / per;
  const f64 length = per * texel;
  const auto texel_at = [&](i64 i, i64 j) noexcept {
    return field.data() + u64{gfx::k_water_field_channels} * (u64(j) * size + u64(i));
  };
  const auto wet = [&](i64 a, i64 b) noexcept {
    return a >= 0 && b >= 0 && a < i64{size} && b < i64{size} && texel_at(a, b)[0] > 0.0f;
  };
  const auto kept2 = [&](i64 a, i64 b) noexcept {
    const f64 k = texel_at(a, b)[6] > 0.0f ? static_cast<f64>(texel_at(a, b)[6]) : 1.0;
    return k * k;
  };
  // One axis: the arrival's slope along it (central between water, one-sided beside a wall) and
  // the kept share squared at the neighbour upwind on it (the texel's own where that is not water).
  const auto axis = [&](i64 i, i64 j, i64 di, i64 dj, f64 own, f64& weight, f64& up) noexcept {
    const bool lo = wet(i - di, j - dj);
    const bool hi = wet(i + di, j + dj);
    const f64 here = static_cast<f64>(texel_at(i, j)[1]);
    f64 g = 0.0;
    if (lo && hi)
      g = 0.5 * static_cast<f64>(texel_at(i + di, j + dj)[1] - texel_at(i - di, j - dj)[1]);
    else if (hi)
      g = static_cast<f64>(texel_at(i + di, j + dj)[1]) - here;
    else if (lo)
      g = here - static_cast<f64>(texel_at(i - di, j - dj)[1]);
    weight = std::fabs(g);
    up = own;
    if (g > 0.0 && lo) up = kept2(i - di, j - dj);
    if (g < 0.0 && hi) up = kept2(i + di, j + dj);
  };
  // The power (W) the field loses in texel (i, j), and the wave there before it broke.
  const auto loss_at = [&](i64 i, i64 j, f64& wave_m) noexcept {
    wave_m = 0.0;
    const f32* t = texel_at(i, j);
    const f64 d = static_cast<f64>(t[0]);
    if (!(d > 0.0)) return 0.0;
    const f64 own = kept2(i, j);
    f64 wx = 0.0, ux = own, wz = 0.0, uz = own;
    axis(i, j, 1, 0, own, wx, ux);
    axis(i, j, 0, 1, own, wz, uz);
    const f64 w = wx + wz;
    if (!(w > 0.0)) return 0.0;
    const f64 up = (wx * ux + wz * uz) / w;
    if (!(up > own)) return 0.0;
    const f64 inc = t[5] > 0.0f ? static_cast<f64>(t[5]) : 1.0;
    const f64 depth = std::max(d, gfx::k_water_celerity_floor_m);
    const f64 shoaled = h0 * gfx::water_shoaling(gfx::water_wavenumber(k0, depth), depth) * inc;
    wave_m = std::min(gfx::k_water_breaking_gamma * d, shoaled * std::sqrt(up));
    // D = -dF/ds over the step to the upwind point the two axes' weights name, times the texel's
    // area: a step of one texel along an axis, 1/sqrt(2) of one on the diagonal.
    return flux0 * inc * inc * (up - own) * texel * (w / std::sqrt(wx * wx + wz * wz));
  };
  for (u32 cj = 0; cj < cells; ++cj) {
    for (u32 ci = 0; ci < cells; ++ci) {
      f64 power = 0.0;
      f64 most = 0.0;
      f64 wave = 0.0;
      i64 bi = -1, bj = -1;
      for (u32 j = cj * per; j < std::min(size, (cj + 1) * per); ++j) {
        for (u32 i = ci * per; i < std::min(size, (ci + 1) * per); ++i) {
          f64 wave_m = 0.0;
          const f64 p = loss_at(i, j, wave_m);
          power += p;
          if (p > most) {
            most = p;
            wave = wave_m;
            bi = i;
            bj = j;
          }
        }
      }
      // Silent where the field loses nothing, or less than a millionth (-60 dB) of the sea's
      // deep-water flux across the cell's width: what the healing leaves of rounding.
      if (bi < 0 || !(power > 1e-6 * flux0 * length)) continue;
      const f32* t = texel_at(bi, bj);
      // The shore runs across the way the train arrives: the arrival's gradient by its neighbours.
      const i64 i0 = bi > 0 ? bi - 1 : bi, i1 = bi + 1 < i64{size} ? bi + 1 : bi;
      const i64 j0 = bj > 0 ? bj - 1 : bj, j1 = bj + 1 < i64{size} ? bj + 1 : bj;
      const f64 gx = static_cast<f64>(texel_at(i1, bj)[1] - texel_at(i0, bj)[1]);
      const f64 gz = static_cast<f64>(texel_at(bi, j1)[1] - texel_at(bi, j0)[1]);
      const f64 gl = std::sqrt(gx * gx + gz * gz);
      SurfRecord r;
      r.at = Vec2{static_cast<f32>(static_cast<f64>(map.origin.x) + static_cast<f64>(bi) * texel),
                  static_cast<f32>(static_cast<f64>(map.origin.y) + static_cast<f64>(bj) * texel)};
      r.along = gl > 1e-9 ? Vec2{static_cast<f32>(-gz / gl), static_cast<f32>(gx / gl)}
                          : Vec2{1.0f, 0.0f};
      r.length_m = static_cast<f32>(length);
      r.power_w_m = static_cast<f32>(power / length);
      r.height_m = static_cast<f32>(wave);
      const f64 slope = t[3] > 0.0f ? static_cast<f64>(t[3]) : static_cast<f64>(surface.slope);
      r.surf_similarity =
          static_cast<f32>(gfx::surf_similarity(slope, static_cast<f64>(r.height_m), period));
      r.period_s = static_cast<f32>(period);
      r.arrival_s = t[1];
      out.push_back(r);
    }
  }
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
  water_surf_records(water, surface, {field.data(), field.size()}, cell_m, out);
}

}  // namespace engine::renderer
