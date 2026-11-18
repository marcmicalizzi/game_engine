// The eye's adaptation's model on the CPU (exposure.h; docs/subsystems/renderer.md, "Exposure: the
// eye's adaptation"; docs/experiments/eye-adaptation.md). No device: the key against the source's
// formula at the points the page quotes, the target against the sky's rule where the two should
// meet, the time constants and the step's independence from how time is cut into frames, and the
// metered view and its centre weighting.

#include <systems/renderer/exposure.h>

#include <doctest/doctest.h>

#include <cmath>

using namespace engine;
using namespace engine::renderer;

TEST_CASE("exposure model: the key is Krawczyk's, falling with the light") {
  // a(Y) = 1.03 - 2 / (2 + log10(Y + 1)) (Krawczyk 2007 eq. 4.11).
  CHECK(exposure_key(0.0) == doctest::Approx(0.03));
  CHECK(exposure_key(19.0) == doctest::Approx(1.03 - 2.0 / (2.0 + std::log10(20.0))));
  CHECK(exposure_key(19.0) == doctest::Approx(0.424).epsilon(0.005));
  CHECK(exposure_key(22000.0) == doctest::Approx(0.715).epsilon(0.005));
  f64 last = exposure_key(1e-4);
  for (f64 l = 1e-3; l < 1e7; l *= 3.0) {
    CHECK(exposure_key(l) > last);
    last = exposure_key(l);
  }
  // The rods' share (eq. 4.7): all rods in the dark, half at 0.04 cd/m^2, nearly none by day.
  CHECK(exposure_rod_share(0.0) == doctest::Approx(1.0));
  CHECK(exposure_rod_share(0.04) == doctest::Approx(0.5));
  CHECK(exposure_rod_share(1000.0) < 1e-4);
}

TEST_CASE("exposure model: sunlit sand near the sky's rule, a room at noon is nine stops up") {
  const ExposureOptions options;
  // Reinhard's operator shows the adapting luminance at a / (1 + a) (Krawczyk 2007 eqs. 4.1, 4.2).
  CHECK(exposure_shown(0.18) == doctest::Approx(0.18 / 1.18));
  CHECK(exposure_shown(1.0) == doctest::Approx(0.5));
  // Sand of albedo 0.7 under 110 klux: 0.7 x 110000 / pi cd/m^2. The sky's rule meters the light
  // itself, log2(110000 / 2.5) = 15.4, and shows the sand at its albedo; the eye shows it at 0.42,
  // three quarters of a stop under.
  const f64 sand = 0.7 * 110000.0 / 3.14159265358979323846;
  const f64 rule = std::log2(110000.0 / 2.5);
  CHECK(exposure_target_ev100(sand, 0.0, options) - rule == doctest::Approx(0.75).epsilon(0.05));
  // A room lit to 100 lux, walls of 0.6: 19 cd/m^2, about EV100 6.3, nine stops up from noon.
  CHECK(exposure_target_ev100(19.0, 0.0, options) == doctest::Approx(6.32).epsilon(0.01));
  // A step of compensation is a stop; the floor and the ceiling hold.
  CHECK(exposure_target_ev100(19.0, 1.0, options) ==
        doctest::Approx(exposure_target_ev100(19.0, 0.0, options) - 1.0));
  CHECK(exposure_target_ev100(1e-6, 0.0, options) ==
        doctest::Approx(static_cast<f64>(options.min_ev100)));
  CHECK(exposure_target_ev100(1e9, 0.0, options) ==
        doctest::Approx(static_cast<f64>(options.max_ev100)));
  // More light, a higher exposure value, everywhere between.
  f64 last = exposure_target_ev100(1e-3, 0.0, options);
  for (f64 l = 3e-3; l < 1e6; l *= 3.0) {
    const f64 ev = exposure_target_ev100(l, 0.0, options);
    CHECK(ev >= last);
    last = ev;
  }
}

TEST_CASE("exposure model: dark adaptation is slower, light adaptation the eye's") {
  ExposureOptions options;
  const f64 dark = static_cast<f64>(options.dark_tau_s);
  const f64 cone = static_cast<f64>(options.cone_tau_s);
  const f64 rod = static_cast<f64>(options.rod_tau_s);
  // Brightening (the target lower) is dark adaptation's constant.
  CHECK(exposure_tau_s(15.0, 6.0, 19.0, options) == doctest::Approx(dark));
  // Darkening by day is the cones', at night mostly the rods' (Durand and Dorsey 2000, 3.3).
  CHECK(exposure_tau_s(6.0, 15.0, 20000.0, options) == doctest::Approx(cone).epsilon(1e-3));
  const f64 night = exposure_tau_s(-2.0, 0.0, 0.01, options);
  CHECK(night == doctest::Approx(0.8 * rod + 0.2 * cone));
  // An exponential in stops: sixty steps of a sixtieth are one step of a second.
  f64 ev = 15.0;
  for (u32 i = 0; i < 60; ++i)
    ev = exposure_step(ev, 6.0, 19.0, 1.0 / 60.0, options);
  CHECK(ev == doctest::Approx(exposure_step(15.0, 6.0, 19.0, 1.0, options)).epsilon(1e-9));
  CHECK(ev == doctest::Approx(6.0 + 9.0 * std::exp(-1.0 / dark)));
  // A time constant of 0 is at once.
  options.dark_tau_s = 0.0f;
  CHECK(exposure_step(15.0, 6.0, 19.0, 1.0 / 60.0, options) == 6.0);
}

TEST_CASE("exposure model: the metered view is the centre one, weighted to its middle") {
  // One view: itself.
  const ExposureViewRect one{0, 0, 1920, 1080};
  CHECK(exposure_metered_view(&one, 1, 1920, 1080) == 0);
  // A surround of three monitors: the centre one, so the side views never pump the exposure.
  const ExposureViewRect three[3] = {
      {0, 0, 3840, 2160}, {3840, 0, 3840, 2160}, {7680, 0, 3840, 2160}};
  CHECK(exposure_metered_view(three, 3, 11520, 2160) == 1);
  // The centre's disc: 16 against 1, three quarters of the weight in it on a 16:9 view, as the
  // centre-weighted meter Durand and Dorsey cite (75% in the 12 mm circle of a 24 x 36 mm frame).
  CHECK(exposure_pixel_weight(one, 960, 540) == k_exposure_centre_weight);
  CHECK(exposure_pixel_weight(one, 0, 0) == 1);
  u64 inside = 0;
  u64 total = 0;
  for (u32 y = 0; y < 1080; y += k_exposure_stride) {
    for (u32 x = 0; x < 1920; x += k_exposure_stride) {
      const u32 w = exposure_pixel_weight(one, x, y);
      total += w;
      if (w > 1) inside += w;
    }
  }
  const f64 share = static_cast<f64>(inside) / static_cast<f64>(total);
  MESSAGE("the centre's share of the weight on a 16:9 view: " << share);
  CHECK(share == doctest::Approx(0.746).epsilon(0.01));
  // In the centre monitor of a surround the disc is the monitor's own.
  CHECK(exposure_pixel_weight(three[1], 3840 + 1920, 1080) == k_exposure_centre_weight);
}
