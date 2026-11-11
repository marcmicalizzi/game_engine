// What a scatter tile costs with and without the site's rules (scatter.md, "Kinds by site"; G144):
// one 32 m tile of a conifer kind at a 3 m spacing on a 31° valley side, unruled, then with an
// elevation limit, its aspect shift and krummholz band, a heat load and a distance to water. The
// difference is what the rules add to a tile's activation. Items are tiles.
#include <domain/scene_gen/scene_gen.h>
#include <foundation/bench/bench.h>
#include <systems/scatter/scatter.h>

using namespace engine;
using namespace engine::scatter;

namespace {

f64 valley(const void*, i64, i64 z_mm) noexcept {
  const f64 z = static_cast<f64>(z_mm) / 1000.0;
  return 0.6 * (z < 0.0 ? -z : z);
}

const scene_gen::Site& site() {
  static const scene_gen::Site s = [] {
    scene::WaterChannel channel;
    channel.points.push_back(WorldPos{-2000.0, 0.0, 0.0});
    channel.points.push_back(WorldPos{2000.0, 0.0, 0.0});
    channel.width_m.push_back(4.0f);
    channel.width_m.push_back(4.0f);
    scene_gen::Site out;
    out.build(46.5, {}, {&channel, 1});
    return out;
  }();
  return s;
}

void run(bench::State& state, bool ruled) {
  Layer layer;
  layer.seed = 5;
  layer.density.per_m2 = 0.06;
  Kind kind;
  kind.name = "conifer";
  kind.mesh = "conifer.glb";
  kind.spacing = 3.0f;
  kind.scale_min = 0.8f;
  kind.scale_max = 1.2f;
  if (ruled) {
    Response limit;
    limit.max = 60.0;
    limit.soft = 20.0;
    kind.elevation = limit;
    kind.aspect_shift_m = 40.0f;
    kind.krummholz_m = 30.0f;
    kind.krummholz_scale = 0.2f;
    Response cool;
    cool.max = 1.3;
    cool.soft = 0.2;
    kind.heat_load = cool;
    Response water;
    water.max = 100.0;
    water.soft = 20.0;
    kind.water_m = water;
  }
  layer.kinds.push_back(kind);
  Placer placer;
  std::string error;
  if (!placer.open(layer, std::string(), 7, &error)) return;
  Site s;
  s.ground.surface_fn = &valley;
  s.site = &site();
  Vector<Point> out;
  i32 tile = 0;
  state.set_items(1);
  while (state.keep_running()) {
    out.clear();
    // Tiles up the valley side, 32 m to 96 m from the stream: within the band and the reach.
    placer.place_tile(scene_gen::TileCoord{tile % 16, 1 + (tile / 16) % 2}, s, ~0u, out);
    ++tile;
    bench::keep(out.size());
  }
}

}  // namespace

ENGINE_BENCH(tile_unruled, "scatter.tile.unruled") { run(state, false); }
ENGINE_BENCH(tile_ruled, "scatter.tile.ruled") { run(state, true); }
