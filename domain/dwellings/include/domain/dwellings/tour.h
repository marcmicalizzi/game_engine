#pragma once

// A walk through a planned house, and a place to stand and look in each room
// (docs/subsystems/dwellings.md, "The tour and the views"). Both are read from the plan alone, in
// the plan's frame, so a walk test, a camera path and a set of captures can all be made for any
// house a filler or an author wrote without anyone measuring it by hand.
//
// The tour goes in at the first outer door, visits every room through the doorways the graph has,
// retraces its steps where the graph makes it, climbs the stair after the ground storey's rooms,
// and ends on the stair's head landing in front of its window (or, with no stair, in the last room
// it reached). Every leg is parallel to a wall, and the line keeps `k_tour_clear_cm` from every
// wall's face and every stack, except where it crosses a doorway.

#include <domain/dwellings/interiors.h>

#include <core/base/types.h>
#include <core/containers/vector.h>

namespace engine::dwellings {

// The least distance the tour's line keeps from a wall's face or a stack: the walker's capsule is
// 0.3 m in radius and stops 0.32 m from a face (plan 14 §14.4).
inline constexpr i32 k_tour_clear_cm = 35;

struct TourPoint {
  i32 x_cm = 0;
  i32 z_cm = 0;
  i32 y_mm = 0;  // the floor under the point, above the ground storey's floor
  u32 room = k_no_room;  // the room the point stands in; k_no_room outside the house
};

// The walk. `outside_cm` is how far before the outer wall's inner face it starts (the wall's
// thickness and a step back). False when the plan has no outer door.
bool tour_interior(const InteriorPlan& plan, i32 outside_cm, Vector<TourPoint>& out);

// Where to stand and where to look, one a room: just inside the way in, looking at the room's
// farthest window (a passage looks at its other door, a stair's foot up its flight, its head
// landing out of its window). Positions are millimetres above the ground storey's floor at the eye
// and at the point looked at.
struct RoomView {
  i32 eye_x_cm = 0;
  i32 eye_z_cm = 0;
  i32 eye_y_mm = 0;
  i32 at_x_cm = 0;
  i32 at_z_cm = 0;
  i32 at_y_mm = 0;
  u32 room = k_no_room;
};

void room_views(const InteriorPlan& plan, i32 eye_height_mm, Vector<RoomView>& out);

}  // namespace engine::dwellings
