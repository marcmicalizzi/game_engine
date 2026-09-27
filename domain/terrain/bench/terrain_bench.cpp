// The terrain capability's hot paths (docs/subsystems/terrain.md, "Performance notes"; plan 11
// §11.8; docs/experiments/e36-desert-generator.md):
//
//   terrain.tile.eval       one 32 m tile at the overlay's 25 cm grid (129 x 129 vertices, an apron
//                           round it), heights, normals, materials and crest lines, at a time a few
//                           years in: what the world ring pays per activation. The argument is the
//                           cells a side (32, 64, 128), since a far ring wants a coarser tile.
//   terrain.tile.eval_erg   the same tile of the erg profile (content/test-scenes/desert-erg: five
//                           bands, a mega-draa kilometres long): what the scale spectrum costs.
//   terrain.tile.eval_far   the same tile a thousand years in: the closed form's claim that any t
//                           costs what t = 0 does, measured.
//   terrain.overlay.decay   a tile's overlay advanced by a game hour: trampled (every block holds a
//                           deviation) and settled (a few footprints). What a held tile pays per
//                           advance; nothing when its grid is empty.
//   terrain.wind.integral   the closed-form integral alone.
//   terrain.field.build     a field from its description: the wind record and the bands.
//   terrain.ring.build      one terrain ring of the erg (rings.h) from scratch — every chunk's
//                           heights, mesh, skirts and cluster LOD DAG on the job pool, and the
//                           merge — at the default sizes: the argument is the ring, 0 the inner
//                           (512 m at 50 cm), 1 the middle (2 km at 1 m), 2 the outer (the erg's
//                           6.1 km grid at 1.5 m, with the middle ring's hole). Items: triangles.
//   terrain.ring.recentre   the camera stepping 150 m and back: the inner ring re-centres every
//                           time, keeping the chunks it still has. What a re-centre costs.
//   terrain.field.reevaluate  the erg's whole grid at a new game time on the job pool
//                           (evaluate_grid): 2,049 or 4,097 vertices a side over its 6.1 km, what
//                           one field of a time-lapse costs (renderer's TerrainMotion). A smoke
//                           run takes 129.
//   terrain.field.window    one 257 x 257 window of the erg at its grid's 1.5 m, on one thread,
//                           the window stepping 400 m along the erg's diagonal each iteration: the
//                           per-sample cost of a field at the density the time-lapse evaluates it,
//                           without the pool — what a profile of evaluate_grid reads (the whole
//                           grid at 129 is all gather). A smoke run takes 33.
#include <core/jobs/job_system.h>
#include <domain/terrain/terrain.h>
#include <foundation/bench/bench.h>

using namespace engine;
using namespace engine::terrain;

