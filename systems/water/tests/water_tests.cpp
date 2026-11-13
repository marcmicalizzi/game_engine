// The water capability on the CPU (docs/subsystems/water.md, "Testing"): the block checked, a
// body's inside and its depth, the mesh and the depths it carries over a synthetic ground, the
// vertex budget, the GLB as the importer reads it, the generator through the registry, and the
// renderer's packing of a body's shading into its material. No device.
#include <core/hash/hash.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/assets/gltf.h>
#include <domain/gfx/visibility_resolve.h>
#include <domain/gfx/water_flux.h>
#include <domain/gfx/water_obstacle.h>
#include <domain/gfx/water_ripples.h>
#include <domain/gfx/water_spectrum.h>
#include <domain/gfx/water_swash.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>
#include <systems/water/water.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <string>

using namespace engine;

namespace {

// A bowl round (100, 50): 2 m deep at its centre, rising 2 cm per square metre of radius, so the
// ground meets a surface at 0 ten metres out and stands above it beyond.
f64 bowl(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 x = static_cast<f64>(x_mm) / 1000.0 - 100.0;
  const f64 z = static_cast<f64>(z_mm) / 1000.0 - 50.0;
  return -2.0 + 0.02 * (x * x + z * z);
}

scene_gen::Ground bowl_ground() {
  scene_gen::Ground g;
  g.surface_fn = &bowl;
  return g;
}

scene::WaterBody circle_block(f64 cx, f64 cz, f32 radius, f64 level) {
  scene::WaterBody b;
  b.name = "pool";
  b.level_m = level;
  scene::WaterCircle c;
  c.center = WorldPos{cx, 0.0, cz};
  c.radius_m = radius;
  b.shape.circle = c;
  return b;
}

water::Body read(const scene::WaterBody& block) {
  water::Body body;
  std::string error;
  REQUIRE_MESSAGE(water::read_body(block, body, &error), error);
  return body;
}

}  // namespace

// ---- the wave as height (S100; water_swash.h; docs/experiments/shore.md, "The wave as height")
// ---

TEST_CASE("water: the dispersion relation at depth, and the deep and shallow limits") {
  f64 worst = 0.0;
  for (const f64 period : {1.5, 2.5, 5.5, 10.0}) {
    const f64 omega = 2.0 * 3.14159265358979323846 / period;
    const f64 k0 = omega * omega / gfx::k_swash_g;
    for (const f64 d : {0.02, 0.1, 0.5, 1.0, 3.0, 10.0, 50.0, 500.0}) {
      const f64 k = gfx::water_wavenumber(k0, d);
      const f64 rel =
          std::fabs(gfx::k_swash_g * k * std::tanh(k * d) - omega * omega) / (omega * omega);
      worst = std::max(worst, rel);
      CHECK(rel < 1e-6);
      CHECK(k >= k0 * (1.0 - 1e-9));  // a wave only shortens as it shoals
    }
    // Deep water is k0; shallow water is omega / sqrt(g d), the speed sqrt(g d).
    CHECK(gfx::water_wavenumber(k0, 2000.0) == doctest::Approx(k0).epsilon(1e-9));
    const f64 shallow = std::max(1e-3 / k0, 2e-3);
    CHECK(gfx::water_wavenumber(k0, shallow) ==
          doctest::Approx(omega / std::sqrt(gfx::k_swash_g * shallow)).epsilon(2e-3));
  }
  MESSAGE("dispersion: worst relative residual of omega^2 = g k tanh(k d) " << worst);
}

TEST_CASE("water: a wave shoals by Green's law and breaks at 0.78 of the depth") {
  const f64 period = 8.0;
  const f64 omega = 2.0 * 3.14159265358979323846 / period;
  const f64 k0 = omega * omega / gfx::k_swash_g;
  // Deep water: no shoaling. Shallow water: H / H0 = (4 k0 d)^(-1/4), H ~ d^(-1/4).
  CHECK(gfx::water_shoaling(gfx::water_wavenumber(k0, 400.0), 400.0) ==
        doctest::Approx(1.0).epsilon(1e-6));
  for (const f64 d : {1e-4 / k0, 1e-3 / k0}) {
    const f64 ks = gfx::water_shoaling(gfx::water_wavenumber(k0, d), d);
    CHECK(ks * std::pow(4.0 * k0 * d, 0.25) == doctest::Approx(1.0).epsilon(0.01));
  }
  // A 1 m wave of 8 s breaks where the shoaled height reaches 0.78 d: found by bisection on the
  // two formulas, and `water_wave_height` is the bore 0.78 d inshore of it and the shoaled wave
  // offshore.
  const f64 h0 = 1.0;
  f64 lo = 0.1, hi = 10.0;
  for (u32 i = 0; i < 60; ++i) {
    const f64 mid = 0.5 * (lo + hi);
    const f64 shoaled = h0 * gfx::water_shoaling(gfx::water_wavenumber(k0, mid), mid);
    (shoaled > gfx::k_water_breaking_gamma * mid ? lo : hi) = mid;
  }
  const f64 breaking = 0.5 * (lo + hi);
  CHECK(gfx::water_wave_height(h0, k0, breaking) ==
        doctest::Approx(gfx::k_water_breaking_gamma * breaking).epsilon(1e-9));
  CHECK(gfx::water_wave_height(h0, k0, 0.8 * breaking) ==
        doctest::Approx(gfx::k_water_breaking_gamma * 0.8 * breaking).epsilon(1e-12));
  const f64 offshore = gfx::water_wave_height(h0, k0, 1.5 * breaking);
  CHECK(offshore < gfx::k_water_breaking_gamma * 1.5 * breaking);
  CHECK(offshore > h0);  // shoaled up before it breaks
  CHECK(gfx::water_wave_height(h0, k0, 0.0) == 0.0);
  CHECK(gfx::water_wave_height(h0, k0, -0.5) == 0.0);
  MESSAGE("a 1 m, 8 s wave breaks in " << breaking << " m of water, " << offshore
                                       << " m high half as deep again offshore");
}

// S102 (docs/experiments/shore-mechanics.md, "Speed, direction and height before breaking"): what a
// wave keeps until something takes it is its energy flux, E c_g with E = rho g H^2 / 8.
TEST_CASE("water: a wave keeps its energy flux to the break, and loses it from there") {
  const f64 g = gfx::k_swash_g;
  f64 worst = 0.0;
  for (const f64 period : {1.5, 4.7, 10.0}) {
    const f64 omega = 2.0 * 3.14159265358979323846 / period;
    const f64 k0 = omega * omega / g;
    const f64 h0 = 0.05 * period * period;  // a steepness of about 0.03 at every period
    // The group speed from the dispersion relation itself, d(omega)/dk by a central difference at
    // the depth — not from the formula the shoaling coefficient is made of.
    const auto group_speed = [&](f64 k, f64 d) {
      const auto omega_of = [&](f64 q) { return std::sqrt(g * q * std::tanh(q * d)); };
      const f64 e = 1e-5 * k;
      return (omega_of(k + e) - omega_of(k - e)) / (2.0 * e);
    };
    const f64 flux0 = h0 * h0 * (0.5 * g / omega);  // deep water: c_g = g / (2 omega)
    // Where it breaks: the shoaled height meets 0.78 d.
    f64 lo = 1e-3, hi = 50.0;
    for (u32 i = 0; i < 80; ++i) {
      const f64 mid = 0.5 * (lo + hi);
      (h0 * gfx::water_shoaling(gfx::water_wavenumber(k0, mid), mid) >
               gfx::k_water_breaking_gamma * mid
           ? lo
           : hi) = mid;
    }
    const f64 breaking = 0.5 * (lo + hi);
    // From 20 / k0 deep down to the break, in forty steps of equal ratio: the flux is deep water's.
    const f64 deep = 20.0 / k0;
    for (u32 s = 0; s <= 40; ++s) {
      const f64 d = deep * std::pow(1.001 * breaking / deep, static_cast<f64>(s) / 40.0);
      const f64 k = gfx::water_wavenumber(k0, d);
      const f64 h = gfx::water_wave_height(h0, k0, d);
      worst = std::max(worst, std::fabs(h * h * group_speed(k, d) / flux0 - 1.0));
    }
    // Inshore of it the bore is 0.78 d and the flux falls with every step towards the shore.
    f64 last = flux0;
    for (u32 s = 1; s <= 20; ++s) {
      const f64 d = breaking * (1.0 - static_cast<f64>(s) / 21.0);
      const f64 h = gfx::water_wave_height(h0, k0, d);
      const f64 flux = h * h * group_speed(gfx::water_wavenumber(k0, d), d);
      CHECK(flux < last);
      last = flux;
    }
    CHECK(last <
          0.01 * flux0);  // nearly all of it is gone a twentieth of the break depth from shore
  }
  MESSAGE("energy flux from deep water to the break: within " << worst << " of deep water's");
  // The bound: the wavenumber's own residual (4e-9 of omega^2, the dispersion case above) and the
  // difference's truncation, (1e-5)^2 of the group speed's curvature: under 1e-7 between them.
  CHECK(worst < 1e-7);
}

TEST_CASE("water: the train arrives in sequence along a straight shore") {
  // A planar beach 1:20, the shore at x = 40 m, the bank beyond: 241 texels of 0.25 m.
  const u32 n = 241;
  const f64 texel = 0.25;
  Vector<f32> depth(n * n);
  for (u32 j = 0; j < n; ++j)
    for (u32 i = 0; i < n; ++i)
      depth[j * n + i] = static_cast<f32>((40.0 - texel * static_cast<f64>(i)) / 20.0);
  const f64 period = gfx::swash_period(4.0);
  Vector<f32> tau(n * n);
  gfx::water_arrival_field(depth.data(), n, texel, period, tau.data());
  const f64 omega = 2.0 * 3.14159265358979323846 / period;
  const f64 k0 = omega * omega / gfx::k_swash_g;
  f64 along_worst = 0.0, cross_worst = 0.0, bank_worst = 0.0;
  for (u32 j = 10; j + 10 < n; ++j) {
    for (u32 i = 1; i + 1 < n; ++i) {
      const u32 at = j * n + i;
      // Along the shore, a crest: the same arrival.
      along_worst = std::max(along_worst, std::fabs(static_cast<f64>(tau[at + n] - tau[at])));
      const f64 d = static_cast<f64>(depth[at]);
      if (i + 4 < n && d > 0.0) {
        const f64 dn = static_cast<f64>(depth[at + 4]);
        if (dn > 0.3 && d < 1.5) {
          // Across it, two places a metre apart differ by the travel time between them at the
          // local celerity and nothing else: the integral of 1 / c(d) along the metre.
          f64 travel = 0.0;
          for (u32 s = 0; s < 1000; ++s) {
            const f64 x = (static_cast<f64>(s) + 0.5) / 1000.0;
            travel += 1e-3 / (omega / gfx::water_wavenumber(k0, d + (dn - d) * x));
          }
          const f64 rel = std::fabs(static_cast<f64>(tau[at + 4] - tau[at]) - travel) / travel;
          cross_worst = std::max(cross_worst, rel);
        }
      }
      if (d < 0.0)
        bank_worst = std::max(bank_worst, std::fabs(static_cast<f64>(tau[at + 1] - tau[at])));
    }
  }
  CHECK(along_worst < 1e-4);
  CHECK(cross_worst < 0.02);
  CHECK(bank_worst < 1e-3);  // the bank takes its shoreline's arrival: the swash's phase
  // The same field twice, to the bit.
  Vector<f32> again(n * n);
  gfx::water_arrival_field(depth.data(), n, texel, period, again.data());
  CHECK(std::memcmp(tau.data(), again.data(), u64{n} * n * sizeof(f32)) == 0);
  MESSAGE("arrival: along the shore " << along_worst << " s, across it " << cross_worst
                                      << " of the travel time, over the bank " << bank_worst
                                      << " s");
}

