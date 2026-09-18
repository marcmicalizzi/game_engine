// The reference path tracer's sampler, on the CPU (docs/plan/04-renderer.md §4.8).
//
// `domain/gfx/shaders/sampling.slang` decides how the reference spends its samples, and a
// sampler is the half of a Monte Carlo integrator whose mistakes are invisible: a density that
// does not integrate to one, or one that does not describe the direction it produced, biases
// every picture by an amount that looks like a shading difference. `brdf_reference.h` carries the
// same routines in double precision — the same PCG generator bit for bit, the same visible-normal
// sampler, the same mixture density — and these cases hold them to the properties that catch it:
//
//   1. the densities integrate to one (quadrature, sharing nothing with the sampler);
//   2. the sampler draws from the density it claims to — a histogram of the half vectors against
//      the same quadrature, band by band;
//   3. what a sample is *worth* is exactly what the model says it is: for a metal, where the
//      diffuse lobe vanishes, `f cos / pdf` reduces analytically to `F G2 / G1`, and that
//      identity holds at every roughness including the ones a quadrature cannot resolve;
//   4. a white furnace comes back white, within the energy single-scatter GGX is known to lose,
//      which this file measures rather than assumes;
//   5. the generator is the shader's, and a pixel's Nth sample depends on nothing but
//      (pixel, N, seed) — which is what makes a capture reproducible.
//
// **A quadrature cannot resolve a narrow lobe**, and the file is arranged around that: at
// roughness 0.05 the GGX lobe is a few tenths of a degree wide while a 512 x 1024 grid steps by
// 0.18°, so a grid integral of it is meaningless. Every quadrature here therefore stays at
// roughness 0.25 and above, where the lobe is many steps wide, and the narrow end is covered by
// the exact identity of (3) instead, which needs no integral at all.
//
// There is no GPU in this file. The shader side of the same code is pinned by the reference
// renderer's own tests (`systems/renderer`), which run the integrator on a device and compare it
// against the resolve.

#include "brdf_reference.h"

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

using namespace engine;
using namespace engine::brdf_ref;