namespace {

const DuneField& bench_field() {
  static const DuneField field = [] {
    FieldDesc d;
    d.seed = 2026;
    d.wind.seed = 2026;
    return DuneField(d);
  }();
  return field;
}

constexpr i64 k_year = 365 * k_us_per_day;

void eval_tile(bench::State& state, i64 time_us) {
  const DuneField& field = bench_field();
  TileOptions options;
  options.cells = static_cast<u32>(state.arg());
  TileOutput out;
  i32 tile = 0;
  while (state.keep_running()) {
    evaluate_tile(field, TileCoord{tile, 3}, time_us, options, nullptr, nullptr, out);
    bench::keep(out.max_um);
    ++tile;
  }
  state.set_items(static_cast<u64>(options.cells + 1) * (options.cells + 1));
}

void fill_overlay(Overlay& overlay, const WindRecord& wind, const OverlayRules& rules, u32 count) {
  overlay.reset(TileCoord{0, 0}, 32'000, 0);
  for (u32 k = 0; k < count; ++k) {
    Stamp s;
    s.time_us = k;
    s.x_mm = static_cast<i32>((k * 7919u) % 32'000u);
    s.z_mm = static_cast<i32>((k * 104'729u) % 32'000u);
    s.radius_mm = 400;
    s.radius2_mm = 400;
    s.depth_mm = 1'500;  // deep enough to outlast every advance of the run
    s.kind = static_cast<u8>(StampKind::dig);
    (void)overlay.push(s, wind, rules);
  }
  overlay.advance(count, wind, rules);
}

}  // namespace

ENGINE_BENCH_ARGS(terrain_tile_eval, "terrain.tile.eval", 32, 64, 128) {
  eval_tile(state, 3 * k_year + 17 * k_us_per_day);
}

namespace {

const DuneField& erg_field() {
  static const DuneField field = [] {
    const auto band = [](const char* name, PrimitiveKind kind, f32 lo, f32 hi, f32 cell,
                         f32 share) {
      BandMetres m;
      m.name = name;
      m.kind = kind;
      m.height_min = lo;
      m.height_max = hi;
      m.cell = cell;
      m.share = share;
      return m;
    };
    BandMetres mega = band("mega-draa", PrimitiveKind::transverse, 80, 200, 2400, 0.85f);
    mega.length_min = 0.6f;
    mega.length_max = 1.0f;
    mega.stoss = 0.42f;
    mega.bend = 0.12f;
    mega.sinuosity = 0.08f;
    mega.spread_deg = 15;
    mega.side_days = 365;
    mega.sharp_days = 120;
    BandMetres draa = band("draa", PrimitiveKind::transverse, 10, 25, 360, 0.7f);
    draa.length_min = 0.5f;
    draa.length_max = 0.9f;
    draa.bend = 0.18f;
    draa.sinuosity = 0.04f;
    draa.spread_deg = 25;
    draa.couple = BandCouple::flanks;
    draa.couple_width = 400;
    BandMetres crest = band("crest", PrimitiveKind::transverse, 2.5f, 6, 110, 0.6f);
    crest.couple = BandCouple::flanks;
    crest.couple_width = 100;
    BandMetres barchan = band("barchan", PrimitiveKind::barchan, 1.5f, 5, 150, 0.35f);
    barchan.couple = BandCouple::floors;
    barchan.couple_width = 80;
    barchan.far = false;
    BandMetres wave = band("wave", PrimitiveKind::transverse, 0.3f, 0.8f, 10, 0.6f);
    wave.length_min = 0.4f;
    wave.length_max = 0.8f;
    wave.stoss = 0.45f;
    wave.bend = 0.2f;
    wave.spread_deg = 30;
    wave.sharpness = 0;
    wave.side_days = 3;
    wave.sharp_days = 1;
    wave.far = false;
    FieldDesc d;
    d.seed = 7;
    d.wind.seed = 7;
    d.dune_height = 8'000;
    d.wavelength = 110'000;
    for (const BandMetres& m : {mega, draa, crest, barchan, wave})
      d.bands.push_back(band_from_metres(m));
    return DuneField(d);
  }();
  return field;
}

void eval_tile_of(bench::State& state, const DuneField& field, i64 time_us) {
  TileOptions options;
  options.cells = static_cast<u32>(state.arg());
  TileOutput out;
  i32 tile = 0;
  while (state.keep_running()) {
    evaluate_tile(field, TileCoord{tile, 3}, time_us, options, nullptr, nullptr, out);
    bench::keep(out.max_um);
    ++tile;
  }
  state.set_items(static_cast<u64>(options.cells + 1) * (options.cells + 1));
}

}  // namespace

ENGINE_BENCH_ARGS(terrain_tile_eval_erg, "terrain.tile.eval_erg", 32, 64, 128) {
  eval_tile_of(state, erg_field(), 3 * k_year + 17 * k_us_per_day);
}

ENGINE_BENCH_ARGS(terrain_tile_eval_far, "terrain.tile.eval_far", 128) {
  eval_tile(state, 1000 * k_year + 17 * k_us_per_day);
}

// The argument is how many deep stamps the overlay holds: 2,000 touch every block, 20 a few.
ENGINE_BENCH_ARGS(terrain_overlay_decay, "terrain.overlay.decay", 20, 2000) {
  const DuneField& field = bench_field();
  const OverlayRules rules;
  Overlay overlay;
  fill_overlay(overlay, field.wind(), rules, static_cast<u32>(state.arg()));
  i64 t = overlay.as_of_us();
  const i64 hour = k_us_per_day / 24;
  u32 advances = 0;
  while (state.keep_running()) {
    t += hour;
    overlay.advance(t, field.wind(), rules);
    bench::keep(overlay.nonzero_blocks());
    // Keep the grid from emptying: start again every ten game days.
    if (++advances % 240 == 0) {
      fill_overlay(overlay, field.wind(), rules, static_cast<u32>(state.arg()));
      t = overlay.as_of_us();
    }
  }
  state.set_items(k_overlay_cells * k_overlay_cells);
}

ENGINE_BENCH(terrain_wind_integral, "terrain.wind.integral") {
  const WindRecord& wind = bench_field().wind();
  i64 t = 0;
  while (state.keep_running()) {
    t += 7'777'777'777;
    bench::keep(wind.integral(t).magnitude);
  }
  state.set_items(1);
}

// A field from its description: the wind record's 2,920 days and their prefix sums, and the bands.
// What a host pays once per world (and the renderer once per scene read).
ENGINE_BENCH(terrain_field_build, "terrain.field.build") {
  FieldDesc d;
  d.seed = 2026;
  d.wind.seed = 2026;
  u64 seed = 0;
  while (state.keep_running()) {
    d.wind.seed = ++seed;
    const DuneField field(d);
    bench::keep(field.hash());
  }
  state.set_items(k_record_days);
}

namespace {

// The erg's rings at the default sizes — an inner ring 256 m either side at 50 cm, a middle ring
// 1,002 m either side at 1 m (a multiple of lcm(1 m, 1.5 m)), the erg's own 6.1 km grid at 1.5 m —
// round a camera a quarter of a kilometre from the middle, three years in. A smoke run builds rings
// a sixteenth the size.
// Pinned, one worker a CPU: the runner pins its own thread to one CPU (docs/subsystems/bench.md),
// and threads it starts afterwards inherit that mask unless they set their own. Until 2026-09-27
// this pool was unpinned, so every worker shared the runner's CPU and `terrain.ring.*` and
// `terrain.field.reevaluate` measured one core's worth of work in the pool's clothes
// (docs/experiments/time-lapse-smoothness-2026-09-27.md).
jobs::JobSystem& ring_pool() {
  static jobs::JobSystem pool(jobs::JobSystemConfig{});
  return pool;
}

TerrainRings& erg_rings() {
  static const FieldRingHeights heights(erg_field(), 3 * k_year);
  static TerrainRings rings = [] {
    const bool smoke = bench::smoke_mode();
    RingParams params;
    params.count = 3;
    params.ring[0] = RingSpec{smoke ? 16'000 : 256'000, 500, 4'000};
    params.ring[1] = RingSpec{smoke ? 63'000 : 1'002'000, 1'000, 8'000};
    params.ring[2] = RingSpec{smoke ? 192'000 : 3'072'000, 1'500, 12'000};
    params.uv_x0_mm = -params.ring[2].half_mm;
    params.uv_z0_mm = -params.ring[2].half_mm;
    params.uv_size_mm = 2 * params.ring[2].half_mm;
    TerrainRings r;
    r.reset(params, smoke ? 16'000 : 250'000, smoke ? -8'000 : -120'000, heights,
            geometry::ClusterLodOptions{}, &ring_pool(), nullptr);
    return r;
  }();
  return rings;
}

}  // namespace

ENGINE_BENCH_ARGS(terrain_ring_build, "terrain.ring.build", 0, 1, 2) {
  TerrainRings& rings = erg_rings();
  const u32 ring = static_cast<u32>(state.arg());
  while (state.keep_running()) {
    rings.drop_chunks();
    rings.rebuild(1u << ring, &ring_pool());
    bench::keep(rings.hash(ring));
  }
  u64 triangles = 0;
  for (const RingChunk& chunk : rings.chunks(ring))
    triangles += chunk.grid_triangles;
  state.set_items(triangles);
}

// A re-centre: the camera steps 150 m and back, so the inner ring moves every time (its trigger
// is half its half-side, 128 m) and the middle ring never does; what is rebuilt is the inner
// ring's chunks that changed and the middle ring's round its old and new hole.
ENGINE_BENCH(terrain_ring_recentre, "terrain.ring.recentre") {
  TerrainRings& rings = erg_rings();
  const bool smoke = bench::smoke_mode();
  const i64 x0 = rings.layout().ring[0].cx, z0 = rings.layout().ring[0].cz;
  const i64 step = smoke ? 9'000 : 150'000;
  bool out = false;
  u32 built = 0;
  while (state.keep_running()) {
    out = !out;
    u32 rebuilt = 0;
    rings.update(out ? x0 + step : x0, z0, &ring_pool(), rebuilt);
    built += rings.last_built();
    bench::keep(rebuilt);
  }
  bench::keep(built);
}

ENGINE_BENCH_ARGS(terrain_field_reevaluate, "terrain.field.reevaluate", 2049, 4097) {
  const u32 n = bench::smoke_mode() ? 129u : static_cast<u32>(state.arg());
  const i64 spacing = 6'144'000 / (n - 1);
  Vector<i64> heights;
  i64 day = 0;
  while (state.keep_running()) {
    evaluate_grid(erg_field(), -3'072'000, -3'072'000, n, n, spacing,
                  3 * k_year + day * k_us_per_day, Detail::dunes, nullptr, &ring_pool(), heights);
    bench::keep(heights[heights.size() / 2]);
    ++day;
  }
  state.set_items(u64{n} * n);
}

ENGINE_BENCH_ARGS(terrain_field_window, "terrain.field.window", 257) {
  const u32 n = bench::smoke_mode() ? 33u : static_cast<u32>(state.arg());
  Vector<i64> heights;
  u32 step = 0;
  while (state.keep_running()) {
    const i64 at = -2'800'000 + static_cast<i64>(step % 14) * 400'000;
    evaluate_grid(erg_field(), at, at, n, n, 1'500, 3 * k_year + step * k_us_per_day, Detail::dunes,
                  nullptr, nullptr, heights);
    bench::keep(heights[heights.size() / 2]);
    ++step;
  }
  state.set_items(u64{n} * n);
}
