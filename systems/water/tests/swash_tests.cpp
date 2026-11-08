// The swash (docs/subsystems/water.md, "The shore"; docs/experiments/shore.md, "The swash"; roadmap
// S94), on the CPU: the signed depth map's round trip and the version-1 read; the block's new
// fields read and refused; the wave a wind raises over a fetch; Stockdon et al.'s run-up against
// worked examples; the front's profile periodic on the water's clock and bounded by its reach; and
// the closed form of the front's last crossing against a brute-force scan of the profile.
#include <core/hash/hash.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/gfx/water_swash.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/water/water.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <span>
#include <string>

using namespace engine;

namespace {

// A plane rising 1:10 along +x from 1 m under the level at x = -10 to 1 m over it at x = +10.
f64 ramp(const void*, i64 x_mm, i64) noexcept { return 0.1 * static_cast<f64>(x_mm) / 1000.0; }

scene_gen::Ground ramp_ground() {
  scene_gen::Ground g;
  g.surface_fn = &ramp;
  return g;
}

water::Body square_body() {
  scene::WaterBody b;
  b.name = "beach";
  b.level_m = 0.0;
  b.shape.polygon = {WorldPos{-10, 0, -10}, WorldPos{10, 0, -10}, WorldPos{10, 0, 10},
                     WorldPos{-10, 0, 10}};
  water::Body body;
  std::string error;
  REQUIRE_MESSAGE(water::read_body(b, body, &error), error);
  return body;
}

}  // namespace

TEST_CASE("swash: the depth map is signed, its bank read back, and a version-1 file read as before") {
  const scene_gen::Ground ground = ramp_ground();
  const water::Body body = square_body();
  water::Mesh mesh;
  water::build_mesh(body, ground, mesh);
  water::DepthMap map;
  water::build_depth_map(body, mesh, ground, map);
  REQUIRE(map.size >= 2u);
  // The ramp is 1 m deep at one side and stands 1 m over the level at the other.
  CHECK(map.depth_max == doctest::Approx(1.0f).epsilon(0.02));
  CHECK(map.bank_max == doctest::Approx(1.0f).epsilon(0.02));
  // Every texel the ground's own signed depth, to the quantization's half step.
  const f64 step = (static_cast<f64>(map.depth_max) + static_cast<f64>(map.bank_max)) / 65535.0;
  f64 worst = 0.0;
  for (u32 j = 0; j < map.size; j += 7) {
    for (u32 i = 0; i < map.size; ++i) {
      const f64 x = static_cast<f64>(map.origin.x) + i * static_cast<f64>(map.texel_m);
      const f64 z = static_cast<f64>(map.origin.y) + j * static_cast<f64>(map.texel_m);
      const f64 want = -ground.surface(WorldPos{body.center.x + x, 0.0, body.center.z + z});
      worst = std::max(worst, std::abs(static_cast<f64>(water::depth_map_depth(map, i, j)) - want));
    }
  }
  CHECK(worst <= 0.5 * step + 1e-6);
  // Round trip.
  const Vector<u8> bytes = water::depth_map_bytes(map);
  CHECK(bytes.size() == 32u + map.size * map.size * 2u);
  CHECK(bytes[4] == 2u);  // the version
  water::DepthMap back;
  std::string error;
  REQUIRE_MESSAGE(water::read_depth_map(std::span<const u8>(bytes.data(), bytes.size()), back,
                                        &error),
                  error);
  CHECK(back.bank_max == map.bank_max);
  CHECK(back.depth_max == map.depth_max);
  CHECK(std::equal(back.texels.begin(), back.texels.end(), map.texels.begin()));
  // A version-1 file: the same layout with 0 in word 3 and texels over [0, depth_max]; read with
  // no bank, as before.
  Vector<u8> v1 = bytes;
  v1[4] = 1u;
  v1[12] = v1[13] = v1[14] = v1[15] = 0u;
  REQUIRE(water::read_depth_map(std::span<const u8>(v1.data(), v1.size()), back, &error));
  CHECK(back.bank_max == 0.0f);
  CHECK(water::depth_map_depth(back, 0, 0) >= 0.0f);
  // A version this reader does not know is refused.
  v1[4] = 3u;
  CHECK_FALSE(water::read_depth_map(std::span<const u8>(v1.data(), v1.size()), back, &error));
}

