// The interior plan (docs/subsystems/dwellings.md, "Invariants"): every plan type at its
// least and at a generous size, over seeds, mirrored and not, passes its own validators; every room
// is reached from an outer door; a stair keeps its rule and its headroom; no window stands in front
// of an inner wall; and the validators find each of those when a plan is broken by hand.

#include <domain/dwellings/interiors.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;
using namespace engine::dwellings;

namespace {

InteriorParams params_for(PlanType type, u8 storeys, bool inner_room, i32 grow_cm, u64 seed) {
  InteriorParams p;
  p.type = type;
  p.storeys = storeys;
  p.inner_room = inner_room;
  p.seed = seed;
  const InteriorExtent least = interior_min_extent(p);
  p.width_cm = least.min_width_cm + grow_cm;
  p.depth_cm = least.min_depth_cm + grow_cm / 4;
  return p;
}

u32 count_kind(const InteriorPlan& plan, RoomKind kind, u8 storey) {
  u32 n = 0;
  for (const Room& r : plan.rooms) {
    if (r.kind == kind && r.storey == storey) ++n;
  }
  return n;
}

const OuterOpening* door_on(const InteriorPlan& plan, Side side) {
  for (const OuterOpening& o : plan.openings) {
    if (o.door() && o.side == side) return &o;
  }
  return nullptr;
}

bool has_rule(const Vector<InteriorFinding>& findings, InteriorRule rule) {
  for (const InteriorFinding& f : findings) {
    if (f.rule == rule) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("interiors: every plan type lays out and passes its validators") {
  u32 plans = 0;
  for (u32 t = 0; t < k_plan_types; ++t) {
    const PlanType type = static_cast<PlanType>(t);
    for (u8 storeys = 1; storeys <= 2; ++storeys) {
      for (u32 variant = 0; variant < 8; ++variant) {
        for (u64 seed = 0; seed < 24; ++seed) {
          InteriorParams p = params_for(type, storeys, (variant & 1) != 0,
                                        (variant >> 1) % 2 == 0 ? 0 : 130, seed * 977 + t);
          p.mirror = (variant & 4) != 0;
          p.hearths = static_cast<u8>(1 + (seed & 1));
          InteriorPlan plan;
          std::string error;
          const bool ok = plan_interior(p, plan, &error);
          INFO(plan_type_name(type) << " storeys " << int{storeys} << " variant " << variant
                                    << " seed " << seed << ": " << error);
          REQUIRE(ok);
          Vector<InteriorFinding> findings;
          CHECK(validate_interior(plan, findings));
          Vector<u8> reached;
          CHECK(reachable_rooms(plan, reached) == plan.rooms.size());
          ++plans;
        }
      }
    }
  }
  CHECK(plans == k_plan_types * 2 * 8 * 24);
}

TEST_CASE("interiors: each type is the plan a person would name") {
  InteriorPlan plan;
  std::string error;

  SUBCASE("a cot is one heated room with its door in the front wall") {
    REQUIRE(plan_interior(params_for(PlanType::Cot, 1, false, 60, 3), plan, &error));
    CHECK(plan.rooms.size() == 1);
    CHECK(plan.walls.empty());
    CHECK(plan.stairs.empty());
    REQUIRE(plan.hearths.size() == 1);
    CHECK((plan.rooms[0].flags & (k_room_heated | k_room_entrance | k_room_habitable)) ==
          (k_room_heated | k_room_entrance | k_room_habitable));
    CHECK(door_on(plan, Side::Front) != nullptr);
  }

  SUBCASE("a hall and parlour: the door opens into the larger, heated hall") {
    REQUIRE(plan_interior(params_for(PlanType::HallParlour, 1, false, 120, 5), plan, &error));
    REQUIRE(plan.rooms.size() == 2);
    const Room& hall = plan.rooms[0];
    const Room& parlour = plan.rooms[1];
    CHECK(hall.kind == RoomKind::Hall);
    CHECK(parlour.kind == RoomKind::Parlour);
    CHECK(hall.rect.x1 - hall.rect.x0 > parlour.rect.x1 - parlour.rect.x0);
    CHECK((hall.flags & k_room_entrance) != 0);
    CHECK((hall.flags & k_room_heated) != 0);
    CHECK((parlour.flags & k_room_entrance) == 0);
    CHECK(plan.doorways.size() == 1);
    // The gable hearth stands at the house's end and its stack under the ridge.
    REQUIRE(plan.hearths.size() == 1);
    const RectCm& stack = plan.hearths[0].stack;
    CHECK(stack.x0 == 0);
    CHECK(stack.z0 + stack.z1 == plan.params.depth_cm);
  }

  SUBCASE("a cross-passage house: opposed doors, and the stack backs onto the passage") {
    REQUIRE(plan_interior(params_for(PlanType::CrossPassage, 1, true, 100, 9), plan, &error));
    CHECK(count_kind(plan, RoomKind::Inner, 0) == 1);
    CHECK(count_kind(plan, RoomKind::Hall, 0) == 1);
    CHECK(count_kind(plan, RoomKind::Passage, 0) == 1);
    CHECK(count_kind(plan, RoomKind::Service, 0) == 1);
    const OuterOpening* front = door_on(plan, Side::Front);
    const OuterOpening* back = door_on(plan, Side::Back);
    REQUIRE(front != nullptr);
    REQUIRE(back != nullptr);
    CHECK(front->at_cm == back->at_cm);
    CHECK(front->room == back->room);
    CHECK(plan.rooms[front->room].kind == RoomKind::Passage);
    REQUIRE(plan.hearths.size() == 1);
    const Room& hall = plan.rooms[plan.hearths[0].room];
    CHECK(hall.kind == RoomKind::Hall);
    CHECK(plan.hearths[0].stack.x1 == plan.rooms[front->room].rect.x0);
    // A passage is a way through, not a room to light.
    for (const OuterOpening& o : plan.openings) {
      if (!o.door()) CHECK(plan.rooms[o.room].kind != RoomKind::Passage);
    }
  }

  SUBCASE("a two-up two-down: a room in front, a room behind, the stair between, two chambers") {
    REQUIRE(plan_interior(params_for(PlanType::TwoUpTwoDown, 2, false, 40, 11), plan, &error));
    CHECK(count_kind(plan, RoomKind::Parlour, 0) == 1);
    CHECK(count_kind(plan, RoomKind::Kitchen, 0) == 1);
    CHECK(count_kind(plan, RoomKind::Stair, 0) == 1);
    CHECK(count_kind(plan, RoomKind::Chamber, 1) == 2);
    CHECK(count_kind(plan, RoomKind::Stair, 1) == 1);
    REQUIRE(plan.stairs.size() == 1);
    const Stair& s = plan.stairs[0];
    // The strip lies between the two rooms, parallel to the lane.
    CHECK(s.strip.x0 == 0);
    CHECK(s.strip.x1 == plan.params.width_cm);
    CHECK((s.rises == Side::Left || s.rises == Side::Right));
    const OuterOpening* front = door_on(plan, Side::Front);
    REQUIRE(front != nullptr);
    CHECK(plan.rooms[front->room].kind == RoomKind::Parlour);
  }

  SUBCASE("an upper storey is two chambers off the head landing, and the landing has a window") {
    REQUIRE(plan_interior(params_for(PlanType::HallParlour, 2, false, 60, 2), plan, &error));
    CHECK(count_kind(plan, RoomKind::Chamber, 1) == 2);
    REQUIRE(plan.stairs.size() == 1);
    const Stair& s = plan.stairs[0];
    u32 landing_windows = 0;
    for (const OuterOpening& o : plan.openings) {
      if (!o.door() && o.room == s.upper_room) {
        ++landing_windows;
        CHECK(o.side == s.rises);
        CHECK(o.storey() == 1);
      }
    }
    CHECK(landing_windows == 1);
  }
}

TEST_CASE("interiors: the stair is one a person climbs") {
  // The rule's own arithmetic: 2.4 m floor to floor is twelve risers of 200 mm.
  CHECK(stair_risers(2400) == 12);
  CHECK(stair_strip_length_cm(2400) == 11 * 28 + 2 * k_stair_landing_cm);
  CHECK(stair_risers(2310) == 11);
  CHECK(stair_risers(2311) == 12);

  for (u16 storey_cm = 225; storey_cm <= 300; storey_cm = static_cast<u16>(storey_cm + 5)) {
    InteriorParams p;
    p.type = PlanType::HallParlour;
    p.storeys = 2;
    p.storey_cm = storey_cm;
    p.seed = storey_cm;
    const InteriorExtent least = interior_min_extent(p);
    p.width_cm = least.min_width_cm + 40;
    p.depth_cm = least.min_depth_cm;
    InteriorPlan plan;
    std::string error;
    INFO("storey " << storey_cm << ": " << error);
    REQUIRE(plan_interior(p, plan, &error));
    REQUIRE(plan.stairs.size() == 1);
    const Stair& s = plan.stairs[0];
    const i32 rise_mm = (s.rise_mm + s.risers - 1) / s.risers;
    const i32 going_mm = k_stair_going_cm * 10;
    CHECK(rise_mm <= k_stair_max_rise_mm);
    CHECK(rise_mm >= 150);
    CHECK(2 * rise_mm + going_mm <= 700);
    CHECK(2 * rise_mm + going_mm >= 550);
    // Pitch at or under 42 degrees: tan 42 is 0.9004.
    CHECK(rise_mm * 10000 <= going_mm * 9004);
    // The flight is exactly its goings, and the last tread's top is one riser under the floor.
    const i32 run = s.rises == Side::Front || s.rises == Side::Back ? s.flight.z1 - s.flight.z0
                                                                  : s.flight.x1 - s.flight.x0;
    CHECK(run == (s.risers - 1) * k_stair_going_cm);
    CHECK(s.tread_mm(s.risers - 1u) == s.rise_mm);
    CHECK(s.rise_mm - s.tread_mm(s.risers - 2u) <= k_stair_max_rise_mm);
    // Headroom over every tread.
    for (u32 t = 0; t + 1 < s.risers; ++t) {
      const RectCm tread = s.tread(t);
      CHECK(stair_headroom_cm(plan, s, (tread.x0 + tread.x1) / 2, (tread.z0 + tread.z1) / 2,
                              s.tread_mm(t)) >= k_headroom_cm);
    }
  }
}

TEST_CASE("interiors: the validators find a plan broken by hand") {
  InteriorPlan plan;
  std::string error;
  REQUIRE(plan_interior(params_for(PlanType::CrossPassage, 2, true, 80, 21), plan, &error));
  Vector<InteriorFinding> findings;
  REQUIRE(validate_interior(plan, findings));

  SUBCASE("a slab over the flight leaves no headroom") {
    // The head landing stretched over the whole strip: the well is gone.
    plan.stairs[0].head = plan.stairs[0].strip;
    CHECK_FALSE(validate_interior(plan, findings));
    CHECK(has_rule(findings, InteriorRule::StairHeadroom));
  }

  SUBCASE("a low upper storey leaves no headroom on the landing") {
    plan.params.upper_clear_cm = 190;
    CHECK_FALSE(validate_interior(plan, findings));
    CHECK(has_rule(findings, InteriorRule::StairHeadroom));
  }

  SUBCASE("a window moved in front of an inner wall") {
    // The first window of the front wall, slid along to the nearest inner wall's end.
    const InnerWall& wall = plan.walls[0];
    REQUIRE(wall.x0 == wall.x1);
    bool moved = false;
    for (OuterOpening& o : plan.openings) {
      if (!o.door() && o.side == Side::Front && o.storey() == wall.storey) {
        o.at_cm = wall.x0;
        moved = true;
        break;
      }
    }
    REQUIRE(moved);
    CHECK_FALSE(validate_interior(plan, findings));
    CHECK(has_rule(findings, InteriorRule::WindowInWall));
  }

  SUBCASE("a doorway removed cuts rooms off") {
    plan.doorways.erase(plan.doorways.begin());
    CHECK_FALSE(validate_interior(plan, findings));
    CHECK(has_rule(findings, InteriorRule::RoomReachable));
  }

  SUBCASE("a doorway narrower than the walker") {
    plan.doorways[0].width_cm = 70;
    CHECK_FALSE(validate_interior(plan, findings));
    CHECK(has_rule(findings, InteriorRule::DoorwayClear));
  }

  SUBCASE("a stack pushed into a doorway's approach") {
    Hearth& h = plan.hearths[0];
    const i32 depth = h.stack.z1 - h.stack.z0;
    h.stack.z0 = 20;
    h.stack.z1 = 20 + depth;
    CHECK_FALSE(validate_interior(plan, findings));
    CHECK(has_rule(findings, InteriorRule::HearthClear));
  }

  SUBCASE("a room with its windows taken away") {
    u32 kept = 0;
    for (u32 i = 0; i < plan.openings.size(); ++i) {
      const OuterOpening o = plan.openings[i];
      if (o.door() || plan.rooms[o.room].kind != RoomKind::Hall) plan.openings[kept++] = o;
    }
    plan.openings.resize(kept);
    CHECK_FALSE(validate_interior(plan, findings));
    CHECK(has_rule(findings, InteriorRule::RoomLit));
  }
}

TEST_CASE("interiors: a plan draws as text, the lane below") {
  for (u32 t = 0; t < k_plan_types; ++t) {
    InteriorPlan plan;
    std::string error;
    InteriorParams p = params_for(static_cast<PlanType>(t), 2, true, 90, 7 + t);
    p.hearths = 2;
    REQUIRE(plan_interior(p, plan, &error));
    for (u8 storey = 0; storey < plan.params.storeys; ++storey) {
      const std::string text = draw_interior(plan, storey, 20);
      const i32 cols = (p.width_cm + 19) / 20;
      const i32 rows = (p.depth_cm + 19) / 20;
      CHECK(text.size() == static_cast<usize>((cols + 3) * (rows + 2)));
      CHECK(text.find('W') != std::string::npos);
      if (storey == 0) {
        CHECK(text.find('D') != std::string::npos);
        CHECK(text.find('H') != std::string::npos);
      }
      if (!plan.stairs.empty()) {
        CHECK(text.find(storey == 0 ? 'f' : 'h') != std::string::npos);
      }
      MESSAGE(plan_type_name(plan.params.type) << ", storey " << int{storey} << ", "
                                               << p.width_cm << " x " << p.depth_cm << " cm\n"
                                               << text);
    }
  }
}

TEST_CASE("interiors: a rectangle too small for its type is refused with a sentence") {
  for (u32 t = 0; t < k_plan_types; ++t) {
    InteriorParams p = params_for(static_cast<PlanType>(t), 2, t == 2, 0, 1);
    InteriorPlan plan;
    std::string error;
    REQUIRE(plan_interior(p, plan, &error));
    p.width_cm -= 60;
    p.depth_cm -= 60;
    CHECK_FALSE(plan_interior(p, plan, &error));
    CHECK(error.rfind("interior: ", 0) == 0);
    CHECK(plan.rooms.empty());
  }
}

TEST_CASE("interiors: a mirrored plan is the plan flipped left to right") {
  for (u32 t = 0; t < k_plan_types; ++t) {
    InteriorParams p = params_for(static_cast<PlanType>(t), 2, true, 70, 40 + t);
    InteriorPlan plain;
    InteriorPlan mirrored;
    std::string error;
    REQUIRE(plan_interior(p, plain, &error));
    p.mirror = true;
    REQUIRE(plan_interior(p, mirrored, &error));
    REQUIRE(plain.rooms.size() == mirrored.rooms.size());
    for (u32 i = 0; i < plain.rooms.size(); ++i) {
      CHECK(mirrored.rooms[i].rect.x0 == p.width_cm - plain.rooms[i].rect.x1);
      CHECK(mirrored.rooms[i].rect.x1 == p.width_cm - plain.rooms[i].rect.x0);
      CHECK(mirrored.rooms[i].rect.z0 == plain.rooms[i].rect.z0);
      CHECK(mirrored.rooms[i].kind == plain.rooms[i].kind);
    }
    REQUIRE(plain.hearths.size() == mirrored.hearths.size());
    for (u32 i = 0; i < plain.hearths.size(); ++i) {
      CHECK(mirrored.hearths[i].stack.x0 == p.width_cm - plain.hearths[i].stack.x1);
    }
    CHECK(hash_interior(plain) != hash_interior(mirrored));
  }
}
