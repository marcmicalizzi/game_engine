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

// ---- the pack's normal and roughness across the levels (G93) ------------------------------------

namespace {

image::Image solid_image(u8 r, u8 g, u8 b, u8 a) {
  image::Image out;
  out.width = 4;
  out.height = 4;
  out.channels = 4;
  out.pixels.assign(64, u8{0});
  for (u32 p = 0; p < 16; ++p) {
    out.pixels[4 * p] = r;
    out.pixels[4 * p + 1] = g;
    out.pixels[4 * p + 2] = b;
    out.pixels[4 * p + 3] = a;
  }
  return out;
}

// One unit quad in the z = 0 plane facing +z, drawn by one 16-texel tile that holds it exactly,
// with `uvs` on its corners: the card's view is the BakeView default (u = x, v = y, normal = z).
struct QuadBake {
  assets::MeshData mesh;
  flora::TreeLevels chain;
  flora::TreeLevel level;
  explicit QuadBake(const Vec2 uvs[4], u32 tile_px = 16) {
    mesh.positions = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    mesh.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    mesh.uvs = {uvs[0], uvs[1], uvs[2], uvs[3]};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    chain.leaf_index_end = 6;
    level.tile_px = tile_px;
    flora::BakeJob job;
    job.ranges.push_back({0, 6});
    level.jobs.push_back(job);
  }
  static flora::BakeLook look() {
    flora::BakeLook l;
    l.leaf = {1, 1, 1, 1};
    l.bark = {1, 1, 1, 1};
    return l;
  }
};

// A texel of the atlas's centre, the three channels as 0..1.
void centre_texel(const Vector<u8>& image, f64 out[3]) {
  const u32 at = (8 * 16 + 8) * 4;
  for (u32 k = 0; k < 3; ++k)
    out[k] = static_cast<f64>(image[at + k]) / 255.0;
}

}  // namespace

