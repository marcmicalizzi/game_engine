// Foliage optics' closed forms (R164; brdf.slang, "foliage"; brdf_reference.h), held to what they
// stand for, with no device: the sphere's share is the n.v-weighted mean, over the normals of a
// sphere the eye sees, of a Lambertian facet's cosine to the light (a reflecting one) or to the
// light behind it (a transmitting one), and the ambient's weights are the same mean for an ambient
// linear in the normal. The shading test holds the GPU to these same functions.
#include "brdf_reference.h"

#include <domain/gfx/visibility_resolve.h>

#include <doctest/doctest.h>

#include <cmath>

using namespace engine;
namespace ref = engine::brdf_ref;

namespace {

// A deterministic stratified walk over the unit sphere (a Fibonacci lattice): `count` normals of
// equal area, so a sum over them is the mean over the sphere.
ref::Dvec3 fibonacci(u32 i, u32 count) {
  const double golden = ref::k_pi * (3.0 - std::sqrt(5.0));
  const double y = 1.0 - (2.0 * (static_cast<double>(i) + 0.5)) / static_cast<double>(count);
  const double r = std::sqrt(1.0 - y * y);
  const double phi = golden * static_cast<double>(i);
  return {r * std::cos(phi), y, r * std::sin(phi)};
}

// The n.v-weighted mean over visible normals of max(n.l, 0) (reflected) and max(-n.l, 0)
// (transmitted), and of n itself.
void sphere_means(ref::Dvec3 l, ref::Dvec3 v, double& reflected, double& transmitted,
                  ref::Dvec3& mean_normal) {
  constexpr u32 k_count = 400000;
  double weight = 0.0;
  reflected = 0.0;
  transmitted = 0.0;
  mean_normal = {};
  for (u32 i = 0; i < k_count; ++i) {
    const ref::Dvec3 n = fibonacci(i, k_count);
    const double n_dot_v = ref::dot(n, v);
    if (n_dot_v <= 0.0) continue;
    const double n_dot_l = ref::dot(n, l);
    weight += n_dot_v;
    reflected += n_dot_v * (n_dot_l > 0.0 ? n_dot_l : 0.0);
    transmitted += n_dot_v * (n_dot_l < 0.0 ? -n_dot_l : 0.0);
    mean_normal = mean_normal + n * n_dot_v;
  }
  reflected /= weight;
  transmitted /= weight;
  mean_normal = mean_normal * (1.0 / weight);
}

}  // namespace

TEST_CASE("foliage optics: the sphere's share is the Lambert sphere's law") {
  const ref::Dvec3 v = ref::normalize(ref::Dvec3{0.2, -0.9, 0.3});
  for (const double phase_deg : {0.0, 30.0, 60.0, 90.0, 120.0, 150.0, 179.0}) {
    // A light at `phase_deg` from the eye, in a plane through the view.
    const double a = phase_deg * ref::k_pi / 180.0;
    ref::Dvec3 t = ref::normalize(ref::cross(v, ref::Dvec3{0.0, 0.0, 1.0}));
    const ref::Dvec3 l = ref::normalize(v * std::cos(a) + t * std::sin(a));
    double reflected = 0.0;
    double transmitted = 0.0;
    ref::Dvec3 mean;
    sphere_means(l, v, reflected, transmitted, mean);
    const double c = ref::dot(l, v);
    CHECK(std::abs(reflected - (2.0 / 3.0) * ref::lambert_phase(c)) < 2e-4);
    CHECK(std::abs(transmitted - (2.0 / 3.0) * ref::lambert_phase(-c)) < 2e-4);
    // The visible normals' weighted mean is (2/3) v, which is where the ambient's 5/6 and 1/6 come
    // from: for A(n) = a + b.n, the mean is a + (2/3) b.v = (5/6) A(v) + (1/6) A(-v).
    CHECK(std::abs(mean.x - (2.0 / 3.0) * v.x) < 2e-4);
    CHECK(std::abs(mean.y - (2.0 / 3.0) * v.y) < 2e-4);
    CHECK(std::abs(mean.z - (2.0 / 3.0) * v.z) < 2e-4);
  }
  // Lambert's law at its ends: one at zero phase, zero straight behind, 1/pi at a right angle.
  CHECK(ref::lambert_phase(1.0) == doctest::Approx(1.0));
  CHECK(ref::lambert_phase(-1.0) == doctest::Approx(0.0));
  CHECK(ref::lambert_phase(0.0) == doctest::Approx(1.0 / ref::k_pi));
}

