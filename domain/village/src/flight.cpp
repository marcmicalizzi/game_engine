// The way into a house as a walk and as a flight (docs/subsystems/village.md, "The way through a
// house").
#include <domain/village/flight.h>

#include <cmath>
#include <cstdio>

namespace engine::village {

namespace {

f64 metres(i64 mm) noexcept { return static_cast<f64>(mm) / 1000.0; }

JsonValue point_json(WorldPos p) {
  // Whole millimetres: a path a person reads and a second run writes the same.
  const auto mm = [](f64 v) { return static_cast<f64>(scene_gen::nearest_mm(v)) / 1000.0; };
  JsonValue a = JsonValue::array();
  a.push_back(JsonValue(mm(p.x)));
  a.push_back(JsonValue(mm(p.y)));
  a.push_back(JsonValue(mm(p.z)));
  return a;
}

}  // namespace

bool walk_points(const VillageLayer& layer, const VillagePlan& plan, u32 house,
                 Vector<WalkPoint>& out, WorldPos& look, std::string& error) {
  out.clear();
  const Plot* plot = nullptr;
  for (const Plot& p : plan.plots)
    if (p.state == PlotState::built && p.house == house) plot = &p;
  if (plot == nullptr) {
    error = "village: no built house " + std::to_string(house) + " (the plan stands " +
            std::to_string(plan.built) + ")";
    return false;
  }
  const House& h = plan.houses[plot->house];
  if (h.route.empty()) {
    error = "village: house " + std::to_string(house) + " (" + h.name + ") names no way through it";
    return false;
  }
  const Plot& p = *plot;
  const f64 half_tread = static_cast<f64>(layer.tread_width_m) * 0.5;
  const f64 top = metres(p.path_top_mm), pad = metres(p.pad_mm);
  // On the tread's middle, opposite the door, and twelve metres back along the lane from there.
  const WorldPos middle{metres(p.path_x_mm) - p.inward_x * half_tread, top,
                        metres(p.path_z_mm) - p.inward_z * half_tread};
  out.push_back(WalkPoint{WorldPos{middle.x - p.along_x * 12.0, top, middle.z - p.along_z * 12.0},
                          "lane", false});
  out.push_back(WalkPoint{middle, "lane at the path", false});
  out.push_back(WalkPoint{WorldPos{metres(p.path_x_mm), top, metres(p.path_z_mm)}, "path", false});
  // A step short of the threshold, on the pad, then the threshold on the house's floor.
  out.push_back(WalkPoint{WorldPos{metres(p.door_x_mm) - p.inward_x * 0.7, pad,
                                   metres(p.door_z_mm) - p.inward_z * 0.7},
                          "before the door", false});
  out.push_back(WalkPoint{WorldPos{metres(p.door_x_mm), pad + metres(h.floor_mm), metres(p.door_z_mm)},
                          "door", true});
  for (u32 k = 0; k < h.route.size(); ++k) {
    const RoutePoint& r = h.route[k];
    out.push_back(WalkPoint{house_point(p, r.x_mm, r.y_mm, r.z_mm),
                            k + 1 == h.route.size() ? "window" : "inside", true});
  }
  look = house_point(p, h.look.x_mm, h.look.y_mm, h.look.z_mm);
  return true;
}

bool flight_json(const VillageLayer& layer, const VillagePlan& plan, u32 house, JsonValue& out,
                 std::string& error, f64 eye_m, f64 speed_m_s) {
  Vector<WalkPoint> walk;
  WorldPos look;
  if (!walk_points(layer, plan, house, walk, look, error)) return false;
  const f64 fps = 60.0;
  JsonValue keys = JsonValue::array(), markers = JsonValue::array();
  const auto eye = [&](const WalkPoint& w) { return WorldPos{w.feet.x, w.feet.y + eye_m, w.feet.z}; };
  const auto add_key = [&](f64 seconds, WorldPos position, WorldPos toward, const char* what) {
    JsonValue k = JsonValue::object();
    k["time"] = JsonValue(std::round(seconds * 1000.0) / 1000.0);
    k["position"] = point_json(position);
    k["target"] = point_json(toward);
    k["fov_deg"] = JsonValue(70.0);
    keys.push_back(std::move(k));
    JsonValue m = JsonValue::object();
    m["frame"] = JsonValue(static_cast<u32>(std::round(seconds * fps)));
    m["name"] = JsonValue(what);
    markers.push_back(std::move(m));
  };
  f64 elapsed = 0.0;
  for (u32 k = 0; k < walk.size(); ++k) {
    const WorldPos at = eye(walk[k]);
    if (k + 1 < walk.size()) {
      // Looking five metres on along the way to the next point.
      const WorldPos next = eye(walk[k + 1]);
      const f64 dx = next.x - at.x, dy = next.y - at.y, dz = next.z - at.z;
      const f64 len = std::sqrt(dx * dx + dy * dy + dz * dz);
      const WorldPos ahead =
          len > 1e-6 ? WorldPos{at.x + dx / len * 5.0, at.y + dy / len * 5.0, at.z + dz / len * 5.0}
                     : next;
      add_key(elapsed, at, ahead, walk[k].what);
      elapsed += std::fmax(len / speed_m_s, 0.75);
    } else {
      // At the window: looking out of it, and staying three seconds.
      add_key(elapsed, at, look, walk[k].what);
      elapsed += 3.0;
      add_key(elapsed, at, look, "window, held");
    }
  }
  out = JsonValue::object();
  out["format"] = JsonValue("engine.camera-path.v1");
  out["name"] = JsonValue((layer.name.empty() ? std::string("village") : layer.name) + " house " +
                          std::to_string(house));
  char text[200];
  std::snprintf(text, sizeof(text),
                "%.0f seconds at %.1f m/s: along the lane, up the path, in at the door, through "
                "the rooms, up the stair and to a window (engine-content village --flight).",
                elapsed, speed_m_s);
  out["description"] = JsonValue(text);
  out["interpolation"] = JsonValue("Smooth");
  out["fps"] = JsonValue(fps);
  out["znear"] = JsonValue(0.05);
  out["keys"] = std::move(keys);
  out["markers"] = std::move(markers);
  return true;
}

}  // namespace engine::village
