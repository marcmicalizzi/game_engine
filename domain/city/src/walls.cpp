// Walls, doors, windows and furniture zones (docs/subsystems/city.md, "Walls, doors and windows").
//
// **Walls from the tiling.** A floor's spaces tile its footprint, so every edge of every space is
// either shared with another space or on the facade. Each line is swept once: its elementary
// stretches are labelled with the space on either side, and consecutive stretches with the same
// two spaces are one wall — a centre line between exactly two spaces, which is what an opening
// needs to say what it connects. Its thickness follows from those two: exterior, core, party
// (between units, or a unit and circulation) or partition.
//
// **Doors by the room-graph rule.** The entrances are the archetype's (a lobby, a shop, a house's
// hall, a warehouse's reception and its loading doors); circulation opens onto the corridor; and
// inside a unit every room opens onto the most public room already reached that it shares a wall
// with — an entry or a landing before a living room, a living room before a bedroom — so the doors
// form a tree rooted at the unit's entry. A room that shares no long enough wall with a reached one
// gets no door, and the reachability validator says so: the rule is the grammar's, the check is
// not.
#include "walls.h"

#include "grid.h"

#include <algorithm>

namespace engine::city {

namespace {

constexpr i32 k_door_margin = 15;

struct Edge {
  i32 line = 0;
  i32 a0 = 0;
  i32 a1 = 0;
  u32 space = 0;
  u8 horizontal = 0;  // along x: the line is a z
  u8 high = 0;        // the space lies on the line's high side
};

bool is_bar(Archetype a) noexcept {
  return a == Archetype::ApartmentTower || a == Archetype::MidRiseOverShops ||
         a == Archetype::Office || a == Archetype::CivicShell;
}

bool is_core_space(SpaceKind k) noexcept {
  return k == SpaceKind::Stair || k == SpaceKind::Elevator || k == SpaceKind::Shaft;
}

u32 rank(SpaceKind k) noexcept {
  switch (k) {
    case SpaceKind::Entry:
    case SpaceKind::Landing:
    case SpaceKind::Stair:
    case SpaceKind::Reception:
    case SpaceKind::Corridor:
    case SpaceKind::Lobby: return 0;
    case SpaceKind::LivingKitchen:
    case SpaceKind::Kitchen:
    case SpaceKind::OpenOffice:
    case SpaceKind::ShopFloor:
    case SpaceKind::BackRoom:
    case SpaceKind::Hall:
    case SpaceKind::Department:
    case SpaceKind::Concourse: return 1;
    default: return 2;
  }
}

i32 wall_length(const Wall& w) noexcept { return (w.x1 - w.x0) + (w.z1 - w.z0); }

void floor_walls(Building& b, const Rules& r, u32 floor) {
  const Floor& fl = b.floors[floor];
  Vector<Edge> edges;
  edges.reserve(fl.space_count * 4);
  for (u32 s = fl.first_space; s < fl.first_space + fl.space_count; ++s) {
    const Rect& q = b.spaces[s].rect;
    edges.push_back(Edge{q.z0, q.x0, q.x1, s, 1, 1});
    edges.push_back(Edge{q.z1, q.x0, q.x1, s, 1, 0});
    edges.push_back(Edge{q.x0, q.z0, q.z1, s, 0, 1});
    edges.push_back(Edge{q.x1, q.z0, q.z1, s, 0, 0});
  }
  std::sort(edges.begin(), edges.end(), [](const Edge& x, const Edge& y) {
    if (x.horizontal != y.horizontal) return x.horizontal > y.horizontal;
    if (x.line != y.line) return x.line < y.line;
    return x.a0 < y.a0;
  });
  Vector<i32> points;
  u32 i = 0;
  while (i < edges.size()) {
    u32 j = i;
    while (j < edges.size() && edges[j].horizontal == edges[i].horizontal &&
           edges[j].line == edges[i].line)
      ++j;
    points.clear();
    for (u32 k = i; k < j; ++k) {
      points.push_back(edges[k].a0);
      points.push_back(edges[k].a1);
    }
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    Wall current;
    bool open = false;
    for (u32 k = 0; k + 1 < points.size(); ++k) {
      const i32 p0 = points[k];
      const i32 p1 = points[k + 1];
      u32 low = k_outside, high = k_outside;
      for (u32 e = i; e < j; ++e) {
        if (edges[e].a0 <= p0 && edges[e].a1 >= p1)
          (edges[e].high != 0 ? high : low) = edges[e].space;
      }
      const bool is_wall = low != high;
      if (open && (!is_wall || current.a != low || current.b != high ||
                   (edges[i].horizontal != 0 ? current.x1 : current.z1) != p0)) {
        b.walls.push_back(current);
        open = false;
      }
      if (!is_wall) continue;
      if (open) {
        (edges[i].horizontal != 0 ? current.x1 : current.z1) = p1;
        continue;
      }
      current = Wall{};
      if (edges[i].horizontal != 0) {
        current.x0 = p0;
        current.x1 = p1;
        current.z0 = current.z1 = edges[i].line;
      } else {
        current.z0 = p0;
        current.z1 = p1;
        current.x0 = current.x1 = edges[i].line;
      }
      current.a = low;
      current.b = high;
      current.floor = static_cast<u16>(floor);
      const Space* sa = low != k_outside ? &b.spaces[low] : nullptr;
      const Space* sb = high != k_outside ? &b.spaces[high] : nullptr;
      if (sa == nullptr || sb == nullptr) {
        current.kind = WallKind::Exterior;
        current.thickness_cm = static_cast<u16>(r.exterior_cm);
      } else if (is_bar(b.archetype) && (is_core_space(sa->kind) || is_core_space(sb->kind))) {
        current.kind = WallKind::Core;
        current.thickness_cm = static_cast<u16>(r.core_wall_cm);
      } else if (sa->unit != sb->unit) {
        current.kind = WallKind::Party;
        current.thickness_cm = static_cast<u16>(r.party_cm);
      } else {
        current.kind = WallKind::Partition;
        current.thickness_cm = static_cast<u16>(r.partition_cm);
      }
      open = true;
    }
    if (open) b.walls.push_back(current);
    i = j;
  }
}

// The facade a wall with the outside on one side lies on: 0 front (z = footprint.z0), 1 right
// (x = x1), 2 back (z = z1), 3 left (x = x0); 4 when it is on none (a courtyard would be, and the
// grammar makes none).
u32 facade_of(const Building& b, const Wall& w) noexcept {
  if (w.z0 == w.z1) {
    if (w.z0 == b.footprint.z0) return 0;
    if (w.z0 == b.footprint.z1) return 2;
  } else {
    if (w.x0 == b.footprint.x1) return 1;
    if (w.x0 == b.footprint.x0) return 3;
  }
  return 4;
}

struct Doors {
  Building& b;
  const Rules& r;

