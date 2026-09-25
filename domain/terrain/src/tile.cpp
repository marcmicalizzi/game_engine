#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <domain/terrain/fixed.h>
#include <domain/terrain/overlay.h>
#include <domain/terrain/tile.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace engine::terrain {

namespace {

using namespace fx;

u64 mix_u32(u64 h, u32 v) noexcept { return hash_combine(h, v); }

// The overlay deviation at a global vertex of the grid the overlays share: the tile that owns the
// vertex is found from the vertex, and its overlay read if it is one of the 3 x 3 loaded.
i64 deviation_um(const Deformation& d, TileCoord centre, i64 x, i64 z, i64 tile_mm) noexcept {
  const i64 tx = floor_div(x, tile_mm);
  const i64 tz = floor_div(z, tile_mm);
  const i64 ox = tx - centre.x;
  const i64 oz = tz - centre.z;
  if (ox < -1 || ox > 1 || oz < -1 || oz > 1) return 0;
  const Overlay* overlay = d.overlays[oz + 1][ox + 1];
  if (overlay == nullptr) return 0;
  return static_cast<i64>(overlay->deviation_mm(x, z)) * 1000;
}

i64 drifts_um(std::span<const DriftDecl> drifts, i64 x, i64 z) noexcept {
  i64 best = 0;
  for (const DriftDecl& drift : drifts)
    best = max_i64(best, drift_height_um(drift, x, z));
  return best;
}

}  // namespace

Vec3 grid_normal(i64 west_um, i64 east_um, i64 south_um, i64 north_um, i64 spacing_mm) noexcept {
  // Heights in µm against a run in mm: scale the run to µm so the three components share a unit.
  const f32 nx = static_cast<f32>(west_um - east_um);
  const f32 ny = static_cast<f32>(2 * spacing_mm * 1000);
  const f32 nz = static_cast<f32>(south_um - north_um);
  const f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
  return Vec3{nx / len, ny / len, nz / len};
}

u8 material_at(const DuneField& field, i64 x, i64 z) noexcept {
  // renderer::terrain_material's thresholds: the basin's floor past 0.55, basin sand past 0.15,
  // rock where a ridge is more than 0.3 of the ground.
  const i32 basin = field.basin_q16(x, z);
  if (basin > 36045) return 3;
  if (basin > 9830) return 2;
  return field.ridge_q16(x, z) > 19661 ? 1 : 0;
}

u64 TileOutput::hash() const noexcept {
  u64 h = hash_bytes("engine.terrain.tile", 19);
  h = mix_u32(h, static_cast<u32>(tile.x));
  h = mix_u32(h, static_cast<u32>(tile.z));
  h = hash_combine(h, static_cast<u64>(time_us));
  h = mix_u32(h, cells);
  for (const i32 v : height_um)
    h = mix_u32(h, static_cast<u32>(v));
  for (const u8 m : material)
    h = mix_u32(h, m);
  for (const Vec3& n : normals) {
    h = mix_u32(h, std::bit_cast<u32>(n.x));
    h = mix_u32(h, std::bit_cast<u32>(n.y));
    h = mix_u32(h, std::bit_cast<u32>(n.z));
  }
  return h;
}