namespace {

Dvec3 direction_of(double theta, double phi) {
  const double s = std::sin(theta);
  return Dvec3{s * std::cos(phi), s * std::sin(phi), std::cos(theta)};
}

// Integral over the upper hemisphere of a function of the direction, by the midpoint rule in
// (theta, phi). Slow and dumb on purpose: it is the independent answer the sampler is checked
// against, so it must share as little code with the sampler as possible.
template <typename F>
double integrate_hemisphere(F&& f, u32 theta_steps = 256, u32 phi_steps = 512) {
  double total = 0.0;
  const double d_theta = (brdf_ref::k_pi * 0.5) / static_cast<double>(theta_steps);
  const double d_phi = (2.0 * brdf_ref::k_pi) / static_cast<double>(phi_steps);
  for (u32 i = 0; i < theta_steps; ++i) {
    const double theta = (static_cast<double>(i) + 0.5) * d_theta;
    const double sin_theta = std::sin(theta);
    double ring = 0.0;
    for (u32 j = 0; j < phi_steps; ++j) {
      const double phi = (static_cast<double>(j) + 0.5) * d_phi;
      ring += f(direction_of(theta, phi));
    }
    total += ring * sin_theta * d_theta * d_phi;
  }
  return total;
}

// The distribution of visible normals itself, written out rather than taken from the sampler:
// D_v(h) = G1(v) max(0, v.h) D(h) / (n.v), which integrates to one over the hemisphere of half
// vectors. `ggx_vndf_pdf` is this divided by the reflection's Jacobian 4(v.h), so a quadrature of
// this is an independent check of the density the sampler is built on.
double vndf_density(Dvec3 v, Dvec3 h, double alpha) {
  const double v_dot_h = dot(v, h);
  if (v_dot_h <= 0.0) return 0.0;
  return ggx_g1(v.z, alpha) * v_dot_h * d_ggx(h.z, alpha) / v.z;
}

// The same integral over one elevation band only, which is what a histogram bin should hold.
template <typename F>
double integrate_band(F&& f, double theta_lo, double theta_hi, u32 theta_steps, u32 phi_steps) {
  double total = 0.0;
  const double d_theta = (theta_hi - theta_lo) / static_cast<double>(theta_steps);
  const double d_phi = (2.0 * brdf_ref::k_pi) / static_cast<double>(phi_steps);
  for (u32 i = 0; i < theta_steps; ++i) {
    const double theta = theta_lo + (static_cast<double>(i) + 0.5) * d_theta;
    const double sin_theta = std::sin(theta);
    double ring = 0.0;
    for (u32 j = 0; j < phi_steps; ++j) {
      const double phi = (static_cast<double>(j) + 0.5) * d_phi;
      ring += f(direction_of(theta, phi));
    }
    total += ring * sin_theta * d_theta * d_phi;
  }
  return total;
}

Surface white_surface(double roughness, double metallic) {
  Surface s;
  s.albedo = splat(1.0);
  s.roughness = roughness;
  s.metallic = metallic;
  s.normal = Dvec3{0.0, 0.0, 1.0};
  return s;
}

// The directional albedo: the fraction of the light arriving from one direction that leaves the
// surface at all, integrated by quadrature over the outgoing hemisphere. 1 is a surface that
// loses nothing.
double directional_albedo_quadrature(const Surface& surface, double n_dot_v) {
  Surface s = surface;
  s.normal = Dvec3{0.0, 0.0, 1.0};
  s.view = Dvec3{std::sqrt(1.0 - n_dot_v * n_dot_v), 0.0, n_dot_v};
  return integrate_hemisphere([&](Dvec3 l) { return eval(s, l).x * l.z; });
}

// The same number estimated the way the path tracer spends a sample: draw from the mixture,
// weight by f cos / pdf, average. If any of the three routines disagrees with the other two this
// lands somewhere else.
double directional_albedo_sampled(const Surface& surface, double n_dot_v, u32 samples,
                                  u32 seed_index) {
  Surface s = surface;
  s.normal = Dvec3{0.0, 0.0, 1.0};
  const Dvec3 v{std::sqrt(1.0 - n_dot_v * n_dot_v), 0.0, n_dot_v};
  s.view = v;
  u32 rng = rng_seed(7u, 11u, seed_index, 1234u);
  double total = 0.0;
  for (u32 i = 0; i < samples; ++i) {
    const double u_lobe = rng_next(rng);
    const double u1 = rng_next(rng);
    const double u2 = rng_next(rng);
    BsdfSample sample;
    if (sample_bsdf(s, v, u_lobe, u1, u2, sample)) total += sample.weight.x;
  }
  return total / static_cast<double>(samples);
}

}  // namespace

