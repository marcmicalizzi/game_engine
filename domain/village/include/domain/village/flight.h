#pragma once

// The way into a village's house, as a walk and as a flight (docs/subsystems/village.md, "The way
// through a house"): from the lane along the house's path, in at its door, through its rooms, up
// its stair and to a window. One list of points serves both: the collision's case walks a capsule
// along it, and `engine-content village --flight` writes it as a camera path at eye height.

#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/math/world.h>
#include <domain/village/village.h>

#include <string>

namespace engine::village {

// A point of the walk: where the feet are, and what it is for a name.
struct WalkPoint {
  WorldPos feet{};
  const char* what = "";
  bool indoors = false;
};

// The walk to the window of built house `house` (an index into `VillagePlan::houses`): on the
// lane's tread opposite the door, the path's end, the door's threshold, then the house's own route.
// The heights outside are the tread's top and the pad's level; inside, the house's floors'. False
// with a sentence for an index past the houses or a house with no route.
bool walk_points(const VillageLayer& layer, const VillagePlan& plan, u32 house,
                 Vector<WalkPoint>& out, WorldPos& look, std::string& error);

// The same as an `engine.scene.CameraPath` (`engine.camera-path.v1`): a key a point at `eye_m`
// over the feet, looking at the next point's eye, the last looking out of the window; `speed_m_s`
// along the way with a pause at the door and at the window; a marker at each key, named by what
// the point is.
bool flight_json(const VillageLayer& layer, const VillagePlan& plan, u32 house, JsonValue& out,
                 std::string& error, f64 eye_m = 1.65, f64 speed_m_s = 1.2);

}  // namespace engine::village