void evaluate_tile(const DuneField& field, TileCoord tile, i64 time_us, const TileOptions& options,
                   const LagField* lag, const Deformation* deformation, TileOutput& out) {
  const u32 n = options.cells;
  const i64 s = options.tile_mm / static_cast<i64>(n);
  const i64 x0 = static_cast<i64>(tile.x) * options.tile_mm;
  const i64 z0 = static_cast<i64>(tile.z) * options.tile_mm;
  out.tile = tile;
  out.time_us = time_us;
  out.tile_mm = options.tile_mm;
  out.cells = n;
  out.detail = options.detail;

  Gather gather;
  field.gather(x0 - s, z0 - s, x0 + options.tile_mm + s, z0 + options.tile_mm + s, time_us, lag,
               gather);
  out.primitives = gather.primitives.size();

  // Heights over the tile and a one-vertex apron round it, so the edge's normals are central
  // differences like every other vertex's and agree with the neighbour's.
  const u32 a = n + 3;
  Vector<i64> apron;
  apron.resize(a * a);
  for (u32 j = 0; j < a; ++j) {
    const i64 z = z0 + (static_cast<i64>(j) - 1) * s;
    for (u32 i = 0; i < a; ++i) {
      const i64 x = x0 + (static_cast<i64>(i) - 1) * s;
      i64 h = field.height_um(gather, x, z, options.detail);
      if (deformation != nullptr) {
        h += drifts_um(deformation->drifts, x, z);
        h += deviation_um(*deformation, tile, x, z, options.tile_mm);
      }
      apron[j * a + i] = h;
    }
  }

  const u32 v = n + 1;
  out.height_um.resize(v * v);
  out.material.resize(v * v);
  out.normals.clear();
  if (options.normals) out.normals.resize(v * v);
  i64 lo = std::numeric_limits<i64>::max();
  i64 hi = std::numeric_limits<i64>::min();
  i64 sum = 0;
  u32 sand = 0;
  for (u32 j = 0; j < v; ++j) {
    for (u32 i = 0; i < v; ++i) {
      const i64 h = apron[(j + 1) * a + (i + 1)];
      out.height_um[j * v + i] = static_cast<i32>(h);
      lo = min_i64(lo, h);
      hi = max_i64(hi, h);
      sum += h;
      const u8 m = material_at(field, x0 + i * s, z0 + j * s);
      out.material[j * v + i] = m;
      if (m != 1) ++sand;
      if (options.normals) {
        out.normals[j * v + i] = grid_normal(apron[(j + 1) * a + i], apron[(j + 1) * a + i + 2],
                                             apron[j * a + i + 1], apron[(j + 2) * a + i + 1], s);
      }
    }
  }
  out.min_um = static_cast<i32>(lo);
  out.max_um = static_cast<i32>(hi);
  out.mean_um = sum / static_cast<i64>(v * v);
  out.sand_vertices = sand;

  // The day's wind, and the sand it moves across this tile.
  const WindDay& day = field.wind().day(day_of(time_us));
  out.wind.direction = Vec2{static_cast<f32>(cos_q15(day.turn)) / 32768.0f,
                            static_cast<f32>(sin_q15(day.turn)) / 32768.0f};
  out.wind.speed_mps = static_cast<f32>(day.speed_q16) / 65536.0f * k_mean_wind_mps;
  out.wind.flux_m2_per_day = static_cast<f32>(day.magnitude) * 1e-4f;
  out.wind.saltation_m2_per_day =
      out.wind.flux_m2_per_day * static_cast<f32>(sand) / static_cast<f32>(v * v);

  // Crest lines: every gathered primitive whose crest crosses the tile.
  out.crests.clear();
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
        // Five points along the crest, bent downwind at its ends on the slip face's side.
        const i64 frac = -58982 + static_cast<i64>(k) * 29491;  // -0.9 .. 0.9 of the half length
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
        // A barchan's brink: the upwind arc of its scoop, from horn to horn.
        const i64 h_mm = p.height / 1000;
        const i64 scoop_r = (h_mm * 7) / 2;
        const i64 scoop_x = scoop_r + (h_mm * 3) / 10;
        const u32 angle = 32768u - 12743u + static_cast<u32>(k) * 6371u;  // 180 +- 70 degrees
        const i64 lx = scoop_x + ((scoop_r * cos_q15(angle)) >> 15);
        const i64 ly = (scoop_r * sin_q15(angle)) >> 15;
        qx = p.cx + ((p.ax * lx - p.az * ly) >> 14);
        qz = p.cz + ((p.az * lx + p.ax * ly) >> 14);
        lee_x = p.ax;
        lee_z = p.az;
      }
      // Back to the world: the band's displacement, less the lag the field reads at the point.
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
    out.crests.push_back(crest);
  }
}

