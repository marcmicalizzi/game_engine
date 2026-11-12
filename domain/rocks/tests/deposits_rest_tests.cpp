// G190: where a pile comes to rest, and that it never ends in a cut (docs/subsystems/rocks.md,
// "Where a pile comes to rest"; docs/experiments/rockfall-deposits.md, G190).
#include <core/schema/json_reflect.h>
#include <domain/rocks/deposits.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>

using namespace engine;
using namespace engine::rocks;
namespace {
// A face at 63 degrees (rise 2 over run 1) standing `face_m` over the top of a transit slope at
// `slope_deg` that falls `drop_m` to a meadow falling gently toward -x from x = 30 m: the forest's
// massif foot, a face whose foot grades through 35 to 50 degrees before the ground can hold debris.
// A shallow gully at z = 16 m makes the contours of the face bend, so a cone stands beside the
// sheet.
struct Graded {
  f64 slope_deg;
  f64 drop_m;
  f64 face_m;
  bool meadow;  // false: the transit slope runs on to the region's edge, with nowhere to rest
};
f64 tan_deg(f64 d) { return std::tan(d * 3.14159265358979323846 / 180.0); }
f64 graded(const void* data, i64 x_mm, i64 z_mm) noexcept {
  const auto& g = *static_cast<const Graded*>(data);
  const f64 x = static_cast<f64>(x_mm) / 1000.0, z = static_cast<f64>(z_mm) / 1000.0;
  const f64 t = tan_deg(g.slope_deg);
  const f64 x0 = g.meadow ? 30.0 : -1000.0;
  const f64 x1 = g.meadow ? x0 + g.drop_m / t : 40.0;
  const f64 top = t * (x1 - x0);
  f64 h;
  if (x < x0)
    h = .08 * (x - x0);
  else if (x < x1)
    h = t * (x - x0);
  else
    h = top + std::min(2.0 * (x - x1), g.face_m);
  const f64 u = (z - 16.0) / 4.0;
  if (x >= x1 && u * u < 1)
    h -= 3.0 * (1 - u * u) * (1 - u * u) * std::clamp((x - x1) / 4.0, 0.0, 1.0);
  return h;
}
DepositLayer layer(u64 seed) {
  DepositLayer p;
  p.seed = seed;
  p.region_min = {0, 0};
  p.region_max = {80, 32};
  p.spacing_m = .5f;
  p.patch.fragments = 8;
  p.patch.fragment_m = .2f;
  p.patch.triangle_budget = 20000;
  p.sediment_depth_m = 0;
  p.blocks_per_100m2 = 0;
  return p;
}
// The step at the outline: for a talus sample beside a bare one, how far its top stands over the
// bare sample's ground beyond what a surface at the repose angle falls over the distance between
// them. A surface no steeper than the repose that meets the ground between the two stands at most
// that much over it, so the step is at most zero there; a surface cut by the run-out or a pile's
// height stands the cut's height over it.
f64 worst_step(const DepositPlan& plan, f64 repose) {
  f64 worst = -1e9;
  const i64 w = plan.width, h = plan.depth;
  const f64 s = static_cast<f64>(plan.spacing_mm) / 1000.0;
  for (i64 z = 0; z < h; ++z)
    for (i64 x = 0; x < w; ++x) {
      const u32 i = static_cast<u32>(z * w + x);
      if (plan.thickness[i] <= 0.0f || plan.coarse[i] <= 0.0f) continue;
      for (i64 dz = -1; dz <= 1; ++dz)
        for (i64 dx = -1; dx <= 1; ++dx) {
          const i64 xx = x + dx, zz = z + dz;
          if ((dx == 0 && dz == 0) || xx < 0 || zz < 0 || xx >= w || zz >= h) continue;
          const u32 j = static_cast<u32>(zz * w + xx);
          if (plan.thickness[j] > 0.0f) continue;
          const f64 d = s * std::sqrt(static_cast<f64>(dx * dx + dz * dz));
          const f64 top = plan.ground[i] + static_cast<f64>(plan.thickness[i]);
          worst = std::max(worst, top - plan.ground[j] - repose * d);
        }
    }
  return worst;
}
// The most a pile may stand over the bare ground past its outline (deposits.cpp, `k_cut_m`: the
// lowest stand of the bed's coarsest clast), plus the edge field's noise, which may lift the first
// bare ring by up to `k_edge_amp`.
constexpr f64 k_bound = 0.22 + k_edge_amp;
constexpr f64 k_repose = 0.70020753820970977;  // tan 35 degrees
}  // namespace

