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