TEST_CASE("sampling: every density integrates to one") {
  // Cosine sampling first, where the answer is known in closed form and a failure means the
  // quadrature itself is wrong rather than the sampler.
  CHECK(integrate_hemisphere([](Dvec3 l) { return cosine_hemisphere_pdf(l.z); }) ==
        doctest::Approx(1.0).epsilon(1e-4));

  // The distribution of visible normals, over the hemisphere of half vectors, at three widths
  // the grid can resolve and three view angles. The grazing cases are the ones that matter: that
  // is where sampling the whole distribution instead of the visible part would still integrate
  // to one and throw half its samples away.
  const double roughnesses[3] = {0.25, 0.5, 1.0};
  const double cosines[3] = {0.98, 0.6, 0.2};
  for (const double roughness : roughnesses) {
    const double alpha = alpha_of(roughness);
    for (const double n_dot_v : cosines) {
      const Dvec3 v{std::sqrt(1.0 - n_dot_v * n_dot_v), 0.0, n_dot_v};
      const double total =
          integrate_hemisphere([&](Dvec3 h) { return vndf_density(v, h, alpha); }, 512, 1024);
      INFO("roughness ", roughness, " n.v ", n_dot_v, " integral ", total);
      CHECK(total == doctest::Approx(1.0).epsilon(5e-3));
    }
  }

  // And `ggx_vndf_pdf` is that density divided by the reflection's Jacobian, 4(v.h) — the step
  // the *direction* density is one change of variables away from the one just integrated.
  const Dvec3 v{0.3, 0.0, std::sqrt(1.0 - 0.09)};
  const double alpha = alpha_of(0.4);
  u32 rng = rng_seed(3u, 5u, 1u, 77u);
  for (u32 i = 0; i < 64; ++i) {
    const Dvec3 h = sample_ggx_vndf(v, alpha, rng_next(rng), rng_next(rng));
    const Dvec3 l = h * (2.0 * dot(v, h)) - v;
    if (l.z <= 0.0) continue;
    CHECK(ggx_vndf_pdf(v, l, alpha) ==
          doctest::Approx(vndf_density(v, h, alpha) / (4.0 * dot(v, h))).epsilon(1e-9));
  }

  // A reflected direction can land **below** the horizon, and does at wide roughness: the
  // estimator drops those samples, which is correct (the BSDF is zero there) and is the whole
  // cost of single-scatter sampling. Measured here so that the waste is a number rather than a
  // surprise: at roughness 1 seen head-on **half** the samples are thrown away, which is the
  // other half of the energy the furnace below finds missing and the reason a very rough metal
  // is the noisiest thing the reference renders.
  u32 below = 0;
  const double wide = alpha_of(1.0);
  const Dvec3 head_on{0.0, 0.0, 1.0};
  for (u32 i = 0; i < 20000u; ++i) {
    const Dvec3 h = sample_ggx_vndf(head_on, wide, rng_next(rng), rng_next(rng));
    const Dvec3 l = h * (2.0 * dot(head_on, h)) - head_on;
    if (l.z <= 0.0) ++below;
  }
  const double wasted = static_cast<double>(below) / 20000.0;
  INFO("roughness 1, head on: ", wasted, " of the VNDF samples reflect below the horizon");
  CHECK(wasted > 0.30);
  CHECK(wasted < 0.60);
}

TEST_CASE("sampling: the sampler draws from the density it reports") {
  // A histogram of the half vectors by elevation band against the same quadrature, which is the
  // statement that `sample_ggx_vndf` and `vndf_density` describe the same distribution. Twelve
  // bands over the hemisphere; only the roughnesses a grid can resolve, as above.
  constexpr u32 k_bands = 12;
  constexpr u32 k_samples = 200000u;
  for (const double roughness : {0.3, 0.6, 1.0}) {
    const double alpha = alpha_of(roughness);
    for (const double n_dot_v : {0.95, 0.4}) {
      const Dvec3 v{std::sqrt(1.0 - n_dot_v * n_dot_v), 0.0, n_dot_v};
      u32 histogram[k_bands] = {};
      u32 rng = rng_seed(static_cast<u32>(roughness * 100.0), static_cast<u32>(n_dot_v * 100.0), 2u,
                         4242u);
      for (u32 i = 0; i < k_samples; ++i) {
        const Dvec3 h = sample_ggx_vndf(v, alpha, rng_next(rng), rng_next(rng));
        const double theta = std::acos(h.z < 1.0 ? h.z : 1.0);
        u32 band = static_cast<u32>(theta / (brdf_ref::k_pi * 0.5) * k_bands);
        if (band >= k_bands) band = k_bands - 1;
        ++histogram[band];
      }
      for (u32 band = 0; band < k_bands; ++band) {
        const double lo = static_cast<double>(band) / k_bands * brdf_ref::k_pi * 0.5;
        const double hi = static_cast<double>(band + 1) / k_bands * brdf_ref::k_pi * 0.5;
        const double expected =
            integrate_band([&](Dvec3 h) { return vndf_density(v, h, alpha); }, lo, hi, 96, 256);
        const double measured = static_cast<double>(histogram[band]) / k_samples;
        INFO("roughness ", roughness, " n.v ", n_dot_v, " band ", band, ": expected ", expected,
             " measured ", measured);
        // Absolute, not relative: a band that should hold a thousandth of the samples is
        // allowed to hold two thousandths, while a band that should hold a third is held to
        // within a percent of the total — which is what 200k samples supports.
        CHECK(std::fabs(measured - expected) < 0.01);
      }
    }
  }
}