TEST_CASE("impostor bake: a leaf's normal map tilts the baked normal in the card's frame") {
  const Vec2 aligned[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  // The quad's u runs along its y and its v along its x: the map's x is the card's v.
  const Vec2 turned[4] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
  // A uniform tilt: x 166/255 -> +0.302, y 128/255 -> +0.004, z rebuilt (the content build's BC5).
  const image::Image tilt = solid_image(166, 128, 255, 255);
  const f64 mx = 166.0 / 255.0 * 2.0 - 1.0;
  const f64 my = 128.0 / 255.0 * 2.0 - 1.0;
  const f64 mz = std::sqrt(1.0 - mx * mx - my * my);
  for (const bool is_turned : {false, true}) {
    QuadBake bake(is_turned ? turned : aligned);
    flora::BakeLook look = QuadBake::look();
    look.leaf_normal_image = &tilt;
    flora::BakedAtlas tilted;
    flora::bake_level(bake.mesh, bake.chain, bake.level, look, tilted);
    f64 n[3];
    centre_texel(tilted.normal, n);
    // Each channel is a byte of 255 (0.004 a step) and the vector went through f32 (1e-7).
    const f64 tolerance = 1.5 / 255.0;
    // T = dP/du and B = dP/dv: aligned, the map's (x, y) are the card's (u, v); turned, its x is
    // along the card's v and its y along the card's u.
    CHECK(n[0] == doctest::Approx((is_turned ? my : mx) * 0.5 + 0.5).epsilon(tolerance));
    CHECK(n[1] == doctest::Approx((is_turned ? mx : my) * 0.5 + 0.5).epsilon(tolerance));
    CHECK(n[2] == doctest::Approx(mz * 0.5 + 0.5).epsilon(tolerance));
    // The tilt is real: with no map the same texel is the flat face (0.5, 0.5, 1).
    flora::BakedAtlas flat;
    flora::bake_level(bake.mesh, bake.chain, bake.level, QuadBake::look(), flat);
    f64 f[3];
    centre_texel(flat.normal, f);
    CHECK(f[0] == doctest::Approx(0.5).epsilon(tolerance));
    CHECK(f[2] == doctest::Approx(1.0).epsilon(tolerance));
    CHECK(std::abs(n[is_turned ? 1 : 0] - f[is_turned ? 1 : 0]) > 0.1);
    // Colour and alpha are untouched by a map, and the atlas carries no roughness image.
    CHECK(tilted.colour == flat.colour);
    CHECK(tilted.roughness.empty());
  }
}

TEST_CASE("impostor bake: a flat normal map and a scalar roughness are the bake they always were") {
  const Vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  QuadBake bake(uvs);
  flora::BakedAtlas before;
  flora::bake_level(bake.mesh, bake.chain, bake.level, QuadBake::look(), before);
  // (128, 128, 255) is the flat normal to the byte; the bake's own bytes round it back to the face.
  const image::Image flat = solid_image(128, 128, 255, 255);
  flora::BakeLook look = QuadBake::look();
  look.leaf_normal_image = &flat;
  look.leaf_normal_scale = 0.65f;
  look.leaf_roughness = 0.7f;  // a scalar with no image: nothing to carry
  flora::BakedAtlas after;
  flora::bake_level(bake.mesh, bake.chain, bake.level, look, after);
  CHECK(after.colour == before.colour);
  CHECK(after.normal == before.normal);
  CHECK(after.roughness.empty());
  CHECK(before.roughness.empty());
}

TEST_CASE("impostor bake: a roughness image is carried per texel, times the material's factor") {
  const Vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  QuadBake bake(uvs);
  // The image is glTF's: G the roughness; R and B are not read.
  const image::Image rough = solid_image(7, 100, 9, 255);
  for (const f64 factor : {1.0, 0.5}) {
    flora::BakeLook look = QuadBake::look();
    look.leaf_roughness_image = &rough;
    look.leaf_roughness = static_cast<f32>(factor);
    flora::BakedAtlas atlas;
    flora::bake_level(bake.mesh, bake.chain, bake.level, look, atlas);
    REQUIRE(atlas.roughness.size() == atlas.colour.size());
    u32 covered = 0;
    for (u32 p = 0; p < 16 * 16; ++p) {
      if (atlas.colour[4 * p + 3] != 255) continue;
      ++covered;
      CHECK(atlas.roughness[4 * p] == 255);
      CHECK(static_cast<i32>(atlas.roughness[4 * p + 1]) ==
            static_cast<i32>(std::floor(100.0 * factor + 0.5)));
      CHECK(atlas.roughness[4 * p + 2] == 0);  // metallic zero
      CHECK(atlas.roughness[4 * p + 3] == 255);
    }
    CHECK(covered == 16 * 16);
    // The bake's colour and normal do not depend on it.
    flora::BakedAtlas plain;
    flora::bake_level(bake.mesh, bake.chain, bake.level, QuadBake::look(), plain);
    CHECK(atlas.colour == plain.colour);
    CHECK(atlas.normal == plain.normal);
  }
}

TEST_CASE("impostor bake: a texel's normal is the renormalized mean of the samples it covers") {
  // One texel over the whole quad, whose map is tilted +x on its left half and -x on its right:
  // the mean of the two is straight, and the bake renormalizes it to the unit vector it was.
  const Vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  QuadBake bake(uvs, 1);
  image::Image halves = solid_image(0, 128, 255, 255);  // x -1 ... overwritten below
  for (u32 p = 0; p < 16; ++p) {
    halves.pixels[4 * p] = p % 4 < 2 ? 220 : 35;  // x +0.725 on the left, -0.725 on the right
    halves.pixels[4 * p + 1] = 128;
  }
  flora::BakeLook look = QuadBake::look();
  look.leaf_normal_image = &halves;
  flora::BakedAtlas atlas;
  flora::bake_level(bake.mesh, bake.chain, bake.level, look, atlas);
  REQUIRE(atlas.normal.size() == 4);
  CHECK(atlas.colour[3] > 250);
  CHECK(static_cast<i32>(atlas.normal[0]) == doctest::Approx(128).epsilon(0.03));  // x straight
  CHECK(atlas.normal[2] > 250);                                                    // z up again
  // One side alone is not straight.
  image::Image left = solid_image(220, 128, 255, 255);
  look.leaf_normal_image = &left;
  flora::BakedAtlas tilted;
  flora::bake_level(bake.mesh, bake.chain, bake.level, look, tilted);
  CHECK(tilted.normal[0] > 200);
}

TEST_CASE("impostor chain: a pack's normal and roughness reach the levels and key the tree") {
  flora::FloraFile file;
  std::string error;
  REQUIRE(flora::read_flora_file(
      R"({"species":"maple","tree":{"height_m":6,"trunk_radius_m":0.15,"card_bake_px":16,"impostor_px":32},)"
      R"("materials":{"leaf":{"base_color":"leaf.png","normal":"leaf_n.png","roughness":"leaf_r.png",)"
      R"("tile_m":1,"normal_scale":1}}})",
      file, error));
  flora::FloraFile colour_only;
  REQUIRE(flora::read_flora_file(
      R"({"species":"maple","tree":{"height_m":6,"trunk_radius_m":0.15,"card_bake_px":16,"impostor_px":32},)"
      R"("materials":{"leaf":{"base_color":"leaf.png","tile_m":1}}})",
      colour_only, error));
  // A solid leaf, a uniform +x tilt and a roughness of 204/255 (0.8).
  struct Images {
    Vector<u8> colour, normal, roughness;
  } images;
  REQUIRE(image::encode_png(4, 4, 4, solid_image(30, 160, 40, 255).pixels, images.colour));
  REQUIRE(image::encode_png(4, 4, 4, solid_image(220, 128, 255, 255).pixels, images.normal));
  REQUIRE(image::encode_png(4, 4, 4, solid_image(204, 204, 204, 255).pixels, images.roughness));
  assets::MeshGeneratorHosts hosts;
  hosts.context = &images;
  hosts.read_image = [](void* context, std::string_view path, Vector<u8>& out, std::string& e) {
    const Images& i = *static_cast<const Images*>(context);
    if (path == "leaf.png")
      out = i.colour;
    else if (path == "leaf_n.png")
      out = i.normal;
    else if (path == "leaf_r.png")
      out = i.roughness;
    else {
      e = "no such image";
      return false;
    }
    return true;
  };
  const assets::MeshGeneratorInputs inputs{nullptr, &hosts};
  geometry::ClusterFileData with, without;
  REQUIRE_MESSAGE(
      flora::build_flora_container(file, {}, 0, with, nullptr, error, nullptr, nullptr, &hosts),
      error);
  REQUIRE_MESSAGE(flora::build_flora_container(colour_only, {}, 0, without, nullptr, error, nullptr,
                                               nullptr, &hosts),
                  error);
  REQUIRE(!with.impostors.empty());
  REQUIRE(with.impostors.size() == without.impostors.size());
  const geometry::ClusterFileMaterial& card = with.materials[with.impostors.front().material];
  const geometry::ClusterFileMaterial& plain =
      without.materials[without.impostors.front().material];
  // The level that has a roughness image names it, with a factor of one; the one without names
  // none.
  const i32 rough_slot = geometry::decode_optional_image(card.metallic_roughness_image);
  REQUIRE(rough_slot >= 0);
  CHECK(rough_slot == card.base_color_image + 2);
  CHECK(card.normal_image == card.base_color_image + 1);
  CHECK(card.roughness == 1.0f);
  CHECK(card.metallic == 0.0f);
  CHECK(geometry::decode_optional_image(plain.metallic_roughness_image) == -1);
  // Every drawn texel of the first branch-group level carries the leaf's roughness, 204 of 255.
  image::Image rough, colour_with, normal_with, normal_without;
  REQUIRE(image::decode_image(with.images[rough_slot].bytes, rough, 4, &error));
  REQUIRE(image::decode_image(with.images[card.base_color_image].bytes, colour_with, 4, &error));
  u32 drawn = 0;
  for (u32 p = 0; p < rough.width * rough.height; ++p) {
    if (colour_with.pixels[4 * p + 3] < 128) continue;  // alpha of the colour atlas
    ++drawn;
    CHECK(rough.pixels[4 * p + 1] == 204);
  }
  CHECK(drawn > 0);
  // The normal atlas has relief the colour-only tree's does not.
  REQUIRE(image::decode_image(with.images[card.normal_image].bytes, normal_with, 4, &error));
  REQUIRE(image::decode_image(without.images[plain.normal_image].bytes, normal_without, 4, &error));
  CHECK(normal_with.pixels != normal_without.pixels);
  // The colour is the colour: the maps move only the normal and the roughness.
  image::Image colour_without;
  REQUIRE(
      image::decode_image(without.images[plain.base_color_image].bytes, colour_without, 4, &error));
  CHECK(colour_with.pixels == colour_without.pixels);

  // The tree is keyed by the maps it now consumes: an edit to either changes its hash, and a
  // pack's colour-only leaf hashes as it did (the colour alone).
  u64 hash = 0, edited = 0, colour_hash = 0;
  REQUIRE(flora::flora_source_hash_with(file, inputs, hash, error));
  CHECK(with.source_hash == hash);
  images.normal.back() ^= 0x55;
  REQUIRE(flora::flora_source_hash_with(file, inputs, edited, error));
  CHECK(edited != hash);
  images.normal.back() ^= 0x55;
  images.roughness.back() ^= 0x55;
  REQUIRE(flora::flora_source_hash_with(file, inputs, edited, error));
  CHECK(edited != hash);
  REQUIRE(flora::flora_source_hash_with(colour_only, inputs, colour_hash, error));
  CHECK(colour_hash == without.source_hash);
  CHECK(colour_hash != hash);
}
