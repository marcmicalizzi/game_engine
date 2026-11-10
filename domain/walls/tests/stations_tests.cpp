// The plan and the standing (docs/subsystems/walls.md, "Stations", "Standing on the ground").

#include "walls_test_support.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace engine;
using namespace engine::walls;
using walls::test::TestPlane;

TEST_CASE("walls: a station's module is decided by the plan, whatever the tile size") {
  const Layer layer = walls::test::field_layer(1000, -2000, 0.3f);
  Plan a, b;
  std::string why;
  const bool planned = make_plan(layer, 32000, a, why);
  INFO(why);
  REQUIRE(planned);
  REQUIRE(make_plan(layer, 64000, b, why));
  REQUIRE(a.slots.size() == b.slots.size());
  for (u32 i = 0; i < a.slots.size(); ++i) {
    const Slot& x = a.slots[i];
    const Slot& y = b.slots[i];
    CHECK(x.s0_cm == y.s0_cm);
    CHECK(x.x0_mm == y.x0_mm);
    CHECK(x.z1_mm == y.z1_mm);
    CHECK(x.kind == y.kind);
    CHECK(x.variant == y.variant);
    // Every station inside a slot finds that slot.
    const Slot* at = slot_at(a, x.line, (x.s0_cm + x.s1_cm) / 2);
    REQUIRE(at != nullptr);
    CHECK(at->id == x.id);
  }
  // The modules cover the line but its gateway, end to end: each run is a whole number of them,
  // 85 % to 118 % of the module.
  for (const Slot& s : a.slots) {
    const i64 len = s.s1_cm - s.s0_cm;
    if (s.kind == ModuleKind::head && len < 200) continue;  // a short run by a corner
    CHECK(len >= 255);
    CHECK(len <= 355);
  }
}

TEST_CASE("walls: a gateway has a head either side, and a ruin takes the share asked") {
  for (const f32 ruined : {0.0f, 0.1f, 0.25f, 0.5f}) {
    const Layer layer = walls::test::field_layer(0, 0, ruined);
    Plan plan;
    std::string why;
    REQUIRE(make_plan(layer, 32000, plan, why));
    const i64 gate = layer.lines[0].gateways_cm[0], half = layer.gateway_cm / 2;
    const Slot* before = slot_at(plan, 0, gate - half - 1);
    const Slot* after = slot_at(plan, 0, gate + half);
    REQUIRE(before != nullptr);
    REQUIRE(after != nullptr);
    CHECK(before->kind == ModuleKind::head);
    CHECK(before->head_at_end);
    CHECK(after->kind == ModuleKind::head);
    CHECK_FALSE(after->head_at_end);
    CHECK(slot_at(plan, 0, gate) == nullptr);  // the gateway is clear
    for (const LinePlan& line : plan.lines) {
      const u32 asked = static_cast<u32>(
          std::floor(static_cast<f64>(ruined) * static_cast<f64>(line.eligible) + 0.5));
      CHECK(line.fallen == asked);
      u32 counted = 0;
      for (u32 i = 0; i < line.slot_count; ++i)
        counted += plan.slots[line.first_slot + i].kind == ModuleKind::fallen ? 1u : 0u;
      CHECK(counted == asked);
    }
  }
}

