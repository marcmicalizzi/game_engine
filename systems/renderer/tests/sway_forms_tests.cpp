// A swaying mesh's form by ring, the CPU half (G204; sway_forms.h; renderer.md, "A swaying mesh's
// form by ring"): what a mesh's motion measures at the rule's wind, and the largest placement that
// stands still seen from a ring's floor — by its size in pixels, and for a field by the far
// field's rest. Hand-built part tables whose answers can be worked out; no device.
#include <domain/geometry/cluster_file.h>
#include <systems/renderer/flora_motion.h>
#include <systems/renderer/sway_forms.h>

#include <doctest/doctest.h>

#include <span>

using namespace engine;
using namespace engine::renderer;

namespace {

using geometry::ClusterFileMotionPart;
using geometry::ClusterFileMotionVertex;

// One upright part of `length` from the origin with a vertex at its tip, weight one: a blade.
struct Blade {
  Vector<ClusterFileMotionPart> parts;
  Vector<Vec3> vertices;
  Vector<ClusterFileMotionVertex> motion;
  FloraMesh mesh;
};

Blade blade_of(const char* species, f32 length) {
  Blade b;
  ClusterFileMotionPart part;
  part.pivot = Vec3{};
  part.direction = Vec3{0.0f, 1.0f, 0.0f};
  part.length = length;
  part.parent = geometry::k_motion_no_parent;
  b.parts.push_back(part);
  b.vertices.push_back(Vec3{0.0f, length, 0.0f});
  b.motion.push_back(ClusterFileMotionVertex{0, 65535});
  REQUIRE(flora_mesh(std::span<const ClusterFileMotionPart>(b.parts.data(), b.parts.size()), {},
                     species, b.mesh));
  return b;
}

SwayMeasure measured(const Blade& b, f32 fit, const SwayFormRules& rules) {
  return measure_sway(
      b.mesh, std::span<const Vec3>(b.vertices.data(), b.vertices.size()),
      std::span<const ClusterFileMotionVertex>(b.motion.data(), b.motion.size()), fit, rules);
}

}  // namespace

TEST_CASE("sway forms: a mesh's reach is the cull's bound at the rule's wind") {
  const Blade fern = blade_of("fern", 0.9f);
  REQUIRE_FALSE(fern.mesh.field);
  const f32 bound = flora_bounds_padding(
      fern.mesh, std::span<const Vec3>(fern.vertices.data(), fern.vertices.size()),
      std::span<const ClusterFileMotionVertex>(fern.motion.data(), fern.motion.size()));
  REQUIRE(bound > 0.0f);
  SwayFormRules rules;
  rules.wind = 1.0f;
  // The bound is the strongest wind's (2); a mean day's is half of it, in metres by the fit.
  const SwayMeasure mean = measured(fern, 1.0f, rules);
  CHECK(mean.padding == bound);
  CHECK(mean.reach_m == doctest::Approx(0.5f * bound));
  CHECK_FALSE(mean.field);
  CHECK(measured(fern, 3.0f, rules).reach_m == doctest::Approx(1.5f * bound));
  rules.wind = 0.7f;
  CHECK(measured(fern, 1.0f, rules).reach_m == doctest::Approx(0.35f * bound));
  // A session that draws no motion: nothing is posed, so no padding and no reach.
  rules.motion = false;
  const SwayMeasure rigid = measured(fern, 1.0f, rules);
  CHECK(rigid.padding == 0.0f);
  CHECK(rigid.reach_m == 0.0f);
}

TEST_CASE("sway forms: a placement stands still where its reach is under the rule's pixels") {
  SwayMeasure sway;
  sway.reach_m = 0.25f;
  SwayFormRules rules;
  rules.still_px = 1.0f;
  // A projection of 2000 px a radian: 0.25 m is one pixel at 500 m.
  const f32 proj = 2000.0f;
  CHECK(still_scale(sway, 500.0f, proj, 0.0f, rules) == doctest::Approx(1.0f));
  CHECK(still_scale(sway, 1000.0f, proj, 0.0f, rules) == doctest::Approx(2.0f));
  CHECK(still_scale(sway, 125.0f, proj, 0.0f, rules) == doctest::Approx(0.25f));
  // Never rising as the floor comes nearer: a ring farther out stills at least what a nearer does.
  f32 before = 0.0f;
  for (f32 floor_m = 10.0f; floor_m < 3000.0f; floor_m += 10.0f) {
    const f32 s = still_scale(sway, floor_m, proj, 0.0f, rules);
    CHECK(s >= before);
    before = s;
  }
  // Half the pixels, half the scale; no pixels, never by size; the rule off, never; no floor (the
  // ring the eye stands in), never.
  rules.still_px = 0.5f;
  CHECK(still_scale(sway, 500.0f, proj, 0.0f, rules) == doctest::Approx(0.5f));
  rules.still_px = 0.0f;
  CHECK(still_scale(sway, 500.0f, proj, 0.0f, rules) == 0.0f);
  rules.still_px = 1.0f;
  CHECK(still_scale(sway, 0.0f, proj, 0.0f, rules) == 0.0f);
  rules.enabled = false;
  CHECK(still_scale(sway, 500.0f, proj, 0.0f, rules) == 0.0f);
  // A mesh that does not move at all (no motion drawn) stands still at every scale.
  rules.enabled = true;
  CHECK(still_scale(SwayMeasure{}, 10.0f, proj, 0.0f, rules) == k_still_every_scale);
}

TEST_CASE("sway forms: a field stands still past the far field's rest by its roots' reach") {
  const Blade grass = blade_of("sward", 0.4f);
  REQUIRE(grass.mesh.field);
  SwayFormRules rules;
  rules.wind = 1.0f;
  rules.still_px = 1.0f;
  rules.field_rest_m = 120.0f;
  const SwayMeasure sway = measured(grass, 1.0f, rules);
  CHECK(sway.field);
  REQUIRE(sway.reach_m > 0.0f);
  const f32 proj = 2000.0f;
  const f32 by_size_100 = still_scale(sway, 100.0f, proj, 0.5f, rules);
  const f32 by_size_130 = still_scale(sway, 130.0f, proj, 0.5f, rules);
  // Short of the rest distance only its size counts; past it a placement whose roots (0.5 m a unit
  // of scale) all lie past the distance is at rest already: (130 - 120) / 0.5.
  CHECK(by_size_100 == doctest::Approx(rules.still_px * 100.0f / (sway.reach_m * proj)));
  CHECK(by_size_100 < 1.0f);
  CHECK(by_size_130 == doctest::Approx(20.0f));
  CHECK(still_scale(sway, 120.0f, proj, 0.5f, rules) < 1.0f);  // on the distance: not past it
  CHECK(still_scale(sway, 130.0f, proj, 0.0f, rules) == k_still_every_scale);
  // The rest rule off (or a session with no rest distance): its size alone again.
  rules.field_rest_m = 0.0f;
  CHECK(still_scale(sway, 130.0f, proj, 0.5f, rules) ==
        doctest::Approx(rules.still_px * 130.0f / (sway.reach_m * proj)));
  // A mesh posed by parts is not the rest rule's: its size alone, whatever the distance says.
  SwayMeasure tree = sway;
  tree.field = false;
  rules.field_rest_m = 120.0f;
  CHECK(still_scale(tree, 130.0f, proj, 0.5f, rules) ==
        doctest::Approx(rules.still_px * 130.0f / (sway.reach_m * proj)));
}
