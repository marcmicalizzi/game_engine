// The description's own rules (docs/subsystems/structures.md): its roles, what it must satisfy to
// be meshed, the rooms' graph, the walk records, and the example cot.

#include "emit.h"

#include <domain/structures/structures.h>

#include <algorithm>

namespace engine::structures {

using structure::Description;
using structure::Surface;

std::string_view role_of(const Description& d, Surface s) noexcept {
  switch (s) {
    case Surface::wall_outer: return d.roles.wall_outer;
    case Surface::plinth: return d.roles.plinth;
    case Surface::wall_inner: return d.roles.wall_inner;
    case Surface::roof: return d.roles.roof;
    case Surface::timber: return d.roles.timber;
    case Surface::floor: return d.roles.floor;
    case Surface::boards: return d.roles.boards;
    case Surface::ceiling: return d.roles.ceiling;
    case Surface::chimney: return d.roles.chimney;
  }
  return {};
}

// What stands behind a role the pack lacks (structures.md, "Materials by role"): the engine's
// generated sets have rock and sawn timber and no plaster, thatch or flagstone of their own
// before this capability's, so stone falls to a rock set and boards to the plank.
std::string_view fallback_role(Surface s) noexcept {
  switch (s) {
    case Surface::plinth:
    case Surface::chimney: return "rock/granite";
    case Surface::floor: return "rock/sandstone";
    case Surface::timber:
    case Surface::boards: return "wood/plank";
    default: break;
  }
  return {};
}

Vec4 fallback_colour(Surface s) noexcept {
  switch (s) {
    case Surface::wall_outer: return {0.80f, 0.76f, 0.66f, 1.0f};  // limewash, weathered
    case Surface::plinth: return {0.42f, 0.40f, 0.37f, 1.0f};
    case Surface::wall_inner: return {0.78f, 0.74f, 0.65f, 1.0f};
    case Surface::roof: return {0.36f, 0.30f, 0.20f, 1.0f};  // weathered straw
    case Surface::timber: return {0.24f, 0.18f, 0.12f, 1.0f};
    case Surface::floor: return {0.40f, 0.38f, 0.35f, 1.0f};
    case Surface::boards: return {0.34f, 0.26f, 0.17f, 1.0f};
    case Surface::ceiling: return {0.74f, 0.70f, 0.62f, 1.0f};
    case Surface::chimney: return {0.40f, 0.37f, 0.34f, 1.0f};
  }
  return {0.5f, 0.5f, 0.5f, 1.0f};
}

namespace {

bool in_reach(i32 v) noexcept { return v >= -k_reach_cm && v <= k_reach_cm; }

const structure::Room* find_room(const Description& d, i32 id) noexcept {
  for (const structure::Room& r : d.rooms)
    if (static_cast<i32>(r.id) == id) return &r;
  return nullptr;
}

bool rect_ok(const i32* r) noexcept {
  return r[0] < r[2] && r[1] < r[3] && in_reach(r[0]) && in_reach(r[1]) && in_reach(r[2]) &&
         in_reach(r[3]);
}

bool rectilinear(const Vector<i32>& outline) noexcept {
  const u32 n = outline.size() / 2;
  for (u32 i = 0; i < n; ++i) {
    const u32 j = (i + 1) % n;
    if (outline[2 * i] != outline[2 * j] && outline[2 * i + 1] != outline[2 * j + 1]) return false;
  }
  return true;
}

std::string num(i64 v) { return std::to_string(v); }

}  // namespace

bool validate(const Description& d, std::string& why) {
  const u32 storeys = d.storeys.size();
  if (storeys == 0 || storeys > k_max_storeys) {
    why =
        "storeys: a building has 1 to " + num(k_max_storeys) + " storeys, this one " + num(storeys);
    return false;
  }
  for (u32 i = 0; i < storeys; ++i) {
    const structure::Storey& s = d.storeys[i];
    if (s.height < 100 || s.height > 600) {
      why = "storey " + num(i) + ": height " + num(s.height) + " cm is outside 100..600";
      return false;
    }
    if (s.floor_thickness < 5 || s.floor_thickness > 60) {
      why = "storey " + num(i) + ": floor_thickness " + num(s.floor_thickness) +
            " cm is outside 5..60";
      return false;
    }
    if (i + 1 < storeys && d.storeys[i + 1].elevation != s.elevation + s.height) {
      why = "storey " + num(i + 1) + ": elevation " + num(d.storeys[i + 1].elevation) +
            " cm is not the storey below's top, " + num(s.elevation + s.height);
      return false;
    }
    if (!in_reach(s.elevation)) {
      why = "storey " + num(i) + ": elevation out of reach";
      return false;
    }
  }
  if (d.foundation < 0 || d.foundation > 300) {
    why = "foundation: " + num(d.foundation) + " cm is outside 0..300";
    return false;
  }
  if (d.footprint.size() < 6 || d.footprint.size() % 2 != 0) {
    why = "footprint: an outline is at least three corners, x and z each";
    return false;
  }
  for (const i32 v : d.footprint)
    if (!in_reach(v)) {
      why = "footprint: a corner at " + num(v) + " cm is outside +-" + num(k_reach_cm);
      return false;
    }
  {
    Vector<u32> triangles;
    if (!detail::triangulate(d.footprint, triangles)) {
      why = "footprint: the outline crosses itself or encloses nothing";
      return false;
    }
  }
  for (u32 i = 0; i < d.rooms.size(); ++i) {
    const structure::Room& r = d.rooms[i];
    if (r.storey >= storeys) {
      why = "room " + num(i) + ": storey " + num(r.storey) + " does not exist";
      return false;
    }
    for (u32 j = 0; j < i; ++j)
      if (d.rooms[j].id == r.id) {
        why = "room " + num(i) + ": id " + num(r.id) + " is room " + num(j) + "'s";
        return false;
      }
    if (r.outline.size() % 2 != 0 || (!r.outline.empty() && r.outline.size() < 6)) {
      why = "room " + num(i) + ": an outline is at least three corners, x and z each";
      return false;
    }
  }
  if (d.walls.size() > k_max_walls) {
    why = "walls: more than " + num(k_max_walls);
    return false;
  }
  for (u32 i = 0; i < d.walls.size(); ++i) {
    const structure::Wall& w = d.walls[i];
    const std::string name = "wall " + num(i);
    if (w.storey >= storeys) {
      why = name + ": storey " + num(w.storey) + " does not exist";
      return false;
    }
    if (w.bulge != 0) {
      why = name + ": an arc (bulge " + num(w.bulge) + ") is not meshed yet";
      return false;
    }
    if (w.thickness < k_min_wall_cm || w.thickness > k_max_wall_cm) {
      why = name + ": thickness " + num(w.thickness) + " cm is outside " + num(k_min_wall_cm) +
            ".." + num(k_max_wall_cm);
      return false;
    }
    if (!in_reach(w.from[0]) || !in_reach(w.from[1]) || !in_reach(w.to[0]) || !in_reach(w.to[1])) {
      why = name + ": an end is outside +-" + num(k_reach_cm) + " cm";
      return false;
    }
    const i64 dx = w.to[0] - w.from[0], dz = w.to[1] - w.from[1];
    if (dx * dx + dz * dz < static_cast<i64>(w.thickness) * w.thickness) {
      why = name + ": shorter than it is thick";
      return false;
    }
    for (const i32 room : {w.a, w.b})
      if (room < -1 || (room >= 0 && !d.rooms.empty() && find_room(d, room) == nullptr)) {
        why = name + ": room " + num(room) + " does not exist";
        return false;
      }
  }
  for (u32 i = 0; i < d.roofs.size(); ++i) {
    const structure::Roof& r = d.roofs[i];
    const std::string name = "roof " + num(i);
    if (!rect_ok(&r.rect[0])) {
      why = name + ": rect is empty or out of reach";
      return false;
    }
    if (r.form == structure::RoofForm::hip) {
      why = name + ": a hip roof is not meshed yet";
      return false;
    }
    if (r.pitch < 50 || r.pitch > 4000) {
      why = name + ": pitch " + num(r.pitch) + " permille is outside 50..4000";
      return false;
    }
    if (r.thickness < 2 || r.thickness > 80 || r.overhang < 0 || r.overhang > 150 || r.verge < 0 ||
        r.verge > 150) {
      why = name + ": thickness 2..80 cm, overhang and verge 0..150 cm";
      return false;
    }
  }
  if (d.openings.size() > k_max_openings) {
    why = "openings: more than " + num(k_max_openings);
    return false;
  }
  for (u32 i = 0; i < d.openings.size(); ++i) {
    const structure::Opening& o = d.openings[i];
    const std::string name = "opening " + num(i);
    if (o.wall >= d.walls.size()) {
      why = name + ": wall " + num(o.wall) + " does not exist";
      return false;
    }
    if (o.width < 30 || o.width > 400) {
      why = name + ": width " + num(o.width) + " cm is outside 30..400";
      return false;
    }
    if (o.sill < 0 || o.head < o.sill + 20) {
      why = name + ": the head, " + num(o.head) + " cm, is not 20 cm over the sill, " + num(o.sill);
      return false;
    }
    const structure::Wall& w = d.walls[o.wall];
    const f64 dx = static_cast<f64>(w.to[0] - w.from[0]),
              dz = static_cast<f64>(w.to[1] - w.from[1]);
    const f64 length = std::sqrt(dx * dx + dz * dz);
    const f64 pier = static_cast<f64>(w.thickness) * 0.5 + static_cast<f64>(k_min_pier_cm);
    const f64 lo = static_cast<f64>(o.at) - static_cast<f64>(o.width) * 0.5;
    const f64 hi = static_cast<f64>(o.at) + static_cast<f64>(o.width) * 0.5;
    if (lo < pier || hi > length - pier) {
      why = name + ": less than " + num(k_min_pier_cm) + " cm of wall " + num(o.wall) +
            " is left between it and the wall's end";
      return false;
    }
    for (u32 j = 0; j < i; ++j) {
      const structure::Opening& p = d.openings[j];
      if (p.wall != o.wall) continue;
      const f64 plo = static_cast<f64>(p.at) - static_cast<f64>(p.width) * 0.5;
      const f64 phi = static_cast<f64>(p.at) + static_cast<f64>(p.width) * 0.5;
      if (lo < phi + static_cast<f64>(k_min_pier_cm) &&
          plo < hi + static_cast<f64>(k_min_pier_cm)) {
        why = name + ": less than " + num(k_min_pier_cm) + " cm from opening " + num(j) +
              " in wall " + num(o.wall);
        return false;
      }
    }
    // Under the wall's top at both jambs, on both faces.
    const f64 ux = dx / length, uz = dz / length;
    const f64 half = static_cast<f64>(w.thickness) * 0.5;
    const f64 head = static_cast<f64>(d.storeys[w.storey].elevation + o.head);
    for (const f64 s : {lo, hi})
      for (const f64 q : {-half, half}) {
        const f64 x = static_cast<f64>(w.from[0]) + ux * s + uz * q;
        const f64 z = static_cast<f64>(w.from[1]) + uz * s - ux * q;
        const f64 top = detail::wall_top(d, o.wall, x, z);
        if (head + 5.0 > top) {
          why = name + ": its head, " + num(o.head) + " cm, is not 5 cm under wall " + num(o.wall) +
                "'s top there";
          return false;
        }
      }
  }
  bool wells = false;
  for (u32 i = 0; i < d.stairs.size(); ++i) {
    const structure::Stair& s = d.stairs[i];
    const std::string name = "stair " + num(i);
    if (s.kind != structure::StairKind::straight) {
      why = name + ": only a straight flight is meshed yet";
      return false;
    }
    if (s.storey + 1 >= storeys) {
      why = name + ": there is no storey above storey " + num(s.storey);
      return false;
    }
    if (s.risers < 2 || s.risers > 40 || s.going < 10 || s.going > 60 || s.width < 40 ||
        s.width > 300 || s.dir > 3) {
      why = name + ": risers 2..40, going 10..60 cm, width 40..300 cm, dir 0..3";
      return false;
    }
    if (!rect_ok(&s.well[0])) {
      why = name + ": well is empty or out of reach";
      return false;
    }
    wells = true;
  }
  if (wells && !rectilinear(d.footprint)) {
    why = "footprint: a floor with a stair's well in it needs an outline of axis-aligned edges";
    return false;
  }
  for (u32 i = 0; i < d.chimneys.size(); ++i) {
    const structure::Chimney& c = d.chimneys[i];
    if (!rect_ok(&c.rect[0]) || c.top <= c.base + 30 ||
        (c.hearth_side > 3 && c.hearth_side != 255)) {
      why =
          "chimney " + num(i) + ": an empty rect, a top under its base, or a hearth_side not 0..3";
      return false;
    }
    if (c.hearth_side > 3) continue;
    // A hearth leaves the stack a back 10 cm thick and a cheek 8 cm wide each side, and its head
    // stands under the stack's cap.
    const i32 sx = c.rect[2] - c.rect[0], sz = c.rect[3] - c.rect[1];
    const i32 deep = c.hearth_side % 2 == 0 ? sx : sz, wide = c.hearth_side % 2 == 0 ? sz : sx;
    if (c.hearth_storey >= storeys || c.hearth_depth < 10 || c.hearth_depth > deep - 10 ||
        c.hearth_width < 30 || c.hearth_width > wide - 16 || c.hearth_head < 40 ||
        d.storeys[c.hearth_storey].elevation + c.hearth_head > c.top - 40) {
      why = "chimney " + num(i) + ": its hearth does not fit its stack (a back of 10 cm, cheeks " +
            "of 8 cm, a head 40 cm under the top), or its storey does not exist";
      return false;
    }
  }
  for (u32 i = 0; i < d.boxes.size(); ++i) {
    const structure::Box& b = d.boxes[i];
    for (u32 k = 0; k < 3; ++k)
      if (b.min[k] >= b.max[k] || !in_reach(b.min[k]) || !in_reach(b.max[k])) {
        why = "box " + num(i) + ": empty or out of reach";
        return false;
      }
    if (static_cast<u32>(b.surface) >= k_surfaces) {
      why = "box " + num(i) + ": an unknown surface";
      return false;
    }
  }
  return true;
}

void room_links(const Description& d, Vector<RoomLink>& out) {
  out.clear();
  for (u32 i = 0; i < d.openings.size(); ++i) {
    const structure::Opening& o = d.openings[i];
    if (o.kind == structure::OpeningKind::window || o.wall >= d.walls.size()) continue;
    const structure::Wall& w = d.walls[o.wall];
    out.push_back(RoomLink{w.a, w.b, i, false});
  }
  for (u32 i = 0; i < d.stairs.size(); ++i)
    out.push_back(RoomLink{d.stairs[i].from_room, d.stairs[i].to_room, i, true});
}

bool rooms_reachable(const Description& d, i32* unreached) {
  Vector<RoomLink> links;
  room_links(d, links);
  Vector<i32> reached;
  reached.push_back(-1);
  for (u32 head = 0; head < reached.size(); ++head) {
    const i32 at = reached[head];
    for (const RoomLink& l : links) {
      i32 other = at;
      if (l.a == at)
        other = l.b;
      else if (l.b == at)
        other = l.a;
      if (other == at) continue;
      bool seen = false;
      for (const i32 r : reached)
        seen = seen || r == other;
      if (!seen) reached.push_back(other);
    }
  }
  for (const structure::Room& r : d.rooms) {
    bool seen = false;
    for (const i32 id : reached)
      seen = seen || id == static_cast<i32>(r.id);
    if (!seen) {
      if (unreached != nullptr) *unreached = static_cast<i32>(r.id);
      return false;
    }
  }
  return true;
}

bool walk_records(const Description& d, Walk& out, std::string& error) {
  out = {};
  if (!validate(d, error)) return false;
  const auto m = [](f64 cm) { return static_cast<f32>(cm * detail::k_m); };
  for (u32 s = 0; s < d.storeys.size(); ++s) {
    WalkFloor f;
    f.storey = s;
    f.y = m(d.storeys[s].elevation);
    f.first = out.outline.size();
    f.count = d.footprint.size() / 2;
    for (u32 i = 0; i < f.count; ++i)
      out.outline.push_back({m(d.footprint[2 * i]), m(d.footprint[2 * i + 1])});
    out.floors.push_back(f);
  }
  for (u32 i = 0; i < d.stairs.size(); ++i) {
    const structure::Stair& s = d.stairs[i];
    out.wells.push_back(
        WalkWell{s.storey + 1, i, m(s.well[0]), m(s.well[1]), m(s.well[2]), m(s.well[3])});
    const structure::Storey& below = d.storeys[s.storey];
    const detail::Frame2 frame = detail::axis_frame(s.foot[0], s.foot[1], s.dir);
    WalkFlight f;
    f.stair = i;
    f.foot = {m(s.foot[0]), m(below.elevation), m(s.foot[1])};
    f.dir = {static_cast<f32>(frame.dx), static_cast<f32>(frame.dz)};
    f.width = m(s.width);
    f.rise = m(static_cast<f64>(below.height) / static_cast<f64>(s.risers));
    f.going = m(s.going);
    f.risers = s.risers;
    f.top = m(below.elevation + below.height);
    out.flights.push_back(f);
  }
  for (u32 i = 0; i < d.walls.size(); ++i) {
    const structure::Wall& w = d.walls[i];
    const structure::Storey& storey = d.storeys[w.storey];
    WalkWall r;
    r.wall = i;
    r.from = {m(w.from[0]), m(w.from[1])};
    r.to = {m(w.to[0]), m(w.to[1])};
    r.thickness = m(w.thickness);
    const bool grounded = w.storey == 0 && w.kind == structure::WallKind::exterior;
    r.y0 = m(grounded ? storey.elevation - d.foundation : storey.elevation);
    const f64 fx = w.from[0], fz = w.from[1], tx = w.to[0], tz = w.to[1];
    const f64 top = std::min({detail::wall_top(d, i, fx, fz), detail::wall_top(d, i, tx, tz),
                              detail::wall_top(d, i, (fx + tx) * 0.5, (fz + tz) * 0.5)});
    r.y1 = m(top);
    out.walls.push_back(r);
  }
  for (u32 i = 0; i < d.openings.size(); ++i) {
    const structure::Opening& o = d.openings[i];
    const structure::Wall& w = d.walls[o.wall];
    const f64 dx = static_cast<f64>(w.to[0] - w.from[0]),
              dz = static_cast<f64>(w.to[1] - w.from[1]);
    const f64 length = std::sqrt(dx * dx + dz * dz);
    const f64 ux = dx / length, uz = dz / length;
    const f64 lo = static_cast<f64>(o.at) - static_cast<f64>(o.width) * 0.5;
    const f64 hi = static_cast<f64>(o.at) + static_cast<f64>(o.width) * 0.5;
    const f64 base = d.storeys[w.storey].elevation;
    WalkOpening r;
    r.opening = i;
    r.wall = o.wall;
    r.kind = o.kind;
    r.a = {m(static_cast<f64>(w.from[0]) + ux * lo), m(static_cast<f64>(w.from[1]) + uz * lo)};
    r.b = {m(static_cast<f64>(w.from[0]) + ux * hi), m(static_cast<f64>(w.from[1]) + uz * hi)};
    r.sill = m(base + static_cast<f64>(o.sill));
    r.head = m(base + static_cast<f64>(o.head));
    r.thickness = m(w.thickness);
    r.room_a = w.a;
    r.room_b = w.b;
    out.openings.push_back(r);
  }
  for (u32 i = 0; i < d.boxes.size(); ++i) {
    const structure::Box& b = d.boxes[i];
    if (!b.solid) continue;
    out.boxes.push_back(WalkBox{i, detail::metres(b.min[0], b.min[1], b.min[2]),
                                detail::metres(b.max[0], b.max[1], b.max[2])});
  }
  return true;
}

structure::Description example_cot(i32 width, i32 depth, i32 wall) {
  wall += wall % 2;
  Description d;
  d.name = "cot";
  d.style = "rural/cot";
  // The walls' centre lines: a rectangle from the origin, the room inside it.
  const i32 a = width + wall, b = depth + wall;
  const i32 corners[4][2] = {{0, 0}, {0, b}, {a, b}, {a, 0}};  // counter-clockwise from above
  for (const auto& c : corners) {
    d.footprint.push_back(c[0]);
    d.footprint.push_back(c[1]);
  }
  structure::Storey storey;
  storey.elevation = 0;
  storey.height = 210;
  storey.floor_thickness = 15;
  d.storeys.push_back(storey);
  structure::Room room;
  room.id = 0;
  room.kind = structure::RoomKind::hall;
  room.name = "hall";
  room.outline = d.footprint;
  d.rooms.push_back(room);
  // Wall 0 the low-x gable (the hearth's), 1 the back (high z), 2 the high-x gable, 3 the front.
  for (u32 i = 0; i < 4; ++i) {
    structure::Wall w;
    w.from[0] = corners[i][0], w.from[1] = corners[i][1];
    w.to[0] = corners[(i + 1) % 4][0], w.to[1] = corners[(i + 1) % 4][1];
    w.thickness = wall;
    w.kind = structure::WallKind::exterior;
    w.a = 0;
    w.b = -1;
    d.walls.push_back(w);
  }
  const auto opening = [&d](u32 w, structure::OpeningKind kind, i32 at, i32 wide, i32 sill,
                            i32 head) {
    structure::Opening o;
    o.wall = w;
    o.kind = kind;
    o.at = at;
    o.width = wide;
    o.sill = sill;
    o.head = head;
    d.openings.push_back(o);
  };
  opening(3, structure::OpeningKind::door, a * 38 / 100, 90, 0, 190);
  opening(3, structure::OpeningKind::window, a * 76 / 100, 80, 95, 180);
  opening(1, structure::OpeningKind::window, a / 2, 90, 95, 180);
  structure::Roof roof;
  roof.form = structure::RoofForm::gable;
  roof.rect[0] = -wall / 2, roof.rect[1] = -wall / 2;
  roof.rect[2] = a + wall - wall / 2, roof.rect[3] = b + wall - wall / 2;
  roof.ridge_along_x = true;
  roof.eaves = storey.height;
  roof.pitch = 1192;  // fifty degrees: a thatch's
  roof.overhang = 45;
  roof.verge = 25;
  roof.thickness = 30;
  d.roofs.push_back(roof);
  structure::Chimney chimney;
  chimney.rect[0] = -wall / 2 - 8, chimney.rect[1] = b / 2 - 65;
  chimney.rect[2] = wall / 2 + 50, chimney.rect[3] = b / 2 + 65;
  chimney.base = -d.foundation;
  const i32 half = (roof.rect[3] - roof.rect[1]) / 2;
  chimney.top = roof.eaves + roof.pitch * half / 1000 + 2 * roof.thickness + 75;
  chimney.hearth_side = 0;
  chimney.hearth_width = 100;
  chimney.hearth_head = 125;
  chimney.hearth_depth = 40;
  d.chimneys.push_back(chimney);
  return d;
}

}  // namespace engine::structures