TEST_CASE("swash: a body's tide and drain are read, and refused with a sentence") {
  scene::WaterBody b;
  b.name = "sea";
  scene::WaterCircle c;
  c.radius_m = 5.0f;
  b.shape.circle = c;
  b.tide_m = 1.5f;
  b.drain_s = 30.0f;
  water::Body body;
  std::string error;
  REQUIRE(water::read_body(b, body, &error));
  CHECK(body.tide_m == 0.0f);  // a closed body has no tide
  CHECK(body.drain_s == 30.0f);
  b.open = true;
  REQUIRE(water::read_body(b, body, &error));
  CHECK(body.tide_m == 1.5f);
  b.tide_m = -1.0f;
  CHECK_FALSE(water::read_body(b, body, &error));
  CHECK(error.find("tide_m") != std::string::npos);
  b.tide_m = 0.0f;
  b.drain_s = 0.0f;
  CHECK_FALSE(water::read_body(b, body, &error));
  CHECK(error.find("drain_s") != std::string::npos);
  // The bank a body with a tide stores covers twice the tide and a metre.
  b.drain_s = 20.0f;
  b.tide_m = 3.0f;
  REQUIRE(water::read_body(b, body, &error));
  water::Mesh mesh;
  const scene_gen::Ground none;
  water::build_mesh(body, none, mesh);
  water::DepthMap map;
  water::build_depth_map(body, mesh, none, map);
  CHECK(map.bank_max == 0.0f);  // a flat ground at the level has no bank
}

TEST_CASE("swash: the wind's wave over a fetch, and Stockdon's run-up") {
  // JONSWAP's peak period against the fetch law's own, 2 pi / (22 (g^2 / U F)^(1/3)), and the
  // significant height against the fetch-limited growth law (Hasselmann et al. 1973; CEM
  // II-2: g Hm0 / U^2 = 1.6e-3 (g F / U^2)^(1/2)), which the spectrum's own variance meets to a
  // fifth.
  const auto law = [](f64 u, f64 f) {
    return 1.6e-3 * u * u / 9.81 * std::sqrt(9.81 * f / (u * u));
  };
  const gfx::SwashWave lake = gfx::swash_wave(5.0, 2000.0);
  const gfx::SwashWave big = gfx::swash_wave(10.0, 20000.0);
  MESSAGE("5 m/s over 2 km: H " << lake.height_m << " m, T " << lake.period_s << " s; 10 m/s over "
                                << "20 km: H " << big.height_m << " m, T " << big.period_s << " s");
  CHECK(lake.period_s == doctest::Approx(1.343).epsilon(0.01));
  CHECK(lake.height_m == doctest::Approx(law(5.0, 2000.0)).epsilon(0.2));
  CHECK(big.height_m == doctest::Approx(law(10.0, 20000.0)).epsilon(0.2));
  CHECK(lake.height_m < 0.2);  // centimetres
  CHECK(big.height_m > 0.3);   // decimetres
  CHECK(gfx::swash_wave(0.0, 2000.0).height_m == 0.0);
  // Stockdon et al. (2006) eq. 19, worked by hand: a lake's 0.1 m, 1.5 s wave on 1:10 runs up
  // 5.48 cm (shore.md); an ocean's 1 m, 10 s wave on a 0.08 foreshore 0.984 m.
  CHECK(gfx::stockdon_r2(0.1, 1.5, 0.1) == doctest::Approx(0.0548).epsilon(0.005));
  CHECK(gfx::stockdon_r2(1.0, 10.0, 0.08) == doctest::Approx(0.984).epsilon(0.005));
  CHECK(gfx::stockdon_r2(0.0, 10.0, 0.08) == 0.0);
  // The period rounded onto the water's clock: whole groups in 1,024 s.
  const f64 t = gfx::swash_period(lake.period_s);
  const f64 groups = gfx::k_swash_clock_s / (t * gfx::k_swash_group);
  CHECK(std::abs(groups - std::round(groups)) < 1e-9);
  CHECK(t == doctest::Approx(lake.period_s).epsilon(0.02));
}

