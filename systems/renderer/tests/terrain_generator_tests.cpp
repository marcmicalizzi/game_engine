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
  REQUIRE(terrain_provider_known(dunes_desc(0.0)));
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

TEST_CASE("renderer: a terrain names its ground provider, and the generator enum reads as one") {
  // ADR-0046: `provider` names the ground by the name it registered under; `generator` is read as
  // the provider it always meant, so a file written either way is the same terrain and the same
  // cache entry.
  const test::TempDir tmp("renderer_terrain_provider");
  std::string error;
  const auto scene = [](const std::string& fields) {
    return std::string(R"({"format":"engine.scene.v1","terrain":{"size":33,"extent":40,"seed":4,)"
                       R"("dune_height":2,"time":86400)") +
           fields + "}}";
  };
  const std::string by_name = tmp.file("by_name.json");
  const std::string by_enum = tmp.file("by_enum.json");
  REQUIRE(write_text(by_name, scene(R"(,"provider":"dunes")")));
  REQUIRE(write_text(by_enum, scene(R"(,"generator":"Dunes")")));
  SceneDesc a, b;
  REQUIRE_MESSAGE(read_scene_file(by_name, a, error), error);
  REQUIRE_MESSAGE(read_scene_file(by_enum, b, error), error);
  CHECK(terrain_provider(a.terrain) == "dunes");
  CHECK(terrain_provider(b.terrain) == "dunes");
  CHECK(a.terrain.generator == TerrainGenerator::dunes);
  CHECK(terrain_hash(a.terrain) == terrain_hash(b.terrain));
  CHECK(terrain_moves(a.terrain));
  CHECK(terrain_has_rings(a.terrain));
  // The waves by name are the default.
  const std::string waves = tmp.file("waves.json");
  REQUIRE(write_text(waves, scene(R"(,"provider":"waves")")));
  SceneDesc w;
  REQUIRE_MESSAGE(read_scene_file(waves, w, error), error);
  CHECK(w.terrain.generator == TerrainGenerator::waves);
  CHECK_FALSE(terrain_moves(w.terrain));
  TerrainDesc plain = w.terrain;
  plain.provider.clear();
  CHECK(terrain_hash(plain) == terrain_hash(w.terrain));
  // Two answers to one question are refused, and so is a name this executable does not carry,
  // with the registry's sentence.
  const std::string both = tmp.file("both.json");
  REQUIRE(write_text(both, scene(R"(,"generator":"Dunes","provider":"waves")")));
  SceneDesc refused;
  CHECK_FALSE(read_scene_file(both, refused, error));
  CHECK(error.find("name one") != std::string::npos);
  const std::string unknown = tmp.file("unknown.json");
  REQUIRE(write_text(unknown, scene(R"(,"provider":"snow")")));
  CHECK_FALSE(read_scene_file(unknown, refused, error));
  CHECK(error.find("names the ground provider \"snow\", which this build does not have") !=
        std::string::npos);
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
  REQUIRE(sampler.moves());
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
  CHECK_FALSE(w.moves());
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
  // A stylized band (terrain.md, "How far the big dunes move"): read, in the hash only when it is
  // not the physical rate, travelling its scale times as far, and refused at zero.
  const std::string styled = tmp.file("styled.json");
  REQUIRE(write_text(styled, scene(R"(,"bands":[{"name":"big","height_min":20,"height_max":40,)"
                                   R"("cell":600,"share":0.8,"celerity_scale":1}])")));
  SceneDesc one;
  REQUIRE_MESSAGE(read_scene_file(styled, one, error), error);
  CHECK(one.terrain.bands[0].celerity_scale == 1.0f);
  TerrainDesc fast = one.terrain;
  fast.bands[0].celerity_scale = 10.0f;
  CHECK(terrain_hash(fast) != terrain_hash(one.terrain));
  const TerrainSampler slow_sampler(one.terrain);
  const TerrainSampler fast_sampler(fast);
  const f64 year = 365.0 * 86'400.0;
  CHECK(terrain_band_travel_m(fast_sampler, 0.0, year) ==
        doctest::Approx(10.0 * terrain_band_travel_m(slow_sampler, 0.0, year)).epsilon(1e-6));
  const std::string still = tmp.file("still.json");
  REQUIRE(write_text(still, scene(R"(,"bands":[{"name":"big","height_min":20,"height_max":40,)"
                                  R"("cell":600,"share":0.8,"celerity_scale":0}])")));
  CHECK_FALSE(read_scene_file(still, desc, error));
  CHECK(error.find("celerity_scale") != std::string::npos);
}