// S102 (docs/experiments/shore-mechanics.md, "A wave has a direction"): a train that comes from
// somewhere is turned by the depth as Snell's law says.
TEST_CASE("water: a train that comes in at an angle turns to the beach by Snell's law") {
  // The same planar beach, 1:20 with the shore at x = 40 m, twice as long along the shore so the
  // part that is read has the wave's whole run behind it: 241 by 241 texels of 0.25 m.
  const u32 n = 241;
  const f64 texel = 0.25;
  Vector<f32> depth(n * n);
  for (u32 j = 0; j < n; ++j)
    for (u32 i = 0; i < n; ++i)
      depth[j * n + i] = static_cast<f32>((40.0 - texel * static_cast<f64>(i)) / 20.0);
  const f64 period = gfx::swash_period(4.0);
  const f64 omega = 2.0 * 3.14159265358979323846 / period;
  const f64 k0 = omega * omega / gfx::k_swash_g;
  // The source: every texel at least min(1 / k0, 0.8 of the deepest) deep, as the field takes it.
  const f64 deepest = 2.0;
  const f64 source = std::min(gfx::k_water_train_deep_kd / k0, 0.8 * deepest);
  const f64 c_source = omega / gfx::water_wavenumber(k0, source);
  // Thirty degrees off the shore's normal, travelling towards +x and +z.
  const f64 sin0 = 0.5;
  const Vec2 direction{0.8660254f, 0.5f};
  Vector<f32> tau(n * n);
  gfx::water_arrival_field(depth.data(), n, texel, period, tau.data(), 3, direction);
  // Snell: along a straight coast the phase's slowness along the shore is kept, sin(theta) / c =
  // sin(theta0) / c0, so the crest's angle to the beach falls with the celerity. Read where the
  // wave has crossed the whole beach from the source (its run along the shore is under 19 m), by
  // central differences over a metre.
  f64 slowness_worst = 0.0, angle_worst_deg = 0.0;
  f64 angle_at[3] = {0.0, 0.0, 0.0};
  const f64 depths[3] = {1.2, 0.6, 0.3};
  for (u32 j = 110; j <= 220; j += 10) {
    for (u32 i = 8; i + 8 < n; ++i) {
      const u32 at = j * n + i;
      const f64 d = static_cast<f64>(depth[at]);
      if (!(d > 0.2 && d < 0.9 * source)) continue;
      const f64 tz = static_cast<f64>(tau[at + 2 * n] - tau[at - 2 * n]) / (4.0 * texel);
      const f64 tx = static_cast<f64>(tau[at + 2] - tau[at - 2]) / (4.0 * texel);
      slowness_worst = std::max(slowness_worst, std::fabs(tz * c_source / sin0 - 1.0));
      const f64 c = omega / gfx::water_wavenumber(k0, d);
      const f64 snell = std::asin(sin0 * c / c_source) * 57.29577951308232;
      const f64 angle = std::atan2(tz, tx) * 57.29577951308232;
      angle_worst_deg = std::max(angle_worst_deg, std::fabs(angle - snell));
      if (j == 160)
        for (u32 q = 0; q < 3; ++q)
          if (std::fabs(d - depths[q]) < 0.5 * texel / 20.0 + 1e-6) angle_at[q] = angle;
    }
  }
  MESSAGE("Snell on 1:20, 4 s, 30 degrees at "
          << source << " m: the along-shore slowness within " << slowness_worst
          << " of sin(theta0) / c0, the angle "
          << "within " << angle_worst_deg << " degrees; " << angle_at[0] << ", " << angle_at[1]
          << ", " << angle_at[2] << " degrees at 1.2, 0.6 and 0.3 m");
  // The scheme is first-order (the destination texel's slowness, as the straight-on case above,
  // which it holds to 2% of the travel time): the same 2% of the slowness, and the angle that
  // follows from it — d(theta) = tan(theta) d(s) / s, under 0.7 degrees at 30.
  CHECK(slowness_worst < 0.02);
  CHECK(angle_worst_deg < 0.7);
  // A crest's angle to the beach falls as it comes in.
  CHECK(angle_at[0] > angle_at[1]);
  CHECK(angle_at[1] > angle_at[2]);
  CHECK(angle_at[2] > 5.0);
  CHECK(angle_at[0] < 30.0);
  // One end of the beach before the other: along the waterline the crest arrives later towards
  // +z, by the same slowness.
  const u32 shore = 159;  // x = 39.75 m, the last wet texel
  const f64 lag = static_cast<f64>(tau[200 * n + shore] - tau[120 * n + shore]);
  CHECK(lag == doctest::Approx(80.0 * texel * sin0 / c_source).epsilon(0.02));
  // No direction is the default: every source texel leaves at once, the crest along the deep
  // water's contour, and the whole waterline is reached together.
  Vector<f32> still(n * n), before(n * n);
  gfx::water_arrival_field(depth.data(), n, texel, period, still.data(), 3, Vec2{0.0f, 0.0f});
  gfx::water_arrival_field(depth.data(), n, texel, period, before.data());
  CHECK(std::memcmp(still.data(), before.data(), u64{n} * n * sizeof(f32)) == 0);
  CHECK(std::fabs(static_cast<f64>(still[200 * n + shore] - still[120 * n + shore])) < 1e-4);
}

TEST_CASE("water: every stretch of shore knows its own slope") {
  // One body, two beaches: 1:30 for z under 40 m, 1:8 past 60 m, eased between; the waterline
  // along x = 30 m. 161 by 161 texels of 0.5 m.
  const u32 n = 161;
  const f64 texel = 0.5;
  Vector<f32> depth(n * n);
  const auto slope_at = [&](f64 z) {
    const f64 t = std::clamp((z - 40.0) / 20.0, 0.0, 1.0);
    const f64 s = t * t * (3.0 - 2.0 * t);
    return 1.0 / 30.0 + (1.0 / 8.0 - 1.0 / 30.0) * s;
  };
  for (u32 j = 0; j < n; ++j)
    for (u32 i = 0; i < n; ++i)
      depth[j * n + i] = static_cast<f32>((30.0 - texel * static_cast<f64>(i)) *
                                          slope_at(texel * static_cast<f64>(j)));
  Vector<f32> slope(n * n);
  gfx::water_shore_slope_field(depth.data(), n, texel, slope.data());
  // Over the bank, at the waterline and out in the water alike, a place has its stretch's slope.
  f64 gentle_worst = 0.0, steep_worst = 0.0;
  for (u32 j = 0; j < n; ++j) {
    const f64 z = texel * static_cast<f64>(j);
    for (const u32 i : {10u, 40u, 59u, 61u, 80u, 120u}) {
      const f64 here = static_cast<f64>(slope[j * n + i]);
      if (z < 36.0) gentle_worst = std::max(gentle_worst, std::fabs(here * 30.0 - 1.0));
      if (z > 64.0) steep_worst = std::max(steep_worst, std::fabs(here * 8.0 - 1.0));
    }
  }
  MESSAGE("shore slope: within " << gentle_worst << " of 1:30 and " << steep_worst << " of 1:8");
  // A plane's central difference is the plane's slope, to the map's f32.
  CHECK(gentle_worst < 1e-4);
  CHECK(steep_worst < 1e-4);
  // Between them it rises monotonically along the waterline.
  for (u32 j = 81; j < 120; ++j)
    CHECK(slope[j * n + 59] >= slope[(j - 1) * n + 59] - 1e-6f);
  // A map with no waterline: the default everywhere. The same field twice, to the bit.
  Vector<f32> dry(n * n), none(n * n);
  for (u32 i = 0; i < n * n; ++i)
    dry[i] = -1.0f;
  gfx::water_shore_slope_field(dry.data(), n, texel, none.data());
  CHECK(none[0] == static_cast<f32>(gfx::k_swash_slope_default));
  CHECK(none[n * n - 1] == static_cast<f32>(gfx::k_swash_slope_default));
  Vector<f32> again(n * n);
  gfx::water_shore_slope_field(depth.data(), n, texel, again.data());
  CHECK(std::memcmp(slope.data(), again.data(), u64{n} * n * sizeof(f32)) == 0);
}

// ---- S102, second pass: the energy flux as a field (water_flux.h; docs/experiments/
// shore-mechanics.md, "What the second pass built")
// ------------------------------------------------

namespace {

// The flux's fields over a square map.
struct Flux {
  Vector<f32> tau;
  Vector<f32> incident;
  Vector<f32> kept;
  Vector<f32> setup;
};

Flux flux_of(const Vector<f32>& depth, u32 n, f64 texel, f64 period, f64 height, Vec2 direction) {
  Flux f;
  f.tau.resize(n * n);
  f.incident.resize(n * n);
  f.kept.resize(n * n);
  f.setup.resize(n * n);
  gfx::water_arrival_field(depth.data(), n, texel, period, f.tau.data(), 3, direction);
  gfx::water_flux_field(depth.data(), f.tau.data(), n, texel, period, height, f.incident.data(),
                        f.kept.data(), f.setup.data());
  return f;
}

// What breaking takes of the flux ratio q per metre along a ray at depth d (water_flux.h's balance
// with the divergence taken out): (alpha / 4) f (gamma d)^2 Qb / (e0 cg0).
f64 flux_loss(f64 q, f64 d, f64 period, f64 height) {
  const f64 omega = 2.0 * 3.14159265358979323846 / period;
  const f64 k0 = omega * omega / gfx::k_swash_g;
  const f64 rms = gfx::water_group_rms();
  const f64 e0 = rms * rms * height * height / 8.0;
  const f64 cg0 = 0.5 * gfx::k_swash_g / omega;
  const f64 limit = gfx::k_water_breaking_gamma * d;
  const f64 crest = height * gfx::water_shoaling(gfx::water_wavenumber(k0, d), d) / limit;
  return gfx::k_water_breaking_alpha / 4.0 / period * limit * limit *
         gfx::water_group_breaking(crest * std::sqrt(std::max(q, 0.0))) / (e0 * cg0);
}

// q along a profile d(x) from x0 (where it is `q`) to x1 by fourth-order Runge-Kutta in steps of
// about a centimetre: the balance's own solution, which the field's sweep is held against.
// `variation` gathers the total variation of dq/dx along the way (`last` the slope it ended on):
// **the sweep's bound**. One step of the sweep is an implicit Euler step of this equation, whose
// right side never falls as q rises (more flux, more of the group breaking), so the steps do not
// amplify each other's errors and the sweep is off by no more than the sum of its local ones,
// (h^2 / 2) |q''| a step: (h / 2) times the total variation of q'.
template <typename Profile>
f64 flux_along(const Profile& d, f64 x0, f64 x1, f64 q, f64 period, f64 height, f64& variation,
               f64& last) {
  const u32 steps = static_cast<u32>(std::ceil((x1 - x0) / 0.01));
  const f64 h = (x1 - x0) / static_cast<f64>(steps);
  for (u32 s = 0; s < steps; ++s) {
    const f64 x = x0 + h * static_cast<f64>(s);
    const f64 k1 = -flux_loss(q, d(x), period, height);
    const f64 k2 = -flux_loss(q + 0.5 * h * k1, d(x + 0.5 * h), period, height);
    const f64 k3 = -flux_loss(q + 0.5 * h * k2, d(x + 0.5 * h), period, height);
    const f64 k4 = -flux_loss(q + h * k3, d(x + h), period, height);
    variation += std::fabs(k1 - last);
    last = k1;
    q += h * (k1 + 2.0 * k2 + 2.0 * k3 + k4) / 6.0;
  }
  return q;
}

}  // namespace

