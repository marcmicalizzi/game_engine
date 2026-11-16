// A texture sized by its asset (texture.md, "Sized by the asset"; roadmap G14): the density
// measure, the option that caps level 0, and the rule that a capped texture's levels are the
// uncapped texture's own.
#include "fixtures.h"

#include <domain/geometry/material_sampling.h>
#include <domain/texture/texel_density.h>
#include <domain/texture/texture_build.h>
#include <domain/texture/texture_file.h>

#include <doctest/doctest.h>

#include <cstring>

using namespace engine;
using namespace engine::texture;

namespace {

// A square of `side` metres in the xz plane, UVs 0..`uv` along both axes, one material whose base
// colour is image 0 and whose normal map is image 1.
struct Quad {
  Vector<Vec3> positions;
  Vector<Vec2> uvs;
  Vector<u32> indices;
  Vector<u32> material;
  Vector<geometry::ClusterFileMaterial> materials;
};

Quad quad(f32 side, f32 uv_u, f32 uv_v) {
  Quad q;
  q.positions = {Vec3{0.0f, 0.0f, 0.0f}, Vec3{side, 0.0f, 0.0f}, Vec3{side, 0.0f, side},
                 Vec3{0.0f, 0.0f, side}};
  q.uvs = {Vec2{0.0f, 0.0f}, Vec2{uv_u, 0.0f}, Vec2{uv_u, uv_v}, Vec2{0.0f, uv_v}};
  q.indices = {0, 2, 1, 0, 3, 2};
  q.material = {0, 0};
  geometry::ClusterFileMaterial m;
  m.base_color_image = 0;
  m.normal_image = 1;
  q.materials.push_back(m);
  return q;
}

void measure(const Quad& q, Vector<ImageDensity>& out,
             std::span<const geometry::ClusterFileMaterialSampling> sampling = {}) {
  measure_image_density({q.positions.data(), q.positions.size()}, {q.uvs.data(), q.uvs.size()},
                        {q.indices.data(), q.indices.size()},
                        {q.material.data(), q.material.size()},
                        {q.materials.data(), q.materials.size()}, sampling, 3, out);
}

}  // namespace

TEST_CASE("texel density: a 20 cm box face mapped once is five UV units a metre") {
  Vector<ImageDensity> d;
  measure(quad(0.2f, 1.0f, 1.0f), d);
  REQUIRE(d.size() == 3);
  CHECK(d[0].triangles == 2);
  CHECK(d[0].surface_m2 == doctest::Approx(0.04).epsilon(1e-6));
  CHECK(d[0].uv_area == doctest::Approx(1.0).epsilon(1e-6));
  CHECK(d[0].uv_per_m == doctest::Approx(5.0).epsilon(1e-6));
  CHECK(d[0].uv_per_m_mean == doctest::Approx(5.0).epsilon(1e-6));
  CHECK(d[1].uv_per_m == doctest::Approx(5.0).epsilon(1e-6));  // the normal map, the same UVs
  CHECK(d[2].triangles == 0);                                  // nothing samples image 2
  CHECK(d[2].uv_per_m == 0.0);
  // A 2,048 image there is 10,240 texels a metre; at 2,000 texels a metre the view needs 400
  // texels a side, which the option keeps as 512: level 2 of the 2,048 chain.
  CHECK(needed_side(d[0].uv_per_m, 1.0, 2000.0) == 512);
  CHECK(texture_levels_dropped(2048, 2048, 512) == 2);
  // An instance twice the size needs twice the texels.
  CHECK(needed_side(d[0].uv_per_m, 2.0, 2000.0) == 1024);
}

TEST_CASE("texel density: the least stretched direction decides, and a slot's scale counts") {
  Vector<ImageDensity> d;
  // A 0.2 m face mapped to 4 by 1 UV units: 20 a metre along x, 5 along z.
  measure(quad(0.2f, 4.0f, 1.0f), d);
  CHECK(d[0].uv_per_m == doctest::Approx(5.0).epsilon(1e-6));        // 1 / 0.2 along z
  CHECK(d[0].uv_per_m_mean == doctest::Approx(10.0).epsilon(1e-6));  // sqrt(4 / 0.04)
  // KHR_texture_transform's scale of 3 on the base colour multiplies the density, and of every
  // slot: the renderer draws a material's slots with the one transform it keeps, the base
  // colour's, so the normal map's own scale of 1 is not what is drawn.
  geometry::TextureSlotSampling slots[geometry::k_material_slots];
  slots[geometry::k_slot_base_color].transform.scale = Vec2{3.0f, 3.0f};
  const geometry::ClusterFileMaterialSampling record =
      geometry::encode_material_sampling(slots, 1.0f);
  measure(quad(0.2f, 1.0f, 1.0f), d, {&record, 1});
  CHECK(d[0].uv_per_m == doctest::Approx(15.0).epsilon(1e-6));
  CHECK(d[1].uv_per_m == doctest::Approx(15.0).epsilon(1e-6));
}

TEST_CASE("texel density: a sliver under one percent of the surface does not hold the cap") {
  Quad q = quad(0.2f, 1.0f, 1.0f);
  // A triangle of a thousandth of the face's area whose UVs barely move: an exporter's squeezed
  // island. It alone would make the minimum near zero.
  const u32 base = static_cast<u32>(q.positions.size());
  q.positions.push_back(Vec3{0.0f, 0.1f, 0.0f});
  q.positions.push_back(Vec3{0.02f, 0.1f, 0.0f});
  q.positions.push_back(Vec3{0.0f, 0.1f, 0.004f});
  q.uvs.push_back(Vec2{0.5f, 0.5f});
  q.uvs.push_back(Vec2{0.5001f, 0.5f});
  q.uvs.push_back(Vec2{0.5f, 0.5001f});
  q.indices.push_back(base);
  q.indices.push_back(base + 2);
  q.indices.push_back(base + 1);
  q.material.push_back(0);
  Vector<ImageDensity> d;
  measure(q, d);
  CHECK(d[0].triangles == 3);
  CHECK(d[0].uv_per_m == doctest::Approx(5.0).epsilon(1e-6));
}

