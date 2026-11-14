// G198 path 1 (renderer.md, "The far tile"; ring_forms.h, `OfferedForm`): a resident mesh a
// streamed entry places is offered its far form, and the rings' rule draws it where it serves and
// costs fewer pairs than the DAG's cut. An imported boulder field with no generator behind it: a
// 16 m sheet strewn with 300 four-centimetre stones, which a DAG cannot remove and a far form
// leaves out. Uses `ring_forms_tests.cpp`'s placement generator "ring-forms-field".
#include <domain/geometry/cluster_file.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/ring_forms.h>
#include <systems/renderer/scene.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <filesystem>
#include <string>

using namespace engine;

namespace {

geometry::ClusterFileData strewn_sheet() {
  Vector<Vec3> positions;
  Vector<Vec2> uvs;
  Vector<u32> indices;
  constexpr u32 n = 65;
  for (u32 z = 0; z < n; ++z)
    for (u32 x = 0; x < n; ++x) {
      const f32 px = static_cast<f32>(x) * 0.25f - 8.0f, pz = static_cast<f32>(z) * 0.25f - 8.0f;
      positions.push_back(Vec3{px, 0.2f * std::sin(px * 0.7f) * std::cos(pz * 0.5f), pz});
      uvs.push_back(Vec2{px / 16.0f, pz / 16.0f});
    }
  for (u32 z = 0; z + 1 < n; ++z)
    for (u32 x = 0; x + 1 < n; ++x) {
      const u32 a = z * n + x, b = a + 1, c = a + n, d = c + 1;
      for (const u32 i : {a, c, b, b, c, d})
        indices.push_back(i);
    }
  for (u32 k = 0; k < 300; ++k) {
    const u32 base = positions.size();
    const Vec3 at{-7.5f + 0.75f * static_cast<f32>(k % 20), 0.5f,
                  -7.5f + 1.0f * static_cast<f32>(k / 20)};
    for (u32 v = 0; v < 8; ++v) {
      positions.push_back(Vec3{at.x + ((v & 1u) ? 0.04f : 0.0f), at.y + ((v & 2u) ? 0.04f : 0.0f),
                               at.z + ((v & 4u) ? 0.04f : 0.0f)});
      uvs.push_back(Vec2{0.5f, 0.5f});
    }
    const u32 faces[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                              {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
    for (const auto& t : faces)
      for (u32 c = 0; c < 3; ++c)
        indices.push_back(base + t[c]);
  }
  Vector<Vec3> normals;
  const std::span<const Vec3> p(positions.data(), positions.size());
  const std::span<const u32> ix(indices.data(), indices.size());
  geometry::compute_vertex_normals(p, ix, normals);
  geometry::ClusterFileData data;
  geometry::AttributeSource attributes;
  attributes.normals = std::span<const Vec3>(normals.data(), normals.size());
  attributes.uvs = std::span<const Vec2>(uvs.data(), uvs.size());
  std::string error;
  REQUIRE_MESSAGE(geometry::build_cluster_lod(p, ix, geometry::ClusterLodOptions{}, data.mesh,
                                              &error, attributes),
                  error);
  data.cluster_material.assign(data.mesh.mesh.clusters.size(), 0u);
  data.materials.push_back(geometry::ClusterFileMaterial{});
  return data;
}

std::string slashes(std::string path) {
  for (char& c : path)
    if (c == '\\') c = '/';
  return path;
}

}  // namespace

TEST_CASE(
    "far offered: a resident field is offered its far form's levels, and the rule draws the "
    "cheaper") {
  const test::TempDir tmp("world_far_offered");
  REQUIRE(tmp.ok());
  std::string error;
  const std::string field = tmp.file("field.clusters");
  REQUIRE_MESSAGE(geometry::write_cluster_file(field, strewn_sheet(), &error), error);
  // `ring_forms_tests.cpp`'s scene with rings of 1.5, 15 and 30 tiles of 8 m: ring 2's floor is
  // about 120 m, where the field's 22.6 m projects near 200 pixels at the default camera, inside
  // what the far form's first level serves (about 250) and too near for the DAG to have merged the
  // stones away (at 2.4 km its cut is one cluster, and wins).
  const std::string text = std::string(R"({"format":"engine.scene.v1","name":"far-offered",)") +
                           R"("placements":[{"generator":"ring-forms-field","params":{"path":")" +
                           slashes(field) +
                           R"("}}],"world":{"tile_size":8,"rings":[{"radius":1.5},)"
                           R"({"radius":15},{"radius":30}]},)" +
                           R"("terrain":{"size":17,"extent":40,"seed":3,"dune_height":0.0}})";
  REQUIRE(io::write_file(tmp.file("scene.json"), text) == io::Status::Ok);
  renderer::SceneDesc desc;
  REQUIRE_MESSAGE(renderer::read_scene_file(tmp.file("scene.json"), desc, error), error);
  desc.cache = false;
  renderer::SceneData scene;
  REQUIRE_MESSAGE(renderer::load_scene(desc, scene, error), error);
  REQUIRE(scene.streamed.size() == 1u);
  const u32 source = scene.streamed[0].meshes[0];
  const renderer::RingFormTable& table = scene.ring_forms;
  REQUIRE(source < table.meshes.size());
  const renderer::MeshRingForms& forms = table.meshes[source];
  REQUIRE(forms.active);
  const renderer::RingForm& far = forms.ring[2];
  MESSAGE("ring 2: mesh ", far.mesh, " kind ", static_cast<u32>(far.kind), ", ",
          scene.parts[far.mesh].cluster_count, " clusters against the source's ",
          scene.parts[source].cluster_count, "; offered ", table.offered, ", cuts ", table.cuts);
  // Its far form's levels are scene meshes of their own, placed as their source is, each a small
  // share of it; and the rule drew whichever served ring 2 for the fewest pairs.
  u32 offered = 0;
  for (u32 m = 0; m < scene.mesh_info.size(); ++m) {
    if (m == source || scene.form_source[m] != source ||
        scene.mesh_info[m].name.find("(far form") == std::string::npos)
      continue;
    ++offered;
    CHECK(scene.form_rigid[m] == 1u);
    CHECK(scene.parts[m].cluster_count * 4 < scene.parts[source].cluster_count);
    MESSAGE(scene.mesh_info[m].name, ": ", scene.parts[m].cluster_count, " clusters");
  }
  CHECK(offered >= 1u);
  CHECK(far.mesh != source);
  CHECK(scene.form_source[far.mesh] == source);
}

