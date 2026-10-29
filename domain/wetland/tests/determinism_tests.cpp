#include <core/hash/hash.h>
#include <domain/wetland/wetland.h>

#include <doctest/doctest.h>

using namespace engine;
template <class T>
u64 bytes(const Vector<T>& v) {
  return hash_bytes(v.data(), v.size() * sizeof(T));
}
TEST_CASE("wetland: version four section identities") {
  // Initial landing pins (v4): v2 seats roots; v3 preserves organs; v4 delays cards.
  // Prelanding v1/v2/v3 capture and pin logs are retained with the local study.
  // Existing flora identities are unchanged.
  // Future replacements record the old hash, reason, version and changing commit here.
  const u64 pins[][4] = {
      {0x9981398c5b957149ull, 0x7e2997cc51775284ull, 0x94b7237e535201d4ull,
       0x557be4544cd57d93ull},  // sedge
      {0xa5ff70b852584b47ull, 0x5868d36a64c48dc9ull, 0x9fd3d655c39d3dd3ull,
       0xc4f562e08098a03eull},  // tussock
      {0xb876fd94e8662b1full, 0x962c212790681572ull, 0x4799cde624d1a103ull,
       0x415688211dbcb0b8ull},  // rush
      {0x9b22d8a32ad44ceeull, 0xfabd4931b7c3ccecull, 0x1181fb0c3e37bb74ull,
       0xdb07bcb835b52d2aull},  // reed
      {0x871959f27e897371ull, 0x90c4e550e20bdbf0ull, 0x1245e11c8ec206bcull,
       0x1c6c9435daeb1ad0ull},  // cattail
      {0x9dcc9d5949e158bcull, 0xcb644ddd58dbb646ull, 0x72905cffc9f7fceeull,
       0xf9f46fbaba89fa7full},  // arrowhead
      {0xbff6a17432e67befull, 0x341d750a135e82cfull, 0xc15736f1805d2878ull,
       0xf8a1d946c51ccd3aull},  // water_lily
      {0x6d631ad9fd25a110ull, 0x7e912816f57b348eull, 0xc15736f1805d2878ull,
       0xa3abaaf35556bc52ull},  // pond_lily
  };
  u32 index = 0;
  for (const auto& p : wetland::profiles()) {
    wetland::Plant recipe;
    recipe.species = geometry::flora_species_info(p.species).name;
    assets::MeshData mesh;
    flora::Motion motion;
    wetland::Report report;
    std::string error;
    REQUIRE_MESSAGE(wetland::solve(recipe, mesh, motion, report, error), error);
    const u64 geometry_hash = hash_combine(hash_combine(bytes(mesh.positions), bytes(mesh.normals)),
                                           hash_combine(bytes(mesh.uvs), bytes(mesh.indices)));
    const u64 motion_hash =
        hash_combine(hash_combine(bytes(motion.parts), bytes(motion.structure)),
                     hash_combine(bytes(motion.vertex_part), bytes(motion.vertex_weight)));
    geometry::ClusterFileData data;
    REQUIRE_MESSAGE(wetland::build_container(recipe, {}, {}, 65536, data, report, error), error);
    CAPTURE(recipe.species);
    CHECK(wetland::source_hash(recipe) == pins[index][0]);
    CHECK(geometry_hash == pins[index][1]);
    CHECK(motion_hash == pins[index][2]);
    CHECK(geometry::cluster_file_hash(data) == pins[index][3]);
    ++index;
  }
}
