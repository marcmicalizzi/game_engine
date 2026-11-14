// G198: a tile of deposits at the forest's density past the first ring, as its near meshes and as
// its far tile (renderer.md, "The far tile"; rocks.md, "A deposit past the first ring"): what one
// instance of the far tile costs in pairs and mesh room against the meshes it stands for. CPU only.
#include <core/hash/hash.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/rocks/deposits.h>
#include <domain/scene_gen/far_tile.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/renderer/far_tile.h>
#include <systems/renderer/scene.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>

using namespace engine;

namespace {
// The kilometre case's massif foot (rocks' deposits tests).
f64 massif(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 z = static_cast<f64>(z_mm) / 1000.0;
  const f64 x = static_cast<f64>(x_mm) / 1000.0 - 12.0 * std::sin(z / 300.0 * 6.283185307179586);
  const f64 d = std::abs(x);
  auto bump = [&](f64 h, f64 w) {
    return d < w ? h * (0.5 + 0.5 * std::cos(3.14159265358979323846 * d / w)) : 0.0;
  };
  return bump(60, 40) + bump(40, 70) + .02 * x;
}

// What a cluster of the mesh room reserves on a device with ray tracing at the default cluster
// size (gpu_scene_meshes.cpp: the descriptor, its LOD record, its material word, 64 vertices of a
// 16-bit position, the attributes and the float position the structures read, 124 triangles of a
// packed word and three index bytes).
constexpr u64 k_room_cluster_bytes = 48 + 48 + 4 + 64 * (6 + 8 + 12) + 124 * (4 + 3);
f64 mib(u64 clusters) { return static_cast<f64>(clusters * k_room_cluster_bytes) / 1048576.0; }
}  // namespace

TEST_CASE(
    "far tile: a dense tile of deposits is one instance of a sixth of its meshes' clusters "
    "past the first ring") {
  // The forest's 128,000 triangles a 16 m patch at 1 m samples, 96 loose stones a patch, one 32 m
  // tile of the massif's foot: the near ring's meshes built as a host builds them (into the cache),
  // then the far tile from the same entries. About 20 s in debug, every second of it the 16 near
  // builds the comparison is against.
  test::TempDir scratch;
  const std::string dir = scratch.path();
  scene_gen::Context c;
  c.dir = dir;
  c.ddc = dir;
  c.ground.surface_fn = &massif;
  c.tile_size = 32;
  rocks::DepositLayer p;
  p.region_min = {0, 0};
  p.region_max = {96, 32};
  p.spacing_m = 1;
  p.patch_m = 16;
  p.patch.triangle_budget = 128000;
  p.patch.fragments = 96;
  p.sediment_depth_m = 0;
  const auto* desc = scene_gen::GeneratorRegistry::global().find_placement("deposits");
  REQUIRE(desc != nullptr);
  std::string error;
  void* state = nullptr;
  REQUIRE_MESSAGE(desc->open(schema::to_json(p), c, &state, &error), error);
  scene_gen::Placements near, far;
  c.ring = 0;
  REQUIRE_MESSAGE(desc->tile(state, {1, 0}, c, near, &error), error);
  c.ring = 1;
  REQUIRE_MESSAGE(desc->tile(state, {1, 0}, c, far, &error), error);
  desc->close(state);
  REQUIRE(far.meshes.size() == 1);

  renderer::PlacementMeshOptions options;
  options.ddc = dir;
  options.cache = true;
  u64 near_clusters = 0, near_triangles = 0;
  const i64 near_start = time::monotonic_ns();
  for (const auto& mesh : near.meshes) {
    renderer::PlacementMeshData data;
    REQUIRE_MESSAGE(renderer::load_placement_mesh(mesh, options, data, error), error);
    near_clusters += data.lod.mesh.clusters.size();
    near_triangles += data.lod.leaf_triangle_count;
  }
  const f64 near_ms = static_cast<f64>(time::monotonic_ns() - near_start) / 1.0e6;
  renderer::PlacementMeshData tile;
  renderer::FarTileStats stats;
  REQUIRE_MESSAGE(renderer::load_far_tile(far.meshes[0], options, tile, error, &stats), error);
  MESSAGE("near: ", near.meshes.size(), " meshes, ", near_clusters, " clusters (",
          mib(near_clusters), " MiB of room), ", near_triangles, " triangles, built in ", near_ms,
          " ms");
  MESSAGE("far tile: 1 mesh, ", stats.clusters, " clusters (", mib(stats.clusters),
          " MiB of room), ", stats.triangles, " triangles, ", stats.materials, " parts, error ",
          stats.error_m, " m; members ", stats.members_ms, " ms from the cache, built in ",
          stats.build_ms, " ms, ", stats.baked, " far forms baked at load");
  std::string levels;
  for (const u32 count : tile.lod.level_cluster_counts)
    levels += std::to_string(count) + " ";
  MESSAGE("far tile clusters by DAG level: ", levels);
  CHECK(stats.members == near.meshes.size());
  CHECK(stats.materials == 1);
  CHECK(stats.clusters <= 4096);  // a streamed host's limit for one mesh
  // Measured 1,368 against 13,939 (2026-11-13, k_far_form_version 2; 2,261 at version 1, before
  // the far form kept the pieces' cuts and welded its input by its bytes): the leaves are 651
  // clusters, and the far tile's own DAG, which coarsens it across the ring, the rest.
  CHECK(stats.clusters * 5 < near_clusters);
  CHECK(stats.source_triangles <= near_triangles);  // less any the weld made degenerate
  CHECK(stats.source_triangles * 100 >= near_triangles * 99);

  // The same far tile again is read whole from its derived-data entry, no member touched, and is
  // the same bytes.
  renderer::PlacementMeshData again;
  renderer::FarTileStats hit;
  REQUIRE(renderer::load_far_tile(far.meshes[0], options, again, error, &hit));
  CHECK(hit.cache_hit);
  MESSAGE("far tile again: from the cache in ", hit.build_ms, " ms");
  const u64 pin = hash_combine(
      hash_combine(
          hash_bytes(tile.lod.mesh.vertices.data(), tile.lod.mesh.vertices.size() * sizeof(Vec3)),
          hash_bytes(tile.lod.mesh.triangles.data(), tile.lod.mesh.triangles.size() * 4)),
      hash_bytes(tile.lod.mesh.attributes.data(),
                 tile.lod.mesh.attributes.size() * sizeof(geometry::VertexAttributes)));
  MESSAGE("far tile hash ", pin);
  // Taken on MSVC 14.51, 2026-11-13 (k_far_form_version 2, k_far_tile_build_version 2, the default LOD
  // options): the merged far tile's positions, triangles and attributes (the normals the weld
  // carries from the near form), built by one thread in the recipe's order, so the same at any job
  // count.
  CHECK(pin == 3832989870798787416ull);
  REQUIRE(again.lod.mesh.vertices.size() == tile.lod.mesh.vertices.size());
  CHECK(hash_bytes(again.lod.mesh.vertices.data(), again.lod.mesh.vertices.size() * sizeof(Vec3)) ==
        hash_bytes(tile.lod.mesh.vertices.data(), tile.lod.mesh.vertices.size() * sizeof(Vec3)));
  CHECK(hash_bytes(again.lod.mesh.triangles.data(), again.lod.mesh.triangles.size() * 4) ==
        hash_bytes(tile.lod.mesh.triangles.data(), tile.lod.mesh.triangles.size() * 4));
}