TEST_CASE("far offered: a resident mesh's offered levels are kept in the derived-data cache") {
  // renderer.md, "What path 1 costs a load": a container that records what it was built from is
  // keyed by it; the first load builds each offered level and writes it, the second reads them and
  // draws the same forms.
  const test::TempDir tmp("world_far_offered_cache");
  REQUIRE(tmp.ok());
  std::string error;
  const std::string field = tmp.file("field.clusters");
  geometry::ClusterFileData data = strewn_sheet();
  data.source_hash = 0x5eed0198u;
  data.build_key = 0xb0170198u;
  REQUIRE_MESSAGE(geometry::write_cluster_file(field, data, &error), error);
  const std::string text = std::string(R"({"format":"engine.scene.v1","name":"far-offered",)") +
                           R"("placements":[{"generator":"ring-forms-field","params":{"path":")" +
                           slashes(field) +
                           R"("}}],"world":{"tile_size":8,"rings":[{"radius":1.5},)"
                           R"({"radius":15},{"radius":30}]},)" +
                           R"("terrain":{"size":17,"extent":40,"seed":3,"dune_height":0.0}})";
  REQUIRE(io::write_file(tmp.file("scene.json"), text) == io::Status::Ok);
  auto offered_counts = [&](Vector<u32>& counts) {
    renderer::SceneDesc desc;
    REQUIRE_MESSAGE(renderer::read_scene_file(tmp.file("scene.json"), desc, error), error);
    desc.cache = true;
    desc.ddc = tmp.file("ddc");
    renderer::SceneData scene;
    REQUIRE_MESSAGE(renderer::load_scene(desc, scene, error), error);
    counts.clear();
    for (u32 m = 0; m < scene.mesh_info.size(); ++m)
      if (scene.mesh_info[m].name.find("(far form") != std::string::npos)
        counts.push_back(scene.parts[m].cluster_count);
  };
  Vector<u32> first, second;
  offered_counts(first);
  REQUIRE_FALSE(first.empty());
  u32 entries = 0;
  for (const auto& e : std::filesystem::recursive_directory_iterator(tmp.file("ddc")))
    entries += e.is_regular_file() && e.path().extension() == ".clusters" ? 1u : 0u;
  MESSAGE(first.size(), " offered levels, ", entries, " cluster entries in the cache");
  CHECK(entries >= first.size());
  offered_counts(second);
  REQUIRE(second.size() == first.size());
  for (u32 k = 0; k < first.size(); ++k)
    CHECK(second[k] == first[k]);
}