TEST_CASE("sampling: what a specular sample is worth is what the model says") {
  // The sharp test, and the only one that reaches the narrow lobes a quadrature cannot. For a
  // metal the diffuse lobe vanishes, so the BSDF is F D V exactly, and the weight a path
  // multiplies its throughput by reduces analytically:
  //
  //     f cos / pdf = (F D V) n.l / (G1(v) D / (4 n.v)) = F (4 n.v n.l V) / G1(v) = F G2 / G1
  //
  // — the ratio of the two-directional masking-shadowing term to the one-directional one, times
  // Fresnel. Nothing in that identity is an integral, so it holds at roughness 0.05 as well as
  // at 1, and it fails the moment `sample_ggx_vndf`, `ggx_vndf_pdf`, and `eval` stop describing
  // the same microfacet model.
  u32 rng = rng_seed(21u, 34u, 3u, 55u);
  u32 checked = 0;
  for (const double roughness : {0.05, 0.15, 0.4, 0.7, 1.0}) {
    const double alpha = alpha_of(roughness);
    for (const double n_dot_v : {0.99, 0.7, 0.3}) {
      Surface s = white_surface(roughness, 1.0);
      const Dvec3 v{std::sqrt(1.0 - n_dot_v * n_dot_v), 0.0, n_dot_v};
      s.view = v;
      for (u32 i = 0; i < 32; ++i) {
        const Dvec3 h = sample_ggx_vndf(v, alpha, rng_next(rng), rng_next(rng));
        const Dvec3 l = h * (2.0 * dot(v, h)) - v;
        if (l.z <= 1e-6) continue;
        const double pdf = ggx_vndf_pdf(v, l, alpha);
        if (!(pdf > 0.0)) continue;
        const double weight = eval(s, l).x * l.z / pdf;
        const double g2 = 4.0 * n_dot_v * l.z * v_smith(n_dot_v, l.z, alpha);
        const double expected =
            f_schlick(f0_of(s.albedo, 1.0), dot(v, h)).x * g2 / ggx_g1(n_dot_v, alpha);
        INFO("roughness ", roughness, " n.v ", n_dot_v, ": weight ", weight, " F G2/G1 ", expected);
        CHECK(weight == doctest::Approx(expected).epsilon(1e-6));
        // And it is a weight, not a gain: a single-scatter lobe never returns more than it got.
        CHECK(weight <= 1.0 + 1e-9);
        ++checked;
      }
    }
  }
  CHECK(checked > 300u);
}

TEST_CASE("sampling: the white furnace, and what single-scatter GGX loses") {
  // A closed white environment of radiance 1. A surface that conserved energy would return
  // exactly 1, and what it does return is the directional albedo — integrated by quadrature,
  // which shares only `eval` with the sampler. Both statements below are measurements; the
  // numbers are quoted in docs/subsystems/renderer.md.
  //
  // **Lambert.** At metallic 0 and roughness 1 the engine's BSDF is a Lambert lobe scaled by
  // (1 - F) plus a very wide GGX lobe. It comes back near 1 and not at 1, and it is not meant
  // to: the diffuse lobe gives up what Fresnel reflects and the single-scatter specular lobe
  // does not give all of it back.
  const double lambert_albedo = directional_albedo_quadrature(white_surface(1.0, 0.0), 0.95);
  INFO("white dielectric, roughness 1, n.v 0.95: ", lambert_albedo);
  CHECK(lambert_albedo < 1.0);
  CHECK(lambert_albedo > 0.94);

  // And the sampler agrees with the quadrature about it, which is the estimator-level version
  // of the identity above: same number, one by grid and one by Monte Carlo.
  const double lambert_sampled =
      directional_albedo_sampled(white_surface(1.0, 0.0), 0.95, 400000u, 9u);
  INFO("the same by Monte Carlo: ", lambert_sampled);
  CHECK(lambert_sampled == doctest::Approx(lambert_albedo).epsilon(0.01));

  // **GGX.** A white metal has no diffuse lobe, so what comes back is the single-scatter GGX
  // albedo alone, and the energy lost to the microsurface's second and later bounces — which
  // this model does not simulate — is the shortfall. It grows with roughness, which is the
  // shape a multiple-scattering compensation term (Kulla-Conty) would undo, and at roughness 1
  // it is most of the energy: a fully rough white metal keeps under a third of what it got.
  // That is a real property of the lighting model the real-time path and the reference share,
  // not an artefact of either, which is why it belongs in a test that states the number.
  double previous = 2.0;
  MESSAGE("furnace, white dielectric roughness 1.00, n.v 0.95: " << lambert_albedo);
  for (const double roughness : {0.3, 0.5, 0.75, 1.0}) {
    const double albedo = directional_albedo_quadrature(white_surface(roughness, 1.0), 0.95);
    // Printed, not only checked: this table is what docs/subsystems/renderer.md quotes, and a
    // number a reader cannot regenerate from the suite is a number nobody will ever update.
    MESSAGE("furnace, white metal roughness " << roughness << ", n.v 0.95: " << albedo);
    INFO("white metal, roughness ", roughness, ", n.v 0.95: ", albedo);
    CHECK(albedo < 1.001);            // never gains energy
    CHECK(albedo > 0.25);             // and the worst case still keeps a quarter
    CHECK(albedo < previous + 1e-6);  // monotone: rougher loses more
    previous = albedo;
  }
  const double rough_metal = directional_albedo_quadrature(white_surface(1.0, 1.0), 0.95);
  INFO("white metal, roughness 1, n.v 0.95: ", rough_metal);
  CHECK(rough_metal > 0.28);
  CHECK(rough_metal < 0.36);
}