TEST_CASE("foliage optics: none is the sheet, and the response is reciprocal") {
  ref::Surface s;
  s.normal = ref::Dvec3{0.0, 1.0, 0.0};
  s.albedo = ref::Dvec3{0.06, 0.13, 0.04};
  s.roughness = 0.6;
  const ref::Dvec3 l = ref::normalize(ref::Dvec3{0.3, 0.8, -0.2});
  s.view = ref::normalize(ref::Dvec3{-0.4, 0.7, 0.5});
  // No optics: exactly the sheet the resolve always drew.
  const ref::Dvec3 sheet = ref::direct(s, l, ref::splat(1.0));
  CHECK(sheet.y == ref::sheet_direct(s, l, ref::splat(1.0)).y);
  // A sheet with transmission, the light behind it: the albedo's Lambertian transmission alone.
  s.transmit = 0.8;
  const ref::Dvec3 behind = ref::Dvec3{l.x, -l.y, l.z};
  const ref::Dvec3 through = ref::direct(s, behind, ref::splat(1.0));
  const double expect = (1.0 - 0.04) * 0.13 / ref::k_pi * 0.8 * l.y;
  CHECK(through.y == doctest::Approx(expect).epsilon(1e-12));
  // The sphere's share, diffuse alone, is reciprocal: the eye and the light may trade places.
  s.transmit = 0.3;
  s.spread = 1.0;
  ref::Surface swapped = s;
  swapped.view = l;
  CHECK(ref::foliage_eval_cos(s, l).y == doctest::Approx(ref::foliage_eval_cos(swapped, s.view).y));
}

// R164-3 (review B4): what a foliage surface returns of a uniform light from every direction, both
// hemispheres, is the albedo it reflects plus the one it transmits, through what the Fresnel term
// leaves. Its diffuse part (the response less the sheet's specular lobe, which is the albedo-free
// part: the response at albedo zero) never passes 1 - F0 for the extremes a pack and the importer
// can state: a texel of albedo 1 or 0.8 under a declared share of 1 (a pack's albedo is an image,
// so no parse can hold the pair under one; the response's `foliage_share` does), the pair that sums
// to exactly one, and glTF's base colour of 1 split by `KHR_materials_diffuse_transmission` as the
// extension defines it, 1 - t reflected and t transmitted (the importer's split; gltf_tests.cpp
// holds it to that). The specular lobe is the sheet's own and is not foliage's to bound: the sheet
// alone, at albedo 1 and a grazing eye, already returns more than one (R164-3's log).
TEST_CASE("foliage optics: reflected plus transmitted never passes one") {
  constexpr u32 k_count = 20000;  // the lattice; its error on a known integral is checked below
  const double k_f0 = 0.04;
  struct Case {
    double albedo;
    double transmit;
    double spread;
  };
  const Case cases[] = {
      // A pack's extremes: a white texel, a light one, and the pair that sums to one.
      {1.0, 1.0, 0.0},  {1.0, 1.0, 1.0},   {0.8, 1.0, 0.0},  {0.8, 1.0, 0.5},
      {0.5, 1.0, 0.0},  {0.5, 1.0, 1.0},
      // The importer's: base colour 1 at t = 0.25, 0.5 and 0.75, reflectance 1 - t and the share
      // t / (1 - t) at most 1, which the extension's colour factor of white does not change.
      {0.75, 1.0 / 3.0, 0.0}, {0.5, 1.0, 0.0}, {0.25, 1.0, 0.0},
  };
  for (const double view_deg : {0.0, 60.0, 89.0}) {
    const double a = view_deg * ref::k_pi / 180.0;
    for (const Case& c : cases) {
      CAPTURE(view_deg);
      CAPTURE(c.albedo);
      CAPTURE(c.transmit);
      CAPTURE(c.spread);
      ref::Surface s;
      s.normal = ref::Dvec3{0.0, 1.0, 0.0};
      s.view = ref::Dvec3{std::sin(a), std::cos(a), 0.0};
      s.roughness = 0.5;
      s.transmit = c.transmit;
      s.spread = c.spread;
      ref::Surface bare = s;  // albedo zero: the sheet's specular share alone
      s.albedo = ref::splat(c.albedo);
      bare.albedo = ref::splat(0.0);
      double diffuse = 0.0;
      double known = 0.0;  // the transmitted cosine's integral, exactly 1
      for (u32 i = 0; i < k_count; ++i) {
        const ref::Dvec3 l = fibonacci(i, k_count);
        diffuse += ref::foliage_eval_cos(s, l).y - ref::foliage_eval_cos(bare, l).y;
        const double n_dot_l = ref::dot(s.normal, l);
        known += n_dot_l < 0.0 ? -n_dot_l / ref::k_pi : 0.0;
      }
      const double solid = 4.0 * ref::k_pi / static_cast<double>(k_count);
      diffuse *= solid;
      known *= solid;
      // The lattice's own error on a clipped cosine, the integrand's worst feature.
      CHECK(std::abs(known - 1.0) < 1e-4);
      CHECK(diffuse <= (1.0 - k_f0) + 1e-4);
    }
  }
  // The share the response takes: the declared one where the pair sums under one, and what the
  // albedo leaves where it would not.
  CHECK(ref::foliage_share(0.36, 0.8) == doctest::Approx(0.8));
  CHECK(ref::foliage_share(0.8, 1.0) == doctest::Approx(0.25));
  CHECK(ref::foliage_share(1.0, 1.0) == 0.0);
}

