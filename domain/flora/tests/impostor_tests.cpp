// The impostor chain (impostor.h; flora.md, "The impostor chain"; R89): the solver's mesh is the
// same bytes with the chain asked for, the bake is the same bytes on every toolchain, the chain's
// levels sit in the DAG where their errors put them, and the container carries them.

#include <core/hash/hash.h>
#include <domain/flora/flora.h>
#include <domain/flora/impostor.h>
#include <domain/flora/trees.h>
#include <domain/geometry/cluster_file.h>
#include <foundation/image/png.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace engine;

namespace {

// A smaller oak than the default, so the whole chain builds in a debug test's few seconds.
flora::Broadleaf small_oak() {
  flora::Broadleaf p;
  p.height_m = 8.0f;
  p.triangle_budget = 30000;
  return p;
}

u64 bytes_hash(const Vector<u8>& bytes) { return hash_bytes(bytes.data(), bytes.size()); }

// The atlases go into the test's scratch directory; compiled with ENGINE_KEEP_IMPOSTOR_BAKE defined
// they are kept there, for a person to look at.
void maybe_keep(test::TempDir& tmp, const flora::BakedAtlas& atlas, const std::string& name) {
  image::write_png(tmp.file(name + "_colour.png"), atlas.width, atlas.height, 4, atlas.colour);
  image::write_png(tmp.file(name + "_normal.png"), atlas.width, atlas.height, 4, atlas.normal);
#if defined(ENGINE_KEEP_IMPOSTOR_BAKE)
  tmp.keep();
#endif
}

}  // namespace

TEST_CASE("impostor chain: the solver's mesh is its bytes with the chain asked for") {
  const flora::Broadleaf p = small_oak();
  assets::MeshData plain;
  flora::Motion plain_motion;
  REQUIRE(flora::solve_broadleaf(p, plain, nullptr, nullptr, &plain_motion));
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeLevels levels;
  REQUIRE(flora::solve_broadleaf_chain(p, mesh, nullptr, nullptr, &motion, &levels));
  // Everything the plain solve makes comes first, unchanged; the cards follow.
  REQUIRE(mesh.positions.size() > plain.positions.size());
  CHECK(std::memcmp(mesh.positions.data(), plain.positions.data(),
                    plain.positions.size() * sizeof(Vec3)) == 0);
  REQUIRE(mesh.indices.size() == plain.indices.size());
  CHECK(std::memcmp(mesh.indices.data(), plain.indices.data(),
                    plain.indices.size() * sizeof(u32)) == 0);
  CHECK(motion.vertex_part.size() == mesh.positions.size());
  CHECK(motion.parts.size() == plain_motion.parts.size());

  // Branch-group levels with rising errors and falling card counts, the tree's star last.
  REQUIRE(levels.levels.size() >= 2);
  CHECK(levels.levels.back().kind == flora::ImpostorKind::tree);
  CHECK(levels.levels.back().groups == 5);
  for (u32 k = 0; k < levels.levels.size(); ++k) {
    const flora::TreeLevel& level = levels.levels[k];
    MESSAGE("level " << k << ": merge " << level.merge << ", " << level.groups << " cards, error "
                     << level.error << " m (parallax " << level.parallax_m << ", texel "
                     << level.texel_m << ")");
    CHECK(level.indices.size() == level.groups * 6u);
    for (const u32 i : level.indices)
      CHECK(i >= plain.positions.size());
    if (k > 0) {
      CHECK(level.error > levels.levels[k - 1].error);
      CHECK(level.groups < levels.levels[k - 1].groups);
    }
    CHECK(level.error >= level.parallax_m);
    CHECK(level.error >= level.texel_m);
  }
}

