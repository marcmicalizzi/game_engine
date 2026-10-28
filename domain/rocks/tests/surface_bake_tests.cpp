#include "../src/surface_bake.h"
#include "rock_checks.h"

#include <domain/materials/materials.h>
#include <domain/rocks/rocks.h>
#include <foundation/image/decode.h>

#include <doctest/doctest.h>

#include <limits>

using namespace engine;
using namespace engine::rocks;

namespace {
RockFile recipe() {
  RockFile file;
  file.boulder = Boulder{};
  file.boulder->triangle_budget = 2000;
  file.boulder->size_x = 0.3f;
  file.boulder->size_y = 0.3f;
  file.boulder->size_z = 0.3f;
  file.boulder->material.surface_maps = RockSurfaceMaps{};
  file.boulder->material.surface_maps->resolution = 256;
  file.boulder->material.surface_maps->grain_m = 0.08f;
  file.boulder->material.surface_maps->veins = 0.8f;
  return file;
}
assets::MeshData solve(const RockFile& file) {
  assets::MeshData mesh;
  std::string error;
  REQUIRE_MESSAGE(solve_rock(file, mesh, nullptr, &error), error);
  return mesh;
}
image::Image decode(const assets::MeshData& mesh, u32 slot) {
  image::Image image;
  REQUIRE(image::decode_image(mesh.images[slot].bytes, image, 3));
  return image;
}
}  // namespace

TEST_CASE("rocks: baked PBR is deterministic, embedded, bounded and does not change geometry") {
  auto file = recipe();
  const auto mesh = solve(file);
  const auto again = solve(file);
  REQUIRE(mesh.images.size() == 3);
  for (u32 i = 0; i < 3; ++i) {
    CHECK(mesh.images[i].uri.empty());
    CHECK(mesh.images[i].mime_type == "image/png");
    CHECK(mesh.images[i].bytes == again.images[i].bytes);
    const auto pixels = decode(mesh, i);
    CHECK(pixels.width == 256);
    CHECK(pixels.height == 256);
  }
  CHECK(mesh.materials[0].base_color_image == 0);
  CHECK(mesh.materials[0].metallic_roughness_image == 1);
  CHECK(mesh.materials[0].normal_image == 2);
  CHECK(mesh.materials[0].metallic == 0.0f);
  const auto colour = decode(mesh, 0);
  // Unused cells/remainder must not contribute black to distant mips.
  for (u8 value : colour.pixels)
    CHECK(value > 0);
  const auto roughness = decode(mesh, 1);
  for (u32 p = 0; p < roughness.pixels.size(); p += 3)
    CHECK(roughness.pixels[p + 2] == 0);
  const auto normal = decode(mesh, 2);
  bool tilted = false;
  for (u32 p = 0; p < normal.pixels.size(); p += 3) {
    CHECK(normal.pixels[p + 2] >= 128);
    tilted = tilted || normal.pixels[p] < 127 || normal.pixels[p] > 129;
  }
  CHECK(tilted);
  for (Vec2 uv : mesh.uvs) {
    CHECK(uv.x > 0.0f);
    CHECK(uv.x < 1.0f);
    CHECK(uv.y > 0.0f);
    CHECK(uv.y < 1.0f);
  }
  file.boulder->material.surface_maps.reset();
  const auto plain = solve(file);
  REQUIRE(plain.positions.size() == mesh.positions.size());
  CHECK(hash_bytes(plain.positions.data(), plain.positions.size() * sizeof(Vec3)) ==
        hash_bytes(mesh.positions.data(), mesh.positions.size() * sizeof(Vec3)));
  CHECK(plain.indices == mesh.indices);
  CHECK(plain.images.empty());
}

TEST_CASE("rocks: chart corner pigments agree across all six UV seams") {
  auto file = recipe();
  file.boulder->material.surface_maps->grain_m = 1.0f;
  file.boulder->material.surface_maps->veins = 0.0f;
  file.boulder->material.surface_maps->rind = RockRind{};
  file.boulder->material.surface_maps->rind->amount = 1.0f;
  const auto mesh = solve(file);
  const auto pixels = decode(mesh, 0);
  const u32 n = static_cast<u32>(std::sqrt(static_cast<f64>(mesh.indices.size() / 36u)));
  const u32 side = n + 1;
  std::vector<u32> corners;
  for (u32 f = 0; f < 6; ++f) {
    const u32 first = f * side * side;
    for (u32 offset : {0u, n, n * side, side * side - 1u})
      corners.push_back(first + offset);
  }
  u32 matches = 0;
  for (u32 a : corners)
    for (u32 b : corners) {
      if (a >= b) continue;
      const Vec3 pa = mesh.positions[a], pb = mesh.positions[b];
      if (pa.x != pb.x || pa.y != pb.y || pa.z != pb.z) continue;
      ++matches;
      const auto pixel = [&](u32 index) {
        const Vec2 uv = mesh.uvs[index];
        return (static_cast<u32>(uv.y * 256.0f) * 256u + static_cast<u32>(uv.x * 256.0f)) * 3u;
      };
      for (u32 c = 0; c < 3; ++c)
        CHECK(pixels.pixels[pixel(a) + c] == pixels.pixels[pixel(b) + c]);
    }
  CHECK(matches == 24);
}