// R164-3 (review B5): the reference path tracer's foliage draw (brdf_reference.h,
// `sample_foliage_bsdf`, path_trace.slang's mirror) is unbiased: under a uniform light of one from
// every direction, the mean of its weights is the response's integral over the sphere. Before the
// mend a grazing view's GGX reflections below the sheet were weighed by the back lobe's density
// alone; measured from 60 to 85 degrees, that read a sheet's mean 0.4 to 1.4 % high and, with the
// sphere's share at 0.8, 121 % high (R164-3's log, pt_bias), each many standard errors past this
// case's four.
TEST_CASE("foliage optics: the path tracer's draw is unbiased") {
  const double a = 80.0 * ref::k_pi / 180.0;
  for (const double spread : {0.0, 0.8}) {
    CAPTURE(spread);
    ref::Surface s;
    s.normal = ref::Dvec3{0.0, 0.0, 1.0};
    s.view = ref::Dvec3{std::sin(a), 0.0, std::cos(a)};
    s.albedo = ref::Dvec3{0.06, 0.13, 0.04};
    s.roughness = 0.6;
    s.transmit = 0.8;
    s.spread = spread;
    constexpr u32 k_lattice = 100000;
    double integral = 0.0;
    for (u32 i = 0; i < k_lattice; ++i)
      integral += ref::foliage_eval_cos(s, fibonacci(i, k_lattice)).y;
    integral *= 4.0 * ref::k_pi / static_cast<double>(k_lattice);
    // A fixed 64-bit LCG (Knuth's MMIX constants), so the case is the same run every time.
    u64 state = 0x9e3779b97f4a7c15ull;
    const auto uniform = [&state]() {
      state = state * 6364136223846793005ull + 1442695040888963407ull;
      return static_cast<double>(state >> 11) * (1.0 / 9007199254740992.0);
    };
    constexpr u32 k_samples = 400000;
    double sum = 0.0;
    double sum_sq = 0.0;
    for (u32 i = 0; i < k_samples; ++i) {
      const double u[4] = {uniform(), uniform(), uniform(), uniform()};
      ref::BsdfSample sample;
      if (!ref::sample_foliage_bsdf(s, s.view, u, sample)) continue;
      sum += sample.weight.y;
      sum_sq += sample.weight.y * sample.weight.y;
    }
    const double mean = sum / k_samples;
    const double error = std::sqrt((sum_sq / k_samples - mean * mean) / k_samples);
    MESSAGE("spread " << spread << ": integral " << integral << ", the draw's mean " << mean
                      << " +- " << error);
    CHECK(std::abs(mean - integral) < 4.0 * error + 1e-4 * integral);
  }
}

TEST_CASE("foliage optics: the codes the material packs") {
  gfx::ResolveMaterial m;
  CHECK((m.flags & gfx::k_material_foliage_mask) == 0u);
  gfx::set_foliage_optics(m, 0.8f, 0.0f);
  CHECK(gfx::foliage_transmit(m) == doctest::Approx(0.8f));
  CHECK(gfx::foliage_spread(m) == 0.0f);
  gfx::set_foliage_optics(m, 0.2f, 1.0f);
  CHECK(gfx::foliage_transmit(m) == doctest::Approx(0.2f));
  CHECK(gfx::foliage_spread(m) == 1.0f);
  // Nothing but the top byte moves.
  gfx::ResolveMaterial masked;
  gfx::set_alpha_mask(masked, 0.5f, 1.0f);
  const u32 before = masked.flags;
  gfx::set_foliage_optics(masked, 0.5f, 0.75f);
  CHECK((masked.flags & ~gfx::k_material_foliage_mask) == before);
  gfx::set_foliage_optics(masked, 0.0f, 0.0f);
  CHECK(masked.flags == before);
}
