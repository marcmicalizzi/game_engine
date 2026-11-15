// A crown's leaf measured from its cards' own masks (docs/subsystems/renderer.md, "Stage 7";
// docs/experiments/static-asset-world.md, "Bought foliage"; roadmap T146). On the CPU: a bought
// tree's branch cards — a metre and more, a third of each kept by its mask — have a crown, and its
// leaf area is the cards' area times what their mask keeps, not a solver's constant; a solver-sized
// leaf card of a solid mask counts its whole card; the same cards under a mask that keeps nothing
// have no crown.
#include <foundation/image/png.h>
#include <systems/renderer/crown_priors.h>
#include <systems/renderer/scene.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr u32 k_texels = 16;

bool write_file(const std::string& path, const void* data, usize size) {
  std::ofstream f(path, std::ios::binary);
  f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
  return f.good();
}

void put_f32(std::vector<u8>& out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((bits >> (8 * i)) & 0xffu));
}

// The mask: a 16 x 16 image whose `kept_columns` leftmost columns are opaque and the rest cut, so
// `kept_columns` / 16 of its texels are leaf.
bool write_mask(const std::string& path, u32 kept_columns) {
  std::vector<u8> rgba(k_texels * k_texels * 4);
  for (u32 y = 0; y < k_texels; ++y) {
    for (u32 x = 0; x < k_texels; ++x) {
      u8* t = &rgba[(y * k_texels + x) * 4];
      t[0] = 40;
      t[1] = 120;
      t[2] = 30;
      t[3] = x < kept_columns ? 255 : 0;
    }
  }
  Vector<u8> png;
  if (!image::encode_png(k_texels, k_texels, 4, std::span<const u8>(rgba.data(), rgba.size()), png))
    return false;
  return write_file(path, png.data(), png.size());
}

// `cards` vertical cards `width` by `height` metres, stacked one above the other from y = 0 and
// turned 60 degrees each, UV (0,0) at the top left of each, double-sided and MASK at 0.5, the base
// colour `mask.png` beside the file: the shape of a bought tree's crossed branch cards.
bool write_cards(const std::string& dir, const char* name, u32 cards, f32 width, f32 height) {
  std::vector<u8> bin;
  std::vector<f32> positions;
  std::vector<f32> normals;
  std::vector<f32> uvs;
  std::vector<u16> indices;
  const f32 axes[3][2] = {{1.0f, 0.0f}, {0.5f, 0.8660254f}, {-0.5f, 0.8660254f}};
  for (u32 c = 0; c < cards; ++c) {
    const f32 ax = axes[c % 3][0] * 0.5f * width;
    const f32 az = axes[c % 3][1] * 0.5f * width;
    const f32 y0 = static_cast<f32>(c) * height * 0.5f;
    const f32 corner[4][4] = {{-1, 0, 0, 1}, {1, 0, 1, 1}, {1, 1, 1, 0}, {-1, 1, 0, 0}};
    const u16 base = static_cast<u16>(positions.size() / 3);
    for (const auto& k : corner) {
      positions.insert(positions.end(), {k[0] * ax, y0 + k[1] * height, k[0] * az});
      normals.insert(normals.end(), {-axes[c % 3][1], 0.0f, axes[c % 3][0]});
      uvs.insert(uvs.end(), {k[2], k[3]});
    }
    indices.insert(indices.end(), {base, static_cast<u16>(base + 1), static_cast<u16>(base + 2),
                                   base, static_cast<u16>(base + 2), static_cast<u16>(base + 3)});
  }
  for (const f32 v : positions)
    put_f32(bin, v);
  for (const f32 v : normals)
    put_f32(bin, v);
  for (const f32 v : uvs)
    put_f32(bin, v);
  for (const u16 i : indices) {
    bin.push_back(static_cast<u8>(i & 0xff));
    bin.push_back(static_cast<u8>(i >> 8));
  }
  const usize vertices = positions.size() / 3;
  const usize p_bytes = vertices * 12;
  const usize uv_bytes = vertices * 8;
  const std::string stem = std::string(name);
  if (!write_file(dir + "/" + stem + ".bin", bin.data(), bin.size())) return false;
  const f32 top = static_cast<f32>(cards - 1) * height * 0.5f + height;
  const std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
      "\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},"
      "\"metallicFactor\":0,\"roughnessFactor\":1},\"doubleSided\":true,"
      "\"alphaMode\":\"MASK\",\"alphaCutoff\":0.5}],"
      "\"samplers\":[{\"magFilter\":9728,\"minFilter\":9728}],"
      "\"textures\":[{\"source\":0,\"sampler\":0}],"
      "\"images\":[{\"uri\":\"mask.png\"}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":" +
      std::to_string(vertices) + ",\"type\":\"VEC3\",\"min\":[" + std::to_string(-0.5f * width) +
      ",0," + std::to_string(-0.5f * width) + "],\"max\":[" + std::to_string(0.5f * width) + "," +
      std::to_string(top) + "," + std::to_string(0.5f * width) +
      "]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":" +
      std::to_string(vertices) +
      ",\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":" +
      std::to_string(vertices) +
      ",\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5123,\"count\":" +
      std::to_string(indices.size()) +
      ",\"type\":\"SCALAR\"}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
      std::to_string(p_bytes) + "},{\"buffer\":0,\"byteOffset\":" + std::to_string(p_bytes) +
      ",\"byteLength\":" + std::to_string(p_bytes) +
      "},{\"buffer\":0,\"byteOffset\":" + std::to_string(2 * p_bytes) +
      ",\"byteLength\":" + std::to_string(uv_bytes) +
      "},{\"buffer\":0,\"byteOffset\":" + std::to_string(2 * p_bytes + uv_bytes) +
      ",\"byteLength\":" + std::to_string(indices.size() * 2) + "}],\"buffers\":[{\"uri\":\"" +
      stem + ".bin\",\"byteLength\":" + std::to_string(bin.size()) + "}]}";
  return write_file(dir + "/" + stem + ".gltf", json.data(), json.size());
}

