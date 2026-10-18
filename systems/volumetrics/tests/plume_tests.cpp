// The crest plumes on the CPU, with no device (docs/subsystems/volumetrics.md, "The crest plumes"):
// the terrain's crest query against `evaluate_tile`, and a plume's shape, determinism and density.

#include <core/containers/vector.h>
#include <domain/terrain/crests.h>
#include <domain/terrain/dunes.h>
#include <domain/terrain/scene_ground.h>
#include <domain/terrain/tile.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>
#include <systems/volumetrics/volumetrics.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>

using namespace engine;
using namespace engine::volumetrics;

TEST_CASE("volumetrics: the crest query is evaluate_tile's crest lines and wind, bit for bit") {
  const std::string path =
      std::string(ENGINE_SOURCE_DIR) + "/content/test-scenes/desert-erg/scene.json";
  renderer::SceneDesc scene;
  std::string error;
  REQUIRE_MESSAGE(renderer::read_scene_file(path, scene, error), error);
  renderer::TerrainSampler ground(scene.terrain);
  REQUIRE_MESSAGE(ground.ok(), ground.error());
  const terrain::DuneField* field = terrain::dune_field(ground.provider());
  REQUIRE(field != nullptr);
  terrain::TileOptions options;
  options.tile_mm = k_plume_tile_mm;
  options.cells = 4;
  options.normals = false;
  terrain::CrestScratch scratch;
  Vector<terrain::CrestLine> crests;
  u32 total = 0;
  for (const i64 time_us : {i64{94'608'000'000'000}, i64{94'665'600'000'000}}) {
    for (const terrain::TileCoord tile :
         {terrain::TileCoord{0, 0}, terrain::TileCoord{-7, 3}, terrain::TileCoord{40, -12}}) {
      terrain::TileOutput out;
      terrain::evaluate_tile(*field, tile, time_us, options, nullptr, nullptr, out);
      terrain::TileWind wind;
      terrain::tile_crests(*field, tile, time_us, options, nullptr, scratch, crests, wind);
      REQUIRE(crests.size() == out.crests.size());
      total += crests.size();
      CHECK(std::memcmp(crests.data(), out.crests.data(),
                        sizeof(terrain::CrestLine) * crests.size()) == 0);
      CHECK(wind.direction.x == out.wind.direction.x);
      CHECK(wind.direction.y == out.wind.direction.y);
      CHECK(wind.speed_mps == out.wind.speed_mps);
      CHECK(wind.saltation_m2_per_day == out.wind.saltation_m2_per_day);
      CHECK(wind.storm == out.wind.storm);
    }
  }
  MESSAGE("crest lines compared: " << total);
  CHECK(total > 0);
}

TEST_CASE("volumetrics: a plume leaves its brink downwind, rises, spreads and fades") {
  terrain::CrestLine crest;
  for (u32 k = 0; k < terrain::k_crest_points; ++k)
    crest.points[k] = Vec3{-8.0f + 4.0f * static_cast<f32>(k), 5.0f, 100.0f};  // along x, 5 m up
  crest.height_m = 4.0f;
  crest.sharpness = 1.0f;
  terrain::TileWind wind;
  wind.direction = Vec2{0.0f, 1.0f};  // towards +z
  wind.speed_mps = 8.0f;
  wind.saltation_m2_per_day = k_plume_mean_saltation;
  const WorldPos eye{0.0, 1.0, 0.0};
  PlumeSegment segments[4];
  REQUIRE(crest_plume_segments(crest, wind, eye, 1.0f, segments, 4) == 4);
  const PlumeSegment& s = segments[1];  // from x -4 to 0, 4 m above the eye, 100 m out
  CHECK(s.a.y == doctest::Approx(4.0));
  CHECK(s.a.z == doctest::Approx(100.0));
  CHECK(s.a.w == doctest::Approx(k_plume_peak_per_m));
  CHECK(s.b.w == doctest::Approx(k_plume_decay_s * 8.0));
  auto at = [&](f32 x, f32 y, f32 z) { return plume_density(s, Vec3{x, y, z}); };
  // At the brink, at its height, the peak; nothing upwind of it.
  CHECK(at(-2.0f, 4.0f, 100.0f) == doctest::Approx(k_plume_peak_per_m));
  CHECK(at(-2.0f, 4.0f, 99.5f) == 0.0f);
  // Downwind the centre rises by the lift, and the column's peak decays and spreads.
  const f32 t = 20.0f;
  const f32 centre = 4.0f + k_plume_lift * t;
  const f32 sigma = k_plume_spread_m + k_plume_spread_per_m * t;
  const f32 expected = k_plume_peak_per_m * (k_plume_spread_m / sigma) * std::exp(-t / s.b.w);
  CHECK(at(-2.0f, centre, 100.0f + t) == doctest::Approx(expected).epsilon(1e-5));
  CHECK(at(-2.0f, centre, 100.0f + t) < at(-2.0f, 4.0f, 100.0f));
  // Half-Gaussian: a metre below the centre is thinner than a metre above it.
  CHECK(at(-2.0f, centre - 1.0f, 100.0f + t) < at(-2.0f, centre + 1.0f, 100.0f + t));
  // Past the brink's end it falls off sideways; inside its span it does not.
  CHECK(at(-3.9f, centre, 100.0f + t) == doctest::Approx(at(-0.1f, centre, 100.0f + t)));
  CHECK(at(6.0f, centre, 100.0f + t) < 0.2f * at(-0.1f, centre, 100.0f + t));
  // A function of its inputs: the same crest and wind, the same segments, to the bit.
  PlumeSegment again[4];
  REQUIRE(crest_plume_segments(crest, wind, eye, 1.0f, again, 4) == 4);
  CHECK(std::memcmp(segments, again, sizeof(segments)) == 0);
  // A calm, a rounded crest or a ripple sheds none; a hundred times the flux, ten times as dense.
  terrain::TileWind calm = wind;
  calm.saltation_m2_per_day = 0.0f;
  CHECK(crest_plume_segments(crest, calm, eye, 1.0f, again, 4) == 0);
  terrain::CrestLine round = crest;
  round.sharpness = 0.2f;
  CHECK(crest_plume_segments(round, wind, eye, 1.0f, again, 4) == 0);
  terrain::CrestLine ripple = crest;
  ripple.height_m = 0.1f;
  CHECK(crest_plume_segments(ripple, wind, eye, 1.0f, again, 4) == 0);
  CHECK(plume_peak(100.0f * k_plume_mean_saltation, 1.0f, 1.0f) ==
        doctest::Approx(10.0f * k_plume_peak_per_m));
}