TEST_CASE("water: the flux between two rays is kept as they turn: Kr on a plane beach is Snell's") {
  // The Snell case's beach and train (1:20, the shore at x = 40 m, 4 s, 30 degrees off the
  // normal), under a wave too low to break anywhere it is read: 2 cm.
  const u32 n = 241;
  const f64 texel = 0.25;
  Vector<f32> depth(n * n);
  for (u32 j = 0; j < n; ++j)
    for (u32 i = 0; i < n; ++i)
      depth[j * n + i] = static_cast<f32>((40.0 - texel * static_cast<f64>(i)) / 20.0);
  const f64 period = gfx::swash_period(4.0);
  const f64 omega = 2.0 * 3.14159265358979323846 / period;
  const f64 k0 = omega * omega / gfx::k_swash_g;
  const f64 source = std::min(gfx::k_water_train_deep_kd / k0, 0.8 * 2.0);
  const f64 c_source = omega / gfx::water_wavenumber(k0, source);
  const f64 sin0 = 0.5;
  const f64 cos0 = std::sqrt(1.0 - sin0 * sin0);
  const Flux f = flux_of(depth, n, texel, period, 0.02, Vec2{0.8660254f, 0.5f});
  // Between two rays a distance b apart along the crest the flux E cg b is kept, and on straight
  // contours b is cos(theta) of the spacing along the shore: H / (H0 Ks) = sqrt(cos(theta0) /
  // cos(theta)), theta Snell's. Read where the wave has crossed the whole beach from the source,
  // as the Snell case reads its angle.
  f64 worst = 0.0, kept_worst = 0.0, least = 1.0;
  for (u32 j = 110; j <= 220; j += 10) {
    for (u32 i = 8; i + 8 < n; ++i) {
      const u32 at = j * n + i;
      const f64 d = static_cast<f64>(depth[at]);
      if (!(d > 0.3 && d < 0.9 * source)) continue;
      const f64 sin_t = sin0 * (omega / gfx::water_wavenumber(k0, d)) / c_source;
      const f64 kr = std::sqrt(cos0 / std::sqrt(1.0 - sin_t * sin_t));
      worst = std::max(worst, std::fabs(static_cast<f64>(f.incident[at]) / kr - 1.0));
      kept_worst = std::max(kept_worst, std::fabs(static_cast<f64>(f.kept[at]) - 1.0));
      least = std::min(least, kr);
    }
  }
  MESSAGE("Kr on 1:20, 4 s, 30 degrees: the field within "
          << worst << " of sqrt(cos0 / cos), "
          << "which falls to " << least << "; kept within " << kept_worst << " of 1");
  // The scheme telescopes the flux through each line of faces exactly, so what is left is the
  // arrival's own angle (held to 0.7 degrees above: d(Kr) / Kr = tan(theta) d(theta) / 2, 0.0035
  // at 30 degrees) and the half texel between a texel and its face (Kr falls 0.07 over 26 m:
  // 0.0004): under 0.005.
  CHECK(worst < 0.005);
  CHECK(least < 0.95);       // and the turn is there to be seen
  CHECK(kept_worst < 1e-6);  // nothing broke
  // The same fields twice, to the bit.
  const Flux g = flux_of(depth, n, texel, period, 0.02, Vec2{0.8660254f, 0.5f});
  CHECK(std::memcmp(f.incident.data(), g.incident.data(), u64{n} * n * sizeof(f32)) == 0);
  CHECK(std::memcmp(f.kept.data(), g.kept.data(), u64{n} * n * sizeof(f32)) == 0);
  CHECK(std::memcmp(f.setup.data(), g.setup.data(), u64{n} * n * sizeof(f32)) == 0);
}

TEST_CASE(
    "water: on a plane beach the flux is kept to the break and the group's breaking takes "
    "it, and the level is set down then up") {
  // 1:30 from 6 m deep to the shore at x = 180 m, the bank beyond: 401 texels of 0.5 m; a 1 m,
  // 6 s sea straight on.
  const u32 n = 401;
  const f64 texel = 0.5;
  const f64 height = 1.0;
  const f64 period = gfx::swash_period(6.0);
  const auto profile = [](f64 x) { return (180.0 - x) / 30.0; };
  Vector<f32> depth(n * n);
  for (u32 j = 0; j < n; ++j)
    for (u32 i = 0; i < n; ++i)
      depth[j * n + i] = static_cast<f32>(profile(texel * static_cast<f64>(i)));
  const Flux f = flux_of(depth, n, texel, period, height, Vec2{1.0f, 0.0f});
  const f64 omega = 2.0 * 3.14159265358979323846 / period;
  const f64 k0 = omega * omega / gfx::k_swash_g;
  const f64 rms = gfx::water_group_rms();
  const u32 row = 200 * n;
  // The source ends where the depth falls under min(1 / k0, 0.8 of the deepest).
  const f64 source = std::min(gfx::k_water_train_deep_kd / k0, 0.8 * 6.0);
  u32 first = 0;
  while (static_cast<f64>(depth[row + first]) >= source)
    ++first;
  // (1) To the break the flux is the deep water's: nothing between two straight rays takes it.
  // (2) From there it is the balance's own solution — dq/dx = -(alpha / 4) f (gamma d)^2 Qb / (e0
  // cg0), Runge-Kutta in centimetres — to the sweep's order (`flux_along` has the bound: half a
  // texel times the total variation of dq/dx so far).
  f64 before_worst = 0.0, surf_worst = 0.0, surf_over = 0.0, variation = 0.0, slope = 0.0;
  u32 unbroken = 0, broken = 0;
  f64 q_ref = 1.0;
  for (u32 i = first; i < n; ++i) {
    const f64 d = static_cast<f64>(depth[row + i]);
    if (!(d > 0.15)) break;
    const f64 x = texel * static_cast<f64>(i);
    q_ref = flux_along(profile, x - texel, x, q_ref, period, height, variation, slope);
    const f64 got = static_cast<f64>(f.kept[row + i]) * static_cast<f64>(f.kept[row + i]);
    CHECK(static_cast<f64>(f.incident[row + i]) == doctest::Approx(1.0).epsilon(1e-6));
    const f64 crest = height * gfx::water_shoaling(gfx::water_wavenumber(k0, d), d) /
                      (gfx::k_water_breaking_gamma * d);
    if (gfx::water_group_breaking(crest) == 0.0) {
      before_worst = std::max(before_worst, std::fabs(got - 1.0));
      ++unbroken;
    } else {
      // The bound, and f32's step on the square of a stored ratio.
      const f64 err = std::fabs(got - q_ref);
      surf_worst = std::max(surf_worst, err);
      surf_over = std::max(surf_over, err - (0.5 * texel * variation + 1e-6));
      ++broken;
    }
  }
  MESSAGE("plane beach 1:30, 1 m at 6 s: the flux within "
          << before_worst << " of deep water's over " << unbroken
          << " texels to the break; through the surf (" << broken << " texels) within "
          << surf_worst << " of the balance's solution, the bound"
          << " at the last of them " << 0.5 * texel * variation << "; " << q_ref
          << " of it left at 15 cm");
  CHECK(unbroken > 50u);
  CHECK(broken > 50u);
  CHECK(before_worst < 1e-6);
  CHECK(surf_over <= 0.0);
  CHECK(q_ref < 0.05);  // nearly all of it is gone by the shore
  // (3) The mean level. Outside the surf it is Longuet-Higgins and Stewart's set-down under the
  // group as drawn, -E k / sinh(2 k d), E = rms^2 H^2 Ks^2 / 8: the sweep integrates d(eta) =
  // -d(S) / (d + eta) from the source's own set-down a texel at a time, and a one-sided step is
  // off by no more than the level's change over it, texel * max |d(eta)/dx|; and the closed form
  // itself leaves eta out beside d, which is worth eta^2 / d.
  f64 down_worst = 0.0, down_steepest = 0.0, lowest = 0.0, shallowest = 6.0;
  const auto set_down = [&](f64 d) {
    const f64 k = gfx::water_wavenumber(k0, d);
    const f64 hs = rms * height * gfx::water_shoaling(k, d);
    return gfx::water_set_down(hs * hs / 8.0, k, d);
  };
  for (u32 i = first; i < n; ++i) {
    const f64 d = static_cast<f64>(depth[row + i]);
    const f64 crest = height * gfx::water_shoaling(gfx::water_wavenumber(k0, d), d) /
                      (gfx::k_water_breaking_gamma * d);
    // The closed form is the uncapped group's: read it where no wave of the group is capped.
    if (!(crest < 0.9)) break;
    down_worst = std::max(down_worst, std::fabs(static_cast<f64>(f.setup[row + i]) - set_down(d)));
    down_steepest =
        std::max(down_steepest, std::fabs(set_down(d) - set_down(d + texel / 30.0)) / texel);
    lowest = std::min(lowest, set_down(d));
    shallowest = std::min(shallowest, d);
  }
  const f64 down_bound = texel * down_steepest + lowest * lowest / shallowest;
  MESSAGE("set-down to " << lowest << " m at " << shallowest << " m of depth: the field within "
                         << down_worst << " m, bound " << down_bound << " m");
  CHECK(lowest < -0.01);
  CHECK(down_worst <= down_bound);
  // Through the surf it rises, to a setup at the shore; and the balance closes: the sum of (d +
  // eta) d(eta) over the profile is the stress lost, to the half d(eta)^2 a step the one-sided
  // depth leaves.
  u32 shore = first;
  while (shore + 1 < n && depth[row + shore + 1] > 0.0f)
    ++shore;
  const f64 at_shore = static_cast<f64>(f.setup[row + shore]);
  CHECK(at_shore > 0.02);
  CHECK(at_shore < 0.5 * height);
  // Over the bank it is its shoreline's.
  CHECK(f.setup[row + shore + 20] == f.setup[row + shore]);
  MESSAGE("setup at the shore " << at_shore << " m under a 1 m sea");
}