TEST_CASE("swash: the front is periodic on the water's clock and bounded by its reach") {
  gfx::SwashPlace p;
  p.run_up_m = 0.08;
  p.period_s = gfx::swash_period(1.6);
  p.phase = 0.37;
  p.jitter = 1.2;
  f64 lo = 1e9, hi = -1e9;
  for (u32 i = 0; i < 20000; ++i) {
    const f64 t = 1024.0 * static_cast<f64>(i) / 20000.0;
    const f64 z = gfx::swash_front(p, t);
    lo = std::min(lo, z);
    hi = std::max(hi, z);
    CHECK(std::abs(gfx::swash_front(p, t + gfx::k_swash_clock_s) - z) < 1e-9);
  }
  MESSAGE("front over 1,024 s: " << lo << " to " << hi << " m");
  CHECK(lo >= -gfx::k_swash_rundown * p.run_up_m - 1e-12);
  CHECK(hi <= p.run_up_m * p.jitter + 1e-12);
  CHECK(hi > 0.99 * p.run_up_m * p.jitter);  // the group's largest reaches it
  // Continuous across a wave's start: every wave starts and ends at the rundown.
  const f64 start = (10.0 - p.phase) * p.period_s;  // wave 10 begins
  CHECK(std::abs(gfx::swash_front(p, start - 1e-9) - gfx::swash_front(p, start + 1e-9)) < 1e-6);
}

TEST_CASE("swash: the last time the front reached a height, closed form against a scan") {
  gfx::SwashPlace p;
  p.run_up_m = 0.1;
  p.period_s = gfx::swash_period(2.0);
  p.phase = 0.61;
  p.jitter = 0.9;
  const f64 dt = p.period_s / 4000.0;
  u64 state = 0x9e3779b97f4a7c15ull;
  const auto next = [&]() {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<f64>(state >> 11) / 9007199254740992.0;
  };
  u32 under = 0, never = 0, aged = 0;
  f64 worst = 0.0;
  for (u32 n = 0; n < 1000; ++n) {
    const f64 t = 50.0 + 900.0 * next();
    const f64 h = -0.03 + 0.13 * next();
    const f64 closed = gfx::swash_age(p, t, h);
    // The scan: back from t in steps of a 4,000th of a period over two groups.
    f64 scan = gfx::k_swash_never;
    const u32 steps = 2 * gfx::k_swash_group * 4000;
    for (u32 s = 0; s <= steps; ++s) {
      if (gfx::swash_front(p, t - s * dt) >= h) {
        scan = s * dt;
        break;
      }
    }
    if (closed == 0.0) ++under;
    if (closed >= gfx::k_swash_never) ++never;
    if (closed > 0.0 && closed < gfx::k_swash_never) ++aged;
    if (scan >= gfx::k_swash_never || closed >= gfx::k_swash_never) {
      CHECK(scan == closed);
    } else {
      worst = std::max(worst, std::abs(scan - closed));
    }
  }
  MESSAGE("t_last: " << under << " under the sheet, " << aged << " drying, " << never
                     << " never reached; worst " << worst << " s against a scan step of " << dt);
  CHECK(worst <= dt + 1e-9);
  CHECK(under > 50u);
  CHECK(aged > 300u);
  CHECK(never > 20u);
}