void evaluate_tiles(const DuneField& field, std::span<const TileCoord> tiles, i64 time_us,
                    const TileOptions& options, const LagField* lag, jobs::JobSystem* jobs,
                    Vector<TileOutput>& out) {
  out.clear();
  out.resize(static_cast<u32>(tiles.size()));
  const auto run = [&](u32 begin, u32 end) {
    for (u32 t = begin; t < end; ++t)
      evaluate_tile(field, tiles[t], time_us, options, lag, nullptr, out[t]);
  };
  if (jobs == nullptr || tiles.size() < 2) {
    run(0, static_cast<u32>(tiles.size()));
    return;
  }
  jobs->parallel_for(jobs::Pool::Performance, static_cast<u32>(tiles.size()), 1,
                     [&](u32 begin, u32 end) { run(begin, end); });
}

void build_tile_mesh(const TileOutput& tile, Vector<Vec3>& positions, Vector<Vec3>& normals,
                     Vector<u32>& indices) {
  const u32 n = tile.cells;
  const u32 v = n + 1;
  const i64 s = tile.tile_mm / static_cast<i64>(n);
  const i64 x0 = static_cast<i64>(tile.tile.x) * tile.tile_mm;
  const i64 z0 = static_cast<i64>(tile.tile.z) * tile.tile_mm;
  positions.resize(v * v);
  for (u32 j = 0; j < v; ++j) {
    for (u32 i = 0; i < v; ++i) {
      positions[j * v + i] =
          Vec3{height_m((x0 + i * s) * 1000), height_m(tile.height_um[j * v + i]),
               height_m((z0 + j * s) * 1000)};
    }
  }
  normals = tile.normals;
  indices.clear();
  indices.reserve(n * n * 6);
  for (u32 j = 0; j < n; ++j) {
    for (u32 i = 0; i < n; ++i) {
      const u32 k = j * v + i;
      indices.push_back(k);
      indices.push_back(k + v);
      indices.push_back(k + 1);
      indices.push_back(k + 1);
      indices.push_back(k + v);
      indices.push_back(k + v + 1);
    }
  }
}

TileSampler::TileSampler(const DuneField& field, i64 time_us, const TileOptions& options,
                         const LagField* lag)
    : field_(&field), time_us_(time_us), options_(options), lag_(lag) {}

TileCoord TileSampler::tile_at(i64 x, i64 z) const noexcept {
  return TileCoord{static_cast<i32>(floor_div(x, options_.tile_mm)),
                   static_cast<i32>(floor_div(z, options_.tile_mm))};
}

const Gather& TileSampler::gather_for(TileCoord tile) {
  if (!has_cache_ || !(cached_ == tile)) {
    const i64 s = options_.tile_mm / static_cast<i64>(options_.cells);
    const i64 x0 = static_cast<i64>(tile.x) * options_.tile_mm;
    const i64 z0 = static_cast<i64>(tile.z) * options_.tile_mm;
    field_->gather(x0 - s, z0 - s, x0 + options_.tile_mm + s, z0 + options_.tile_mm + s, time_us_,
                   lag_, gather_);
    cached_ = tile;
    has_cache_ = true;
  }
  return gather_;
}

i64 TileSampler::height_um(i64 x, i64 z) {
  return field_->height_um(gather_for(tile_at(x, z)), x, z, options_.detail);
}

Vec3 TileSampler::normal(f32 xf, f32 zf) {
  const i64 x = to_mm(xf);
  const i64 z = to_mm(zf);
  const i64 s = options_.tile_mm / static_cast<i64>(options_.cells);
  const Gather& g = gather_for(tile_at(x, z));
  return grid_normal(field_->height_um(g, x - s, z, options_.detail),
                     field_->height_um(g, x + s, z, options_.detail),
                     field_->height_um(g, x, z - s, options_.detail),
                     field_->height_um(g, x, z + s, options_.detail), s);
}

}  // namespace engine::terrain
