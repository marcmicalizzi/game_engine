// A scene whose terrain names the dune generator (renderer.md, "The terrain"; terrain.md;
// ADR-0043). Compiled only where the terrain capability is, with no device: the scene file's
// version 2 fields read and a version 1 terrain read as the waves it always drew; a waves terrain's
// hash untouched by the new fields and a generator terrain's hash moving with its time; the
// sampler, the direct function and the mesh the same heights to the bit; the ground (what ruins
// stand on) the floor, which does not move while the dunes do; and, with the ruins capability, a
// scene's buildings standing on that ground.
#include <core/math/math.h>
#include <domain/terrain/dunes.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>

#if ENGINE_RENDERER_RUINS
#include <domain/ruins/assembler.h>
#include <domain/ruins/synthetic_kit.h>
#endif

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <fstream>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

TerrainDesc dunes_desc(f64 time_s) {
  TerrainDesc d;
  d.enabled = true;
  d.size = 97;
  d.extent = 120.0f;
  d.seed = 9;
  d.dune_height = 3.0f;
  d.dune_wavelength = 60.0f;
  d.ridges.push_back(TerrainRidge{Vec2{-100.0f, 60.0f}, Vec2{100.0f, 50.0f}, 12.0f, 25.0f, 0.15f});
  d.basins.push_back(TerrainBasin{Vec2{-40.0f, -50.0f}, 30.0f, 3.0f});
  d.generator = TerrainGenerator::dunes;
  d.time_s = time_s;
  return d;
}

}  // namespace

TEST_CASE("renderer: a scene's terrain names the dune generator, and an old one reads as before") {
  const test::TempDir tmp("renderer_terrain_generator");
  std::string error;
  REQUIRE(terrain_generator_available());
  const std::string path = tmp.file("dunes.json");
  REQUIRE(write_text(path, R"({"format":"engine.scene.v1","name":"dunes",)"
                           R"("terrain":{"size":33,"extent":40,"seed":4,"dune_height":2,)"
                           R"("generator":"Dunes","time":86400.5,"sand_flux":150}})"));
  SceneDesc desc;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  CHECK(desc.terrain.generator == TerrainGenerator::dunes);
  CHECK(desc.terrain.time_s == 86400.5);
  CHECK(desc.terrain.sand_flux == 150.0f);

  const std::string old_path = tmp.file("waves.json");
  REQUIRE(write_text(old_path, R"({"format":"engine.scene.v1","name":"waves",)"
                               R"("terrain":{"size":33,"extent":40,"seed":4,"dune_height":2}})"));
  SceneDesc old;
  REQUIRE_MESSAGE(read_scene_file(old_path, old, error), error);
  CHECK(old.terrain.generator == TerrainGenerator::waves);

  const std::string bad = tmp.file("bad.json");
  REQUIRE(write_text(bad, R"({"terrain":{"size":33,"extent":40,"generator":"Dunes","time":-5}})"));
  SceneDesc refused;
  CHECK_FALSE(read_scene_file(bad, refused, error));
  CHECK(error.find("time") != std::string::npos);
}

TEST_CASE("renderer: the generator's fields enter the terrain hash only when it is named") {
  TerrainDesc waves = dunes_desc(0.0);
  waves.generator = TerrainGenerator::waves;
  const u64 before = terrain_hash(waves);
  waves.time_s = 12345.0;  // ignored by the waves, and by their hash and cache entry
  waves.sand_flux = 17.0f;
  CHECK(terrain_hash(waves) == before);
  CHECK(terrain_hash(dunes_desc(0.0)) != before);
  CHECK(terrain_hash(dunes_desc(0.0)) != terrain_hash(dunes_desc(86400.0)));
  CHECK(terrain_hash(dunes_desc(86400.0)) == terrain_hash(dunes_desc(86400.0)));
}

