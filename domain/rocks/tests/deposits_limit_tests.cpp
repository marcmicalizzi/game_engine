// G191: no mesh a tile of deposits hands is over a streamed host's cluster limit, at the forest's
// density (docs/subsystems/rocks.md, "A patch under the cluster limit"). This file names nothing
// G191's review added to the module's header, so the same case runs against the branch before it
// (where a dense patch was one mesh, over the limit) and fails there.
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/rocks/deposits.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>

using namespace engine;
using namespace engine::rocks;
namespace {
// The kilometre case's massif foot (deposits_rest_tests.cpp): a 60 m ridge 40 m across over a
// 40 m skirt 70 m across, its line wandering 12 m over 300 m along z.
f64 massif(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 z = static_cast<f64>(z_mm) / 1000.0;
  const f64 x = static_cast<f64>(x_mm) / 1000.0 - 12.0 * std::sin(z / 300.0 * 6.283185307179586);
  const f64 d = std::abs(x);
  auto bump = [&](f64 h, f64 w) {
    return d < w ? h * (0.5 + 0.5 * std::cos(3.14159265358979323846 * d / w)) : 0.0;
  };
  return bump(60, 40) + bump(40, 70) + .02 * x;
}
}  // namespace

TEST_CASE("deposits: no mesh a tile hands at the forest's density is over a streamed host's "
          "cluster limit") {
  // A streamed host refuses a tile's mesh over `renderer.meshes.max_clusters` (4,096) by name at
  // the walk, and the tile draws the rest without it: a hole in the talus that appears as a
  // walker comes near. The forest's 128,000 triangles a 16 m patch at 1 m samples, with 160 loose
  // stones a patch (the forest's 96, and more, within what the generator's open admitted before
  // G191's review: a budget and blocks under 160,000 triangles together); the tile over the
  // massif's foot. Every mesh the tile hands is built as a host builds it.
  test::TempDir scratch;
  const std::string dir = scratch.path();
  scene_gen::Context c;
  c.dir = dir;
  c.ddc = dir;
  c.ground.surface_fn = &massif;
  c.tile_size = 32;
  DepositLayer p;
  p.region_min = {0, 0};
  p.region_max = {96, 32};
  p.spacing_m = 1;
  p.patch_m = 16;
  p.patch.triangle_budget = 128000;
  p.patch.fragments = 160;
  p.sediment_depth_m = 0;
  const auto* desc = scene_gen::GeneratorRegistry::global().find_placement("deposits");
  REQUIRE(desc != nullptr);
  std::string error;
  void* state = nullptr;
  REQUIRE_MESSAGE(desc->open(schema::to_json(p), c, &state, &error), error);
  scene_gen::Placements tile;
  c.ring = 0;
  REQUIRE_MESSAGE(desc->tile(state, {1, 0}, c, tile, &error), error);
  desc->close(state);
  REQUIRE_FALSE(tile.meshes.empty());
  u64 most = 0, triangles = 0, blocks = 0, fragments = 0;
  for (const auto& m : tile.meshes) {
    JsonValue json;
    REQUIRE(parse_json(m.recipe, json).ok);
    DepositPatch patch;
    schema::ReadContext ctx;
    REQUIRE(schema::from_json(patch, json, ctx));
    geometry::ClusterFileData data;
    REQUIRE_MESSAGE(build_deposit_container(patch, {}, 0, data, error), error);
    const u64 clusters = data.mesh.mesh.clusters.size();
    MESSAGE("a mesh of ", patch.width, " by ", patch.depth, " samples, ", patch.fragments,
            " loose stones and ", patch.blocks.size(), " blocks: ", clusters, " clusters, ",
            data.mesh.leaf_triangle_count, " triangles at the finest form");
    CHECK(clusters <= 4096);
    most = std::max(most, clusters);
    triangles += data.mesh.leaf_triangle_count;
    blocks += patch.blocks.size();
    fragments += patch.fragments;
  }
  MESSAGE(tile.meshes.size(), " meshes, ", triangles, " triangles at the finest form, ", fragments,
          " loose stones and ", blocks, " blocks; the most clusters a mesh ", most);
}
