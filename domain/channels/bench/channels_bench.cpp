// The channels capability's cost a tile (docs/subsystems/channels.md, "What a tile costs"): the
// busiest piece of the gentle test valley's 64 m tiles built into its container, which is what a
// piece's build at a tile's activation costs the host's build thread (the solve and the cluster
// DAGs, without the derived-data cache's read or write), and laying every clast of the region,
// which is what `open` adds to the plan. Measured with `--wait-quiet` (bench.md, "Measuring on a
// shared machine").
#include <domain/channels/channels.h>
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