TEST_CASE("renderer: the generator's sampler, direct function and mesh agree to the bit") {
  const TerrainDesc desc = dunes_desc(3.0 * 365.0 * 86400.0);
  Vector<Vec3> positions;
  Vector<u32> indices;
  Vector<Vec2> uvs;
  std::string error;
  REQUIRE_MESSAGE(build_terrain_mesh(desc, positions, indices, uvs, &error), error);
  REQUIRE(positions.size() == desc.size * desc.size);
  const TerrainSampler sampler(desc);
  REQUIRE(sampler.dunes_field() != nullptr);
  u32 bad = 0;
  f32 lo = 1e9f, hi = -1e9f;
  for (const Vec3& p : positions) {
    bad += sampler.height(p.x, p.z) != p.y;
    lo = std::min(lo, p.y);
    hi = std::max(hi, p.y);
  }
  CHECK(bad == 0);
  for (u32 k = 0; k < positions.size(); k += 97)
    CHECK(terrain_height(desc, positions[k].x, positions[k].z) == positions[k].y);
  MESSAGE("dune generator terrain: " << lo << " .. " << hi << " m");
  // The ridge stands above the sand and the basin sinks below it: the features are the waves'.
  CHECK(sampler.height(0.0f, 55.0f) > 8.0f);
  CHECK(sampler.height(-40.0f, -50.0f) < -1.0f);
  // The generator's own sand, height for height (its function, plus the ridges and basins here).
  f32 ridge_mask = 0.0f, flatten = 1.0f;
  const f32 features = sampler.features(10.0f, -20.0f, ridge_mask, flatten);
  const terrain::DuneField field([&] {
    terrain::FieldDesc f;
    f.seed = 9;
    f.wind.seed = 9;
    f.dune_height = 3000;
    f.wavelength = 60'000;
    f.ridges.push_back(terrain::RidgeFeature{-100'000, 60'000, 100'000, 50'000, 25'000});
    f.basins.push_back(terrain::BasinFeature{-40'000, -50'000, 30'000});
    return f;
  }());
  const i64 t_us = static_cast<i64>(desc.time_s * 1e6);
  CHECK(sampler.height(10.0f, -20.0f) ==
        terrain::height_m(field.height_um(10'000, -20'000, t_us)) + features);
}

TEST_CASE("renderer: the dunes move with time and the ground does not") {
  const TerrainDesc now = dunes_desc(100.0 * 86400.0);
  const TerrainDesc later = dunes_desc(400.0 * 86400.0);
  const TerrainSampler a(now);
  const TerrainSampler b(later);
  u32 moved = 0, ground_moved = 0;
  for (f32 x = -100.0f; x <= 100.0f; x += 7.5f) {
    for (f32 z = -100.0f; z <= 100.0f; z += 7.5f) {
      moved += a.height(x, z) != b.height(x, z);
      ground_moved += a.ground(x, z) != b.ground(x, z);
    }
  }
  MESSAGE(moved << " of 729 points moved in 300 days");
  CHECK(moved > 300);
  CHECK(ground_moved == 0);
  // With the waves the ground is the surface, as it always was.
  TerrainDesc waves = now;
  waves.generator = TerrainGenerator::waves;
  const TerrainSampler w(waves);
  CHECK(w.dunes_field() == nullptr);
  CHECK(w.ground(12.0f, 3.0f) == w.height(12.0f, 3.0f));
}

#if ENGINE_RENDERER_RUINS
TEST_CASE("renderer: ruins over the dune generator stand on its ground") {
  const test::TempDir tmp("renderer_terrain_generator_ruins");
  std::string error;
  std::string kit_path;
  REQUIRE_MESSAGE(
      ruins::write_synthetic_kit(tmp.file("kit"), ruins::SyntheticKitOptions{}, &error, &kit_path),
      error);
  const auto scene = [](const char* time) {
    return std::string(R"({"format":"engine.scene.v1","name":"ruins-on-dunes",)"
                       R"("terrain":{"size":65,"extent":60,"seed":3,"dune_height":2,)"
                       R"("generator":"Dunes","time":)") +
           time +
           R"(},"ruins":[{"name":"town","kit":"kit/kit.json","seed":11,"tile_size":24,)"
           R"("tile_min":[-1,-1],"tile_max":[0,0],"count":4,"wind_deg":45}]})";
  };
  const std::string early = tmp.file("early.json");
  const std::string late = tmp.file("late.json");
  REQUIRE(write_text(early, scene("0")));
  REQUIRE(write_text(late, scene("31536000")));  // a year on
  SceneDesc a, b;
  REQUIRE_MESSAGE(read_scene_file(early, a, error), error);
  REQUIRE_MESSAGE(read_scene_file(late, b, error), error);
  CHECK(a.ruin_buildings == 4);
  // The same buildings a year apart, to the bit: they stand on the floor, which does not move.
  REQUIRE(a.instances.size() == b.instances.size());
  for (u32 i = 0; i + 1 < a.instances.size(); ++i)
    CHECK(a.instances[i].transform == b.instances[i].transform);
  // And on the ground: nothing of a building floats above the floor at its origin.
  const TerrainSampler ground(a.terrain);
  u32 floating = 0;
  for (u32 i = 0; i + 1 < a.instances.size(); ++i) {
    const Vec3 p = a.instances[i].transform.position;
    floating += p.y > ground.ground(p.x, p.z) + 1.0f;
  }
  CHECK(floating == 0);
}
#endif