TEST_CASE(
    "water: a wave that broke on a bar carries less across the trough, and breaks again "
    "only where the depth asks") {
  // 5 m deep, up 1:30 to a bar 0.8 m under the level and 10 m wide, down 1:15 into a trough 2.5 m
  // deep and 30 m wide, then 1:30 to the shore: 0.5 m texels, a 1 m, 6 s sea straight on.
  const f64 texel = 0.5;
  const f64 height = 1.0;
  const f64 period = gfx::swash_period(6.0);
  const f64 bar0 = 126.0, bar1 = 136.0, trough0 = 161.5, trough1 = 191.5, shore_x = 266.5;
  const auto profile = [&](f64 x) {
    if (x < bar0) return 0.8 + (bar0 - x) / 30.0;
    if (x < bar1) return 0.8;
    if (x < trough0) return 0.8 + (x - bar1) / 15.0;
    if (x < trough1) return 2.5;
    return (shore_x - x) / 30.0;
  };
  const u32 n = 561;
  Vector<f32> depth(n * n);
  for (u32 j = 0; j < n; ++j)
    for (u32 i = 0; i < n; ++i)
      depth[j * n + i] = static_cast<f32>(profile(texel * static_cast<f64>(i)));
  const Flux f = flux_of(depth, n, texel, period, height, Vec2{1.0f, 0.0f});
  const f64 omega = 2.0 * 3.14159265358979323846 / period;
  const f64 k0 = omega * omega / gfx::k_swash_g;
  const u32 row = 280 * n;
  const auto crest_at = [&](f64 d) {
    return height * gfx::water_shoaling(gfx::water_wavenumber(k0, d), d) /
           (gfx::k_water_breaking_gamma * d);
  };
  // On the bar the unbroken wave stands over the limit, and in the trough under it.
  REQUIRE(crest_at(0.8) > 1.3);
  REQUIRE(gfx::water_group_breaking(crest_at(2.5)) == 0.0);
  // Across the trough the flux is one number: what the bar left.
  f64 lo = 1.0, hi = 0.0;
  for (u32 i = static_cast<u32>(trough0 / texel) + 2; i < static_cast<u32>(trough1 / texel) - 2;
       ++i) {
    const f64 q = static_cast<f64>(f.kept[row + i]) * static_cast<f64>(f.kept[row + i]);
    lo = std::min(lo, q);
    hi = std::max(hi, q);
  }
  // And it is the balance's own solution over the bar, to the sweep's order (the plane beach's
  // bound).
  const f64 source = std::min(gfx::k_water_train_deep_kd / k0, 0.8 * 5.0);
  f64 x0 = 0.0;
  while (profile(x0) >= source)
    x0 += texel;
  f64 variation = 0.0, slope = 0.0;
  const f64 q_ref =
      flux_along(profile, x0 - texel, trough0 + 5.0, 1.0, period, height, variation, slope);
  MESSAGE("bar: " << lo << " of the flux crosses the trough (the balance's solution " << q_ref
                  << ", bound " << 0.5 * texel * variation << "), the same to " << hi - lo
                  << " across it; the wave there " << std::sqrt(lo) << " of the unbroken one");
  CHECK(hi - lo < 1e-6);  // f32's own step on a ratio: nothing is taken, nothing given back
  CHECK(lo < 0.7);        // it broke
  CHECK(std::fabs(lo - q_ref) <= 0.5 * texel * variation + 1e-6);
  // The wave it carries is under the limit across the trough — no foam is born there, where the
  // closed form of the depth alone was whole again — and on the beach behind it breaks in
  // shallower water than a wave that had met no bar.
  f64 first_break = 0.0, plain_break = 0.0;
  for (u32 i = static_cast<u32>(trough1 / texel); i < n; ++i) {
    const f64 d = static_cast<f64>(depth[row + i]);
    if (!(d > 0.05)) break;
    const f64 here = crest_at(d);
    if (plain_break == 0.0 && gfx::water_group_breaking(here) > 0.0) plain_break = d;
    if (first_break == 0.0 &&
        gfx::water_group_breaking(here * static_cast<f64>(f.kept[row + i])) > 0.0)
      first_break = d;
  }
  MESSAGE("behind the bar the group's largest wave breaks at "
          << first_break << " m; with no bar at " << plain_break << " m");
  CHECK(first_break > 0.0);
  CHECK(first_break < plain_break);
}

TEST_CASE("water: a wall casts a lee, and the sea beside it is the sea") {
  // A shelf 2 m deep behind 5 m of deep water, a wall of dry ground 40 texels wide across the
  // train's way, 161 texels of 0.5 m; a low 4 s train straight on, so nothing breaks.
  const u32 n = 161;
  const f64 texel = 0.5;
  const u32 wall_x = 60, wall_lo = 60, wall_hi = 100;  // the wall: one texel thick, j in [60, 100)
  Vector<f32> depth(n * n);
  for (u32 j = 0; j < n; ++j) {
    for (u32 i = 0; i < n; ++i) {
      f32 d = i < 20 ? 5.0f : 2.0f;
      if (i == wall_x && j >= wall_lo && j < wall_hi) d = -1.0f;
      depth[j * n + i] = d;
    }
  }
  const Flux f = flux_of(depth, n, texel, gfx::swash_period(4.0), 0.02, Vec2{1.0f, 0.0f});
  // Far to the side of the wall the sea is the sea; nothing focuses on a level shelf.
  f64 side_worst = 0.0, most = 0.0;
  for (u32 i = 20; i < n; ++i) {
    side_worst = std::max(side_worst, std::fabs(static_cast<f64>(f.incident[10 * n + i]) - 1.0));
    for (u32 j = 0; j < n; ++j)
      most = std::max(most, static_cast<f64>(f.incident[j * n + i]));
  }
  CHECK(side_worst < 1e-6);
  (void)most;
  // **What the wall took is missing behind it at every distance, and nothing else**: the scheme is
  // conservative, so the flux through a whole line of faces across the map is the same line after
  // line where nothing absorbs it — the sea's 161 texels' worth before the wall, and 121 behind it,
  // however the rays that creep round its ends have spread what is left. The flux through a face is
  // the upwind texel's incident^2 times the face's direction along x, the two texels' mean, as the
  // sweep takes it. (The stored ratio's floor is worth 0.02^2 a shadowed texel: 0.02 over the
  // wall.)
  const auto travel_x = [&](u32 i, u32 j) {
    const auto wet = [&](i64 x, i64 z) {
      return x >= 0 && z >= 0 && x < i64{n} && z < i64{n} &&
             depth[static_cast<u32>(z * i64{n} + x)] > 0.0f;
    };
    const auto tau = [&](i64 x, i64 z) {
      return static_cast<f64>(f.tau[static_cast<u32>(z * i64{n} + x)]);
    };
    const i64 a = i, b = j;
    f64 gx = 0.0, gz = 0.0;
    if (wet(a - 1, b) && wet(a + 1, b))
      gx = 0.5 * (tau(a + 1, b) - tau(a - 1, b));
    else if (wet(a + 1, b))
      gx = tau(a + 1, b) - tau(a, b);
    else if (wet(a - 1, b))
      gx = tau(a, b) - tau(a - 1, b);
    if (wet(a, b - 1) && wet(a, b + 1))
      gz = 0.5 * (tau(a, b + 1) - tau(a, b - 1));
    else if (wet(a, b + 1))
      gz = tau(a, b + 1) - tau(a, b);
    else if (wet(a, b - 1))
      gz = tau(a, b) - tau(a, b - 1);
    const f64 len = std::sqrt(gx * gx + gz * gz);
    return len > 1e-12 ? gx / len : 0.0;
  };
  const auto through = [&](u32 i) {
    f64 sum = 0.0;
    for (u32 j = 0; j < n; ++j) {
      const f64 face = 0.5 * (travel_x(i, j) + travel_x(i + 1, j));
      const f64 up = static_cast<f64>(f.incident[j * n + (face > 0.0 ? i : i + 1)]);
      sum += face * up * up;
    }
    return sum;
  };
  const f64 width = static_cast<f64>(wall_hi - wall_lo);
  const f64 before = through(40);
  MESSAGE("a wall " << width << " texels wide: " << before << " texels' flux before it, "
                    << through(wall_x + 3) << ", " << through(wall_x + 20) << " and "
                    << through(wall_x + 60) << " at 3, 20 and 60 texels behind");
  CHECK(before == doctest::Approx(static_cast<f64>(n)).epsilon(1e-6));
  for (const u32 behind : {3u, 20u, 60u})
    CHECK(std::fabs(through(wall_x + behind) - (static_cast<f64>(n) - width)) < 0.05);
  // The lee is quiet behind the wall's middle and fills in with distance.
  const f64 near_wall = static_cast<f64>(f.incident[80 * n + wall_x + 3]);
  const f64 far_behind = static_cast<f64>(f.incident[80 * n + wall_x + 80]);
  MESSAGE("behind the wall's middle: " << near_wall << " of the sea's height 3 texels behind, "
                                       << far_behind << " at 80");
  CHECK(near_wall < far_behind);
  CHECK(far_behind < 1.0);
}

TEST_CASE(
    "water: a wind sea's second train is the spectrum's own, eight waves in the peak's "
    "seven") {
  const f64 period = gfx::swash_period(4.7);
  const gfx::WaterCross c = gfx::water_cross_of_sea(1.1, period, Vec2{1.0f, 0.0f}, 1u);
  // The two trains hold the sea's energy between them, in JONSWAP's ratio of the spectrum at 8/7
  // of the peak to the peak's: (8/7)^-5 exp(-(5/4)((7/8)^4 - 1)) 3.3^(exp(-(1/7)^2 / (2 0.09^2)) -
  // 1) = 0.5129 * 1.6775 * 0.4252 = 0.3658.
  CHECK(c.train_m * c.train_m + c.cross_m * c.cross_m == doctest::Approx(1.1 * 1.1).epsilon(1e-12));
  CHECK(c.cross_m * c.cross_m / (c.train_m * c.train_m) == doctest::Approx(0.3658).epsilon(0.002));
  CHECK(c.height_m == doctest::Approx(1.1));
  // Eight of its waves in the first's seven, so the pair is periodic in the group and the water's
  // clock holds a whole number of both.
  CHECK(c.period_s * 8.0 == doctest::Approx(period * 7.0).epsilon(1e-12));
  const f64 turns = gfx::k_swash_clock_s / c.period_s;
  CHECK(std::fabs(turns - std::floor(turns + 0.5)) < 1e-9);
  // Turned by the half width at half maximum of Donelan and Banner's sech^2(beta theta) at 8/7 of
  // the peak: acosh(sqrt 2) / (2.28 (8/7)^-1.3) = 0.8814 / 1.9166 = 26.35 degrees; the seed picks
  // the side.
  const f64 angle = std::atan2(static_cast<f64>(c.direction.y), static_cast<f64>(c.direction.x)) *
                    57.29577951308232;
  CHECK(angle == doctest::Approx(26.35).epsilon(0.005));
  const gfx::WaterCross other = gfx::water_cross_of_sea(1.1, period, Vec2{1.0f, 0.0f}, 2u);
  CHECK(other.direction.y == doctest::Approx(-static_cast<f64>(c.direction.y)));
  // Beside a swell the second train is the wind's peak wave, its period moved onto the group.
  const gfx::WaterCross s =
      gfx::water_cross_of_swell(0.9, gfx::swash_period(10.0), 1.1, 4.7, Vec2{0.0f, 1.0f});
  CHECK(s.train_m == doctest::Approx(0.9));
  CHECK(s.cross_m == doctest::Approx(1.1));
  const f64 waves = 7.0 * gfx::swash_period(10.0) / s.period_s;
  CHECK(std::fabs(waves - std::floor(waves + 0.5)) < 1e-9);
  CHECK(s.period_s == doctest::Approx(4.7).epsilon(0.05));
  CHECK(s.height_m == doctest::Approx(std::sqrt(0.9 * 0.9 + 1.1 * 1.1)));
}

