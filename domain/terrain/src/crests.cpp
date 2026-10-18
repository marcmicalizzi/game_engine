#include <domain/terrain/crests.h>
#include <domain/terrain/fixed.h>
#include <domain/terrain/wind.h>

namespace engine::terrain {

using namespace fx;

// evaluate_tile's wind and crest lines (tile.cpp), expression for expression, without its grid: the
// sand share is the same count over the same vertices, the crests the same loop over the same
// gather. A change to either side is a change to both; the equality test says when they part.
void tile_crests(const DuneField& field, TileCoord tile, i64 time_us, const TileOptions& options,
                 const LagField* lag, CrestScratch& scratch, Vector<CrestLine>& crests,
                 TileWind& wind) {
  const u32 n = options.cells;
  const i64 s = options.tile_mm / static_cast<i64>(n);
  const i64 x0 = static_cast<i64>(tile.x) * options.tile_mm;
  const i64 z0 = static_cast<i64>(tile.z) * options.tile_mm;
  Gather& gather = scratch.gather;
  field.gather(x0 - s, z0 - s, x0 + options.tile_mm + s, z0 + options.tile_mm + s, time_us, lag,
               gather);

  const u32 v = n + 1;
  u32 sand = 0;
  for (u32 j = 0; j < v; ++j) {
    for (u32 i = 0; i < v; ++i) {
      if (material_at(field, x0 + i * s, z0 + j * s) != 1) ++sand;
    }
  }
  const WindDay& day = field.wind().day(day_of(time_us));
  const WindAt now = field.wind().wind_at(time_us);
  wind.direction = Vec2{static_cast<f32>(cos_q15(now.turn)) / 32768.0f,
                        static_cast<f32>(sin_q15(now.turn)) / 32768.0f};
  wind.speed_mps = static_cast<f32>(now.speed_q16) / 65536.0f * k_mean_wind_mps;
  wind.flux_m2_per_day = static_cast<f32>(now.flux.magnitude * 24) * 1e-4f;
  wind.storm = now.storm;
  wind.saltation_m2_per_day =
      wind.flux_m2_per_day * static_cast<f32>(sand) / static_cast<f32>(v * v);

  crests.clear();
  if (!options.crests) return;
  const i64 wx = field.wind().prevailing_x_q14();
  const i64 wz = field.wind().prevailing_z_q14();
  for (const Primitive& p : gather.primitives) {
    const u32 b = p.band;
    if (options.detail == Detail::coarse && !field.band(b).far) continue;
    CrestLine crest;
    crest.band = p.band;
    crest.kind = p.kind;
    crest.cell_hash = p.cell_hash;
    crest.height_m = height_m(p.height);
    crest.sharpness = static_cast<f32>(p.sharp_q16) / 65536.0f;
    crest.celerity_m_per_day = static_cast<f32>(static_cast<f64>(day.magnitude) * 100.0 /
                                                static_cast<f64>(field.band_height(b)) * 1e-3);
    bool inside = false;
    i64 lee_x = 0;
    i64 lee_z = 0;
    for (u32 k = 0; k < k_crest_points; ++k) {
      i64 qx = 0;
      i64 qz = 0;
      if (p.kind == static_cast<u8>(PrimitiveKind::transverse)) {
        const i64 frac = -58982 + static_cast<i64>(k) * 29491;
        const i64 along = (static_cast<i64>(p.half_length) * frac) >> 16;
        const i64 sa = abs_i64(frac);
        const i64 bend = (p.bend * ((sa * sa) >> 16)) >> 16;
        const i64 side = p.side_q16 >= 0 ? 1 : -1;
        const i64 nx = p.az * side;
        const i64 nz = -p.ax * side;
        qx = p.cx + ((p.ax * along + nx * bend) >> 14);
        qz = p.cz + ((p.az * along + nz * bend) >> 14);
        lee_x = nx;
        lee_z = nz;
      } else {
        const i64 h_mm = p.height / 1000;
        const i64 scoop_r = (h_mm * 7) / 2;
        const i64 scoop_x = scoop_r + (h_mm * 3) / 10;
        const u32 angle = 32768u - 12743u + static_cast<u32>(k) * 6371u;
        const i64 lx = scoop_x + ((scoop_r * cos_q15(angle)) >> 15);
        const i64 ly = (scoop_r * sin_q15(angle)) >> 15;
        qx = p.cx + ((p.ax * lx - p.az * ly) >> 14);
        qz = p.cz + ((p.az * lx + p.ax * ly) >> 14);
        lee_x = p.ax;
        lee_z = p.az;
      }
      i64 x = qx + gather.dx[b];
      i64 z = qz + gather.dz[b];
      const i64 held = (lag != nullptr ? lag->lag_mm(lag_slot(b), x, z) : 0);
      x -= (wx * held) >> 14;
      z -= (wz * held) >> 14;
      if (x >= x0 && x < x0 + options.tile_mm && z >= z0 && z < z0 + options.tile_mm) inside = true;
      crest.points[k] = Vec3{height_m(x * 1000), 0.0f, height_m(z * 1000)};
      crest.points[k].y = height_m(field.height_um(gather, x, z, options.detail));
    }
    if (!inside) continue;
    crest.lee = Vec2{static_cast<f32>(lee_x) / 16384.0f, static_cast<f32>(lee_z) / 16384.0f};
    crests.push_back(crest);
  }
}

}  // namespace engine::terrain
