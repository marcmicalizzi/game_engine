// The tour and the views (docs/subsystems/dwellings.md, "The tour and the views"): the walk
// reaches every room of every plan, every leg is parallel to a wall, the line keeps the walker's
// clearance from walls and stacks and crosses walls only through doorways, it ends on the head
// landing where there is a stair, and every room has a place to stand and a window to look at.

#include <domain/dwellings/rural_filler.h>
#include <domain/dwellings/tour.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;
using namespace engine::dwellings;

namespace {

bool in_rect(const RectCm& r, i32 x, i32 z, i32 margin) {
  return x >= r.x0 + margin && x <= r.x1 - margin && z >= r.z0 + margin && z <= r.z1 - margin;
}

// A crossing from one room to another is a doorway's (through its middle, with the walker's
// clearance to each jamb) or a stair's.
bool crossing_ok(const InteriorPlan& plan, const TourPoint& a, const TourPoint& b) {
  for (const Stair& s : plan.stairs) {
    if ((a.room == s.lower_room && b.room == s.upper_room) ||
        (a.room == s.upper_room && b.room == s.lower_room)) {
      return true;
    }
  }
  for (const Doorway& d : plan.doorways) {
    const InnerWall& w = plan.walls[d.wall];
    if (!((w.a == a.room && w.b == b.room) || (w.a == b.room && w.b == a.room))) continue;
    const bool along_z = w.x0 == w.x1;
    const i32 line = along_z ? w.x0 : w.z0;
    const i32 pa = along_z ? a.x_cm : a.z_cm;
    const i32 pb = along_z ? b.x_cm : b.z_cm;
    const i32 along_a = along_z ? a.z_cm : a.x_cm;
    const i32 along_b = along_z ? b.z_cm : b.x_cm;
    const i32 mid = (along_z ? w.z0 : w.x0) + d.at_cm;
    const i32 slack = d.width_cm / 2 - k_tour_clear_cm;
    const bool straddles = (pa < line) != (pb < line);
    if (straddles && along_a == along_b && along_a >= mid - slack && along_a <= mid + slack) {
      return true;
    }
  }
  return false;
}

void check_tour(const InteriorPlan& plan) {
  Vector<TourPoint> tour;
  REQUIRE(tour_interior(plan, 150, tour));
  REQUIRE(tour.size() >= 3);
  CHECK(tour[0].room == k_no_room);
  Vector<u8> seen;
  seen.resize(plan.rooms.size(), u8{0});
  for (u32 i = 1; i < tour.size(); ++i) {
    const TourPoint& a = tour[i - 1];
    const TourPoint& b = tour[i];
    REQUIRE(b.room < plan.rooms.size());
    seen[b.room] = 1;
    // Parallel to a wall.
    CHECK((a.x_cm == b.x_cm || a.z_cm == b.z_cm));
    // In its room, clear of the room's walls; a door point stands in the doorway's own depth.
    const Room& room = plan.rooms[b.room];
    CHECK(in_rect(room.rect, b.x_cm, b.z_cm, k_tour_clear_cm));
    CHECK(b.y_mm == (room.storey == 0 ? 0 : plan.params.storey_cm * 10));
    // Clear of every stack.
    for (const Hearth& h : plan.hearths) {
      if (h.storey > room.storey) continue;
      const RectCm grown{h.stack.x0 - k_tour_clear_cm + 1, h.stack.z0 - k_tour_clear_cm + 1,
                         h.stack.x1 + k_tour_clear_cm - 1, h.stack.z1 + k_tour_clear_cm - 1};
      const i32 x0 = a.x_cm < b.x_cm ? a.x_cm : b.x_cm;
      const i32 x1 = a.x_cm < b.x_cm ? b.x_cm : a.x_cm;
      const i32 z0 = a.z_cm < b.z_cm ? a.z_cm : b.z_cm;
      const i32 z1 = a.z_cm < b.z_cm ? b.z_cm : a.z_cm;
      if (a.room == b.room) {
        CHECK_FALSE((x0 <= grown.x1 && grown.x0 <= x1 && z0 <= grown.z1 && grown.z0 <= z1));
      }
    }
    if (a.room != k_no_room && a.room != b.room) CHECK(crossing_ok(plan, a, b));
  }
  for (u32 r = 0; r < plan.rooms.size(); ++r)
    CHECK(seen[r] == 1);
  if (!plan.stairs.empty()) {
    const Stair& s = plan.stairs[0];
    const TourPoint& last = tour.back();
    CHECK(last.room == s.upper_room);
    CHECK(in_rect(s.head, last.x_cm, last.z_cm, 0));
    CHECK(last.y_mm == s.rise_mm);
  }

  Vector<RoomView> views;
  room_views(plan, 1650, views);
  REQUIRE(views.size() == plan.rooms.size());
  for (const RoomView& v : views) {
    const Room& room = plan.rooms[v.room];
    CHECK(in_rect(room.rect, v.eye_x_cm, v.eye_z_cm, 30));
    CHECK((v.eye_x_cm != v.at_x_cm || v.eye_z_cm != v.at_z_cm));
    if ((room.flags & k_room_habitable) != 0) {
      // It looks at a window: a point on an outer wall's inner face, at glass height.
      const bool on_wall = v.at_x_cm == 0 || v.at_x_cm == plan.params.width_cm || v.at_z_cm == 0 ||
                           v.at_z_cm == plan.params.depth_cm;
      CHECK(on_wall);
      const i32 above = v.at_y_mm - (room.storey == 0 ? 0 : plan.params.storey_cm * 10);
      CHECK(above == (plan.params.sill_cm + plan.params.head_cm) * 5);
    }
  }
}

}  // namespace

TEST_CASE("tour: the walk reaches every room of every plan type, clear of walls and stacks") {
  for (u32 t = 0; t < k_plan_types; ++t) {
    for (u8 storeys = 1; storeys <= 2; ++storeys) {
      for (u32 variant = 0; variant < 16; ++variant) {
        InteriorParams p;
        p.type = static_cast<PlanType>(t);
        p.storeys = storeys;
        p.inner_room = (variant & 1) != 0;
        p.mirror = (variant & 2) != 0;
        p.hearths = static_cast<u8>(1 + ((variant >> 2) & 1));
        p.seed = variant * 31 + t;
        const InteriorExtent least = interior_min_extent(p);
        p.width_cm = least.min_width_cm + ((variant & 8) != 0 ? 110 : 0);
        p.depth_cm = least.min_depth_cm + ((variant & 8) != 0 ? 30 : 0);
        InteriorPlan plan;
        std::string error;
        INFO(plan_type_name(p.type) << " storeys " << int{storeys} << " variant " << variant);
        REQUIRE(plan_interior(p, plan, &error));
        check_tour(plan);
      }
    }
  }
}

TEST_CASE("tour: the walk through the filler's houses") {
  const RuralStyle style;
  for (u8 persons = 1; persons <= 9; ++persons) {
    for (u8 means = 0; means <= 2; ++means) {
      for (u64 seed = 0; seed < 12; ++seed) {
        RuralChoice choice;
        InteriorPlan plan;
        std::string error;
        INFO("persons " << int{persons} << " means " << int{means} << " seed " << seed);
        REQUIRE(plan_rural(RuralPlot{2400, 1500}, Household{persons, means}, style, seed, choice,
                           plan, &error));
        check_tour(plan);
      }
    }
  }
}