TEST_CASE("impostor chain: the bake is the same bytes every time and on every toolchain") {
  const flora::Broadleaf p = small_oak();
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeLevels levels;
  REQUIRE(flora::solve_broadleaf_chain(p, mesh, nullptr, nullptr, &motion, &levels));
  REQUIRE(levels.levels.size() >= 2);
  flora::BakeLook look;
  look.leaf_outline = true;
  test::TempDir tmp("impostor_bake");
  const flora::TreeLevel& cards = levels.levels.front();
  const flora::TreeLevel& tree = levels.levels.back();
  flora::BakedAtlas a;
  flora::BakedAtlas b;
  flora::bake_level(mesh, levels, cards, look, a);
  flora::bake_level(mesh, levels, cards, look, b);
  CHECK(a.colour == b.colour);
  CHECK(a.normal == b.normal);
  maybe_keep(tmp, a, "cards");
  flora::BakedAtlas t;
  flora::bake_level(mesh, levels, tree, look, t);
  maybe_keep(tmp, t, "tree");
  CHECK(t.width == 3 * tree.tile_px);
  CHECK(t.height == 2 * tree.tile_px);
  // Something is drawn in every tile that has a job, and the tree's sixth tile is empty.
  u32 covered = 0;
  for (u32 i = 3; i < t.colour.size(); i += 4)
    covered += t.colour[i] != 0u ? 1u : 0u;
  CHECK(covered > t.width * t.height / 10u);
  for (u32 y = tree.tile_px; y < t.height; ++y)
    for (u32 x = 2 * tree.tile_px; x < t.width; ++x)
      CHECK(t.colour[(y * t.width + x) * 4u + 3u] == 0u);
  // Pinned on MSVC; GCC and Clang at v3 and GCC at v2 reproduce them (flora.md, "Determinism").
  const u64 hashes[4] = {bytes_hash(a.colour), bytes_hash(a.normal), bytes_hash(t.colour),
                         bytes_hash(t.normal)};
  char line[160];
  std::snprintf(
      line, sizeof(line), "0x%016llxull, 0x%016llxull, 0x%016llxull, 0x%016llxull",
      static_cast<unsigned long long>(hashes[0]), static_cast<unsigned long long>(hashes[1]),
      static_cast<unsigned long long>(hashes[2]), static_cast<unsigned long long>(hashes[3]));
  MESSAGE("bake hashes: " << line);
  constexpr u64 k_golden[4] = {0x39ae66191f133683ull, 0xcf17ef6dd4680187ull, 0x6d4215b4e5824d5aull,
                               0xcbc4eb14ce9b941dull};
  for (u32 k = 0; k < 4; ++k)
    CHECK(hashes[k] == k_golden[k]);
}

TEST_CASE("impostor chain: the container's levels switch where their errors say, not nearer") {
  flora::FloraFile file;
  file.species = flora::k_species_broadleaf;
  file.broadleaf = small_oak();
  geometry::ClusterFileData data;
  std::string error;
  REQUIRE_MESSAGE(
      flora::build_flora_container(file, geometry::ClusterLodOptions{}, 0, data, nullptr, error),
      error);
  REQUIRE(data.impostors.size() >= 2);
  const geometry::ClusterLodMesh& lod = data.mesh;
  // Every transition the chain makes carries one sphere.
  const Vec4 sphere = geometry::authored_lod_sphere(lod, data.impostors.front().error);
  REQUIRE(sphere.w > 0.0f);
  geometry::LodView view;
  view.znear = 0.1f;
  view.proj_scale = 1.0f / std::tan(radians(60.0f) * 0.5f) * 1080.0f * 0.5f;
  view.threshold_px = 1.0f;
  const Vec3 centre{sphere.x, sphere.y, sphere.z};
  const auto materials_drawn = [&](f32 distance, Vector<u32>& count) {
    view.camera = centre + Vec3{distance, 0.0f, 0.0f};
    Vector<u32> cut;
    geometry::select_lod(lod, view, cut);
    count.assign(data.materials.size(), 0u);
    for (const u32 c : cut)
      ++count[data.cluster_material[c]];
  };
  for (u32 k = 0; k < data.impostors.size(); ++k) {
    const geometry::ClusterFileImpostor& level = data.impostors[k];
    const f32 at = level.error * view.proj_scale / view.threshold_px + sphere.w;
    MESSAGE("level " << k << " (" << std::string(level.kind == 2u ? "tree" : "cards") << ", "
                     << level.groups << ") switches in at " << at << " m from the tree's centre");
    Vector<u32> inside;
    Vector<u32> past;
    materials_drawn(at * 0.99f, inside);
    materials_drawn(at * 1.01f, past);
    CHECK(inside[level.material] == 0u);
    CHECK(past[level.material] > 0u);
    // Past it nothing finer of the leaves draws; past the tree's, nothing else at all.
    CHECK(past[1] == 0u);
    if (level.kind == 2u) {
      for (u32 m = 0; m < past.size(); ++m)
        if (m != level.material) CHECK(past[m] == 0u);
    } else {
      CHECK(past[0] > 0u);  // the bark still draws under the cards
    }
  }

  // The section round-trips.
  test::TempDir tmp("impostor_container");
  REQUIRE_MESSAGE(geometry::write_cluster_file(tmp.file("oak.clusters"), data, &error), error);
  geometry::ClusterFileData back;
  REQUIRE_MESSAGE(geometry::read_cluster_file(tmp.file("oak.clusters"), back, &error), error);
  REQUIRE(back.impostors.size() == data.impostors.size());
  CHECK(std::memcmp(back.impostors.data(), data.impostors.data(),
                    data.impostors.size() * sizeof(geometry::ClusterFileImpostor)) == 0);
}

TEST_CASE("impostor chain: off is the container it was") {
  flora::FloraFile file;
  file.species = flora::k_species_broadleaf;
  file.broadleaf = small_oak();
  file.broadleaf.card_bake_px = 0;
  file.broadleaf.impostor_px = 0;
  geometry::ClusterFileData data;
  std::string error;
  REQUIRE(
      flora::build_flora_container(file, geometry::ClusterLodOptions{}, 0, data, nullptr, error));
  CHECK(data.impostors.empty());
  CHECK(data.materials.size() == 2);
  CHECK(data.images.empty());
}