TEST_CASE("texel density: needed_side rounds up to a power of two and gives up past the largest") {
  CHECK(needed_side(5.0, 1.0, 5.0) == 1);
  CHECK(needed_side(5.0, 1.0, 5.1) == 2);
  CHECK(needed_side(1.0, 1.0, 16384.0) == 16384);
  CHECK(needed_side(1.0, 1.0, 16385.0) == 0);
  CHECK(needed_side(0.0, 1.0, 100.0) == 0);
  CHECK(needed_side(5.0, 0.0, 100.0) == 0);
  CHECK(needed_side(5.0, 1.0, 0.0) == 0);
}

TEST_CASE("texture cap: levels dropped follow the shorter side and stop at the chain's end") {
  CHECK(texture_levels_dropped(2048, 2048, 0) == 0);
  CHECK(texture_levels_dropped(2048, 2048, 4096) == 0);
  CHECK(texture_levels_dropped(2048, 2048, 2048) == 0);
  CHECK(texture_levels_dropped(2048, 2048, 1024) == 1);
  CHECK(texture_levels_dropped(2048, 2048, 600) == 1);  // kept as 1,024
  CHECK(texture_levels_dropped(2048, 512, 256) == 1);   // 1,024 x 256
  CHECK(texture_levels_dropped(1500, 1500, 1024) == 0);
  CHECK(texture_levels_dropped(1500, 1500, 512) == 1);  // 750
  CHECK(texture_levels_dropped(64, 64, 1) == 6);        // the 1x1 level is the last
  CHECK(texture_levels_dropped(64, 16, 1) == 6);        // 1x1 too: the longer side decides
}

TEST_CASE("texture cap: packed in its own bits, absent from every word that states none") {
  TextureBuildOptions options;
  options.format = FormatChoice::bc7;
  const u32 uncapped = pack_texture_options(options);
  CHECK((uncapped & 0xf000u) == 0);
  options.needed_side = 512;
  const u32 capped = pack_texture_options(options);
  CHECK(capped == (uncapped | (10u << 12)));
  TextureBuildOptions back;
  REQUIRE(unpack_texture_options(capped, back));
  CHECK(back.needed_side == 512);
  options.needed_side = 600;  // not a power of two: kept as the next one up
  REQUIRE(unpack_texture_options(pack_texture_options(options), back));
  CHECK(back.needed_side == 1024);
  options.needed_side = 1;
  REQUIRE(unpack_texture_options(pack_texture_options(options), back));
  CHECK(back.needed_side == 1);
  options.needed_side = 20000;  // past the largest side: nothing can be left out
  CHECK(pack_texture_options(options) == uncapped);
  options.needed_side = 0;
  CHECK(texture_cache_key(0x1234u, options) != 0);
  TextureBuildOptions capped_options = options;
  capped_options.needed_side = 16;
  CHECK(texture_cache_key(0x1234u, capped_options) != texture_cache_key(0x1234u, options));
}

TEST_CASE("texture cap: the kept levels are the uncapped texture's own, byte for byte") {
  struct Case {
    bool normal;
    bool srgb;
    u8 cutoff;
  };
  const Case cases[] = {{false, true, 0}, {true, false, 0}, {false, true, 128}};
  for (const Case& c : cases) {
    CAPTURE(c.normal);
    CAPTURE(static_cast<u32>(c.cutoff));
    Vector<u8> image;
    if (c.normal) {
      test_fixtures::normal_image(64, 32, image);
    } else {
      test_fixtures::colour_image(64, 32, c.cutoff != 0, image);
    }
    TextureBuildOptions options;
    options.normal_map = c.normal;
    options.color_space = c.srgb ? ColorSpace::srgb : ColorSpace::linear;
    options.alpha_cutoff = c.cutoff;
    options.edge_x = EdgeMode::repeat;
    TextureData full;
    std::string error;
    REQUIRE_MESSAGE(
        build_texture({image.data(), image.size()}, 64, 32, 4, options, full, nullptr, &error),
        error);
    options.needed_side = 8;  // 64 x 32 -> 16 x 8: two levels left out
    TextureData capped;
    REQUIRE_MESSAGE(
        build_texture({image.data(), image.size()}, 64, 32, 4, options, capped, nullptr, &error),
        error);
    REQUIRE(full.levels.size() == 7);
    REQUIRE(capped.levels.size() == 5);
    CHECK(capped.width == 16);
    CHECK(capped.height == 8);
    CHECK(capped.format == full.format);
    CHECK(capped.flags == full.flags);
    for (u32 i = 0; i < capped.levels.size(); ++i) {
      const std::span<const u8> a = capped.level_bytes(i);
      const std::span<const u8> b = full.level_bytes(i + 2);
      REQUIRE(a.size() == b.size());
      CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
    }
    if (c.normal) {
      // The normal-variance chain loses the same two levels.
      REQUIRE(!full.normal_variance.empty());
      CHECK(capped.normal_variance.size() < full.normal_variance.size());
    }
    // Without mips the kept level alone, the same bytes again.
    options.mips = false;
    TextureData single;
    REQUIRE(
        build_texture({image.data(), image.size()}, 64, 32, 4, options, single, nullptr, &error));
    REQUIRE(single.levels.size() == 1);
    const std::span<const u8> a = single.level_bytes(0);
    const std::span<const u8> b = full.level_bytes(2);
    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
  }
}