TEST_CASE("sampling: the generator is a function of pixel, sample index, and seed") {
  // Determinism first: the same triple gives the same stream, and that is what makes a capture
  // reproducible and a capture split into several dispatches equal to one dispatch.
  u32 a = rng_seed(13u, 29u, 7u, 99u);
  u32 b = rng_seed(13u, 29u, 7u, 99u);
  CHECK(a == b);
  for (u32 i = 0; i < 8; ++i) {
    CHECK(rng_next(a) == rng_next(b));
  }
  CHECK(rng_seed(13u, 29u, 7u, 99u) != rng_seed(13u, 29u, 8u, 99u));
  CHECK(rng_seed(13u, 29u, 7u, 99u) != rng_seed(14u, 29u, 7u, 99u));
  CHECK(rng_seed(13u, 29u, 7u, 99u) != rng_seed(13u, 29u, 7u, 100u));

  // And the stream is in range and flat enough to integrate with: the mean of a uniform variate
  // and an eight-bin histogram over a hundred thousand draws from a hundred different pixels.
  u32 bins[8] = {};
  double total = 0.0;
  u32 count = 0;
  for (u32 pixel = 0; pixel < 100u; ++pixel) {
    u32 state = rng_seed(pixel % 10u, pixel / 10u, 0u, 5u);
    for (u32 i = 0; i < 1000u; ++i) {
      const double v = rng_next(state);
      REQUIRE(v >= 0.0);
      REQUIRE(v < 1.0);
      ++bins[static_cast<u32>(v * 8.0)];
      total += v;
      ++count;
    }
  }
  CHECK(total / static_cast<double>(count) == doctest::Approx(0.5).epsilon(0.01));
  for (const u32 bin : bins) {
    CHECK(bin > count / 10u);  // every eighth is within 20% of its share
    CHECK(bin < count / 6u);
  }
}

TEST_CASE("sampling: the power heuristic weights a pair of strategies to one") {
  CHECK(mis_power_heuristic(1.0, 1.0) == doctest::Approx(0.5));
  CHECK(mis_power_heuristic(2.0, 0.0) == doctest::Approx(1.0));
  CHECK(mis_power_heuristic(0.0, 2.0) == doctest::Approx(0.0));
  CHECK(mis_power_heuristic(0.0, 0.0) == doctest::Approx(0.0));
  // A pair of strategies always splits one unit of weight between them, which is the property
  // that makes a combined estimator unbiased.
  for (const double a : {0.1, 1.0, 7.5}) {
    for (const double b : {0.2, 3.0, 12.0}) {
      CHECK(mis_power_heuristic(a, b) + mis_power_heuristic(b, a) == doctest::Approx(1.0));
    }
  }
}
