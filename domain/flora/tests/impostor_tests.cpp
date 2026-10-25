// The impostor chain (impostor.h; flora.md, "The impostor chain"; R89): the solver's mesh is the
// same bytes with the chain asked for, the bake is the same bytes on every toolchain, the chain's
// levels sit in the DAG where their errors put them, and the container carries them.

#include <core/hash/hash.h>
#include <domain/assets/mesh_generator.h>
#include <domain/flora/flora.h>
#include <domain/flora/impostor.h>
#include <domain/flora/trees.h>
#include <domain/geometry/cluster_file.h>
#include <foundation/image/decode.h>
#include <foundation/image/png.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
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
  // Every finest leaf triangle names its sub-tree's card.
  CHECK(levels.leaf_element.size() * 3u == levels.leaf_index_end - levels.leaf_first_index);
  for (const u32 e : levels.leaf_element)
    CHECK(e < levels.levels.front().elements);

  // Branch-group levels with rising errors and falling card counts, the tree's star last.
  REQUIRE(levels.levels.size() >= 2);
  CHECK(levels.levels.back().kind == flora::ImpostorKind::tree);
  CHECK(levels.levels.back().groups == 16);  // four depth slices per vertical view
  for (u32 k = 0; k < levels.levels.size(); ++k) {
    const flora::TreeLevel& level = levels.levels[k];
    MESSAGE("level " << k << ": merge " << level.merge << ", " << level.groups << " cards, error "
                     << level.error << " m (parallax " << level.parallax_m << ", texel "
                     << level.texel_m << "), feature " << level.feature_m << " m");
    CHECK(level.indices.size() == level.groups * 6u);
    // The nesting: an element a card (the star is one), and every card's parent in the next level.
    CHECK(level.element_of_triangle.size() * 3u == level.indices.size());
    CHECK(level.elements == (level.kind == flora::ImpostorKind::tree ? 1u : level.groups / 3u));
    if (k + 1 < levels.levels.size()) {
      REQUIRE(level.parent_of_element.size() == level.elements);
      for (const u32 parent : level.parent_of_element)
        CHECK(parent < levels.levels[k + 1].elements);
    }
    CHECK(level.feature_m > 0.0f);
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
  CHECK(t.width == tree.columns * tree.tile_px);
  CHECK(t.height == tree.rows * tree.tile_px);
  // Something is drawn in the four planes' tiles.
  u32 covered = 0;
  for (u32 i = 3; i < t.colour.size(); i += 4)
    covered += t.colour[i] != 0u ? 1u : 0u;
  CHECK(covered > t.width * t.height / 10u);
  // The coverage bake is the same number every time, and a card is neither the geometry nor
  // nothing: its difference is inside (0, 1).
  const f32 card_difference = flora::measure_level(mesh, levels, cards, look, a);
  CHECK(card_difference == flora::measure_level(mesh, levels, cards, look, b));
  const f32 tree_difference = flora::measure_level(mesh, levels, tree, look, t);
  MESSAGE("coverage bake: cards differ by " << card_difference << ", the star by "
                                            << tree_difference);
  CHECK(card_difference > 0.0f);
  CHECK(card_difference < 1.0f);
  CHECK(tree_difference > 0.0f);
  CHECK(tree_difference < 1.0f);
  // Pinned on MSVC; GCC and Clang at v3 and GCC at v2 reproduce them (flora.md, "Determinism").
  const u64 hashes[4] = {bytes_hash(a.colour), bytes_hash(a.normal), bytes_hash(t.colour),
                         bytes_hash(t.normal)};
  char line[160];
  std::snprintf(
      line, sizeof(line), "0x%016llxull, 0x%016llxull, 0x%016llxull, 0x%016llxull",
      static_cast<unsigned long long>(hashes[0]), static_cast<unsigned long long>(hashes[1]),
      static_cast<unsigned long long>(hashes[2]), static_cast<unsigned long long>(hashes[3]));
  MESSAGE("bake hashes: " << line);
  // R91 moved all four: the plane fit by area (the cards) and the star's four planes (the tree).
  // G85 changes only the whole-tree pair: supported wood contours and their normals. MSVC and
  // Clang reproduce both; the leaf-only cards remain byte-identical.
  // G87: three orthogonal group planes and four depth slices per far-tree direction.
  // G88: the local trunk-tip crown changes leaf/wood groups and their coverage.
  constexpr u64 k_golden[4] = {0x2c54e962dfb33177ull, 0x814e39ddea5d3d29ull, 0x10d253a81a6aa4ccull,
                               0x66dc658f1c1b0cc2ull};
  for (u32 k = 0; k < 4; ++k)
    CHECK(hashes[k] == k_golden[k]);
}

