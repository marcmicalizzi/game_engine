// Where bedrock shows through a soil mantle (G142; rocks.md, "Exposed bedrock (G142)"): the rule
// on its own, before any mesh — the thresholds, the curvature's sign, the bed at the surface, and
// the bits a point's exposure is on every toolchain.
#include <domain/gfx/formation.h>
#include <domain/rocks/exposure.h>

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <string>

using namespace engine;
using namespace engine::rocks;

namespace {

// Level beds 2 m thick: sandstone (resistant) over shale (weak), origin at the world's.
scene::Formation level_beds() {
  scene::Formation f;
  f.dip_deg = 0.0f;
  scene::FormationBed shale;
  shale.name = "shale";
  shale.thickness_m = 2.0f;
  shale.resistance = 0.1f;
  scene::FormationBed sandstone;
  sandstone.name = "sandstone";
  sandstone.thickness_m = 2.0f;
  sandstone.resistance = 0.9f;
  f.beds.push_back(shale);
  f.beds.push_back(sandstone);
  return f;
}

// A plane rising along +x at a slope given in thousandths (a whole number of millimetres a metre).
struct Incline {
  i64 rise_per_m_mm = 0;
};
f64 plane_height(const void* context, i64 x_mm, i64) noexcept {
  const auto& p = *static_cast<const Incline*>(context);
  return static_cast<f64>(x_mm * p.rise_per_m_mm) / 1.0e6;
}
// A nose: the same plane with a parabolic ridge across z, 0.02 1/m of convexity.
f64 nose_height(const void* context, i64 x_mm, i64 z_mm) noexcept {
  const f64 z = static_cast<f64>(z_mm) / 1000.0;
  return plane_height(context, x_mm, z_mm) - 0.01 * z * z;
}
f64 hollow_height(const void* context, i64 x_mm, i64 z_mm) noexcept {
  const f64 z = static_cast<f64>(z_mm) / 1000.0;
  return plane_height(context, x_mm, z_mm) + 0.01 * z * z;
}
scene_gen::Ground ground_of(scene_gen::HeightFn fn, const Incline& plane) {
  scene_gen::Ground g;
  g.surface_fn = fn;
  g.context = &plane;
  return g;
}

ExposureThresholds default_rule() {
  ExposureThresholds t;
  std::string error;
  ExposureRule plain;
  plain.patch_m = 0.0f;  // no patches: the rule itself, every resistant bed past the soil slope
  REQUIRE_MESSAGE(prepare_exposure(plain, t, error), error);
  return t;
}

}  // namespace

TEST_CASE("exposure: the rule's ranges are refused by field") {
  ExposureThresholds t;
  std::string error;
  ExposureRule rule;
  REQUIRE(prepare_exposure(rule, t, error));
  CHECK(t.soil_slope == doctest::Approx(0.57735).epsilon(1e-4));  // tan 30
  CHECK(t.rock_slope == doctest::Approx(1.0).epsilon(1e-9));      // tan 45
  CHECK(t.arm_mm == 4000);
  rule.rock_slope_deg = 25.0f;
  CHECK_FALSE(prepare_exposure(rule, t, error));
  CHECK(error.find("rock_slope_deg") != std::string::npos);
  rule = {};
  rule.scale_m = 0.5f;
  CHECK_FALSE(prepare_exposure(rule, t, error));
  CHECK(error.find("scale_m") != std::string::npos);
  rule = {};
  rule.min_resistance = 1.5f;
  CHECK_FALSE(prepare_exposure(rule, t, error));
  CHECK(error.find("min_resistance") != std::string::npos);
}

TEST_CASE("exposure: the ground's shape is exact in whole millimetres") {
  // 4 m arm; east 2.8 m above west: a slope of 0.35. The centre 40 mm above the mean of its four
  // neighbours: a convexity of 4 * 0.04 / 16 = 0.01 1/m.
  const GroundShape s = ground_shape(1040, -400, 2400, 1000, 1000, 4000);
  CHECK(s.slope_x == 0.35);
  CHECK(s.slope_z == 0.0);
  CHECK(s.slope == 0.35);
  CHECK(s.convexity == doctest::Approx(0.01).epsilon(1e-12));
  CHECK(ground_shape(960, -400, 2400, 1000, 1000, 4000).convexity < 0.0);  // a hollow
}

