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
  // G203 (2026-11-14, b63cc940; still v4): the container hash of every row moved and nothing
  // else did. A container records the key of the alpha-tested image it carries, and a mask's
  // texture key now mixes `texture::k_alpha_mask_version` (2), so that the masks of a cache
  // recook under the chain that holds the filtered coverage. Checked, not inferred: with the mix
  // switched off this build hashes all eight containers to the values before it, and the sedge's
  // and the reed's containers written both ways differ in section 32 (the texture records) alone,
  // 38 other sections identical. The container hashes before it, in
  // the rows' order: 0x557be4544cd57d93, 0xc4f562e08098a03e, 0x415688211dbcb0b8,
  // 0xdb07bcb835b52d2a, 0x1c6c9435daeb1ad0, 0xf9f46fbaba89fa7f, 0xf8a1d946c51ccd3a,
  // 0xa3abaaf35556bc52.
  const u64 pins[][4] = {
      {0x21a8e103f3470865ull, 0x7e2997cc51775284ull, 0x94b7237e535201d4ull,
       0x7a02579290148cc0ull},  // sedge
      {0xe10b36b1ce5d11c0ull, 0x5868d36a64c48dc9ull, 0x9fd3d655c39d3dd3ull,
       0xdb8c5f94ebfcd19full},  // tussock
      {0x063d9c518bf172bcull, 0x962c212790681572ull, 0x4799cde624d1a103ull,
       0xa4a43d7bb01c0843ull},  // rush
      {0xa327762addb4e112ull, 0xfabd4931b7c3ccecull, 0x1181fb0c3e37bb74ull,
       0x2ea7806da4db3a89ull},  // reed
      {0x7ff98e2af1e1c7c0ull, 0x90c4e550e20bdbf0ull, 0x1245e11c8ec206bcull,
       0x7e24fcc7b06d6437ull},  // cattail
      {0xa4d93dd2b636e620ull, 0xcb644ddd58dbb646ull, 0x72905cffc9f7fceeull,
       0x368a1c23a5139e94ull},  // arrowhead
      {0x482754e31b7481dbull, 0x341d750a135e82cfull, 0xc15736f1805d2878ull,
       0xb1ec81e9d389a0caull},  // water_lily
      {0x155aac8d67963f4bull, 0x7e912816f57b348eull, 0xc15736f1805d2878ull,
       0xb2624ff47c0d3d77ull},  // pond_lily
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
