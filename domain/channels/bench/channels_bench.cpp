// The channels capability's cost a tile (docs/subsystems/channels.md, "What a tile costs"): the
// busiest piece of the gentle test valley's 64 m tiles built into its container, which is what a
// piece's build at a tile's activation costs the host's build thread (the solve and the cluster
// DAGs, without the derived-data cache's read or write), and laying every clast of the region,
// which is what `open` adds to the plan. Measured with `--wait-quiet` (bench.md, "Measuring on a
// shared machine").
#include <domain/channels/channels.h>
#include <domain/channels/falls.h>
#include <foundation/bench/bench.h>

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::channels;

namespace {

// The channels test's gentle valley (0.6 % down its floor, a low wave across it), drained at W93's
// scratch scene's hydraulic geometry, so its trunk is a stream metres wide.
f64 gentle(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  const f64 floor_x = 128 + 6 * std::sin(zz / 40.0);
  return .006 * zz + .04 * std::abs(xx - floor_x) + .2 * std::sin(xx / 13.0) * std::cos(zz / 17.0);
}

struct Valley {
  ChannelLayer layer;
  scene_gen::Ground ground;
  ChannelNetwork network;
  Vector<PlacedPiece> pieces;
  u32 busiest = 0;
  bool ok = false;
};

// The smoke run lays a quarter of the valley: does it run at all, in seconds.
Valley valley() {
  Valley v;
  v.layer.name = "bench";
  v.layer.region_min = {0, 0};
  v.layer.region_max = {256, bench::smoke_mode() ? 128.0 : 512.0};
  v.layer.width_at_km2_m = 14;
  v.layer.depth_at_km2_m = 1.2f;
  v.layer.head_threshold_m2 = 300;
  v.ground.surface_fn = &gentle;
  std::string error;
  if (!plan_channels(v.layer, v.ground, 3, nullptr, v.network, error)) return v;
  if (!place_pieces(v.layer, v.network, v.ground, 64, v.pieces, error) || v.pieces.empty())
    return v;
  for (u32 i = 1; i < v.pieces.size(); ++i)
    if (clast_count(v.pieces[i].recipe) > clast_count(v.pieces[v.busiest].recipe)) v.busiest = i;
  v.ok = true;
  return v;
}

}  // namespace

ENGINE_BENCH(channels_tile_activation, "channels.tile_activation") {
  const Valley v = valley();
  if (!v.ok) return;
  const ChannelPiece& piece = v.pieces[v.busiest].recipe;
  std::string error;
  u64 triangles = 0;
  while (state.keep_running()) {
    geometry::ClusterFileData data;
    build_piece_container(piece, {}, 0, data, error);
    triangles = data.mesh.leaf_triangle_count;
    bench::keep(triangles);
  }
  state.set_items(clast_count(piece));
}

// **The falls' cook** (S104; channels.md, "Where the water leaves its bed (S104)"): a network made
// by hand, so the planner is not in it: 64 straight links (8 in the smoke run) 200 m long in half
// metres, falling 3 %, each with ten cliffs of 2 to 11 m, every one a run the cook marches nine
// columns down. Items are runs: the seconds a fall costs to cook.
ENGINE_BENCH(channels_falls_cook, "channels.falls_cook") {
  ChannelNetwork n;
  const u32 links = bench::smoke_mode() ? 8u : 64u;
  for (u32 k = 0; k < links; ++k) {
    ChannelLink link;
    for (u32 i = 0; i <= 400; ++i) {
      const f64 x = .5 * i;
      f64 down = 0;
      for (u32 c = 0; c < 10; ++c)
        down += x > 10.0 + 18.0 * c + static_cast<f64>(k % 5) ? 2.0 + static_cast<f64>(c) : 0.0;
      ChannelPoint pt;
      pt.at = WorldPos{x, 100 - .03 * x - down, 10.0 * k};
      pt.width_m = 1.2f + .1f * static_cast<f32>(k % 7);
      pt.depth_m = .1f;
      pt.slope = .03f;
      pt.area_m2 = 6e4f;
      link.line.push_back(pt);
      link.bed.push_back(pt.at.y - .12);
      link.reach.push_back(static_cast<u8>(ChannelReach::PoolRiffle));
      link.knick.push_back(0);
    }
    link.keep = link.line.size();
    n.links.push_back(std::move(link));
  }
  ChannelLayer layer;
  layer.name = "bench";
  std::string error;
  u64 runs = 0;
  while (state.keep_running()) {
    FallSet set;
    cook_falls(layer, n, 0, FallOptions{}, nullptr, set, error);
    runs = set.stats.runs;
    bench::keep(set.vertices.size());
  }
  state.set_items(runs);
}

ENGINE_BENCH(channels_lay_region, "channels.lay_region") {
  const Valley v = valley();
  if (!v.ok) return;
  std::string error;
  while (state.keep_running()) {
    Vector<PlacedPiece> pieces;
    place_pieces(v.layer, v.network, v.ground, 64, pieces, error);
    bench::keep(pieces.size());
  }
}