// Loads the one mesh on the CPU and builds the scene's crown table from it.
CrownPriorTable crowns_of(const test::TempDir& tmp, const char* name, bool release_uvs = false) {
  SceneDesc desc;
  desc.meshes.push_back(tmp.file(std::string(name) + ".gltf"));
  desc.cache = false;
  SceneInstance one;
  desc.instances.push_back(one);
  SceneData data;
  std::string error;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  // A host that released the uploaded streams first (`release_uploaded_streams`) has no UVs.
  if (release_uvs) {
    data.lod.mesh.attributes.clear();
    data.lod.mesh.attributes.shrink_to_fit();
  }
  CrownPriorTable table;
  build_crown_priors(data, table);
  return table;
}

}  // namespace

TEST_CASE("crown prior: a bought tree's metre-wide branch cards count the leaf their mask keeps") {
  test::TempDir tmp("crown-prior-measure");
  // 12 cards of 1.2 m by 1.0 m, half overlapping up to 6.5 m: 14.4 m2 of card, each triangle
  // 0.6 m2, twelve times the 0.05 m2 the prior once refused. 5 of 16 columns kept: 0.3125.
  REQUIRE(write_mask(tmp.file("mask.png"), 5));
  REQUIRE(write_cards(tmp.path(), "branch", 12, 1.2f, 1.0f));
  const CrownPriorTable table = crowns_of(tmp, "branch");
  REQUIRE(table.crowns == 1);
  CHECK(table.leaf_triangles == 24);
  const f64 card = 12.0 * 1.2 * 1.0;
  // The mask is asked at the 16 sub-triangle centroids of each half-card, as the coverage bake asks
  // it: their u lie a twelfth apart, so the kept columns' edge at u = 5/16 falls between points and
  // the quadrature reads 4 of 16 (0.25) of the texels' 0.3125. Held to a twelfth either way of the
  // texels' share, the points' own spacing.
  MESSAGE("leaf " << table.leaf_area_m2 << " m2 of " << card << " m2 of card");
  CHECK(table.leaf_area_m2 >= card * (5.0 / 16.0 - 1.0 / 12.0));
  CHECK(table.leaf_area_m2 <= card * (5.0 / 16.0 + 1.0 / 12.0));
  // Never the solver's constant: 0.7 of the card would be 10.1 m2.
  CHECK(table.leaf_area_m2 < card * 0.5);
  // With the UVs released, each triangle counts its image's mean, which is the texels' 5/16
  // exactly: these cards map the whole image.
  const CrownPriorTable meaned = crowns_of(tmp, "branch", true);
  REQUIRE(meaned.crowns == 1);
  CHECK(meaned.leaf_area_m2 == doctest::Approx(card * 5.0 / 16.0).epsilon(1e-5));
}

TEST_CASE("crown prior: a solid mask counts the whole card and an empty one has no crown") {
  test::TempDir tmp("crown-prior-solid");
  REQUIRE(write_mask(tmp.file("mask.png"), k_texels));
  REQUIRE(write_cards(tmp.path(), "leaves", 30, 0.2f, 0.2f));
  const CrownPriorTable solid = crowns_of(tmp, "leaves");
  // 30 leaf cards of 0.2 m stacked 3.1 m tall: a crown, and every card is leaf.
  REQUIRE(solid.crowns == 1);
  CHECK(solid.leaf_area_m2 == doctest::Approx(30.0 * 0.2 * 0.2).epsilon(1e-4));
  // The same cards cut away entirely: no leaf, and the grid's march finds nothing to shade.
  REQUIRE(write_mask(tmp.file("mask.png"), 0));
  const CrownPriorTable empty = crowns_of(tmp, "leaves");
  CHECK(empty.leaf_area_m2 == 0.0);
}
