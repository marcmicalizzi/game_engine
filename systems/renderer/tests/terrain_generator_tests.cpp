// A scene whose terrain names the dune generator (renderer.md, "The terrain"; terrain.md;
// ADR-0043). Compiled only where the terrain capability is, with no device: the scene file's
// version 2 fields read and a version 1 terrain read as the waves it always drew; a waves terrain's
// hash untouched by the new fields and a generator terrain's hash moving with its time; the
// sampler, the direct function and the mesh the same heights to the bit; the ground (what ruins
// stand on) the floor, which does not move while the dunes do; and, with the ruins capability, a
// scene's buildings standing on that ground.
#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/math/math.h>
#include <domain/terrain/dunes.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/terrain_time.h>

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

TEST_CASE("renderer: a terrain's band table reads, hashes, and is validated") {
  const test::TempDir tmp("renderer_terrain_bands");
  std::string error;
  const auto scene = [](const std::string& bands) {
    return std::string(R"({"format":"engine.scene.v1","terrain":{"size":33,"extent":400,"seed":4,)"
                       R"("generator":"Dunes","time":1000)") +
           bands + "}}";
  };
  const std::string good = tmp.file("good.json");
  REQUIRE(write_text(
      good, scene(R"(,"bands":[{"name":"big","height_min":20,"height_max":40,)"
                  R"("cell":600,"share":0.8,"sinuosity":0.05},)"
                  R"({"name":"small","kind":"Barchan","height_min":1,"height_max":3,)"
                  R"("cell":80,"share":0.4,"couple":"Floors","couple_width":80,"far":false}])")));
  SceneDesc desc;
  REQUIRE_MESSAGE(read_scene_file(good, desc, error), error);
  REQUIRE(desc.terrain.has_bands);
  REQUIRE(desc.terrain.bands.size() == 2);
  CHECK(desc.terrain.bands[1].kind == 1);
  CHECK(desc.terrain.bands[1].couple == 2);
  CHECK_FALSE(desc.terrain.bands[1].far);
  // The table is part of what the terrain is: its hash and so its cache entry.
  TerrainDesc without = desc.terrain;
  without.has_bands = false;
  without.bands.clear();
  CHECK(terrain_hash(without) != terrain_hash(desc.terrain));
  // And it is what the sampler draws: a 20-40 m band stands where the default's 3 m field does not.
  const TerrainSampler with_bands(desc.terrain);
  const TerrainSampler default_bands(without);
  f32 tallest = 0.0f, tallest_default = 0.0f;
  for (f32 x = -400.0f; x <= 400.0f; x += 10.0f) {
    for (f32 z = -400.0f; z <= 400.0f; z += 10.0f) {
      tallest = std::max(tallest, with_bands.height(x, z));
      tallest_default = std::max(tallest_default, default_bands.height(x, z));
    }
  }
  MESSAGE("tallest with the table " << tallest << " m, with the default " << tallest_default
                                    << " m");
  CHECK(tallest > 15.0f);
  CHECK(tallest_default < 6.0f);

  const std::string empty = tmp.file("empty.json");
  REQUIRE(write_text(empty, scene(R"(,"bands":[])")));
  CHECK_FALSE(read_scene_file(empty, desc, error));
  CHECK(error.find("empty") != std::string::npos);
  const std::string zero = tmp.file("zero.json");
  REQUIRE(write_text(
      zero,
      scene(R"(,"bands":[{"name":"none","height_min":1,"height_max":2,"cell":90,"share":0}])")));
  CHECK_FALSE(read_scene_file(zero, desc, error));
  CHECK(error.find("zero share") != std::string::npos);
  const std::string small = tmp.file("small.json");
  REQUIRE(write_text(
      small, scene(R"(,"bands":[{"name":"tight","height_min":20,"height_max":40,"cell":50}])")));
  CHECK_FALSE(read_scene_file(small, desc, error));
  CHECK(error.find("smaller than its own dune") != std::string::npos);
}

namespace {

// Keeps every grid a time-lapse hands over, with its time.
class KeepHeights final : public TerrainHeightSink {
 public:
  void heights(f64 time_s, std::span<const f32> h) override {
    times.push_back(time_s);
    grids.push_back(Vector<f32>(h.begin(), h.end()));
  }
  Vector<f64> times;
  Vector<Vector<f32>> grids;
};

}  // namespace