// S102's stage 3, as far as it is built: the closed forms of a disc in the swash (water_obstacle.h;
// shore-mechanics.md, "Stage 3: a shore of rock, and a boulder in the swash"). Nothing draws them.
TEST_CASE(
    "water: the way round a boulder in the swash is the tangent's, and the pile is the "
    "front's speed head") {
  gfx::WaterDisc disc;
  disc.x = 3.0;
  disc.z = -2.0;
  disc.radius = 0.8;
  const f64 r = disc.radius;
  const f64 wx = 0.6, wz = 0.8;   // the sheet's way (unit)
  const f64 ax = -0.8, az = 0.6;  // across it
  const auto at = [&](f64 along, f64 across) {
    return gfx::water_disc_detour(disc, wx, wz, disc.x + along * wx + across * ax,
                                  disc.z + along * wz + across * az);
  };
  const f64 half_pi = 1.5707963267948966;
  // Touching the disc's back, the front has gone a quarter of the way round less the radius it
  // would have crossed; before the disc, beside it and on the shadow's edge it has gone no further.
  CHECK(at(r, 0.0) == doctest::Approx((half_pi - 1.0) * r).epsilon(1e-12));
  CHECK(at(-0.5, 0.2) == 0.0);
  CHECK(at(2.0, 1.2 * r) == 0.0);
  CHECK(at(2.0, r) == 0.0);
  CHECK(at(2.0, 0.999999 * r) < 1e-6);
  // Far behind, the lee closes as r^2 / (2 rho).
  CHECK(at(40.0 * r, 0.0) == doctest::Approx(r / 80.0).epsilon(1e-3));
  // Against a scan of every way round: to the disc's side, a quarter turn or less along its back,
  // and straight on from wherever the straight line leaves the disc outward; less the open
  // beach's way to the same place.
  f64 worst = 0.0;
  for (const f64 along : {0.3, 0.9, 1.5, 4.0}) {
    for (const f64 across : {0.0, 0.25, 0.5, 0.75}) {
      if (along * along + across * across < r * r) continue;
      f64 best = 1e30;
      const u32 steps = 20000;
      for (u32 s = 0; s <= steps; ++s) {
        const f64 turn = half_pi * static_cast<f64>(s) / static_cast<f64>(steps);
        const f64 lx = r * std::sin(turn);  // along the way, from the side round the back
        const f64 lc = r * std::cos(turn);  // across it, on the point's side
        if ((along - lx) * lx + (across - lc) * lc < 0.0) continue;  // would cut through the disc
        best = std::min(best, r + r * turn + std::hypot(along - lx, across - lc));
      }
      worst = std::max(worst, std::fabs(best - (along + r) - at(along, across)));
    }
  }
  MESSAGE("the detour round a disc: the closed form within " << worst << " m of the scan");
  CHECK(worst < 1e-7);
  // The pile against the face is what the front had left to climb: u^2 / 2g with u^2 = 2 g E.
  CHECK(gfx::water_disc_pile(gfx::swash_front_speed(0.3), 1.0) == doctest::Approx(0.3));
  CHECK(gfx::water_disc_pile(gfx::swash_front_speed(0.3), 0.5) == doctest::Approx(0.075));
  CHECK(gfx::water_disc_pile(gfx::swash_front_speed(0.3), -0.2) == 0.0);
  // A rock lower than the pile is overtopped; one that stands over it is climbed if its face is a
  // ramp and throws the sheet if it is steep (xi = slope / sqrt(H / L0): 5.6 and 1.1 under the
  // beach scene's 1.1 m, 4.7 s sea, either side of Battjes's 3.3).
  disc.top_m = 0.2;
  CHECK(gfx::water_disc_meeting(disc, gfx::swash_front_speed(0.3), 1.0, 1.1, 4.7) ==
        gfx::WaterDiscMeeting::Overtops);
  disc.top_m = 1.0;
  CHECK(gfx::water_disc_meeting(disc, gfx::swash_front_speed(0.3), 1.0, 1.1, 4.7) ==
        gfx::WaterDiscMeeting::Thrown);
  CHECK(gfx::water_disc_meeting(disc, gfx::swash_front_speed(0.3), 0.2, 1.1, 4.7) ==
        gfx::WaterDiscMeeting::RunsUp);
}

// S102's stage 4, as far as it is built: the published forms of a wave ripple on a sandy bed
// (water_ripples.h; shore-mechanics.md, "Stage 4: the sand"). Nothing draws them.
TEST_CASE("water: wave ripples on a sandy bed follow the orbit at the bed and the grain") {
  // By hand, a 0.2 m, 4 s wave over 3 m of water on 0.3 mm sand: k0 = (2 pi / 4)^2 / g = 0.2515,
  // k d tanh(k d) = 0.7546 at k d = 0.994, sinh = 1.166; A = 0.1 / 1.166 = 0.0857 m, U0 = A omega =
  // 0.1347 m/s; psi = 0.01814 / (1.65 * 9.81 * 0.0003) = 3.74; lambda = A (2.2 - 0.345 * 3.74^0.34)
  // = 0.0857 * 1.660 = 0.142 m; A / D = 286, far under 5000, so the steepness is the whole 0.15.
  const gfx::WaveRipple r = gfx::water_wave_ripple(0.2, 4.0, 3.0, 3e-4);
  CHECK(r.orbit_m == doctest::Approx(0.0857).epsilon(0.01));
  CHECK(r.mobility == doctest::Approx(3.74).epsilon(0.02));
  CHECK(r.length_m == doctest::Approx(0.142).epsilon(0.02));
  CHECK(r.height_m == doctest::Approx(0.15 * r.length_m).epsilon(1e-9));
  // Long and low under a sea offshore, shorter as the depth falls and the bed is worked harder,
  // gone under the surf; short and sharp under a lake's wavelets; none on a bed coarser than sand.
  const gfx::WaveRipple deep = gfx::water_wave_ripple(1.1, 4.7, 5.0, 3e-4);
  const gfx::WaveRipple shallow = gfx::water_wave_ripple(1.1, 4.7, 3.0, 3e-4);
  const gfx::WaveRipple surf = gfx::water_wave_ripple(1.1, 4.7, 1.2, 3e-4);
  const gfx::WaveRipple lake = gfx::water_wave_ripple(0.13, 1.34, 0.5, 3e-4);
  MESSAGE("ripples under 1.1 m at 4.7 s on 0.3 mm sand: "
          << deep.length_m << " m at 5 m (psi " << deep.mobility << "), " << shallow.length_m
          << " m at 3 m (psi " << shallow.mobility << "), " << surf.length_m << " m at 1.2 m (psi "
          << surf.mobility << "); under a lake's 0.13 m at 1.34 s over 0.5 m " << lake.length_m
          << " m (psi " << lake.mobility << ")");
  CHECK(deep.mobility < shallow.mobility);
  CHECK(shallow.mobility < surf.mobility);
  CHECK(deep.length_m > shallow.length_m);
  CHECK(shallow.length_m > 0.0);
  CHECK(surf.length_m == 0.0);  // psi past where Nielsen's form reaches zero
  CHECK(lake.length_m > 0.02);
  CHECK(lake.length_m < 0.12);
  CHECK(gfx::water_wave_ripple(0.2, 4.0, 3.0, 5e-3).length_m == 0.0);
  CHECK(gfx::water_wave_ripple(0.0, 4.0, 3.0, 3e-4).length_m == 0.0);
}

TEST_CASE("water: a body with a wind has its mesh grown over the bank, one without keeps its own") {
  const scene_gen::Ground ground = bowl_ground();
  scene::WaterBody block = circle_block(100.0, 50.0, 10.0f, 0.0);
  const water::Body still = read(block);
  water::Mesh plain;
  water::build_mesh(still, ground, plain);
  CHECK(plain.rim == 0u);
  block.wind_m_s = 8.0f;
  const water::Body windy = read(block);
  water::Mesh grown;
  water::build_mesh(windy, ground, grown);
  CHECK(grown.rim > 0u);
  CHECK(grown.edge == plain.edge);
  REQUIRE(grown.positions.size() == grown.edge * grown.rings + 1);
  // The windless mesh says so of itself: one spacing, no grade.
  CHECK(plain.near_spacing == plain.spacing);
  CHECK(plain.grade == 0.0f);
  // The rim's vertices stand outside the outline over the bank, the outermost first.
  for (u32 j = 0; j < grown.edge; ++j) {
    const Vec3 p = grown.positions[j];
    CHECK(std::sqrt(p.x * p.x + p.z * p.z) > 10.0f);
    CHECK(grown.uvs[j].x == 0.0f);
  }
  // **Rings closer together at the outline** (S100 stage B; re-pinned: until then the rings inside
  // were the windless mesh's, vertex for vertex). The first ring inside the rim is the outline, the
  // same vertices the windless mesh starts with; the gaps from it inwards start at `near_spacing`
  // and each is at most 1.15 of the one outside it and `spacing`, as are the rim's outwards.
  CHECK(grown.near_spacing < grown.spacing);
  CHECK(grown.near_spacing >= 0.25f);
  CHECK(grown.grade == doctest::Approx(0.15f));
  const u32 skip = grown.rim * grown.edge;
  for (u32 j = 0; j < grown.edge; ++j) {
    CHECK(grown.positions[skip + j] == plain.positions[j]);
    CHECK(grown.uvs[skip + j] == plain.uvs[j]);
  }
  const auto radius = [&](u32 ring) {
    const Vec3 p = grown.positions[ring * grown.edge];
    return static_cast<f64>(std::sqrt(p.x * p.x + p.z * p.z));
  };
  f64 last_gap = 0.0;
  for (u32 r = grown.rim; r + 1 < grown.rings; ++r) {
    const f64 gap = radius(r) - radius(r + 1);
    if (r == grown.rim)
      CHECK(gap == doctest::Approx(static_cast<f64>(grown.near_spacing)).epsilon(1e-4));
    CHECK(gap > 0.0);
    CHECK(gap <= static_cast<f64>(grown.spacing) * 1.0001);
    if (last_gap > 0.0) CHECK(gap <= last_gap * 1.1501);
    if (last_gap > 0.0) CHECK(gap >= last_gap * 0.9999);
    last_gap = gap;
  }
  CHECK(radius(grown.rim - 1) - radius(grown.rim) ==
        doctest::Approx(static_cast<f64>(grown.near_spacing)).epsilon(1e-4));
  MESSAGE("an 8 m/s wind over a 20 m pool: "
          << grown.rim << " rim rings, " << grown.rings << " rings in all against the windless "
          << plain.rings << ", the gaps from " << grown.near_spacing << " m to " << grown.spacing
          << " m, " << grown.positions.size() - plain.positions.size() << " vertices more");
}

TEST_CASE("water: the polynomial sine is the sine to 1e-14 everywhere a circle asks") {
  for (u32 i = 0; i <= 4096; ++i) {
    const f64 a = 6.283185307179586 * static_cast<f64>(i) / 4096.0;
    f64 s = 0.0;
    f64 c = 0.0;
    water::sin_cos(a, s, c);
    CHECK(std::fabs(s - std::sin(a)) < 1e-14);
    CHECK(std::fabs(c - std::cos(a)) < 1e-14);
  }
}

