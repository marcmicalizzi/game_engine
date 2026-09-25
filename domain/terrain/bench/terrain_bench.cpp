// The terrain capability's hot paths (docs/subsystems/terrain.md, "Performance notes"; plan 11
// §11.8; docs/experiments/e36-desert-generator.md):
//
//   terrain.tile.eval       one 32 m tile at the overlay's 25 cm grid (129 x 129 vertices, an apron
//                           round it), heights, normals, materials and crest lines, at a time a few
//                           years in: what the world ring pays per activation. The argument is the
//                           cells a side (32, 64, 128), since a far ring wants a coarser tile.
//   terrain.tile.eval_far   the same tile a thousand years in: the closed form's claim that any t
//                           costs what t = 0 does, measured.
//   terrain.overlay.decay   a tile's overlay advanced by a game hour: trampled (every block holds a
//                           deviation) and settled (a few footprints). What a held tile pays per
//                           advance; nothing when its grid is empty.
//   terrain.wind.integral   the closed-form integral alone.
//   terrain.field.build     a field from its description: the wind record and the bands.
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