TEST_CASE("exposure: nothing under the soil slope, everything past the rock slope") {
  const ExposureThresholds rule = default_rule();
  GroundShape s;
  // Under 30 degrees nothing crops out, however sharp the nose.
  s.slope = 0.5;
  s.convexity = 10.0;
  CHECK(exposure_fraction(rule, s) == 0.0);
  // On planar ground the fraction rises linearly from the soil slope to the rock slope.
  s.convexity = 0.0;
  s.slope = rule.soil_slope;
  CHECK(exposure_fraction(rule, s) == 0.0);
  s.slope = 0.5 * (rule.soil_slope + rule.rock_slope);
  CHECK(exposure_fraction(rule, s) == doctest::Approx(0.5));
  s.slope = rule.rock_slope;
  CHECK(exposure_fraction(rule, s) == 1.0);
  s.slope = 3.0;
  CHECK(exposure_fraction(rule, s) == 1.0);
  // A nose raises it and a hollow lowers it, by the convexity over the critical curvature.
  s.slope = 0.5 * (rule.soil_slope + rule.rock_slope);
  s.convexity = 0.5 * rule.critical_curvature;
  CHECK(exposure_fraction(rule, s) == doctest::Approx(1.0));
  s.convexity = -0.25 * rule.critical_curvature;
  CHECK(exposure_fraction(rule, s) == doctest::Approx(0.25));
  s.convexity = -10.0;
  CHECK(exposure_fraction(rule, s) == 0.0);
  // The deepest hollow still loses its fill by the rock slope, and the fraction is monotone in
  // the slope on the way there.
  f64 last = 0.0;
  for (u32 i = 0; i <= 100; ++i) {
    s.slope = rule.soil_slope + (rule.rock_slope - rule.soil_slope) * static_cast<f64>(i) / 100.0;
    const f64 e = exposure_fraction(rule, s);
    CHECK(e >= last);
    last = e;
  }
  CHECK(last == 1.0);
}

TEST_CASE("exposure: a bed stands only at or past the resistance threshold") {
  const ExposureThresholds rule = default_rule();
  CHECK(resistance_factor(rule, 0.0) == 0.0);
  CHECK(resistance_factor(rule, 0.49) == 0.0);
  CHECK(resistance_factor(rule, 0.5) == doctest::Approx(0.35));
  CHECK(resistance_factor(rule, 1.0) == doctest::Approx(1.0));
  CHECK(resistance_factor(rule, 0.75) > resistance_factor(rule, 0.6));
}