TEST_CASE(
    "renderer: a time-lapse field is the mesh's heights at its time, on any number of threads") {
  const TerrainDesc desc = dunes_desc(1'000.0);
  const TerrainSampler sampler(desc);
  const TerrainLattice lattice = terrain_scene_lattice(desc);
  const f64 at = 1'000.0 + 3.0 * 86'400.0;
  const u32 n = desc.size;
  const u32 blocks = terrain_window_blocks(n, n);
  jobs::JobSystem one(jobs::JobSystemConfig{.performance_workers = 1, .pin_threads = false});
  jobs::JobSystem three(jobs::JobSystemConfig{.performance_workers = 3, .pin_threads = false});
  jobs::JobSystem* pools[3] = {nullptr, &one, &three};
  u64 hashes[3] = {};
  Vector<f32> heights[3];
  for (u32 p = 0; p < 3; ++p) {
    heights[p].resize(n * n);
    const std::span<f32> out(heights[p].data(), heights[p].size());
    if (pools[p] == nullptr) {
      REQUIRE(evaluate_terrain_window(sampler, at, lattice, 0, 0, n, n, 0, blocks, out));
    } else {
      pools[p]->parallel_for(jobs::Pool::Performance, blocks, 1, [&](u32 b, u32 e) {
        evaluate_terrain_window(sampler, at, lattice, 0, 0, n, n, b, e, out);
      });
    }
    hashes[p] = hash_bytes(heights[p].data(), heights[p].size() * sizeof(f32));
  }
  CHECK(hashes[1] == hashes[0]);
  CHECK(hashes[2] == hashes[0]);
  // The mesh built for the description at that time has the same heights to the bit, and the
  // dunes have moved in three days.
  TerrainDesc then = desc;
  then.time_s = at;
  Vector<Vec3> positions;
  Vector<Vec3> before;
  Vector<u32> indices;
  Vector<Vec2> uvs;
  std::string error;
  REQUIRE(build_terrain_mesh(then, positions, indices, uvs, &error));
  REQUIRE(build_terrain_mesh(desc, before, indices, uvs, &error));
  u32 differ = 0;
  u32 moved = 0;
  for (u32 v = 0; v < positions.size(); ++v) {
    differ += positions[v].y != heights[0][v];
    moved += before[v].y != positions[v].y;
  }
  CHECK(differ == 0u);
  CHECK(moved > 0u);
  // The scene lattice's points are the mesh's, and a ring's are the world's grid at its spacing:
  // a window of either is the generator's heights at those points, the ridges and basins added.
  const TerrainSampler at_then(then);
  const TerrainLattice ring = terrain_ring_lattice(500);
  Vector<f32> window(9 * 7);
  REQUIRE(evaluate_terrain_window(sampler, at, ring, -40, 30, 9, 7, 0, terrain_window_blocks(9, 7),
                                  std::span<f32>(window.data(), window.size())));
  u32 off = 0;
  for (u32 j = 0; j < 7; ++j) {
    for (u32 i = 0; i < 9; ++i) {
      const f32 x = static_cast<f32>(-40 + static_cast<i32>(i)) * 0.5f;
      const f32 z = static_cast<f32>(30 + static_cast<i32>(j)) * 0.5f;
      CHECK(ring.x(-40 + static_cast<i32>(i)) == x);
      off += window[j * 9 + i] != at_then.height(x, z);
    }
  }
  CHECK(off == 0u);
}

TEST_CASE("renderer: the cadence rule keeps the fastest band within a fraction of a sample") {
  const TerrainDesc desc = dunes_desc(1'000.0);
  const TerrainSampler sampler(desc);
  const f64 year = 365.0 * 86'400.0;
  const f64 from = 1'000.0 + 40.0 * 86'400.0;
  for (const f64 spacing : {0.5, 1.0, 1.5}) {
    for (const f64 fraction : {0.25, 0.1}) {
      const f64 bound = fraction * spacing;
      const f64 at = terrain_next_time(sampler, from, spacing, fraction, 1.0, year);
      CAPTURE(spacing);
      CAPTURE(fraction);
      CHECK(at > from);
      // Within the bound, and the longest step that is, to a second.
      CHECK(terrain_band_travel_m(sampler, from, at) <= bound);
      CHECK(terrain_band_travel_m(sampler, from, at + 1.0) > bound);
    }
  }
  // A smaller share of a sample is a shorter step, and the clamps hold.
  const f64 quarter = terrain_next_time(sampler, from, 1.0, 0.25, 1.0, year) - from;
  const f64 tenth = terrain_next_time(sampler, from, 1.0, 0.1, 1.0, year) - from;
  CHECK(tenth < quarter);
  CHECK(terrain_next_time(sampler, from, 1.0, 0.25, quarter * 2.0, year) == from + quarter * 2.0);
  CHECK(terrain_next_time(sampler, from, 1.0, 0.25, 1.0, quarter / 2.0) == from + quarter / 2.0);
  // Storms move sand faster, so the same stretch of the record takes more fields: count the steps
  // across sixty days with and without them.
  TerrainDesc stormy = desc;
  stormy.storms_per_year = 31;
  stormy.storm_strength = 3.0f;
  const TerrainSampler storm_sampler(stormy);
  const auto steps = [&](const TerrainSampler& s) {
    u32 count = 0;
    for (f64 t = from; t < from + 60.0 * 86'400.0;
         t = terrain_next_time(s, t, 1.5, 0.25, 1.0, year))
      ++count;
    return count;
  };
  CHECK(steps(storm_sampler) > steps(sampler));
}

namespace {

// A dune profile travelling downwind at `speed` metres a game second, sampled on a line of `n`
// points `spacing` apart: 2 m tall, 12 m from crest to crest, with a sharp brink — the kind of
// shape whose straight-line blend between two times is worst.
Vector<f32> travelling_field(f64 time_s, f64 speed, u32 n, f64 spacing) {
  Vector<f32> out(n);
  for (u32 i = 0; i < n; ++i) {
    const f64 x = static_cast<f64>(i) * spacing - speed * time_s;
    const f64 u = x / 12.0 - std::floor(x / 12.0);  // 0..1 along one wavelength
    out[i] = static_cast<f32>(u < 0.8 ? 2.0 * u / 0.8 : 2.0 * (1.0 - u) / 0.2);
  }
  return out;
}

struct BlendRun {
  f64 max_move = 0.0;      // the most any sample changed between two consecutive frames
  f64 max_reported = 0.0;  // the most the blend said it moved
  f64 worst_excess = 0.0;  // how far a frame's real change passed what the blend said
  u32 held = 0;
  u32 capped = 0;
  u32 installed = 0;
  f64 final_lag = 0.0;
  bool monotonic = true;
};

// Drives `terrain_blend_frame` a frame at a time at `rate` game seconds a real second (a frame a
// sixtieth of a second), with fields timed by displacement (`step` game seconds apart) and ready
// `latency` frames after they are asked for — and none at all for `stall` frames from frame
// `stall_at` — and checks the heights the pool pass would draw, sample by sample.
BlendRun run_blend(f64 rate, f64 speed, f64 step, u32 latency, u32 stall_at, u32 stall, u32 frames,
                   f64 budget) {
  constexpr u32 n = 256;
  constexpr f64 spacing = 0.5;
  gfx::TerrainField window{};
  window.nx = n;
  window.nz = 1;
  const f64 t0 = 1'000.0;
  TerrainBlend blend;
  blend.time_a = blend.time_b = blend.surface_s = t0;
  Vector<f32> a = travelling_field(t0, speed, n, spacing);
  Vector<f32> b = a;
  Vector<f32> latest = a;
  f64 latest_time = t0;
  TerrainNextField next;
  Vector<f32> next_field;
  bool pending = false;
  u32 ready_at = 0;
  Vector<f32> previous = a;
  BlendRun run;
  f64 last_surface = t0;
  for (u32 f = 1; f <= frames; ++f) {
    const f64 target = t0 + rate * static_cast<f64>(f) / 60.0;
    const bool stalled = f >= stall_at && f < stall_at + stall;
    if (!next.ready && !pending && !stalled) {
      pending = true;
      ready_at = f + latency;
    }
    if (pending && f >= ready_at && !stalled) {
      const f64 at = latest_time + step;
      next_field = travelling_field(at, speed, n, spacing);
      next.ready = true;
      next.time_s = at;
      next.delta_m = terrain_field_delta(std::span<const f32>(latest.data(), n), window,
                                         std::span<const f32>(next_field.data(), n), window);
      latest = next_field;
      latest_time = at;
      pending = false;
    }
    const bool had_b = blend.has_b;
    const TerrainFrameResult result = terrain_blend_frame(blend, target, budget, next);
    if (result.installed > 0) {
      if (had_b) a = b;
      b = next_field;
    }
    run.held += result.held ? 1u : 0u;
    run.capped += result.capped ? 1u : 0u;
    run.installed += result.installed;
    run.monotonic = run.monotonic && blend.surface_s >= last_surface;
    last_surface = blend.surface_s;
    // What the pool pass draws: `a (1 - t) + b t` in floats, as deform.slang writes it.
    const f32 t = static_cast<f32>(blend.blend());
    f64 frame_move = 0.0;
    for (u32 i = 0; i < n; ++i) {
      const f32 h = blend.has_b ? a[i] * (1.0f - t) + b[i] * t : a[i];
      frame_move =
          std::max(frame_move, std::abs(static_cast<f64>(h) - static_cast<f64>(previous[i])));
      previous[i] = h;
    }
    run.max_move = std::max(run.max_move, frame_move);
    run.max_reported = std::max(run.max_reported, result.moved_m);
    run.worst_excess = std::max(run.worst_excess, frame_move - result.moved_m);
    run.final_lag = target - blend.surface_s;
  }
  return run;
}

}  // namespace

TEST_CASE("renderer: the blend never moves a sample more than its bound in a frame") {
  // A quarter of the 0.5 m spacing a frame, the default. The profile moves 0.9 m a game day — the
  // erg's waves, the fastest band — so the cadence rule times fields a quarter of a sample of
  // travel apart: 0.125 / (0.9 / 86,400) = 12,000 game seconds.
  const f64 budget = 0.25 * 0.5;
  const f64 speed = 0.9 / 86'400.0;
  const f64 step = budget / speed;
  // Float rounding of a blend of two heights of 2 m: a few ulps.
  const f64 slack = 1.0e-5;
  struct Case {
    const char* what;
    f64 rate;
    u32 latency;
    u32 stall_at;
    u32 stall;
  };
  const Case cases[] = {
      {"a real clock, fields on time", 1.0, 0, 0, 0},
      {"a game day a second, fields on time", 86'400.0, 0, 0, 0},
      {"a game day a second, every field 20 frames late", 86'400.0, 20, 0, 0},
      {"a game week a second, every field 45 frames late", 604'800.0, 45, 0, 0},
      {"a game day a second, no field for 400 frames", 86'400.0, 2, 100, 400},
      {"a game month a second, fields on time", 2'592'000.0, 0, 0, 0},
  };
  for (const Case& c : cases) {
    CAPTURE(c.what);
    const BlendRun run =
        run_blend(c.rate, speed, step, c.latency, c.stall_at, c.stall, 1'200, budget);
    // No sample ever changes by more than the bound between two frames — not when a field is late,
    // not when none comes for seconds and the surface then catches up — and never by more than
    // the blend says it moved, which is the step the bound is kept on.
    CHECK(run.max_move <= budget + slack);
    CHECK(run.worst_excess <= slack);
    CHECK(run.max_reported <= budget + 1.0e-12);
    CHECK(run.monotonic);
    CHECK(run.installed > 0u);
    if (c.stall > 0 || c.latency > 0 || c.rate > 86'400.0) {
      CHECK(run.held + run.capped > 0u);  // it had to wait or slow down, and did
    }
  }
  // At a real clock the fields are hours apart, the surface keeps game time exactly and nothing
  // stops it; at a day a second with fields on time it keeps game time within a frame.
  const BlendRun still = run_blend(1.0, speed, step, 0, 0, 0, 600, budget);
  CHECK(still.held == 0u);
  CHECK(still.capped == 0u);
  CHECK(still.final_lag == 0.0);
  const BlendRun day = run_blend(86'400.0, speed, step, 0, 0, 0, 600, budget);
  CHECK(day.final_lag <= 86'400.0 / 60.0);
  CHECK(day.max_move > 0.0);
}

TEST_CASE("renderer: a blend step is exact at its ends and never goes past b") {
  TerrainBlend blend;
  blend.time_a = 10.0;
  blend.time_b = 20.0;
  blend.has_b = true;
  blend.delta_m = 2.0;
  blend.surface_s = 10.0;
  // Unbounded: straight to the target, and no further than b.
  TerrainBlendStep step = terrain_blend_step(blend, 15.0, 100.0);
  CHECK(step.surface_s == 15.0);
  CHECK(step.moved_m == doctest::Approx(1.0));
  CHECK_FALSE(step.at_b);
  step = terrain_blend_step(blend, 99.0, 100.0);
  CHECK(step.surface_s == 20.0);
  CHECK(step.at_b);
  // Bounded: a budget of 0.5 m is a quarter of the pair's 2 m, so a quarter of its ten seconds.
  step = terrain_blend_step(blend, 99.0, 0.5);
  CHECK(step.surface_s == doctest::Approx(12.5));
  CHECK(step.moved_m == doctest::Approx(0.5));
  // Never backwards.
  blend.surface_s = 18.0;
  step = terrain_blend_step(blend, 12.0, 100.0);
  CHECK(step.surface_s == 18.0);
  CHECK(step.moved_m == 0.0);
  // The blend itself: 0 at a, 1 at b, 0 with no b.
  blend.surface_s = 20.0;
  CHECK(blend.blend() == 1.0);
  blend.surface_s = 10.0;
  CHECK(blend.blend() == 0.0);
  blend.has_b = false;
  CHECK(blend.blend() == 0.0);
  // A frame that crosses a pair's end takes the next field and goes on with what is left.
  blend.has_b = true;
  blend.surface_s = 19.0;
  TerrainNextField next{true, 30.0, 2.0};
  const TerrainFrameResult result = terrain_blend_frame(blend, 25.0, 100.0, next);
  CHECK(result.installed == 1u);
  CHECK_FALSE(next.ready);
  CHECK(blend.time_a == 20.0);
  CHECK(blend.time_b == 30.0);
  CHECK(blend.surface_s == 25.0);
  CHECK(result.moved_m == doctest::Approx(0.2 + 1.0));
}

TEST_CASE("renderer: the terrain's levels share one surface time, each within its own bound") {
  // The scene's grid at a metre and a ring at 25 cm over the same travelling profile, each with
  // fields timed by its own cadence (a quarter of its own spacing of travel apart). The grid's
  // arrive two frames after they are asked for; every third of the ring's is 40 frames late.
  constexpr u32 n = 256;
  const f64 speed = 0.9 / 86'400.0;
  const f64 rate = 604'800.0;
  const f64 t0 = 1'000.0;
  const f64 slack = 1.0e-5;
  struct Level {
    f64 spacing = 0.0;
    f64 budget = 0.0;
    f64 step = 0.0;
    gfx::TerrainField window{};
    Vector<f32> a, b, latest, next_field, previous;
    f64 latest_time = 0.0;
    bool pending = false;
    u32 ready_at = 0;
    u32 asked = 0;
    u32 installed = 0;
    f64 max_move = 0.0;
  };
  Level levels[2];
  TerrainBlend blends[2];
  TerrainNextField next[2];
  for (u32 k = 0; k < 2; ++k) {
    Level& l = levels[k];
    l.spacing = k == 0 ? 1.0 : 0.25;
    l.budget = 0.25 * l.spacing;
    l.step = l.budget / speed;
    l.window.nx = n;
    l.window.nz = 1;
    l.a = travelling_field(t0, speed, n, l.spacing);
    l.b = l.a;
    l.latest = l.a;
    l.previous = l.a;
    l.latest_time = t0;
    blends[k].time_a = blends[k].time_b = blends[k].surface_s = t0;
  }
  u32 held_by_ring = 0;
  bool monotonic = true;
  f64 last = t0;
  for (u32 f = 1; f <= 900; ++f) {
    const f64 target = t0 + rate * static_cast<f64>(f) / 60.0;
    for (u32 k = 0; k < 2; ++k) {
      Level& l = levels[k];
      if (!next[k].ready && !l.pending) {
        l.pending = true;
        l.ready_at = f + (k == 0 ? 2u : (l.asked % 3 == 2 ? 40u : 1u));
        ++l.asked;
      }
      if (l.pending && f >= l.ready_at) {
        const f64 at = l.latest_time + l.step;
        l.next_field = travelling_field(at, speed, n, l.spacing);
        next[k].ready = true;
        next[k].time_s = at;
        next[k].delta_m =
            terrain_field_delta(std::span<const f32>(l.latest.data(), n), l.window,
                                std::span<const f32>(l.next_field.data(), n), l.window);
        l.latest = l.next_field;
        l.latest_time = at;
        l.pending = false;
      }
    }
    const bool had_b[2] = {blends[0].has_b, blends[1].has_b};
    const f64 budgets[2] = {levels[0].budget, levels[1].budget};
    f64 moved[2] = {};
    u32 installed[2] = {};
    const TerrainSurfaceResult result = terrain_surface_frame(
        std::span<TerrainBlend>(blends, 2), target, std::span<const f64>(budgets, 2),
        std::span<TerrainNextField>(next, 2), std::span<f64>(moved, 2),
        std::span<u32>(installed, 2));
    CHECK(blends[0].surface_s == blends[1].surface_s);
    CHECK(result.surface_s == blends[0].surface_s);
    monotonic = monotonic && result.surface_s >= last;
    last = result.surface_s;
    held_by_ring += result.held && result.limiting == 1 ? 1u : 0u;
    for (u32 k = 0; k < 2; ++k) {
      Level& l = levels[k];
      if (installed[k] > 0) {
        if (had_b[k]) l.a = l.b;
        l.b = l.next_field;
        l.installed += installed[k];
      }
      // What the pool pass draws for the level, against the level's own bound.
      const f32 t = static_cast<f32>(blends[k].blend());
      f64 frame_move = 0.0;
      for (u32 i = 0; i < n; ++i) {
        const f32 h = blends[k].has_b ? l.a[i] * (1.0f - t) + l.b[i] * t : l.a[i];
        frame_move =
            std::max(frame_move, std::abs(static_cast<f64>(h) - static_cast<f64>(l.previous[i])));
        l.previous[i] = h;
      }
      l.max_move = std::max(l.max_move, frame_move);
      CHECK(moved[k] <= l.budget + 1.0e-12);
    }
  }
  CHECK(monotonic);
  CHECK(held_by_ring > 0u);  // a late ring field held the grid too
  for (u32 k = 0; k < 2; ++k) {
    CAPTURE(k);
    CHECK(levels[k].installed > 1u);
    CHECK(levels[k].max_move <= levels[k].budget + slack);
  }
  // One level is the one-level blend exactly.
  TerrainBlend single;
  single.time_a = 10.0;
  single.time_b = 20.0;
  single.has_b = true;
  single.delta_m = 2.0;
  single.surface_s = 19.0;
  TerrainBlend shared = single;
  TerrainNextField n1{true, 30.0, 2.0};
  TerrainNextField n2 = n1;
  const TerrainFrameResult one = terrain_blend_frame(single, 25.0, 0.9, n1);
  f64 moved = 0.0;
  u32 installed = 0;
  const f64 budget = 0.9;
  const TerrainSurfaceResult many =
      terrain_surface_frame(std::span<TerrainBlend>(&shared, 1), 25.0,
                            std::span<const f64>(&budget, 1), std::span<TerrainNextField>(&n2, 1),
                            std::span<f64>(&moved, 1), std::span<u32>(&installed, 1));
  CHECK(shared.surface_s == single.surface_s);
  CHECK(shared.time_a == single.time_a);
  CHECK(shared.time_b == single.time_b);
  CHECK(installed == one.installed);
  CHECK(moved == doctest::Approx(one.moved_m));
  CHECK(many.capped == one.capped);
}