TEST_CASE("deposits: a pile stands where rock comes to rest and never ends in a cut") {
  // The forest's foot (asks-audit gaps 41 and 42): below the face the ground falls at 35, 42 or
  // 50 degrees for 12 m before it can hold debris. Before G190 the foot was the first sample under
  // `source_deg`, the pile's surface stood over ground steeper than itself, and the flood cut it at
  // its height: a wall metres high along the bed's downslope edge.
  for (f64 slope : {35.0, 42.0, 50.0})
    for (u64 seed : {1ull, 7ull, 23ull}) {
      CAPTURE(slope);
      CAPTURE(seed);
      Graded g{slope, 12.0, 20.0, true};
      scene_gen::Ground ground;
      ground.surface_fn = &graded;
      ground.context = &g;
      DepositPlan plan;
      std::string error;
      REQUIRE(plan_deposits(layer(seed), ground, plan, error));
      REQUIRE_FALSE(plan.piles.empty());
      const f64 step = worst_step(plan, k_repose);
      f64 held = 0, asked = 0, refused = 0, deepest = 0;
      for (const auto& pile : plan.piles) {
        held += pile.held_m3;
        asked += pile.volume_m3;
      }
      for (const auto& r : plan.refused)
        refused += r.volume_m3;
      // Every kept pile's foot is on ground that can hold it: gentler than the repose.
      u32 steep_feet = 0;
      for (const auto& pile : plan.piles) {
        const u32 f = pile.foot, x = f % plan.width;
        const f64 left = x > 0 ? plan.ground[f] - plan.ground[f - 1] : 0.0;
        if (left / .5 >= k_repose) ++steep_feet;
      }
      for (u32 i = 0; i < plan.thickness.size(); ++i)
        deepest = std::max(deepest, static_cast<f64>(plan.thickness[i]));
      MESSAGE("slope ", slope, " seed ", seed, ": piles ", plan.piles.size(), ", moved ",
              plan.moved, ", refused ", plan.refused.size(), " (", refused, " m3), held ", held,
              " m3 of ", asked, " asked, deepest ", deepest, " m, worst outline step ", step,
              " m, worst cut ", plan.worst_cut_m, " m, rounds ", plan.rounds);
      CHECK(step <= k_bound);
      CHECK(plan.worst_cut_m <= 0.22 + 1e-9);
      CHECK(steep_feet == 0);
      // The face's fall is stood, not refused: there is a meadow below to hold it, and what is
      // refused is only what fell off the region's two ends (no pile is refused for a cut). A pile
      // holds what it was asked to, not a wedge that fills the transit slope (the base held six
      // times it); G143's planar face holds 5% over, and the bound is a quarter, for the wedge on a
      // meadow that falls away from it. On a back slope at the repose itself the pile's surface
      // lies parallel to it and holds only its toe, a third of what it was asked (rocks.md, "Where
      // a pile comes to rest"), so the lower bound is a quarter.
      for (const auto& r : plan.refused)
        CHECK(r.cut_m == 0.0);
      CHECK(held <= 1.25 * asked);
      CHECK(held >= 0.25 * asked);
    }
}

// A face standing on a narrow bench at the brink of a 45-degree drop to the meadow: the bench is
// gentle enough to hold debris, but a pile on it would drape the drop below it.
f64 benched(const void* data, i64 x_mm, i64) noexcept {
  const f64 bench = *static_cast<const f64*>(data);
  const f64 x = static_cast<f64>(x_mm) / 1000.0;
  if (x < 30) return .08 * (x - 30);
  if (x < 40) return x - 30;
  if (x < 40 + bench) return 10 + .05 * (x - 40);
  return 10 + .05 * bench + std::min(2.0 * (x - 40 - bench), 20.0);
}

TEST_CASE("deposits: a pile on a bench at a brink is moved on down to the meadow below") {
  // The pile the bench's foot would stand ends in a cut over the drop (the flood stops it at its
  // own height): it is moved on over the brink to the meadow, and stands there whole.
  for (f64 bench : {2.0, 4.0}) {
    CAPTURE(bench);
    scene_gen::Ground ground;
    ground.surface_fn = &benched;
    ground.context = &bench;
    DepositPlan plan;
    std::string error;
    REQUIRE(plan_deposits(layer(1), ground, plan, error));
    f64 held = 0, asked = 0, refused = 0;
    for (const auto& pile : plan.piles) {
      held += pile.held_m3;
      asked += pile.volume_m3;
    }
    for (const auto& r : plan.refused)
      refused += r.volume_m3;
    const f64 step = worst_step(plan, k_repose);
    MESSAGE("bench ", bench, " m: piles ", plan.piles.size(), ", moved ", plan.moved, ", refused ",
            plan.refused.size(), " (", refused, " m3), held ", held, " m3 of ", asked,
            " asked, worst outline step ", step, " m, worst cut ", plan.worst_cut_m, " m, rounds ",
            plan.rounds);
    // Every pile the bench would have stood is moved, none refused; the moved pile against the
    // 45-degree drop holds less than it was asked (rocks.md, "Where a pile comes to rest": 40% on
    // a 2 m bench, 57% on a 4 m one), never more (it held five times it on the bench).
    CHECK(plan.moved > 0);
    CHECK(plan.refused.empty());
    CHECK(step <= k_bound);
    CHECK(plan.worst_cut_m <= 0.22 + 1e-9);
    CHECK(held >= 0.25 * asked);
    CHECK(held <= 1.25 * asked);
  }
}