TEST_CASE("exposure: on a slope the resistant bed is exposed and the weak bed is not") {
  const ExposureThresholds rule = default_rule();
  gfx::FormationParams field;
  std::string error;
  REQUIRE_MESSAGE(gfx::pack_formation(level_beds(), field, &error), error);
  // 0.8 m a metre: 38.7 degrees, between the soil slope and the rock slope.
  const Incline steep{800};
  const scene_gen::Ground ground = ground_of(&plane_height, steep);
  u32 exposed = 0, mantled = 0;
  for (i64 x_mm = 0; x_mm < 40000; x_mm += 250) {
    const Exposure e = exposure_at(rule, field, ground, x_mm, 0);
    CHECK(e.shape.slope == doctest::Approx(0.8));
    CHECK(e.shape.convexity == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(e.coordinate == doctest::Approx(e.height_m));  // level beds, origin at zero
    // Beds 2 m thick, the shale first: heights 0-2 shale, 2-4 sandstone, repeating.
    const bool sandstone = std::fmod(e.height_m, 4.0) >= 2.0;
    CHECK(e.bed == (sandstone ? 1u : 0u));
    CHECK(e.exposed == sandstone);
    if (sandstone) {
      CHECK(e.fraction == doctest::Approx((0.8 - rule.soil_slope) / (1.0 - rule.soil_slope)));
      CHECK(e.stand == doctest::Approx(e.fraction * resistance_factor(rule, 0.9)));
      ++exposed;
    } else {
      CHECK(e.stand == 0.0);
      ++mantled;
    }
  }
  CHECK(exposed > 60);
  CHECK(mantled > 60);
  // The same beds under a 20 degree slope: nothing, on either bed.
  const Incline gentle{364};
  const scene_gen::Ground low = ground_of(&plane_height, gentle);
  for (i64 x_mm = 0; x_mm < 40000; x_mm += 250)
    CHECK_FALSE(exposure_at(rule, field, low, x_mm, 0).exposed);
}

TEST_CASE("exposure: a nose is barer than the plane, a hollow keeps its soil") {
  const ExposureThresholds rule = default_rule();
  gfx::FormationParams field;
  std::string error;
  REQUIRE_MESSAGE(gfx::pack_formation(level_beds(), field, &error), error);
  const Incline steep{700};  // 35 degrees
  // x = 4 m: 2.8 m up, in the sandstone on all three grounds at z = 0.
  const Exposure plane = exposure_at(rule, field, ground_of(&plane_height, steep), 4000, 0);
  const Exposure nose = exposure_at(rule, field, ground_of(&nose_height, steep), 4000, 0);
  const Exposure hollow = exposure_at(rule, field, ground_of(&hollow_height, steep), 4000, 0);
  CHECK(nose.shape.convexity == doctest::Approx(0.02));
  CHECK(hollow.shape.convexity == doctest::Approx(-0.02));
  CHECK(plane.exposed);
  CHECK(nose.fraction > plane.fraction + 0.5);
  CHECK(hollow.fraction == 0.0);
  CHECK_FALSE(hollow.exposed);
  MESSAGE("35 degrees: plane " << plane.fraction << ", nose " << nose.fraction << ", hollow "
                               << hollow.fraction);
}

TEST_CASE("exposure: under the soil's patches the share of a slope that is rock rises with it") {
  // The default rule: patches 12 m across. On planar ground of one resistant bed, the share of
  // points where rock shows is 0 under the soil slope, rises with the slope, and is all of it
  // past the rock slope; a point's cover is the world's, whichever way it is reached.
  ExposureThresholds rule;
  std::string error;
  REQUIRE(prepare_exposure(ExposureRule{}, rule, error));
  rule.seed = 5;
  CHECK(rule.patch_mm == 12000);
  f64 last = -1.0;
  for (const f64 degrees : {25.0, 32.0, 36.0, 40.0, 44.0, 50.0}) {
    GroundShape s;
    s.slope = std::tan(degrees * 3.14159265358979323846 / 180.0);
    const f64 fraction = exposure_fraction(rule, s);
    u32 rock = 0, points = 0;
    for (i64 z_mm = -100000; z_mm < 100000; z_mm += 2500)
      for (i64 x_mm = -100000; x_mm < 100000; x_mm += 2500) {
        const f64 cover = soil_cover(rule, x_mm, z_mm);
        CHECK(cover >= 0.0);
        CHECK(cover < 1.0);
        const f64 stand = standing_share(rule, fraction, cover);
        CHECK(stand >= 0.0);
        CHECK(stand <= 1.0);
        rock += stand > 0.0 ? 1u : 0u;
        ++points;
      }
    const f64 share = static_cast<f64>(rock) / static_cast<f64>(points);
    MESSAGE(degrees << " degrees: fraction " << fraction << ", " << share * 100.0 << " % rock");
    CHECK(share >= last);
    last = share;
    if (degrees < 30.0) CHECK(rock == 0);
    if (degrees > 45.0) CHECK(rock == points);
  }
  // With no patches the cover is nothing and the share that stands is the fraction.
  ExposureThresholds plain = default_rule();
  CHECK(soil_cover(plain, 12345, -6789) == 0.0);
  CHECK(standing_share(plain, 0.4, 0.0) == 0.4);
}

// The same bits from every toolchain: taken on MSVC (msvc-debug, x86-64-v3) and reproduced by
// Clang 18 and GCC 13 in the containers, v3 and v2.
TEST_CASE("exposure: a point's exposure is pinned to the bit") {
  ExposureRule authored;
  authored.soil_slope_deg = 28.0f;
  authored.rock_slope_deg = 47.0f;
  ExposureThresholds rule;
  std::string error;
  REQUIRE(prepare_exposure(authored, rule, error));
  rule.seed = 77;
  scene::Formation f = level_beds();
  f.dip_deg = 17.0f;
  f.azimuth_deg = 35.0f;
  f.fold_amplitude_m = 3.0f;
  f.fold_wavelength_m = 180.0f;
  f.origin = WorldPos{12.5, -3.0, 40.25};
  gfx::FormationParams field;
  REQUIRE(gfx::pack_formation(f, field, &error));
  const Incline steep{730};
  const scene_gen::Ground ground = ground_of(&nose_height, steep);
  u64 h = 0xcbf29ce484222325ull;
  const auto fold = [&](f64 v) {
    h ^= std::bit_cast<u64>(v);
    h *= 0x100000001b3ull;
  };
  for (i64 z_mm = -3000; z_mm <= 3000; z_mm += 750)
    for (i64 x_mm = 1000; x_mm <= 30000; x_mm += 1250) {
      const Exposure e = exposure_at(rule, field, ground, x_mm, z_mm);
      fold(e.shape.slope);
      fold(e.shape.convexity);
      fold(e.coordinate);
      fold(e.fraction);
      fold(e.cover);
      fold(e.stand);
      h ^= e.bed;
    }
  fold(rule.soil_slope);
  fold(rule.rock_slope);
  MESSAGE("exposure pin " << h);
  CHECK(h == 16263762372318913364ull);
}