  bool overlaps(u32 wall, i32 at, i32 width) const noexcept {
    for (const Opening& o : b.openings) {
      if (o.wall != wall) continue;
      const i32 gap = (static_cast<i32>(o.width_cm) + width) / 2 + k_door_margin;
      if (at > o.at_cm - gap && at < o.at_cm + gap) return true;
    }
    return false;
  }

  void add(u32 wall, OpeningKind kind, i32 at, i32 width, i32 sill, i32 head) {
    Opening o;
    o.wall = wall;
    o.kind = kind;
    o.at_cm = at;
    o.width_cm = static_cast<u16>(width);
    o.sill_cm = static_cast<u16>(sill);
    o.head_cm = static_cast<u16>(head);
    b.openings.push_back(o);
  }

  i32 door_head() const noexcept { return 210; }

  // The longest wall between two spaces, or none.
  u32 between(u32 x, u32 y) const noexcept {
    u32 best = k_no_id;
    i32 len = 0;
    const u32 f = b.spaces[x].floor;
    const Floor& fl = b.floors[f];
    for (u32 w = fl.first_wall; w < fl.first_wall + fl.wall_count; ++w) {
      const Wall& wall = b.walls[w];
      if (!((wall.a == x && wall.b == y) || (wall.a == y && wall.b == x))) continue;
      if (wall_length(wall) > len) {
        len = wall_length(wall);
        best = w;
      }
    }
    return best;
  }