TEST_CASE("impostor chain: each card switches on its own sub-tree, the star where its errors say") {
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
  // Section 39: a coverage word a cluster, and the levels' coverage errors rising.
  REQUIRE(data.lod_coverage.size() == lod.lod.size());
  for (u32 k = 0; k < data.impostors.size(); ++k) {
    const geometry::ClusterFileImpostor& r = data.impostors[k];
    MESSAGE("level " << k << ": error " << r.error << " m, coverage " << r.coverage
                     << " m (difference " << r.difference << ")");
    CHECK(r.coverage > 0.0f);
    CHECK(r.difference > 0.0f);
    CHECK(r.difference < 1.0f);
    if (k > 0) CHECK(r.coverage > data.impostors[k - 1].coverage);
  }
  geometry::LodView view;
  view.znear = 0.1f;
  view.proj_scale = 1.0f / std::tan(radians(60.0f) * 0.5f) * 1080.0f * 0.5f;
  view.threshold_px = 1.0f;
  const std::span<const u32> coverage(data.lod_coverage.data(), data.lod_coverage.size());
  const auto materials_drawn = [&](Vec3 at, Vector<u32>& count) {
    view.camera = at;
    Vector<u32> cut;
    geometry::select_lod(lod, view, coverage, cut);
    count.assign(data.materials.size(), 0u);
    for (const u32 c : cut)
      ++count[data.cluster_material[c]];
  };
  // The star carries the whole tree's sphere, and the bark's hand-over is the same test: 1 % inside
  // its distance the star is not drawn, 1 % past it nothing else is — by the geometric error alone
  // (no foliage threshold) and by the coverage error under one.
  const geometry::ClusterFileImpostor& star = data.impostors.back();
  const Vec4 sphere = geometry::authored_lod_sphere(lod, star.error);
  REQUIRE(sphere.w > 0.0f);
  const Vec3 centre{sphere.x, sphere.y, sphere.z};
  for (const f32 foliage : {0.0f, 3.0f}) {
    view.foliage_px = foliage;
    f32 at = star.error * view.proj_scale / view.threshold_px;
    if (foliage > 0.0f) {
      const f32 by_coverage = star.coverage * view.proj_scale / foliage;
      at = by_coverage < at ? by_coverage : at;
    }
    at += sphere.w;
    MESSAGE("the star switches in at "
            << at << " m from the tree's centre with a foliage threshold of " << foliage << " px");
    Vector<u32> inside;
    Vector<u32> past;
    materials_drawn(centre + Vec3{at * 0.99f, 0.0f, 0.0f}, inside);
    materials_drawn(centre + Vec3{at * 1.01f, 0.0f, 0.0f}, past);
    CHECK(inside[star.material] == 0u);
    CHECK(past[star.material] > 0u);
    for (u32 m = 0; m < past.size(); ++m)
      if (m != star.material) CHECK(past[m] == 0u);
  }
  // **Per sub-tree**: somewhere between the crown's edge and twenty crown radii out, the near side
  // of the crown is still leaves while the far side is already first-level cards — which one shared
  // sphere could never draw.
  view.foliage_px = 3.0f;
  const u32 cards = data.impostors.front().material;
  bool mixed = false;
  for (u32 step = 2; step <= 80 && !mixed; ++step) {
    Vector<u32> count;
    materials_drawn(centre + Vec3{sphere.w * 0.25f * static_cast<f32>(step), 0.0f, 0.0f}, count);
    mixed = count[1] > 0u && count[cards] > 0u;
  }
  CHECK(mixed);

  // The sections round-trip.
  test::TempDir tmp("impostor_container");
  REQUIRE_MESSAGE(geometry::write_cluster_file(tmp.file("oak.clusters"), data, &error), error);
  geometry::ClusterFileData back;
  REQUIRE_MESSAGE(geometry::read_cluster_file(tmp.file("oak.clusters"), back, &error), error);
  REQUIRE(back.impostors.size() == data.impostors.size());
  CHECK(std::memcmp(back.impostors.data(), data.impostors.data(),
                    data.impostors.size() * sizeof(geometry::ClusterFileImpostor)) == 0);
  CHECK(back.lod_coverage == data.lod_coverage);
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
  CHECK(data.lod_coverage.empty());
  CHECK(data.materials.size() == 2);
  CHECK(data.images.empty());
}