TEST_CASE("walls: a module stands on a slope with its footing under the ground, courses level") {
  // Planar ground up to a grade of 1 in 2 along the wall and 1 in 5 across it: the footing's bed
  // at both faces' feet, at nine places along every module, is under the ground or within 5 mm of
  // it; and the module is pitched by at most the half class its kit leaves (3.6 degrees).
  Layer layer;
  WallLine line;
  line.points_mm = {0, 0, 60000, 0, 60000, 30000};
  layer.lines.push_back(line);
  Plan plan;
  std::string why;
  REQUIRE(make_plan(layer, 32000, plan, why));
  const f64 L = static_cast<f64>(layer.module_cm) * 0.01;
  const f64 hw = 0.5 * static_cast<f64>(layer.base_width_cm) * 0.01 + k_scarcement_m;
  const f64 foot = static_cast<f64>(layer.footing_cm) * 0.01;
  f64 worst = -1e9, worst_pitch = 0;
  for (const f64 along : {0.0, 0.1, 0.2, 0.3, 0.4, 0.5})
    for (const f64 across : {0.0, 0.2, -0.2}) {
      const TestPlane TestPlane{along, across, 0.0};
      const scene_gen::Ground ground = walls::test::ground_of(TestPlane);
      for (const Slot& slot : plan.slots) {
        const Standing st = stand_slot(plan, layer, slot, ground);
        const f64 gk = static_cast<f64>(st.grade_class * k_grade_step_permille) / 1000.0;
        const f64 sign = (slot.kind == ModuleKind::run || slot.kind == ModuleKind::fallen)
                             ? (st.grade < 0 ? -1.0 : 1.0)
                             : (slot.head_at_end ? -1.0 : 1.0);
        worst_pitch = std::max(worst_pitch, std::abs(sign * st.grade - gk));
        for (i32 i = 0; i <= 8; ++i) {
          const f64 x = -0.5 * L + L * static_cast<f64>(i) / 8.0;
          for (const f64 z : {-hw, hw}) {
            const Vec3 local{static_cast<f32>(x), static_cast<f32>(gk * x - foot),
                             static_cast<f32>(z)};
            const WorldPos w = walls::test::world_of(st.placement, local);
            const f64 g = TestPlane.gx * w.x + TestPlane.gz * w.z;
            worst = std::max(worst, w.y - g);
          }
        }
      }
    }
  INFO("worst gap " << worst << " m, worst pitch " << worst_pitch);
  CHECK(worst < 0.005);
  CHECK(worst_pitch <= 0.0625 + 1e-9);
}

TEST_CASE("walls: two modules either side of a tile's border meet within a millimetre") {
  // A line along x crossing x = 32 m, on a TestPlane rising 1 in 4 along it and 1 in 10 across: the
  // module ending at a station and the one beginning there, realised by two tiles, put their
  // shared end's foot at the same place.
  Layer layer;
  WallLine line;
  line.points_mm = {1000, 5000, 63000, 5000};
  layer.lines.push_back(line);
  Plan plan;
  std::string why;
  REQUIRE(make_plan(layer, 32000, plan, why));
  const TestPlane TestPlane{0.25, 0.1, 0.0};
  const scene_gen::Ground ground = walls::test::ground_of(TestPlane);
  const f64 L = static_cast<f64>(layer.module_cm) * 0.01;
  u32 crossings = 0;
  f64 worst = 0;
  for (u32 i = 0; i + 1 < plan.slots.size(); ++i) {
    const Slot& a = plan.slots[i];
    const Slot& b = plan.slots[i + 1];
    const Standing sa = stand_slot(plan, layer, a, ground), sb = stand_slot(plan, layer, b, ground);
    const f64 ga = static_cast<f64>(sa.grade_class * k_grade_step_permille) / 1000.0;
    const f64 gb = static_cast<f64>(sb.grade_class * k_grade_step_permille) / 1000.0;
    // Both run uphill along +x here, so a's high end meets b's low end.
    const bool ra = a.head_at_end || sa.grade < 0, rb = b.head_at_end || sb.grade < 0;
    const f64 xa = ra ? -0.5 * L : 0.5 * L, xb = rb ? 0.5 * L : -0.5 * L;
    const WorldPos pa =
        walls::test::world_of(sa.placement, {static_cast<f32>(xa), static_cast<f32>(ga * xa), 0});
    const WorldPos pb =
        walls::test::world_of(sb.placement, {static_cast<f32>(xb), static_cast<f32>(gb * xb), 0});
    const f64 d = std::sqrt((pa.x - pb.x) * (pa.x - pb.x) + (pa.y - pb.y) * (pa.y - pb.y) +
                            (pa.z - pb.z) * (pa.z - pb.z));
    worst = std::max(worst, d);
    const auto ta = tile_of((a.x0_mm + a.x1_mm) / 2, (a.z0_mm + a.z1_mm) / 2, plan.tile_mm);
    const auto tb = tile_of((b.x0_mm + b.x1_mm) / 2, (b.z0_mm + b.z1_mm) / 2, plan.tile_mm);
    crossings += (ta.x != tb.x || ta.z != tb.z) ? 1u : 0u;
  }
  INFO("worst " << worst * 1000.0 << " mm");
  CHECK(crossings >= 1);
  CHECK(worst < 0.001);
}
