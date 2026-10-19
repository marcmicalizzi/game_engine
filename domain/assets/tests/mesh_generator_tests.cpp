// The mesh-generator registry (docs/subsystems/assets.md, "Generated meshes"): which paths name a
// generator, and a registry of the test's own refusing, finding and naming.
#include <domain/assets/mesh_generator.h>
#include <domain/materials/materials.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;

namespace {

bool fake_hash(std::span<const u8> bytes, u64& out, std::string&) {
  out = bytes.size();
  return true;
}
bool fake_build(std::span<const u8>, const assets::MeshGeneratorInputs&,
                const geometry::ClusterLodOptions&, u32, geometry::ClusterFileData&,
                std::string& error) {
  error = "not built";
  return false;
}

}  // namespace

TEST_CASE("assets: a generated mesh's name is the second extension before .json") {
  CHECK(assets::mesh_generator_of("content/flora/date-palm.flora.json") == "flora");
  CHECK(assets::mesh_generator_of("C:\\x\\y.z\\palm.flora.json") == "flora");
  CHECK(assets::mesh_generator_of("palm.flora.json") == "flora");
  CHECK(assets::mesh_generator_of("scene.json").empty());
  CHECK(assets::mesh_generator_of("dir.flora/scene.json").empty());
  CHECK(assets::mesh_generator_of("mesh.gltf").empty());
  CHECK(assets::mesh_generator_of("x/.flora.json").empty());
  CHECK(assets::mesh_generator_of("x/palm..json").empty());
  CHECK(assets::mesh_generator_of(".json").empty());
}

TEST_CASE("assets: the registry finds a generator by name, refuses a second, and names the rest") {
  assets::MeshGeneratorRegistry registry;
  constexpr assets::MeshGeneratorDesc k_a{
      .name = "alpha", .source_hash = fake_hash, .build = fake_build};
  constexpr assets::MeshGeneratorDesc k_b{
      .name = "beta", .source_hash = fake_hash, .build = fake_build};
  constexpr assets::MeshGeneratorDesc k_a_again{
      .name = "alpha", .source_hash = fake_hash, .build = fake_build};
  constexpr assets::MeshGeneratorDesc k_incomplete{.name = "gamma"};
  CHECK(registry.add(k_b));
  CHECK(registry.add(k_a));
  CHECK(registry.add(k_a));  // the same descriptor again: one library in two images
  CHECK_FALSE(registry.add(k_a_again));
  CHECK_FALSE(registry.add(k_incomplete));
  CHECK(registry.find("alpha") == &k_a);
  CHECK(registry.find("gamma") == nullptr);
  const Vector<std::string_view> names = registry.names();
  REQUIRE(names.size() == 2);
  CHECK(names[0] == "alpha");
  CHECK(names[1] == "beta");
  const std::string sentence = registry.unknown("palm.flora.json", "flora");
  CHECK(sentence.find("palm.flora.json") != std::string::npos);
  CHECK(sentence.find("ENGINE_WITH_FLORA") != std::string::npos);
  CHECK(sentence.find("alpha, beta") != std::string::npos);
}

TEST_CASE("assets: a generated mesh's key is the cache key, and a pack with roles moves it") {
  const geometry::ClusterLodOptions lod;
  const u64 plain = geometry::cluster_cache_key(0x1234u, lod, false, 65536u);
  CHECK(assets::generated_mesh_build_key(0x1234u, assets::MeshGeneratorInputs{}, lod, 65536u) ==
        plain);
  materials::Pack empty;  // a pack with no roles keys as none: nothing it holds can be drawn
  empty.hash = 99u;
  CHECK(assets::generated_mesh_build_key(0x1234u, assets::MeshGeneratorInputs{&empty}, lod,
                                         65536u) == plain);
  materials::Pack pack;
  pack.hash = 99u;
  pack.roles.push_back(materials::PackRole{"palm/bark", {}});
  const u64 with_pack =
      assets::generated_mesh_build_key(0x1234u, assets::MeshGeneratorInputs{&pack}, lod, 65536u);
  CHECK(with_pack != plain);
  pack.hash = 100u;
  CHECK(assets::generated_mesh_build_key(0x1234u, assets::MeshGeneratorInputs{&pack}, lod,
                                         65536u) != with_pack);
}