TEST_CASE("water: a block is read, and refused with a sentence when it cannot be drawn") {
  const water::Body body = read(circle_block(100.0, 50.0, 12.0f, 0.0));
  CHECK(body.circle());
  CHECK(body.center == WorldPos{100.0, 0.0, 50.0});
  std::string error;
  water::Body out;
  scene::WaterBody none = circle_block(0.0, 0.0, 5.0f, 0.0);
  none.shape.circle.reset();
  CHECK_FALSE(water::read_body(none, out, &error));
  CHECK(error.find("exactly one") != std::string::npos);
  scene::WaterBody both = circle_block(0.0, 0.0, 5.0f, 0.0);
  both.shape.polygon = {WorldPos{0, 0, 0}, WorldPos{1, 0, 0}, WorldPos{0, 0, 1}};
  CHECK_FALSE(water::read_body(both, out, &error));
  scene::WaterBody flat = circle_block(0.0, 0.0, 0.0f, 0.0);
  CHECK_FALSE(water::read_body(flat, out, &error));
  scene::WaterBody rough = circle_block(0.0, 0.0, 5.0f, 0.0);
  rough.roughness = 0.0f;
  CHECK_FALSE(water::read_body(rough, out, &error));
  CHECK(error.find("roughness") != std::string::npos);
  scene::WaterBody chop = circle_block(0.0, 0.0, 5.0f, 0.0);
  chop.chop = 1.5f;
  CHECK_FALSE(water::read_body(chop, out, &error));
  // A polygon in either winding, its centroid the centre; one not star-shaped from it is refused.
  scene::WaterBody square;
  square.level_m = 1.0;
  square.shape.polygon = {WorldPos{10, 0, 10}, WorldPos{10, 0, 14}, WorldPos{14, 0, 14},
                          WorldPos{14, 0, 10}};
  const water::Body sq = read(square);
  CHECK(sq.center.x == doctest::Approx(12.0));
  CHECK(sq.center.z == doctest::Approx(12.0));
  CHECK(sq.center.y == 1.0);
  REQUIRE(sq.outline.size() == 4);
  scene::WaterBody arrow;
  // A star from its centroid it is not: a C whose centroid, (4.08, 5), is in its own slot.
  arrow.shape.polygon = {WorldPos{0, 0, 0},   WorldPos{10, 0, 0}, WorldPos{10, 0, 2},
                         WorldPos{2, 0, 2},   WorldPos{2, 0, 8},  WorldPos{10, 0, 8},
                         WorldPos{10, 0, 10}, WorldPos{0, 0, 10}};
  CHECK_FALSE(water::read_body(arrow, out, &error));
  CHECK(error.find("star-shaped") != std::string::npos);
}

TEST_CASE("water: inside a body, at its edge, and outside it") {
  const water::Body circle = read(circle_block(0.0, 0.0, 10.0f, 0.0));
  CHECK(water::inside_distance(circle, 0.0f, 0.0f) == 10.0f);
  CHECK(water::inside_distance(circle, 6.0f, 8.0f) == 0.0f);
  CHECK(water::inside_distance(circle, 9.0f, 0.0f) == doctest::Approx(1.0f));
  CHECK(water::inside_distance(circle, 11.0f, 0.0f) < 0.0f);
  scene::WaterBody square;
  square.shape.polygon = {WorldPos{-4, 0, -4}, WorldPos{4, 0, -4}, WorldPos{4, 0, 4},
                          WorldPos{-4, 0, 4}};
  const water::Body sq = read(square);
  CHECK(water::inside_distance(sq, 0.0f, 0.0f) == doctest::Approx(4.0f));
  CHECK(water::inside_distance(sq, 3.0f, 0.0f) == doctest::Approx(1.0f));
  CHECK(water::inside_distance(sq, 4.0f, 4.0f) == doctest::Approx(0.0f).epsilon(1e-5));
  CHECK(water::inside_distance(sq, 4.5f, 0.0f) < 0.0f);
  CHECK(water::inside_distance(sq, -3.0f, 3.9f) > 0.0f);
}

TEST_CASE(
    "water: the depth at a point is the level less the ground, inside a body, never below 0") {
  const scene_gen::Ground ground = bowl_ground();
  water::Body bodies[2] = {read(circle_block(100.0, 50.0, 12.0f, 0.0)),
                           read(circle_block(100.0, 50.0, 3.0f, 0.5))};
  // At the centre the deeper of the two: the small one's level is half a metre higher.
  CHECK(water::depth_at(bodies, ground, WorldPos{100.0, 7.0, 50.0}) == doctest::Approx(2.5f));
  CHECK(water::depth_at(std::span<const water::Body>(bodies, 1), ground,
                        WorldPos{100.0, 0.0, 50.0}) == doctest::Approx(2.0f));
  // Five metres out: 2 - 0.5 = 1.5 m.
  CHECK(water::depth_at(bodies, ground, WorldPos{105.0, 0.0, 50.0}) == doctest::Approx(1.5f));
  // The shore: inside the outline, the ground above the water.
  CHECK(water::depth_at(bodies, ground, WorldPos{111.0, 0.0, 50.0}) == 0.0f);
  // Outside every body.
  CHECK(water::depth_at(bodies, ground, WorldPos{113.0, 0.0, 50.0}) == 0.0f);
  CHECK(water::depth_at(std::span<const water::Body>(), ground, WorldPos{100.0, 0.0, 50.0}) ==
        0.0f);
}

TEST_CASE("water: the mesh is rings to a fan, facing up, carrying the depth under every vertex") {
  const scene_gen::Ground ground = bowl_ground();
  const water::Body body = read(circle_block(100.0, 50.0, 12.0f, 0.0));
  water::Mesh mesh;
  water::build_mesh(body, ground, mesh);
  // The shore's default 2 m asks for two thirds of a metre: 114 edge samples and 18 rings.
  CHECK(mesh.spacing == doctest::Approx(2.0f / 3.0f));
  CHECK(mesh.edge == 114u);
  CHECK((mesh.rings == 18u || mesh.rings == 19u));  // 12 m over a float's two thirds
  REQUIRE(mesh.positions.size() == mesh.edge * mesh.rings + 1);
  REQUIRE(mesh.uvs.size() == mesh.positions.size());
  REQUIRE(mesh.normals.size() == mesh.positions.size());
  CHECK(mesh.indices.size() == (mesh.rings - 1) * mesh.edge * 6 + mesh.edge * 3);
  MESSAGE("a 24 m pool: " << mesh.positions.size() << " vertices, " << mesh.indices.size() / 3
                          << " triangles");
  for (u32 i = 0; i < mesh.positions.size(); ++i) {
    const Vec3 p = mesh.positions[i];
    CHECK(p.y == 0.0f);
    CHECK(mesh.normals[i] == Vec3{0.0f, 1.0f, 0.0f});
    // The depth: the level less the ground under the vertex, clamped at zero, to the bit.
    const WorldPos at{100.0 + static_cast<f64>(p.x), 0.0, 50.0 + static_cast<f64>(p.z)};
    const f32 depth = static_cast<f32>(std::max(0.0, 0.0 - ground.surface(at)));
    CHECK(mesh.uvs[i].y == depth);
    // The distance from the edge, which for a circle is the radius less the vertex's.
    const f32 r = std::sqrt(p.x * p.x + p.z * p.z);
    CHECK(mesh.uvs[i].x == doctest::Approx(12.0f - r).epsilon(1e-4));
  }
  // The edge ring is at the edge, beyond the shore, where the ground stands above the water.
  for (u32 j = 0; j < mesh.edge; ++j) {
    CHECK(mesh.uvs[j].x == 0.0f);
    CHECK(mesh.uvs[j].y == 0.0f);
  }
  CHECK(mesh.max_depth == doctest::Approx(2.0f));
  // Every triangle faces +y: a pool faces up, which its normal cones assume.
  u32 down = 0;
  for (u32 t = 0; t < mesh.indices.size(); t += 3) {
    const Vec3 a = mesh.positions[mesh.indices[t]];
    const Vec3 b = mesh.positions[mesh.indices[t + 1]];
    const Vec3 c = mesh.positions[mesh.indices[t + 2]];
    const Vec3 n = cross(b - a, c - a);
    if (!(n.y > 0.0f)) ++down;
  }
  CHECK(down == 0u);
}

// The depth map (water.md, "The depth map"): a grid over the body's bounding square at a quarter of
// the mesh's spacing, every texel the level less the ground under it to the quantization's half
// step, the vertices' depths what the map interpolates to within the bowl's curvature over a
// texel, the bytes read back as written, the same bytes twice and a pinned hash.
TEST_CASE("water: the depth map is the ground under the body, a quarter of the spacing apart") {
  const scene_gen::Ground ground = bowl_ground();
  const water::Body body = read(circle_block(100.0, 50.0, 12.0f, 0.0));
  water::Mesh mesh;
  water::build_mesh(body, ground, mesh);
  water::DepthMap map;
  water::build_depth_map(body, mesh, ground, map);
  // 24 m at a sixth of a metre: 145 texels a side.
  CHECK(map.size == 145u);
  REQUIRE(map.texels.size() == map.size * map.size);
  CHECK(map.texel_m == doctest::Approx(24.0f / 144.0f).epsilon(1e-5));
  CHECK(map.origin.x == doctest::Approx(-12.0f).epsilon(1e-5));
  CHECK(map.origin.y == doctest::Approx(-12.0f).epsilon(1e-5));
  CHECK(map.depth_max == doctest::Approx(2.0f).epsilon(1e-3));
  // Signed since version 2 (S94): the bank is stored as a negative depth down to `bank_max`, which
  // is the highest bank in the square up to the cap (2 m with no tide).
  CHECK(map.bank_max > 0.0f);
  CHECK(map.bank_max <= water::k_depth_map_bank_min);
  const f64 unit = (static_cast<f64>(map.depth_max) + static_cast<f64>(map.bank_max)) / 65535.0;
  const auto texel = [&](u32 i, u32 j) {
    return static_cast<f64>(map.texels[j * map.size + i]) * unit - static_cast<f64>(map.bank_max);
  };
  f64 worst = 0.0;
  for (u32 j = 0; j < map.size; ++j) {
    for (u32 i = 0; i < map.size; ++i) {
      const f64 x = static_cast<f64>(map.origin.x) + i * static_cast<f64>(map.texel_m);
      const f64 z = static_cast<f64>(map.origin.y) + j * static_cast<f64>(map.texel_m);
      const f64 want = std::max(-static_cast<f64>(map.bank_max),
                                -ground.surface(WorldPos{100.0 + x, 0.0, 50.0 + z}));
      worst = std::max(worst, std::abs(texel(i, j) - want));
    }
  }
  CHECK(worst <= 0.5 * unit + 1e-12);
  // At every vertex under water the map's bilinear depth is the vertex's: the bowl's 0.04 m of
  // curvature a square metre over a sixth of a metre is under a millimetre. (At the waterline the
  // clamp at zero is a kink a texel of bilinear rounds off, by up to 2 cm on this bowl's 0.4 m a
  // metre: those vertices are left out.)
  f64 worst_vertex = 0.0;
  for (u32 v = 0; v < mesh.positions.size(); ++v) {
    if (!(mesh.uvs[v].y > 0.1f)) continue;
    const f64 tx = (static_cast<f64>(mesh.positions[v].x) - static_cast<f64>(map.origin.x)) /
                   static_cast<f64>(map.texel_m);
    const f64 tz = (static_cast<f64>(mesh.positions[v].z) - static_cast<f64>(map.origin.y)) /
                   static_cast<f64>(map.texel_m);
    const u32 i = std::min(static_cast<u32>(tx), map.size - 2);
    const u32 j = std::min(static_cast<u32>(tz), map.size - 2);
    const f64 ax = tx - i, az = tz - j;
    const f64 d = (texel(i, j) * (1.0 - ax) + texel(i + 1, j) * ax) * (1.0 - az) +
                  (texel(i, j + 1) * (1.0 - ax) + texel(i + 1, j + 1) * ax) * az;
    worst_vertex = std::max(worst_vertex, std::abs(d - static_cast<f64>(mesh.uvs[v].y)));
  }
  MESSAGE("depth map " << map.size << " texels a side at " << map.texel_m << " m; worst texel "
                       << worst << " m, worst vertex " << worst_vertex << " m");
  CHECK(worst_vertex < 1e-3);
  // The bytes: 44 of header (version 3: the mesh's spacings after the deepest) and two a texel,
  // read back as written, the same twice.
  const Vector<u8> bytes = water::depth_map_bytes(map);
  CHECK(bytes.size() == 44u + map.size * map.size * 2u);
  water::DepthMap back;
  std::string error;
  REQUIRE_MESSAGE(
      water::read_depth_map(std::span<const u8>(bytes.data(), bytes.size()), back, &error), error);
  CHECK(back.size == map.size);
  CHECK(back.origin == map.origin);
  CHECK(back.texel_m == map.texel_m);
  CHECK(back.depth_max == map.depth_max);
  CHECK(back.bank_max == map.bank_max);
  CHECK(std::equal(back.texels.begin(), back.texels.end(), map.texels.begin()));
  water::DepthMap again;
  water::build_depth_map(body, mesh, ground, again);
  const Vector<u8> twice = water::depth_map_bytes(again);
  CHECK(std::equal(twice.begin(), twice.end(), bytes.begin()));
  const u64 hash = hash_bytes(bytes.data(), bytes.size(), 0);
  MESSAGE("depth map hash " << std::hex << hash);
  // Re-pinned by S94: the map is signed (version 2), its bank stored below zero. Re-pinned by S100
  // stage B: version 3's header holds the mesh's three spacings (twelve bytes more).
  CHECK(hash == 6934579250243124016ull);
  // Refused: not a map, and a size its bytes do not hold.
  CHECK_FALSE(water::read_depth_map(std::span<const u8>(bytes.data(), 16), back, &error));
  CHECK_FALSE(
      water::read_depth_map(std::span<const u8>(bytes.data(), bytes.size() - 2), back, &error));
  // A puddle keeps the floor of 64 texels a side.
  const water::Body puddle = read(circle_block(100.0, 50.0, 1.0f, 0.0));
  water::Mesh small;
  water::build_mesh(puddle, ground, small);
  water::build_depth_map(puddle, small, ground, map);
  CHECK(map.size == water::k_depth_map_min);
}