TEST_CASE("renderer: the time-lapse's step rule fires exactly when it should") {
  // The rule alone: a boundary crossed since the last one evaluated, enough frames since the last
  // start, and nothing in flight.
  CHECK(time_lapse_due(0, 0.9, 1.0, 5, 3, false) == -1);  // no boundary crossed
  CHECK(time_lapse_due(0, 1.0, 1.0, 5, 3, false) == 1);   // exactly on one
  CHECK(time_lapse_due(0, 3.7, 1.0, 5, 3, false) == 3);   // several: the last
  CHECK(time_lapse_due(0, 3.7, 1.0, 2, 3, false) == -1);  // too soon
  CHECK(time_lapse_due(0, 3.7, 1.0, 5, 3, true) == -1);   // one in flight
  CHECK(time_lapse_due(3, 3.9, 1.0, 5, 3, false) == -1);  // already evaluated

  // Driven a frame at a time: half a game day a frame, a step of a day, three frames apart at
  // least. Boundaries fall on every other frame, so the frame spacing decides which are
  // evaluated: day 1 on frame 2, day 2 on frame 5 (frame 4 is too soon), day 4 on frame 8 (day 3's
  // boundary came and went while waiting) — each at the boundary's own time, not the frame's.
  const TerrainDesc desc = dunes_desc(1'000.0);
  TimeLapseConfig config;
  config.rate = 0.5 * 86'400.0 * 60.0;  // game seconds per real second, at 1/60 s a frame
  config.step_s = 86'400.0;
  config.min_frames = 3;
  TerrainTimeLapse lapse;
  std::string error;
  REQUIRE(lapse.start(desc, config, nullptr, &error));
  KeepHeights keep;
  Vector<u32> started_on;
  for (u32 frame = 1; frame <= 12; ++frame) {
    if (lapse.tick(1.0 / 60.0, keep)) started_on.push_back(frame);
  }
  REQUIRE(started_on.size() == 4u);
  CHECK(started_on[0] == 2u);
  CHECK(started_on[1] == 5u);
  CHECK(started_on[2] == 8u);
  CHECK(started_on[3] == 11u);
  REQUIRE(keep.times.size() == 4u);
  CHECK(keep.times[0] == 1'000.0 + 86'400.0);
  CHECK(keep.times[1] == 1'000.0 + 2 * 86'400.0);
  CHECK(keep.times[2] == 1'000.0 + 4 * 86'400.0);
  CHECK(keep.times[3] == 1'000.0 + 5 * 86'400.0);
  // A waves terrain has no time to run, and a rate of zero is not a time-lapse.
  TerrainDesc waves = desc;
  waves.generator = TerrainGenerator::waves;
  TerrainTimeLapse refused;
  CHECK_FALSE(refused.start(waves, config, nullptr, &error));
  config.rate = 0.0;
  CHECK_FALSE(refused.start(desc, config, nullptr, &error));
}

TEST_CASE("renderer: a time-lapse's grid is the mesh's at that time, on any number of threads") {
  const TerrainDesc desc = dunes_desc(1'000.0);
  TimeLapseConfig config;
  config.rate = 3.0 * 86'400.0 * 60.0;  // three game days a frame
  config.step_s = 86'400.0;
  config.min_frames = 1;
  u64 hashes[3] = {};
  f64 times[3] = {};
  jobs::JobSystem one(jobs::JobSystemConfig{.performance_workers = 1, .pin_threads = false});
  jobs::JobSystem three(jobs::JobSystemConfig{.performance_workers = 3, .pin_threads = false});
  jobs::JobSystem* pools[3] = {nullptr, &one, &three};
  for (u32 p = 0; p < 3; ++p) {
    TerrainTimeLapse lapse;
    std::string error;
    REQUIRE(lapse.start(desc, config, pools[p], &error));
    KeepHeights keep;
    lapse.tick(1.0 / 60.0, keep);
    lapse.finish(keep);
    REQUIRE(keep.grids.size() == 1u);
    times[p] = keep.times[0];
    hashes[p] = hash_bytes(keep.grids[0].data(), keep.grids[0].size() * sizeof(f32));
    if (p == 0) {
      // The mesh built for a description at that time has the same heights to the bit.
      TerrainDesc at = desc;
      at.time_s = keep.times[0];
      Vector<Vec3> positions;
      Vector<u32> indices;
      Vector<Vec2> uvs;
      REQUIRE(build_terrain_mesh(at, positions, indices, uvs, &error));
      u32 differ = 0;
      for (u32 v = 0; v < positions.size(); ++v)
        differ += positions[v].y != keep.grids[0][v];
      CHECK(differ == 0u);
      // And the dunes have moved in three days.
      Vector<Vec3> before;
      REQUIRE(build_terrain_mesh(desc, before, indices, uvs, &error));
      u32 moved = 0;
      for (u32 v = 0; v < before.size(); ++v)
        moved += before[v].y != positions[v].y;
      CHECK(moved > 0u);
    }
  }
  CHECK(times[0] == 1'000.0 + 3 * 86'400.0);
  CHECK(hashes[1] == hashes[0]);
  CHECK(hashes[2] == hashes[0]);
}
