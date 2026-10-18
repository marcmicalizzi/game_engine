// The far-field clouds on the CPU, with no device (docs/subsystems/volumetrics.md, "The far-field
// clouds"): the field is a function of its seed and coordinate, its coverage is what it says, and
// the wind record's drift carries it — continuous, a function of the time, the field at a time and
// a point the field a minute earlier where the wind has carried it from.

#include <domain/terrain/dunes.h>
#include <domain/terrain/scene_ground.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>
#include <systems/volumetrics/volumetrics.h>

#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::volumetrics;

namespace {

// The share of a horizontal sample grid at mid-layer the field has cloud at.
f32 cover(const CloudParams& p, f32 dx = 0.0f) {
  u32 cloudy = 0;
  u32 n = 0;
  for (u32 j = 0; j < 40; ++j) {
    for (u32 i = 0; i < 40; ++i) {
      ++n;
      const f32 alt = 0.5f * (p.base_m + p.top_m) - 0.2f * (p.top_m - p.base_m);
      if (cloud_density(p, dx + 997.0f * static_cast<f32>(i), alt, 1013.0f * static_cast<f32>(j)) >
          0.0f)
        ++cloudy;
    }
  }
  return static_cast<f32>(cloudy) / static_cast<f32>(n);
}

}  // namespace

TEST_CASE("volumetrics: the cloud field is its seed's, its coverage's and its coordinate's alone") {
  CloudParams p;
  p.coverage = 0.5f;
  // The same inputs, the same number.
  CHECK(cloud_density(p, 1234.5f, 2600.0f, -77.0f) == cloud_density(p, 1234.5f, 2600.0f, -77.0f));
  // Outside the slab there is none.
  CHECK(cloud_density(p, 1234.5f, p.base_m - 1.0f, -77.0f) == 0.0f);
  CHECK(cloud_density(p, 1234.5f, p.top_m + 1.0f, -77.0f) == 0.0f);
  // The field repeats over its period, which is what lets the CPU reduce the coordinate.
  CHECK(cloud_density(p, 1234.5f, 2600.0f, -77.0f) ==
        doctest::Approx(
            cloud_density(p, 1234.5f + k_cloud_period_m, 2600.0f, -77.0f - k_cloud_period_m)));
  // Coverage: none is clear, more is cloudier.
  CloudParams clear = p;
  clear.coverage = 0.0f;
  CHECK(cover(clear) == 0.0f);
  CloudParams thin = p;
  thin.coverage = 0.25f;
  CloudParams thick = p;
  thick.coverage = 0.8f;
  const f32 a = cover(thin);
  const f32 b = cover(p);
  const f32 c = cover(thick);
  MESSAGE("cloud cover at coverage 0.25, 0.5, 0.8: " << a << ", " << b << ", " << c);
  CHECK(a < b);
  CHECK(b < c);
  CHECK(c > 0.3f);
  // Another seed is another sky.
  CloudParams other = p;
  other.seed = 2;
  u32 differ = 0;
  for (u32 i = 0; i < 200; ++i) {
    const f32 x = 311.0f * static_cast<f32>(i);
    if (cloud_density(p, x, 2400.0f, 5.0f * x) != cloud_density(other, x, 2400.0f, 5.0f * x))
      ++differ;
  }
  CHECK(differ > 20);
}

TEST_CASE("volumetrics: the wind record carries the clouds, continuously and by its own hours") {
  const std::string path =
      std::string(ENGINE_SOURCE_DIR) + "/content/test-scenes/desert-erg-storm/scene.json";
  renderer::SceneDesc scene;
  std::string error;
  REQUIRE_MESSAGE(renderer::read_scene_file(path, scene, error), error);
  renderer::TerrainSampler ground(scene.terrain);
  REQUIRE_MESSAGE(ground.ok(), ground.error());
  const terrain::DuneField* field = terrain::dune_field(ground.provider());
  REQUIRE(field != nullptr);
  CloudDrift drift;
  drift.build(field);
  const f64 period = static_cast<f64>(k_cloud_period_m);
  auto delta = [&](f64 a, f64 b) {
    const Vec2 oa = drift.offset_m(a);
    const Vec2 ob = drift.offset_m(b);
    f64 dx = static_cast<f64>(ob.x) - oa.x;
    f64 dz = static_cast<f64>(ob.y) - oa.y;
    dx -= std::round(dx / period) * period;  // across the period's seam
    dz -= std::round(dz / period) * period;
    return Vec2{static_cast<f32>(dx), static_cast<f32>(dz)};
  };
  const f64 t = 94'608'000.0 + 15.5 * 3600.0;  // the storm erg's day at 15:30
  // A function of the time: the same time, the same drift, in a second drift built alike.
  CloudDrift again;
  again.build(field);
  CHECK(drift.offset_m(t).x == again.offset_m(t).x);
  CHECK(drift.offset_m(t).y == again.offset_m(t).y);
  // Within an hour it moves at the hour's wind aloft: a minute's drift is the wind times a minute.
  const MediumWind wind = wind_now(field, t);
  const Vec2 minute = delta(t, t + 60.0);
  const f32 speed = wind.speed_mps * k_cloud_speed_factor;
  CHECK(std::sqrt(minute.x * minute.x + minute.y * minute.y) ==
        doctest::Approx(60.0f * speed).epsilon(1e-3));
  CHECK(minute.x == doctest::Approx(60.0f * speed * wind.direction.x).epsilon(1e-2));
  // Continuous across an hour's start: a second either side moves two seconds of wind, no more.
  const f64 hour = std::floor(t / 3600.0 + 1.0) * 3600.0;
  const Vec2 across = delta(hour - 1.0, hour + 1.0);
  CHECK(std::sqrt(across.x * across.x + across.y * across.y) < 2.0f * 3.0f * speed + 0.05f);
  // The field at a point a minute later is the field a minute earlier where the wind carried it
  // from: the noise coordinate is the point less the drift.
  CloudParams p;
  p.coverage = 0.6f;
  u32 checked = 0;
  for (u32 i = 0; i < 50; ++i) {
    const f32 x = 523.0f * static_cast<f32>(i);
    const f32 z = 389.0f * static_cast<f32>(i);
    const Vec2 o0 = drift.offset_m(t);
    const Vec2 o1 = drift.offset_m(t + 60.0);
    const f32 later = cloud_density(p, x - o1.x, 2500.0f, z - o1.y);
    const f32 earlier = cloud_density(p, x - minute.x - o0.x, 2500.0f, z - minute.y - o0.y);
    CHECK(later == doctest::Approx(earlier).epsilon(1e-3));
    if (later > 0.0f) ++checked;
  }
  MESSAGE("cloud samples with cloud in them: " << checked << " of 50; aloft " << speed << " m/s");
}
