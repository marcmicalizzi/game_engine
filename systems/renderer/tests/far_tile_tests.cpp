// The far tile (far_tile.h; renderer.md, "The far tile"; G198): a tile mesh that is its members'
// far forms, one rigid mesh. CPU only: no device.
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/far_form.h>
#include <domain/scene_gen/far_tile.h>
#include <systems/renderer/far_tile.h>
#include <systems/renderer/scene.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>

using namespace engine;

namespace {

// An imported boulder field's mesh as a container on disk, with no generator behind it: a 16 m
// sheet of 0.25 m quads and a few loose cubes on it.
geometry::ClusterFileData boulders() {
  Vector<Vec3> positions;
  Vector<Vec2> uvs;
  Vector<u32> indices;
  constexpr u32 n = 65;
  for (u32 z = 0; z < n; ++z)
    for (u32 x = 0; x < n; ++x) {
      const f32 px = static_cast<f32>(x) * 0.25f, pz = static_cast<f32>(z) * 0.25f;
      positions.push_back(Vec3{px, 0.3f * std::sin(px * 0.7f) * std::cos(pz * 0.5f), pz});
      uvs.push_back(Vec2{px / 16.0f, pz / 16.0f});
    }
  for (u32 z = 0; z + 1 < n; ++z)
    for (u32 x = 0; x + 1 < n; ++x) {
      const u32 a = z * n + x, b = a + 1, c = a + n, d = c + 1;
      for (const u32 i : {a, c, b, b, c, d})
        indices.push_back(i);
    }
  for (u32 k = 0; k < 8; ++k) {
    const u32 base = positions.size();
    const f32 side = k % 2 == 0 ? 0.05f : 0.8f;
    const Vec3 at{1.0f + 1.8f * static_cast<f32>(k), 0.4f, 8.0f};
    for (u32 v = 0; v < 8; ++v) {
      positions.push_back(Vec3{at.x + ((v & 1u) ? side : 0.0f), at.y + ((v & 2u) ? side : 0.0f),
                               at.z + ((v & 4u) ? side : 0.0f)});
      uvs.push_back(Vec2{0.5f, 0.5f});
    }
    const u32 faces[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                              {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
    for (const auto& t : faces)
      for (u32 c = 0; c < 3; ++c)
        indices.push_back(base + t[c]);
  }
  Vector<Vec3> normals;
  geometry::compute_vertex_normals(positions, indices, normals);
  geometry::ClusterFileData data;
  geometry::AttributeSource attributes;
  attributes.normals = std::span<const Vec3>(normals.data(), normals.size());
  attributes.uvs = std::span<const Vec2>(uvs.data(), uvs.size());
  std::string error;
  REQUIRE_MESSAGE(geometry::build_cluster_lod(positions, indices, geometry::ClusterLodOptions{},
                                              data.mesh, &error, attributes),
                  error);
  data.cluster_material.assign(data.mesh.mesh.clusters.size(), 0u);
  geometry::ClusterFileMaterial grey;
  grey.base_color = Vec4{0.4f, 0.38f, 0.35f, 1.0f};
  grey.roughness = 0.9f;
  grey.metallic = 0.0f;
  data.materials.push_back(grey);
  return data;
}

}  // namespace

TEST_CASE("far tile: an imported mesh's far forms, placed three times, are one rigid mesh") {
  test::TempDir scratch;
  const std::string path = scratch.path() + "/boulders.clusters";
  geometry::ClusterFileData data = boulders();
  std::string error;
  REQUIRE_MESSAGE(geometry::write_cluster_file(path, data, &error), error);
  REQUIRE(geometry::ensure_far_form(data));  // what the far tile bakes, for the counts below
  const geometry::FarFormLevel& level = data.far_form.levels[0];

  scene_gen::FarTile tile;
  tile.level = 0;
  tile.meshes.push_back(scene_gen::PlacementMesh(path, "boulders"));
  const i64 places[3][3] = {{0, 0, 0}, {16000, 0, 0}, {0, 500, 16000}};
  for (const auto& at : places) {
    scene_gen::FarTilePlacement p;
    p.x_mm = at[0];
    p.y_mm = at[1];
    p.z_mm = at[2];
    tile.placements.push_back(p);
  }
  tile.placements[2].rotation = quat_from_axis_angle(Vec3::unit_y(), 0.5f);
  scene_gen::PlacementMesh mesh;
  REQUIRE_MESSAGE(scene_gen::far_tile_mesh(tile, scratch.path(), 40.0f, mesh, &error), error);
  CHECK(scene_gen::is_far_tile(mesh));

  renderer::PlacementMeshOptions options;
  renderer::PlacementMeshData far;
  renderer::FarTileStats stats;
  REQUIRE_MESSAGE(renderer::load_far_tile(mesh, options, far, error, &stats), error);
  MESSAGE("far tile: ", stats.triangles, " triangles of ", stats.source_triangles, ", ",
          stats.clusters, " clusters against ", 3 * data.mesh.mesh.clusters.size(),
          " for the three meshes; error ", stats.error_m, " m");
  CHECK(stats.members == 1);
  CHECK(stats.placements == 3);
  CHECK(stats.baked == 1);  // the file carried none
  CHECK(stats.materials == 1);
  CHECK(stats.triangles == 3 * level.triangles);
  CHECK(stats.clusters * 4 < 3 * data.mesh.mesh.clusters.size());
  CHECK(far.lod.mesh.clusters.size() == stats.clusters);
  CHECK(far.source.part_of_cluster.size() == stats.clusters);
  REQUIRE(far.source.data.materials.size() == 1);
  CHECK(far.source.data.materials[0].roughness == doctest::Approx(0.9f));
  CHECK(far.reach > 16.0f);

  // Through the tile mesh's one door, the same mesh.
  renderer::PlacementMeshData through;
  REQUIRE_MESSAGE(renderer::load_placement_mesh(mesh, options, through, error), error);
  CHECK(through.lod.mesh.clusters.size() == far.lod.mesh.clusters.size());
  CHECK(through.lod.mesh.vertices.size() == far.lod.mesh.vertices.size());

  // A container that carries the far form draws the same far tile without baking it.
  REQUIRE_MESSAGE(geometry::write_cluster_file(path, data, &error), error);
  renderer::PlacementMeshData carried;
  renderer::FarTileStats carried_stats;
  REQUIRE_MESSAGE(renderer::load_far_tile(mesh, options, carried, error, &carried_stats), error);
  CHECK(carried_stats.baked == 0);
  REQUIRE(carried.lod.mesh.vertices.size() == far.lod.mesh.vertices.size());
  bool same = carried.lod.mesh.triangles.size() == far.lod.mesh.triangles.size();
  for (u32 v = 0; same && v < far.lod.mesh.vertices.size(); ++v)
    same = carried.lod.mesh.vertices[v].x == far.lod.mesh.vertices[v].x &&
           carried.lod.mesh.vertices[v].y == far.lod.mesh.vertices[v].y &&
           carried.lod.mesh.vertices[v].z == far.lod.mesh.vertices[v].z;
  CHECK(same);
}