TEST_CASE("water: a 20 m pool's vertex budget, and a lake's spacing grown to fit the budget") {
  const scene_gen::Ground none;
  water::Mesh pool;
  water::build_mesh(read(circle_block(0.0, 0.0, 10.0f, 0.0)), none, pool);
  MESSAGE("a 20 m pool: " << pool.positions.size() << " vertices, " << pool.indices.size() / 3
                          << " triangles at " << pool.spacing << " m");
  CHECK(pool.positions.size() < 2000u);
  // With no ground the ground is at 0, and a surface at 0 is everywhere 0 deep.
  CHECK(pool.max_depth == 0.0f);
  water::Mesh lake;
  water::build_mesh(read(circle_block(0.0, 0.0, 2000.0f, 0.0)), none, lake);
  CHECK(lake.positions.size() <= water::k_max_vertices);
  CHECK(lake.spacing > 1.0f);
  MESSAGE("a 4 km lake: " << lake.positions.size() << " vertices at " << lake.spacing << " m");
}

TEST_CASE("water: a polygon's mesh is rings of its outline, and its edge is at its outline") {
  scene::WaterBody block;
  block.shore_m = 1.5f;
  block.shape.polygon = {WorldPos{-6, 0, -3}, WorldPos{6, 0, -3}, WorldPos{6, 0, 3},
                         WorldPos{0, 0, 5}, WorldPos{-6, 0, 3}};
  const water::Body body = read(block);
  water::Mesh mesh;
  water::build_mesh(body, scene_gen::Ground{}, mesh);
  REQUIRE(mesh.positions.size() == mesh.edge * mesh.rings + 1);
  for (u32 j = 0; j < mesh.edge; ++j) {
    const Vec3 p = mesh.positions[j];
    CHECK(water::inside_distance(body, p.x, p.z) == doctest::Approx(0.0f).epsilon(1e-4));
  }
  u32 down = 0;
  for (u32 t = 0; t < mesh.indices.size(); t += 3) {
    const Vec3 a = mesh.positions[mesh.indices[t]];
    const Vec3 b = mesh.positions[mesh.indices[t + 1]];
    const Vec3 c = mesh.positions[mesh.indices[t + 2]];
    if (!(cross(b - a, c - a).y > 0.0f)) ++down;
  }
  CHECK(down == 0u);
}

TEST_CASE("water: the GLB is what the importer reads, and the same bytes every time") {
  const scene_gen::Ground ground = bowl_ground();
  const water::Body body = read(circle_block(100.0, 50.0, 12.0f, 0.0));
  water::Mesh mesh;
  water::build_mesh(body, ground, mesh);
  const Vec3 shallow{0.3f, 0.55f, 0.5f};
  const Vector<u8> glb = water::mesh_glb(mesh, shallow, 0.08f);
  water::Mesh again;
  water::build_mesh(body, ground, again);
  const Vector<u8> glb2 = water::mesh_glb(again, shallow, 0.08f);
  REQUIRE(glb.size() == glb2.size());
  CHECK(std::equal(glb.begin(), glb.end(), glb2.begin()));
  const std::string name = water::mesh_file_name(std::span<const u8>(glb.data(), glb.size()));
  CHECK(name.rfind("water-", 0) == 0);
  // The same bytes from every toolchain: f32 + - * / and sqrt, the sine our own (water.h), the
  // numbers printed with %.9g. Pinned on MSVC; the Linux containers reproduce it.
  const u64 h = hash_bytes(glb.data(), glb.size());
  MESSAGE("GLB " << glb.size() << " bytes, hash " << h);
  CHECK(glb.size() == 114560u);
  CHECK(h == 10307564092827010698ull);
  assets::MeshData data;
  std::string error;
  REQUIRE_MESSAGE(
      assets::load_gltf_memory(std::span<const u8>(glb.data(), glb.size()), "", data, &error),
      error);
  REQUIRE(data.positions.size() == mesh.positions.size());
  REQUIRE(data.uvs.size() == mesh.uvs.size());
  for (u32 i = 0; i < data.uvs.size(); ++i)
    CHECK(data.uvs[i] == mesh.uvs[i]);
  REQUIRE(data.materials.size() == 1);
  CHECK(data.materials[0].base_color.x == shallow.x);
  CHECK(data.materials[0].roughness == 0.08f);
  CHECK(data.materials[0].metallic == 0.0f);
}

TEST_CASE("water: the generator makes one mesh and one placement per body, in order") {
  const test::TempDir tmp("water_generator");
  const scene_gen::PlacementGeneratorDesc* generator =
      scene_gen::GeneratorRegistry::global().find_placement(water::k_generator);
  REQUIRE(generator != nullptr);
  scene::WaterBody blocks[2] = {circle_block(100.0, 50.0, 12.0f, 0.0),
                                circle_block(-40.0, 10.0, 4.0f, -1.5)};
  blocks[1].name = "puddle";
  scene_gen::Context context;
  context.ground = bowl_ground();
  const std::string dir = tmp.path();
  context.dir = dir;
  context.where = "test";
  void* state = nullptr;
  std::string error;
  REQUIRE_MESSAGE(generator->open(water::generator_params(blocks), context, &state, &error), error);
  scene_gen::Placements placed;
  REQUIRE_MESSAGE(generator->expand(state, context, placed, &error), error);
  generator->close(state);
  REQUIRE(placed.meshes.size() == 2);
  REQUIRE(placed.instances.size() == 2);
  CHECK(placed.meshes[0].name == "water/pool");
  CHECK(placed.meshes[1].name == "water/puddle");
  CHECK(io::exists(placed.meshes[0].path));
  CHECK(io::exists(placed.meshes[1].path));
  // The depth map beside each mesh, under the one name: `<stem>.glb` and `<stem>.depth`.
  for (const scene_gen::PlacementMesh& m : placed.meshes) {
    REQUIRE(m.path.size() > 4);
    CHECK(m.path.substr(m.path.size() - 4) == ".glb");
    CHECK(io::exists(m.path.substr(0, m.path.size() - 4) + ".depth"));
  }
  CHECK(placed.instances[0].position == WorldPos{100.0, 0.0, 50.0});
  CHECK(placed.instances[1].position == WorldPos{-40.0, -1.5, 10.0});
  CHECK(placed.instances[1].mesh == 1u);
  CHECK(placed.instances[0].rotation == Quat::identity());
  // A body the capability refuses is refused through the generator with its sentence.
  scene::WaterBody bad = circle_block(0.0, 0.0, -1.0f, 0.0);
  CHECK_FALSE(generator->open(water::generator_params(std::span<const scene::WaterBody>(&bad, 1)),
                              context, &state, &error));
  CHECK(error.find("radius_m") != std::string::npos);
}

// **Open water** (water.md, "To the horizon"; S93): an open body's mesh and depth map are the
// closed body's bytes — what differs is what the resolve draws past the outline, and where
// `depth_at` finds water — and a scene has at most one.
TEST_CASE("water: an open body is the closed one's mesh and map, and its sea goes on past it") {
  const scene_gen::Ground ground = bowl_ground();
  scene::WaterBody block = circle_block(100.0, 50.0, 6.0f, 0.0);
  const water::Body closed = read(block);
  CHECK_FALSE(closed.open);
  block.open = true;
  const water::Body open = read(block);
  CHECK(open.open);
  // The mesh and the map: the same bytes either way, pinned (the closed body's hash is the
  // capability's own; the open body names the same files).
  water::Mesh closed_mesh;
  water::Mesh open_mesh;
  water::build_mesh(closed, ground, closed_mesh);
  water::build_mesh(open, ground, open_mesh);
  const Vec3 shallow{0.3f, 0.55f, 0.5f};
  const Vector<u8> closed_glb = water::mesh_glb(closed_mesh, shallow, 0.08f);
  const Vector<u8> open_glb = water::mesh_glb(open_mesh, shallow, 0.08f);
  REQUIRE(closed_glb.size() == open_glb.size());
  CHECK(std::equal(closed_glb.begin(), closed_glb.end(), open_glb.begin()));
  water::DepthMap closed_map;
  water::DepthMap open_map;
  water::build_depth_map(closed, closed_mesh, ground, closed_map);
  water::build_depth_map(open, open_mesh, ground, open_map);
  const Vector<u8> closed_bytes = water::depth_map_bytes(closed_map);
  const Vector<u8> open_bytes = water::depth_map_bytes(open_map);
  REQUIRE(closed_bytes.size() == open_bytes.size());
  CHECK(std::equal(closed_bytes.begin(), closed_bytes.end(), open_bytes.begin()));
  const u64 glb_hash = hash_bytes(open_glb.data(), open_glb.size());
  const u64 map_hash = hash_bytes(open_bytes.data(), open_bytes.size());
  MESSAGE("open body: GLB " << open_glb.size() << " bytes, hash " << glb_hash << "; map "
                            << open_bytes.size() << " bytes, hash " << map_hash);
  CHECK(open_glb.size() == 29056u);
  CHECK(glb_hash == 17133894412657858236ull);
  // Re-pinned by S100 stage B, both: the map's header is version 3's (twelve bytes more).
  CHECK(open_bytes.size() == 10702u);
  CHECK(map_hash == 17247269276869333348ull);  // re-pinned by S94: the signed map, version 2
  // The depth past the outline: the bowl's ground meets the level 10 m from its centre, so 8 m out
  // (2 m past a 6 m outline) the open sea is 2 - 0.02 * 64 = 0.72 m deep and the closed pool has
  // none; 12 m out the ground stands above the level and neither has water; inside the outline the
  // two answer alike.
  const water::Body closed_one[1] = {closed};
  const water::Body open_one[1] = {open};
  CHECK(water::depth_at(closed_one, ground, WorldPos{108.0, 0.0, 50.0}) == 0.0f);
  CHECK(water::depth_at(open_one, ground, WorldPos{108.0, 0.0, 50.0}) == doctest::Approx(0.72f));
  CHECK(water::depth_at(open_one, ground, WorldPos{100.0, 0.0, 58.0}) == doctest::Approx(0.72f));
  CHECK(water::depth_at(open_one, ground, WorldPos{112.0, 0.0, 50.0}) == 0.0f);
  CHECK(water::depth_at(open_one, ground, WorldPos{103.0, 0.0, 50.0}) ==
        water::depth_at(closed_one, ground, WorldPos{103.0, 0.0, 50.0}));
  // One open body a scene, refused through the generator with a sentence naming both.
  const test::TempDir tmp("water_open");
  const scene_gen::PlacementGeneratorDesc* generator =
      scene_gen::GeneratorRegistry::global().find_placement(water::k_generator);
  REQUIRE(generator != nullptr);
  scene::WaterBody blocks[3] = {block, circle_block(-40.0, 10.0, 4.0f, -1.5), block};
  blocks[0].name = "sea";
  blocks[2].name = "ocean";
  scene_gen::Context context;
  context.ground = ground;
  const std::string dir = tmp.path();
  context.dir = dir;
  context.where = "test";
  void* state = nullptr;
  std::string error;
  CHECK_FALSE(generator->open(water::generator_params(blocks), context, &state, &error));
  MESSAGE(error);
  CHECK(error.find("\"ocean\" is open, and so is \"sea\"") != std::string::npos);
  CHECK(error.find("at most one open body") != std::string::npos);
  // One open body beside a closed one is a scene.
  REQUIRE_MESSAGE(
      generator->open(water::generator_params(std::span<const scene::WaterBody>(blocks, 2)),
                      context, &state, &error),
      error);
  generator->close(state);
}