TEST_CASE("rocks: baked container carries maps and ignores a tiled material pack") {
  const auto file = recipe();
  geometry::ClusterFileData data;
  std::string error;
  materials::Pack pack;
  materials::PackRole role;
  role.role = "rock/granite";
  role.set.base_color = "should-not-be-read.png";
  pack.roles.push_back(role);
  REQUIRE_MESSAGE(build_rock_container(file, {}, 0, data, nullptr, error, &pack), error);
  REQUIRE(data.images.size() == 3);
  CHECK(data.textures.size() == 3);
  for (u32 i = 0; i < 3; ++i) {
    CHECK(data.image_paths[i].empty());
    CHECK_FALSE(data.images[i].bytes.empty());
  }
  CHECK(data.materials[0].base_color.x == 1.0f);
}

TEST_CASE(
    "rocks: baked maps reject ambiguous paths, unsupported cliffs and excessive chart counts") {
  auto file = recipe();
  std::string error;
  file.boulder->material.base_color = "tiled.png";
  CHECK_FALSE(rock_file_in_range(file, error));
  file = recipe();
  file.boulder->material.surface_maps->veins = std::numeric_limits<f32>::quiet_NaN();
  CHECK_FALSE(rock_file_in_range(file, error));
  file = recipe();
  file.boulder->material.surface_maps->resolution = 8192;
  CHECK_FALSE(rock_file_in_range(file, error));
  Cliff cliff;
  cliff.material.surface_maps = RockSurfaceMaps{};
  CHECK_FALSE(cliff_in_range(cliff, error));
  file = RockFile{};
  file.shape = RockShape::Scree;
  file.scree = Scree{};
  file.scree->count = 64;
  file.scree->triangle_budget = 100000;
  file.scree->material.surface_maps = RockSurfaceMaps{};
  file.scree->material.surface_maps->resolution = 256;
  assets::MeshData mesh;
  CHECK_FALSE(solve_rock(file, mesh, nullptr, &error));
  CHECK(error.find("interior texels") != std::string::npos);
  CHECK(mesh.positions.empty());
}

TEST_CASE("rocks: both grouped shapes keep independent padded charts") {
  for (RockShape shape : {RockShape::Outcrop, RockShape::Scree}) {
    RockFile file;
    file.shape = shape;
    RockSurfaceMaps settings;
    settings.resolution = 256;
    if (shape == RockShape::Outcrop) {
      file.outcrop = Outcrop{};
      file.outcrop->count = 3;
      file.outcrop->triangle_budget = 2000;
      file.outcrop->material.surface_maps = settings;
    } else {
      file.scree = Scree{};
      file.scree->count = 3;
      file.scree->triangle_budget = 2000;
      file.scree->material.surface_maps = settings;
    }
    const auto mesh = solve(file);
    checks::check_closed(mesh, 3);
    CHECK(mesh.images.size() == 3);
  }
}