TEST_CASE("deposits: a fall with nowhere to rest is refused and the plan says how much and where") {
  // The transit slope runs on past the region's edge: no ground in the region can hold the face's
  // fall, so nothing is stood and every pile and fall is recorded as refused.
  for (f64 slope : {42.0, 50.0}) {
    CAPTURE(slope);
    Graded g{slope, 0.0, 20.0, false};
    scene_gen::Ground ground;
    ground.surface_fn = &graded;
    ground.context = &g;
    DepositPlan plan;
    std::string error;
    REQUIRE(plan_deposits(layer(1), ground, plan, error));
    f64 refused = 0;
    for (const auto& r : plan.refused) {
      refused += r.volume_m3;
      CHECK(r.x_mm >= 0);
      CHECK(r.x_mm <= 80000);
    }
    MESSAGE("slope ", slope, ": refused ", plan.refused.size(), " (", refused, " m3), kept piles ",
            plan.piles.size(), ", worst outline step ", worst_step(plan, k_repose), " m");
    CHECK_FALSE(plan.refused.empty());
    CHECK(refused > 0.0);
    CHECK(worst_step(plan, k_repose) <= k_bound);
  }
}

TEST_CASE("deposits: a tile's patches arrive with it, none past far_rings, and one plan a load") {
  // G191 (the forest's gap 43): nothing is resident from the scene's read and nothing is written;
  // a tile hands its own patches' recipes, and a ring at or past `far_rings` draws none. T120 (gap
  // 44): the reader's open and the world's consumer's open of the same layer on the same ground
  // plan once; another seed plans again.
  test::TempDir scratch;
  const std::string dir = scratch.path();
  Graded g{42.0, 12.0, 20.0, true};
  scene_gen::Context c;
  c.dir = dir;
  c.ddc = dir;
  c.ground.surface_fn = &graded;
  c.ground.context = &g;
  c.tile_size = 16;
  const auto* desc = scene_gen::GeneratorRegistry::global().find_placement("deposits");
  REQUIRE(desc != nullptr);
  CHECK(desc->meshes == nullptr);
  CHECK(desc->tile_meshes);
  DepositLayer p = layer(991);
  p.far_rings = 2;
  std::string error;
  const u64 made = deposit_plans_made();
  void* first = nullptr;
  void* second = nullptr;
  REQUIRE_MESSAGE(desc->open(schema::to_json(p), c, &first, &error), error);
  REQUIRE_MESSAGE(desc->open(schema::to_json(p), c, &second, &error), error);
  CHECK(deposit_plans_made() == made + 1);
  scene_gen::Placements whole;
  REQUIRE(desc->expand(first, c, whole, &error));
  REQUIRE_FALSE(whole.instances.empty());
  CHECK(whole.meshes.size() == whole.instances.size());
  CHECK_FALSE(io::exists(dir + "/deposits"));
  // The tile under the transit slope's foot (x 30 m) in each ring.
  u32 near = 0, far = 0;
  for (u8 ring = 0; ring < 3; ++ring) {
    c.ring = ring;
    for (i32 tz = 0; tz < 2; ++tz)
      for (i32 tx = 0; tx < 5; ++tx) {
        scene_gen::Placements one;
        REQUIRE(desc->tile(second, {tx, tz}, c, one, &error));
        CHECK(one.meshes.size() == one.instances.size());
        for (const auto& instance : one.instances) {
          REQUIRE(instance.mesh < one.meshes.size());
          CHECK_FALSE(one.meshes[instance.mesh].recipe.empty());
        }
        if (ring < 2) near += one.instances.size();
        if (ring >= 2) far += one.instances.size();
        CHECK(one.kind == (ring < 2 ? 0 : 1));
      }
  }
  CHECK(near == 2 * whole.instances.size());
  CHECK(far == 0);
  CHECK_FALSE(io::exists(dir + "/deposits"));
  desc->close(first);
  desc->close(second);
  // A close and the next open of the same entry: the plan let go is kept until another is made.
  void* again = nullptr;
  REQUIRE_MESSAGE(desc->open(schema::to_json(p), c, &again, &error), error);
  CHECK(deposit_plans_made() == made + 1);
  desc->close(again);
  p.seed = 992;
  REQUIRE_MESSAGE(desc->open(schema::to_json(p), c, &again, &error), error);
  CHECK(deposit_plans_made() == made + 2);
  desc->close(again);
}