// **An open sea's waves from the spectrum** (gfx::water_spectrum; water.md, "The spectrum"; S93):
// the table's sixteen waves hold the variance JONSWAP puts in the band they are drawn from, centred
// on the wind with Donelan-Banner's spread, each a whole number of cycles a period, the crests'
// steepness within its cap, the same table from the same seed — and no waves without a wind.
TEST_CASE("water: an open sea's waves are JONSWAP's at the wind, spread about it") {
  const f64 u10 = 10.0;
  const f64 fetch = 100000.0;
  const Vec2 wind{0.6f, 0.8f};
  gfx::WaterWave waves[gfx::k_water_waves];
  gfx::water_spectrum(u10, fetch, wind, 7, waves);
  // The fetch laws (ocean-surface.md): at 10 m/s over 100 km, omega_p = 22 (g^2 / (U F))^(1/3).
  const f64 peak = gfx::jonswap_peak(u10, fetch);
  const f64 alpha = gfx::jonswap_alpha(u10, fetch);
  CHECK(peak == doctest::Approx(22.0 * std::cbrt(9.81 * 9.81 / (u10 * fetch))));
  CHECK(alpha == doctest::Approx(0.076 * std::pow(u10 * u10 / (fetch * 9.81), 0.22)));
  // gamma = 1 is Pierson-Moskowitz's shape: the enhancement at the peak is gamma itself.
  CHECK(gfx::jonswap(peak, alpha, peak) /
            (alpha * 9.81 * 9.81 / std::pow(peak, 5.0) * std::exp(-1.25)) ==
        doctest::Approx(3.3));
  // The variance: the sixteen sines' a^2 / 2 against JONSWAP integrated over the band.
  f64 variance = 0.0;
  f64 mean_cos = 0.0;
  f64 steepness = 0.0;
  for (const gfx::WaterWave& w : waves) {
    const f64 a = static_cast<f64>(w.amplitude);
    variance += 0.5 * a * a;
    const f64 c = static_cast<f64>(w.direction.x * wind.x + w.direction.y * wind.y);
    mean_cos += 0.5 * a * a * c;
    steepness += static_cast<f64>(w.steepness);
    CHECK(w.cycles == std::floor(w.cycles));
    CHECK(w.cycles >= 1.0f);
    CHECK(w.phase >= 0.0f);
    CHECK(w.phase < 1.0f);
    CHECK(std::fabs(std::sqrt(w.direction.x * w.direction.x + w.direction.y * w.direction.y) -
                    1.0f) < 1e-5f);
    // Deep water: omega^2 = g k, and the frequency the cycles round it to.
    const f64 omega = static_cast<f64>(w.cycles) * 6.283185307179586 / 1024.0;
    CHECK(omega * omega / 9.81 == doctest::Approx(static_cast<f64>(w.k)).epsilon(0.02));
  }
  mean_cos /= variance;
  f64 integral = 0.0;
  const f64 lo = 0.5 * peak;
  const f64 hi = 4.0 * peak;
  constexpr u32 k_steps = 20000;
  for (u32 i = 0; i < k_steps; ++i) {
    const f64 omega = lo + (hi - lo) * (static_cast<f64>(i) + 0.5) / k_steps;
    integral += gfx::jonswap(omega, alpha, peak) * (hi - lo) / k_steps;
  }
  MESSAGE("10 m/s over 100 km: peak "
          << peak << " rad/s (" << 2.0 * 3.14159265 * 9.81 / (peak * peak) << " m), alpha " << alpha
          << ", Hs " << 4.0 * std::sqrt(variance) << " m from the waves and "
          << 4.0 * std::sqrt(integral) << " m from the band; energy-weighted cos to the wind "
          << mean_cos << ", steepness " << steepness);
  CHECK(variance == doctest::Approx(integral).epsilon(0.03));
  CHECK(mean_cos > 0.8);
  CHECK(steepness <= gfx::k_water_max_steepness + 1e-6);
  // The same seed, the same table; another seed, other directions and phases.
  gfx::WaterWave again[gfx::k_water_waves];
  gfx::water_spectrum(u10, fetch, wind, 7, again);
  CHECK(std::memcmp(waves, again, sizeof(waves)) == 0);
  gfx::water_spectrum(u10, fetch, wind, 8, again);
  CHECK(std::memcmp(waves, again, sizeof(waves)) != 0);
  // No wind, no waves.
  gfx::water_spectrum(0.0, fetch, wind, 7, again);
  for (const gfx::WaterWave& w : again)
    CHECK(w.amplitude == 0.0f);
  // Packed after the material's next entry, raw, with the flag.
  gfx::ResolveMaterial water;
  gfx::ResolveMaterial entries[gfx::k_water_wave_entries];
  gfx::set_water_spectrum(water, entries, waves);
  CHECK((water.flags & gfx::k_material_water_spectrum) != 0u);
  CHECK(std::memcmp(entries, waves, sizeof(waves)) == 0);
}

TEST_CASE("water: a body's shading is packed into its material where only the branch reads it") {
  gfx::WaterShading s;
  s.shallow = Vec3{0.1f, 0.2f, 0.3f};
  s.deep = Vec3{0.01f, 0.02f, 0.03f};
  s.roughness = 0.07f;
  s.depth_fade_m = 2.5f;
  s.wave_amplitude_m = 0.03f;
  s.wave_length_m = 1.75f;
  s.chop = 0.4f;
  s.shore_m = 1.25f;
  s.direction = Vec2{0.6f, 0.8f};
  s.seed = 77;
  gfx::ResolveMaterial m;
  m.albedo_texture = 5;  // anything the material had is replaced
  gfx::set_water_material(m, s);
  CHECK(m.flags == gfx::k_material_water);
  CHECK(m.albedo == Vec4{0.1f, 0.2f, 0.3f, 0.07f});
  CHECK(m.emissive == Vec4{});
  CHECK(m.uv_transform == Vec4{0.01f, 0.02f, 0.03f, 2.5f});
  CHECK(m.uv_offset == Vec4{0.03f, 1.75f, 0.4f, 1.25f});
  CHECK(m.normal_scale == 0.6f);
  CHECK(m.occlusion_strength == 0.8f);
  CHECK(m.sampler == 77u);
  // What every other reader sees: an untextured dielectric of the shallow colour, the UVs as given.
  CHECK(m.albedo_texture == gfx::k_no_texture);
  CHECK(m.metallic_roughness_texture == gfx::k_no_texture);
  CHECK(m.normal_texture == gfx::k_no_texture);
  CHECK(m.occlusion_texture == gfx::k_no_texture);
  CHECK(m.emissive_texture == gfx::k_no_texture);
  CHECK(m.uv_scale == 1.0f);
  CHECK((m.flags & gfx::k_material_uv_transform) == 0u);
  // The water's clock: the frame's time modulo the period, in double.
  CHECK(gfx::water_clock(0.0) == 0.0f);
  CHECK(gfx::water_clock(1024.0 * 1000.0 + 12.5) == 12.5f);
  CHECK(gfx::water_clock(-1.0) == 1023.0f);
  // The volume tier's optics (water.md, "Translucent by volume"): the flag on the water's material
  // and the entry after it — the extinction and the transmission, the scattering, the ground's UV
  // frame and its material — with the turbidity's sediment added to both rates.
  gfx::WaterOptics o;
  o.absorption = Vec3{0.5f, 0.1f, 0.05f};
  o.scattering = Vec3{0.02f, 0.03f, 0.04f};
  o.transmission = 0.75f;
  o.turbidity = 0.5f;
  o.ground = 12;
  o.ground_uv = Vec2{0.5f, 0.25f};
  o.ground_uv_per_m = 0.004f;
  gfx::ResolveMaterial optics;
  gfx::set_water_optics(m, optics, o);
  CHECK(m.flags == (gfx::k_material_water | gfx::k_material_water_volume));
  const Vec3 scatter = o.scattering + gfx::k_water_silt_scatter * 0.5f;
  const Vec3 extinction = o.absorption + gfx::k_water_silt_absorb * 0.5f + scatter;
  CHECK(optics.albedo == Vec4{extinction, 0.75f});
  CHECK(optics.emissive == Vec4{scatter, 0.0f});
  CHECK(optics.uv_transform == Vec4{0.5f, 0.25f, 0.004f, 0.0f});
  CHECK(optics.sampler == 12u);
  CHECK(optics.flags == 0u);
  CHECK(optics.albedo_texture == gfx::k_no_texture);
}

TEST_CASE("water: the volume tier's optics are read and refused with a sentence") {
  scene::WaterBody ok = circle_block(0.0, 0.0, 5.0f, 0.0);
  ok.absorption = Vec3{0.4f, 0.1f, 0.0f};
  ok.scattering = Vec3{0.0f, 0.0f, 0.0f};
  ok.transmission = 0.0f;
  ok.turbidity = 3.0f;
  water::Body out;
  std::string error;
  CHECK(water::read_body(ok, out, &error));
  scene::WaterBody absorbs = ok;
  absorbs.absorption = Vec3{0.4f, -0.1f, 0.0f};
  CHECK_FALSE(water::read_body(absorbs, out, &error));
  CHECK(error.find("absorption") != std::string::npos);
  scene::WaterBody scatters = ok;
  scatters.scattering = Vec3{0.0f, 0.0f, std::nanf("")};
  CHECK_FALSE(water::read_body(scatters, out, &error));
  CHECK(error.find("scattering") != std::string::npos);
  scene::WaterBody passes = ok;
  passes.transmission = 1.5f;
  CHECK_FALSE(water::read_body(passes, out, &error));
  CHECK(error.find("transmission") != std::string::npos);
  scene::WaterBody silty = ok;
  silty.turbidity = -1.0f;
  CHECK_FALSE(water::read_body(silty, out, &error));
  CHECK(error.find("turbidity") != std::string::npos);
}