  bool door(u32 x, u32 y, OpeningKind kind = OpeningKind::Door) {
    const u32 w = between(x, y);
    if (w == k_no_id) return false;
    const i32 width = r.door_cm;
    const i32 len = wall_length(b.walls[w]);
    if (len < width + 2 * k_door_margin) return false;
    const i32 at = len / 2;
    if (overlaps(w, at, width)) return false;
    add(w, kind, at, width, 0, door_head());
    return true;
  }

  // A door from the outside on `facade` into `space`, at its facade wall's centre — or, with
  // `at_end`, at the wall's start, leaving the rest of the wall to the windows.
  bool exterior(u32 space, u32 facade, OpeningKind kind, i32 width, bool at_end = false) {
    const Floor& fl = b.floors[b.spaces[space].floor];
    u32 best = k_no_id;
    i32 len = 0;
    for (u32 w = fl.first_wall; w < fl.first_wall + fl.wall_count; ++w) {
      const Wall& wall = b.walls[w];
      if (!((wall.a == space && wall.b == k_outside) || (wall.b == space && wall.a == k_outside)))
        continue;
      if (facade_of(b, wall) != facade || wall_length(wall) <= len) continue;
      len = wall_length(wall);
      best = w;
    }
    // An entrance is as wide as the rules ask where the wall allows, and never narrower than a
    // door.
    width = std::min(width, len - 2 * k_door_margin);
    if (best == k_no_id || width < r.door_cm) return false;
    const i32 head = kind == OpeningKind::Loading ? 450 : door_head();
    add(best, kind, at_end ? width / 2 + 2 * k_door_margin : len / 2, width, 0, head);
    return true;
  }
};

}  // namespace

void make_walls_and_openings(const Plan& plan, Building& b, const Rules& r) {
  (void)plan;
  b.walls.clear();
  b.openings.clear();
  for (u32 f = 0; f < b.floors.size(); ++f) {
    b.floors[f].first_wall = b.walls.size();
    floor_walls(b, r, f);
    b.floors[f].wall_count = b.walls.size() - b.floors[f].first_wall;
  }
  Doors doors{b, r};

  // Entrances.
  for (u32 s = 0; s < b.spaces.size(); ++s) {
    const Space& sp = b.spaces[s];
    if (sp.floor != 0) continue;
    const bool front = sp.rect.z0 == b.footprint.z0;
    if (!front) continue;
    if (sp.kind == SpaceKind::Lobby ||
        (sp.kind == SpaceKind::Entry && sp.unit != k_no_id &&
         b.units[sp.unit].kind == UnitKind::House) ||
        (sp.kind == SpaceKind::Reception && b.archetype == Archetype::Warehouse)) {
      doors.exterior(s, 0, OpeningKind::Entrance, r.entrance_cm);
    } else if (sp.kind == SpaceKind::ShopFloor) {
      // A shop's entrance at one end of its front, so a narrow shop keeps a window beside it (the
      // first E18 run found fourteen 6 m shop fronts whose centred door left no pier for one).
      doors.exterior(s, 0, OpeningKind::Entrance, r.entrance_cm, true);
    }
  }
  if (b.archetype == Archetype::Warehouse) {
    for (u32 s = 0; s < b.spaces.size(); ++s) {
      if (b.spaces[s].kind != SpaceKind::Hall) continue;
      const i32 n = std::max(1, b.footprint.width() / 2000);
      const Floor& fl = b.floors[0];
      for (u32 w = fl.first_wall; w < fl.first_wall + fl.wall_count; ++w) {
        const Wall& wall = b.walls[w];
        if (facade_of(b, wall) != 2 || (wall.a != s && wall.b != s)) continue;
        const i32 len = wall_length(wall);
        for (i32 k = 0; k < n; ++k)
          doors.add(w, OpeningKind::Loading, static_cast<i32>(i64{len} * (2 * k + 1) / (2 * n)),
                    r.loading_cm, 0, 450);
      }
    }
  }

  // Circulation onto the corridor: stairs, elevators, the lobby, storage, and every unit's entry
  // that is not a shop's (a shop opens onto the street).
  for (u32 f = 0; f < b.floors.size(); ++f) {
    const Floor& fl = b.floors[f];
    u32 corridor = k_no_id;
    for (u32 s = fl.first_space; s < fl.first_space + fl.space_count; ++s) {
      if (b.spaces[s].kind == SpaceKind::Corridor) corridor = s;
    }
    if (corridor == k_no_id) continue;
    for (u32 s = fl.first_space; s < fl.first_space + fl.space_count; ++s) {
      const Space& sp = b.spaces[s];
      bool opens = false;
      if (sp.unit == k_no_id) {
        opens = sp.kind == SpaceKind::Stair || sp.kind == SpaceKind::Elevator ||
                sp.kind == SpaceKind::Lobby || sp.kind == SpaceKind::Storage;
      } else {
        const Unit& u = b.units[sp.unit];
        opens = u.entry == s && u.kind != UnitKind::Shop;
      }
      if (opens) doors.door(s, corridor);
    }
  }

  // Inside each unit, floor by floor: every room onto the most public reached room it shares a
  // wall with. A house's upper floors start from their stair, which the links reach.
  Vector<u8> reached(b.spaces.size(), u8{0});
  for (u32 ui = 0; ui < b.units.size(); ++ui) {
    const Unit& u = b.units[ui];
    if (u.space_count == 0) continue;
    for (u32 f = u.floor; f <= u.floor_to; ++f) {
      for (u32 s = u.first_space; s < u.first_space + u.space_count; ++s) {
        const Space& sp = b.spaces[s];
        if (sp.floor != f) continue;
        if ((f == u.floor && s == u.entry) || (f != u.floor && sp.kind == SpaceKind::Stair))
          reached[s] = 1;
      }
      bool progress = true;
      while (progress) {
        progress = false;
        for (u32 s = u.first_space; s < u.first_space + u.space_count; ++s) {
          if (reached[s] != 0 || b.spaces[s].floor != f) continue;
          u32 best = k_no_id;
          for (u32 t = u.first_space; t < u.first_space + u.space_count; ++t) {
            if (reached[t] == 0 || b.spaces[t].floor != f) continue;
            const u32 w = doors.between(s, t);
            if (w == k_no_id || wall_length(b.walls[w]) < r.door_cm + 2 * k_door_margin) continue;
            if (best == k_no_id || rank(b.spaces[t].kind) < rank(b.spaces[best].kind)) best = t;
          }
          if (best != k_no_id && doors.door(s, best)) {
            reached[s] = 1;
            progress = true;
          }
        }
      }
    }
  }

  // Windows: every habitable room, on each of its exterior walls on a facade with daylight, as many
  // as the wall takes at a window's width and a pier either side, clear of the doors.
  const i32 ww = r.window_cm;
  for (u32 w = 0; w < b.walls.size(); ++w) {
    const Wall& wall = b.walls[w];
    const u32 s = wall.a != k_outside ? wall.a : wall.b;
    if (s == k_outside || (wall.a != k_outside && wall.b != k_outside)) continue;
    if ((b.spaces[s].flags & k_space_habitable) == 0) continue;
    const u32 facade = facade_of(b, wall);
    if (facade > 3 || b.exposed[facade] == 0) continue;
    const i32 len = wall_length(wall);
    const i32 width = std::min(ww, len - 2 * 30);
    if (width < 60) continue;
    const i32 n = std::max(1, len / (ww + 120));
    for (i32 k = 0; k < n; ++k) {
      const i32 at = static_cast<i32>(i64{len} * (2 * k + 1) / (2 * n));
      if (doors.overlaps(w, at, width)) continue;
      doors.add(w, OpeningKind::Window, at, width, r.window_sill_cm, r.window_head_cm);
    }
  }
}

void make_zones(Building& b, const Rules& r) {
  b.zones.clear();
  auto zone = [&](u32 space, ZoneKind kind, const Rect& rect) {
    Zone z;
    z.space = space;
    z.kind = kind;
    z.rect = rect;
    b.zones.push_back(z);
  };
  for (u32 s = 0; s < b.spaces.size(); ++s) {
    const Rect& q = b.spaces[s].rect;
    const i32 w = q.width();
    const i32 d = q.depth();
    const i32 cx = (q.x0 + q.x1) / 2;
    const i32 cz = (q.z0 + q.z1) / 2;
    switch (b.spaces[s].kind) {
      case SpaceKind::Bedroom: {
        // The bed along the longer side, its head against the far wall, with its clearance.
        const bool along_z = d >= w;
        const i32 bw = r.bed_width_cm + r.bed_clearance_cm;
        const i32 bl = r.bed_length_cm + r.bed_clearance_cm;
        const i32 zw = along_z ? std::min(bw, w) : std::min(bl, w);
        const i32 zd = along_z ? std::min(bl, d) : std::min(bw, d);
        zone(s, ZoneKind::Bed, Rect{cx - zw / 2, cz - zd / 2, cx - zw / 2 + zw, cz - zd / 2 + zd});
        break;
      }
      case SpaceKind::LivingKitchen:
      case SpaceKind::Kitchen: {
        const i32 run = std::min(w, 240);
        zone(s, ZoneKind::KitchenRun, Rect{q.x0, q.z0, q.x0 + run, q.z0 + std::min(d, 60)});
        if (b.spaces[s].kind == SpaceKind::LivingKitchen && d > 200 && w > 200) {
          zone(s, ZoneKind::Sofa, Rect{cx - 100, q.z1 - 100, cx + 100, q.z1 - 10});
          zone(s, ZoneKind::Table, Rect{cx - 60, cz - 40, cx + 60, cz + 40});
        }
        break;
      }
      case SpaceKind::Bathroom:
        zone(s, ZoneKind::Bath, Rect{q.x0, q.z0, q.x0 + std::min(w, 170), q.z0 + std::min(d, 75)});
        zone(s, ZoneKind::Toilet, Rect{q.x1 - std::min(w, 70), q.z1 - std::min(d, 70), q.x1, q.z1});
        break;
      case SpaceKind::Wc:
        zone(s, ZoneKind::Toilet, Rect{q.x0, q.z1 - std::min(d, 70), q.x0 + std::min(w, 70), q.z1});
        break;
      case SpaceKind::OpenOffice: {
        // Desks in rows: 1.6 by 0.8 with 0.8 behind each, as many as the room takes.
        const i32 pitch_x = 180;
        const i32 pitch_z = 160;
        for (i32 z = q.z0 + 80; z + pitch_z <= q.z1 - 40; z += pitch_z) {
          for (i32 x = q.x0 + 40; x + pitch_x <= q.x1 - 40; x += pitch_x)
            zone(s, ZoneKind::Desk, Rect{x, z, x + 160, z + 80});
        }
        break;
      }
      case SpaceKind::Meeting:
        zone(s, ZoneKind::Table, Rect{cx - 90, cz - 50, cx + 90, cz + 50});
        break;
      case SpaceKind::ShopFloor:
        zone(s, ZoneKind::Counter,
             Rect{q.x0 + 40, cz - 40, q.x0 + 40 + std::min(w - 80, 200), cz + 40});
        zone(s, ZoneKind::Shelving, Rect{q.x1 - 60, q.z0 + 40, q.x1, q.z1 - 40});
        break;
      case SpaceKind::BackRoom:
        zone(s, ZoneKind::Shelving, Rect{q.x0, q.z0, q.x0 + std::min(w, 60), q.z1});
        break;
      case SpaceKind::Hall: {
        for (i32 x = q.x0 + 300; x + 120 <= q.x1 - 300; x += 420)
          zone(s, ZoneKind::Racking, Rect{x, q.z0 + 300, x + 120, q.z1 - 600});
        break;
      }
      case SpaceKind::Reception:
      case SpaceKind::Lobby:
        zone(s, ZoneKind::Seating, Rect{cx - 80, cz - 40, cx + 80, cz + 40});
        break;
      case SpaceKind::Department:
        zone(s, ZoneKind::Desk, Rect{cx - 80, cz - 40, cx + 80, cz + 40});
        break;
      default: break;
    }
  }
}

}  // namespace engine::city
