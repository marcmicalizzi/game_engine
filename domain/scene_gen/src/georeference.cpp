#include <domain/scene_gen/georeference.h>

#include <cmath>

namespace engine::scene_gen {

namespace {

// The engine axis an `EngineAxis` names: 0 for x, 2 for z, and -1 for none.
int axis_index(scene::EngineAxis a) noexcept {
  switch (a) {
    case scene::EngineAxis::PlusX:
    case scene::EngineAxis::MinusX: return 0;
    case scene::EngineAxis::PlusZ:
    case scene::EngineAxis::MinusZ: return 2;
    case scene::EngineAxis::Unset: break;
  }
  return -1;
}

bool negative(scene::EngineAxis a) noexcept {
  return a == scene::EngineAxis::MinusX || a == scene::EngineAxis::MinusZ;
}

// The world position's coordinate along `a`, signed. A negation is exact.
f64 along(WorldPos p, scene::EngineAxis a) noexcept {
  const f64 v = axis_index(a) == 0 ? p.x : p.z;
  return negative(a) ? -v : v;
}

// Writes `v`, a coordinate along `a`, into the world position.
void place(WorldPos& p, scene::EngineAxis a, f64 v) noexcept {
  const f64 signed_v = negative(a) ? -v : v;
  if (axis_index(a) == 0)
    p.x = signed_v;
  else
    p.z = signed_v;
}

bool blank(const std::string& s) noexcept {
  for (const char c : s)
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
  return true;
}

}  // namespace

bool validate_georeference(const scene::Georeference& g, std::string* why) {
  auto refuse = [why](const char* text) {
    if (why != nullptr) *why = std::string("georeference: ") + text;
    return false;
  };
  if (blank(g.horizontal_crs))
    return refuse("horizontal_crs is empty: name the system (\"EPSG:2952\") or give its WKT");
  if (blank(g.vertical_crs))
    return refuse("vertical_crs is empty: name the system (\"EPSG:6647\") or give its WKT");
  if (!std::isfinite(g.origin_easting) || !std::isfinite(g.origin_northing) ||
      !std::isfinite(g.origin_height))
    return refuse("the origin's easting, northing and height must be finite");
  if (!std::isfinite(g.unit_m) || !(g.unit_m > 0.0))
    return refuse("unit_m must be a finite length above zero");
  if (g.east == scene::EngineAxis::Unset || g.north == scene::EngineAxis::Unset)
    return refuse("east and north must both name an engine axis (PlusX, MinusX, PlusZ, MinusZ)");
  if (axis_index(g.east) == axis_index(g.north))
    return refuse("east and north lie along the same engine axis; they must be x and z");
  return true;
}

MapCoord map_from_world(const scene::Georeference& g, WorldPos p) noexcept {
  MapCoord m;
  m.easting = g.origin_easting + along(p, g.east) / g.unit_m;
  m.northing = g.origin_northing + along(p, g.north) / g.unit_m;
  m.height = g.origin_height + p.y;
  return m;
}

WorldPos world_from_map(const scene::Georeference& g, MapCoord m) noexcept {
  WorldPos p;
  place(p, g.east, (m.easting - g.origin_easting) * g.unit_m);
  place(p, g.north, (m.northing - g.origin_northing) * g.unit_m);
  p.y = m.height - g.origin_height;
  return p;
}

}  // namespace engine::scene_gen