TEST_CASE("rocks: a rind preserves the complete mineral field on fresh faces") {
  using namespace engine::rocks::detail;
  const auto bake = [](bool rind, f64 fresh, bool mixed = false) {
    RockSurfaceMaps settings;
    settings.resolution = 256;
    settings.grain_m = 0.025f;
    settings.veins = 1.0f;
    if (mixed) {
      settings.veins = 0.0f;
      settings.variation = 0.0f;
    }
    if (rind) {
      settings.rind = RockRind{};
      settings.rind->amount = 1.0f;
      settings.rind->roughness = 0.24f;
      settings.rind->grain_scale = 0.0f;
      settings.rind->tint = std::array<f32, 3>{0.2f, 0.3f, 0.4f};
    }
    RockGeology geology;
    geology.weathering = 1.0f;
    const auto fabric = make_fabric(RockType::Gneiss, geology, 17);
    assets::MeshData mesh;
    mesh.materials.emplace_back();
    Vector<D3> samples;
    Vector<f64> provenance;
    // Six identical planar charts isolate the baker from the shape generator.
    for (u32 face = 0; face < 6; ++face)
      for (u32 y = 0; y < 2; ++y)
        for (u32 x = 0; x < 2; ++x) {
          mesh.positions.push_back(Vec3{static_cast<f32>(x) * 0.1f, static_cast<f32>(y) * 0.1f, 0});
          samples.push_back(D3{static_cast<f64>(x) * 0.1, static_cast<f64>(y) * 0.1, 0});
          provenance.push_back(mixed ? (x == 0 && y == 0 ? 0.0 : 1.0) : fresh);
        }
    SurfaceBake surface;
    std::string error;
    REQUIRE(surface.init(settings, 1, error));
    const u8 diagonals[6] = {1, 0, 1, 0, 1, 0};
    surface.body(0, 1, 0, fabric, RockType::Gneiss, samples, provenance, diagonals, mesh);
    REQUIRE(surface.finish(mesh, 1));
    return mesh;
  };
  const auto uncoated = bake(false, 0);
  const auto fresh = bake(true, 1);
  for (u32 i = 0; i < 3; ++i)
    CHECK(uncoated.images[i].bytes == fresh.images[i].bytes);
  const auto old = bake(true, 0);
  const auto mixed = decode(bake(true, 1, true), 1);
  // At chart coordinate (.25,.5), the old corner weighs .5 on AC and .25 on BD.
  // Roughness blends from 1 to .24: .62 and .81, independently of the mineral field.
  CHECK(mixed.pixels[(42u * 256u + 23u) * 3u + 1u] == 158);
  CHECK(mixed.pixels[(42u * 256u + 108u) * 3u + 1u] == 207);
  const auto roughness = decode(old, 1);
  const auto normal = decode(old, 2);
  const auto colour = decode(old, 0);
  for (u32 p = 0; p < roughness.pixels.size(); p += 3) {
    CHECK(roughness.pixels[p + 1] == 61);
    CHECK(normal.pixels[p] == 128);
    CHECK(normal.pixels[p + 1] == 128);
    CHECK(normal.pixels[p + 2] == 255);
    CHECK(colour.pixels[p] == colour.pixels[0]);
    CHECK(colour.pixels[p + 1] == colour.pixels[1]);
    CHECK(colour.pixels[p + 2] == colour.pixels[2]);
  }
}

TEST_CASE(
    "rocks: actual cut provenance gives mixed old and fresh surfaces without geometry changes") {
  for (RockType type : {RockType::Granite, RockType::Basalt, RockType::Obsidian}) {
    auto file = recipe();
    file.boulder->type = type;
    file.boulder->geology.weathering = 1.0f;
    file.boulder->triangle_budget = 20000;
    auto& settings = *file.boulder->material.surface_maps;
    settings.variation = 0;
    settings.veins = 0;
    const auto original = solve(file);
    settings.rind = RockRind{};
    settings.rind->amount = 0;
    const auto disabled = solve(file);
    for (u32 i = 0; i < 3; ++i)
      CHECK(original.images[i].bytes == disabled.images[i].bytes);
    settings.rind->amount = 1;
    settings.rind->roughness = 1;
    const auto coated = solve(file);
    const auto again = solve(file);
    CHECK(original.indices == coated.indices);
    REQUIRE(original.positions.size() == coated.positions.size());
    CHECK(hash_bytes(original.positions.data(), original.positions.size() * sizeof(Vec3)) ==
          hash_bytes(coated.positions.data(), coated.positions.size() * sizeof(Vec3)));
    CHECK(coated.images[0].bytes == again.images[0].bytes);
    const auto roughness = decode(coated, 1);
    u8 low = 255, high = 0;
    for (u32 p = 0; p < roughness.pixels.size(); p += 3) {
      low = std::min(low, roughness.pixels[p + 1]);
      high = std::max(high, roughness.pixels[p + 1]);
    }
    CAPTURE(static_cast<u32>(type));
    CHECK(high > 245);
    CHECK(static_cast<u32>(high) - low > 30);
    file.boulder->geology.weathering = 0;
    const auto zero_exposure = solve(file);
    settings.rind.reset();
    const auto no_rind = solve(file);
    for (u32 i = 0; i < 3; ++i)
      CHECK(zero_exposure.images[i].bytes == no_rind.images[i].bytes);
  }
}

TEST_CASE("rocks: rind values reject nonfinite and out-of-range input") {
  auto file = recipe();
  file.boulder->material.surface_maps->rind = RockRind{};
  auto& rind = *file.boulder->material.surface_maps->rind;
  std::string error;
  for (f32 bad :
       {-1.0f, 3.0f, std::numeric_limits<f32>::quiet_NaN(), std::numeric_limits<f32>::infinity()}) {
    rind.amount = bad;
    CHECK_FALSE(rock_file_in_range(file, error));
    rind = RockRind{};
    rind.roughness = bad;
    CHECK_FALSE(rock_file_in_range(file, error));
    rind = RockRind{};
    rind.grain_scale = bad;
    CHECK_FALSE(rock_file_in_range(file, error));
    rind = RockRind{};
    rind.tint = std::array<f32, 3>{0.5f, bad, 0.5f};
    CHECK_FALSE(rock_file_in_range(file, error));
    rind = RockRind{};
  }
}