TEST_CASE(
    "impostor chain: real leaf colour and alpha are baked and image edits invalidate the tree") {
  flora::FloraFile file;
  std::string error;
  REQUIRE(flora::read_flora_file(
      R"({"species":"maple","tree":{"height_m":6,"trunk_radius_m":0.15,"card_bake_px":16,"impostor_px":32},)"
      R"("materials":{"leaf":{"base_color":"leaves.png","tile_m":1}}})",
      file, error));
  Vector<u8> pixels(4 * 4 * 4, 0u);
  for (u32 p = 0; p < 16; ++p) {
    pixels[p * 4] = 220;
    pixels[p * 4 + 1] = 15;
    pixels[p * 4 + 2] = 30;
    pixels[p * 4 + 3] = p % 4 < 2 ? 255 : 0;
  }
  Vector<u8> bytes;
  REQUIRE(image::encode_png(4, 4, 4, pixels, bytes));
  assets::MeshGeneratorHosts hosts;
  hosts.context = &bytes;
  hosts.read_image = [](void* context, std::string_view path, Vector<u8>& out, std::string&) {
    if (path != "leaves.png") return false;
    out = *static_cast<Vector<u8>*>(context);
    return true;
  };
  const assets::MeshGeneratorInputs inputs{nullptr, &hosts};
  u64 red_hash = 0;
  REQUIRE(flora::flora_source_hash_with(file, inputs, red_hash, error));
  geometry::ClusterFileData red;
  REQUIRE_MESSAGE(
      flora::build_flora_container(file, {}, 0, red, nullptr, error, nullptr, nullptr, &hosts),
      error);
  CHECK(red.source_hash == red_hash);
  REQUIRE(!red.impostors.empty());
  REQUIRE(red.impostors.front().kind == static_cast<u32>(flora::ImpostorKind::branch_group));
  const i32 slot = red.materials[red.impostors.front().material].base_color_image;
  REQUIRE(slot >= 0);
  image::Image atlas;
  REQUIRE(image::decode_image(red.images[slot].bytes, atlas, 4, &error));
  u64 r = 0, g = 0;
  u32 occupied = 0, empty = 0;
  for (u32 p = 0; p < atlas.width * atlas.height; ++p) {
    if (atlas.pixels[4 * p + 3] < 128) {
      ++empty;
      continue;
    }
    ++occupied;
    r += atlas.pixels[4 * p];
    g += atlas.pixels[4 * p + 1];
  }
  CHECK(occupied > 0);
  CHECK(empty > 0);
  CHECK(r > g * 4);  // the far foliage is red, regardless of the solver's green default
  for (u32 p = 0; p < 16; ++p) {
    pixels[4 * p] = 15;
    pixels[4 * p + 1] = 220;
  }
  REQUIRE(image::encode_png(4, 4, 4, pixels, bytes));
  u64 green_hash = 0;
  REQUIRE(flora::flora_source_hash_with(file, inputs, green_hash, error));
  CHECK(green_hash != red_hash);  // same path and recipe, different image bytes
  CHECK(flora::flora_build_key(green_hash, {}, 0) != red.build_key);
  hosts.read_image = [](void*, std::string_view, Vector<u8>&, std::string& e) {
    e = "missing leaf image";
    return false;
  };
  CHECK_FALSE(flora::flora_source_hash_with(file, inputs, green_hash, error));
  CHECK(error == "missing leaf image");
}

TEST_CASE("impostor chain: leaf alpha cuts actual pixels and bark ignores image alpha") {
  assets::MeshData mesh;
  mesh.positions = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
  mesh.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
  mesh.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  mesh.indices = {0, 1, 2, 0, 2, 3};
  flora::TreeLevels chain;
  chain.leaf_index_end = 6;
  flora::TreeLevel level;
  level.tile_px = 16;
  flora::BakeJob job;
  job.ranges.push_back({0, 6});
  level.jobs.push_back(job);
  image::Image texture;
  texture.width = 4;
  texture.height = 4;
  texture.channels = 4;
  texture.pixels.assign(64, u8{255});
  for (u32 p = 0; p < 16; ++p) {
    texture.pixels[4 * p + 1] = 0;
    texture.pixels[4 * p + 2] = 0;
    texture.pixels[4 * p + 3] = p % 4 < 2 ? 255 : 0;
  }
  flora::BakeLook look;
  look.leaf = {1, 1, 1, 1};
  look.bark = {1, 1, 1, 1};
  look.leaf_image = &texture;
  look.bark_image = &texture;
  flora::BakedAtlas half, opaque;
  flora::bake_level(mesh, chain, level, look, half);
  chain.leaf_first_index = 6;  // same quad and texture, now an opaque bark surface
  flora::bake_level(mesh, chain, level, look, opaque);
  u64 half_alpha = 0, full_alpha = 0;
  for (u32 p = 0; p < 16 * 16; ++p) {
    half_alpha += half.colour[4 * p + 3];
    full_alpha += opaque.colour[4 * p + 3];
    if (half.colour[4 * p + 3] != 0) {
      CHECK(half.colour[4 * p] > 200);
      CHECK(half.colour[4 * p + 1] == 0);
    }
  }
  REQUIRE(full_alpha > 0);
  CHECK(static_cast<f64>(half_alpha) / full_alpha == doctest::Approx(.5).epsilon(.01));
}
