// The house filler the generator plans with (docs/subsystems/village.md, "The house seam" and "The
// way through a house"): the one file of the module that knows what a house is made by. It asks
// the structures capability for a building description, reads the description's walk records
// (floors, walls, openings, stair flights: `structures::walk_records`), holds them to what the
// walker can pass, and turns them into what the plan knows — a box, a door, a recipe — and the way
// through the house.
#include "detail.h"

#include <core/hash/hash.h>
#include <domain/dwellings/describe.h>
#include <domain/dwellings/furnish.h>
#include <domain/dwellings/tour.h>
#include <domain/structures/structures.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace engine::village {

namespace {

using structures::Walk;
using structures::WalkOpening;

// ---- what the walker can pass (plan 14 §14.4; physics::CharacterConfig's capsule) ---------------
//
// The capsule is 0.6 m across and 1.8 m tall and climbs a 0.35 m step. A doorway's clear width is
// its opening less the frame's 6 cm each side (structures.md, "Openings").
constexpr f64 k_frame_m = 0.06;
constexpr f64 k_min_clear_m = 0.66;  // the capsule and 3 cm each side
constexpr f64 k_min_head_m = 1.85;   // the capsule and 5 cm
constexpr f64 k_max_rise_m = 0.25;   // plan 14's riser, under the walker's 0.35 m step
constexpr f64 k_min_going_m = 0.20;  // a tread a foot stands on

// ---- one storey's free ground, as a grid ------------------------------------------------------
//
// Where a walker's middle may be on a storey: inside a floor, a capsule's radius and a little from
// every wall's face (a wall is solid here: a doorway is crossed by a straight stretch through its
// middle, never by the grid), off every solid member, off a stair's flight and off a well.
constexpr f64 k_cell_m = 0.1;
constexpr f64 k_clear_m = 0.34;
constexpr f64 k_leaf_clear_m = 0.33;
constexpr f64 k_through_m = 0.5;  // how far each side of a wall's face a doorway's stretch runs

struct Grid {
  f64 x0 = 0, z0 = 0;
  i32 nx = 0, nz = 0;
  Vector<u8> open;
  bool at(i32 i, i32 j) const noexcept {
    return i >= 0 && j >= 0 && i < nx && j < nz && open[static_cast<u32>(j * nx + i)] != 0;
  }
  f64 x(i32 i) const noexcept { return x0 + k_cell_m * static_cast<f64>(i); }
  f64 z(i32 j) const noexcept { return z0 + k_cell_m * static_cast<f64>(j); }
};

f64 segment_distance(f64 px, f64 pz, f64 ax, f64 az, f64 bx, f64 bz) noexcept {
  const f64 dx = bx - ax, dz = bz - az, len2 = dx * dx + dz * dz;
  f64 t = len2 > 0.0 ? ((px - ax) * dx + (pz - az) * dz) / len2 : 0.0;
  t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
  const f64 qx = ax + t * dx - px, qz = az + t * dz - pz;
  return std::sqrt(qx * qx + qz * qz);
}

bool inside_outline(const Walk& walk, u32 first, u32 count, f64 px, f64 pz) noexcept {
  bool in = false;
  for (u32 k = 0, j = count - 1; k < count; j = k++) {
    const f64 ax = static_cast<f64>(walk.outline[first + k].x),
              az = static_cast<f64>(walk.outline[first + k].y);
    const f64 bx = static_cast<f64>(walk.outline[first + j].x),
              bz = static_cast<f64>(walk.outline[first + j].y);
    if ((az > pz) != (bz > pz) && px < (bx - ax) * (pz - az) / (bz - az) + ax) in = !in;
  }
  return in;
}

bool storey_grid(const structure::Description& d, const Walk& walk, u32 storey, Grid& g) {
  const f64 level = static_cast<f64>(d.storeys[storey].elevation) / 100.0;
  f64 lo_x = 1e30, lo_z = 1e30, hi_x = -1e30, hi_z = -1e30;
  for (const structures::WalkFloor& f : walk.floors) {
    if (f.storey != storey) continue;
    for (u32 k = 0; k < f.count; ++k) {
      lo_x = std::min(lo_x, static_cast<f64>(walk.outline[f.first + k].x));
      hi_x = std::max(hi_x, static_cast<f64>(walk.outline[f.first + k].x));
      lo_z = std::min(lo_z, static_cast<f64>(walk.outline[f.first + k].y));
      hi_z = std::max(hi_z, static_cast<f64>(walk.outline[f.first + k].y));
    }
  }
  if (!(hi_x > lo_x) || !(hi_z > lo_z)) return false;
  g.x0 = lo_x;
  g.z0 = lo_z;
  g.nx = static_cast<i32>((hi_x - lo_x) / k_cell_m) + 2;
  g.nz = static_cast<i32>((hi_z - lo_z) / k_cell_m) + 2;
  g.open.resize(static_cast<u32>(g.nx * g.nz));
  for (i32 j = 0; j < g.nz; ++j)
    for (i32 i = 0; i < g.nx; ++i) {
      const f64 px = g.x(i), pz = g.z(j);
      bool ok = false;
      for (const structures::WalkFloor& f : walk.floors)
        ok = ok || (f.storey == storey && inside_outline(walk, f.first, f.count, px, pz));
      for (const structures::WalkWall& w : walk.walls) {
        if (!ok) break;
        if (!(static_cast<f64>(w.y0) <= level + 0.5 && static_cast<f64>(w.y1) > level + 0.5))
          continue;
        if (segment_distance(px, pz, static_cast<f64>(w.from.x), static_cast<f64>(w.from.y),
                             static_cast<f64>(w.to.x), static_cast<f64>(w.to.y)) <
            static_cast<f64>(w.thickness) * 0.5 + k_clear_m)
          ok = false;
      }
      for (const structures::WalkBox& b : walk.boxes) {
        if (!ok) break;
        if (static_cast<f64>(b.max.y) < level + 0.1 || static_cast<f64>(b.min.y) > level + 1.8)
          continue;
        if (px > static_cast<f64>(b.min.x) - k_clear_m &&
            px < static_cast<f64>(b.max.x) + k_clear_m &&
            pz > static_cast<f64>(b.min.z) - k_clear_m &&
            pz < static_cast<f64>(b.max.z) + k_clear_m)
          ok = false;
      }
      // A leaf standing open is a slab in the room beside its doorway: the capsule's radius, its
      // padding and a centimetre from its faces (less than a wall's margin, because the way
      // through a doorway its own leaf has narrowed passes that near it).
      for (const structures::WalkLeaf& leaf : walk.leaves) {
        if (!ok) break;
        if (static_cast<f64>(leaf.y1) < level + 0.1 || static_cast<f64>(leaf.y0) > level + 1.8)
          continue;
        if (segment_distance(px, pz, static_cast<f64>(leaf.hinge.x), static_cast<f64>(leaf.hinge.y),
                             static_cast<f64>(leaf.tip.x), static_cast<f64>(leaf.tip.y)) <
            static_cast<f64>(leaf.thickness) * 0.5 + k_leaf_clear_m)
          ok = false;
      }
      for (const structures::WalkWell& w : walk.wells) {
        if (!ok) break;
        if (w.storey != storey) continue;
        if (px > static_cast<f64>(w.x0) - 0.1 && px < static_cast<f64>(w.x1) + 0.1 &&
            pz > static_cast<f64>(w.z0) - 0.1 && pz < static_cast<f64>(w.z1) + 0.1)
          ok = false;
      }
      for (const structures::WalkFlight& f : walk.flights) {
        if (!ok) break;
        if (f.stair >= d.stairs.size() || d.stairs[f.stair].storey != storey) continue;
        // The steps themselves, from the first riser on: the floor before the foot stays open (an
        // enclosed stair's landing is a metre deep, and a margin behind the foot would close it).
        const f64 run = static_cast<f64>(f.risers) * static_cast<f64>(f.going);
        const f64 rx = px - static_cast<f64>(f.foot.x), rz = pz - static_cast<f64>(f.foot.z);
        const f64 along = rx * static_cast<f64>(f.dir.x) + rz * static_cast<f64>(f.dir.y);
        const f64 across = rx * static_cast<f64>(f.dir.y) - rz * static_cast<f64>(f.dir.x);
        if (along > -0.05 && along < run + k_clear_m &&
            std::fabs(across) < static_cast<f64>(f.width) * 0.5 + k_clear_m)
          ok = false;
      }
      g.open[static_cast<u32>(j * g.nx + i)] = ok ? 1 : 0;
    }
  return true;
}

// The open cell nearest a point, within 0.8 m; false when there is none.
bool snap(const Grid& g, f64 px, f64 pz, i32& ci, i32& cj) noexcept {
  const i32 i0 = static_cast<i32>(std::floor((px - g.x0) / k_cell_m + 0.5));
  const i32 j0 = static_cast<i32>(std::floor((pz - g.z0) / k_cell_m + 0.5));
  f64 best = 1e30;
  bool found = false;
  for (i32 dj = -8; dj <= 8; ++dj)
    for (i32 di = -8; di <= 8; ++di) {
      if (!g.at(i0 + di, j0 + dj)) continue;
      const f64 ex = g.x(i0 + di) - px, ez = g.z(j0 + dj) - pz;
      const f64 e = ex * ex + ez * ez;
      if (e < best) {
        best = e;
        ci = i0 + di;
        cj = j0 + dj;
        found = true;
      }
    }
  return found;
}

// Whether the straight stretch between two open cells crosses open cells only.
bool clear_line(const Grid& g, i32 ai, i32 aj, i32 bi, i32 bj) noexcept {
  const i32 steps = 2 * std::max(std::abs(bi - ai), std::abs(bj - aj));
  for (i32 s = 1; s < steps; ++s) {
    const f64 t = static_cast<f64>(s) / static_cast<f64>(steps);
    const f64 fi = static_cast<f64>(ai) + t * static_cast<f64>(bi - ai);
    const f64 fj = static_cast<f64>(aj) + t * static_cast<f64>(bj - aj);
    if (!g.at(static_cast<i32>(std::floor(fi + 0.5)), static_cast<i32>(std::floor(fj + 0.5))))
      return false;
  }
  return true;
}

// The way from (ax, az) to (bx, bz) over the storey's open cells: a breadth-first search (so the
// fewest cells; ties in a fixed neighbour order, so the same way every time), then pulled straight
// wherever a straight stretch is clear. Appends the points after the start, the end included, at
// height `y_mm`. False when the two are not joined.
bool way(const Grid& g, f64 ax, f64 az, f64 bx, f64 bz, i64 y_mm, Vector<RoutePoint>& out) {
  i32 si = 0, sj = 0, ti = 0, tj = 0;
  if (!snap(g, ax, az, si, sj) || !snap(g, bx, bz, ti, tj)) return false;
  const u32 cells = static_cast<u32>(g.nx * g.nz);
  Vector<i32> from;
  from.resize(cells);
  for (u32 k = 0; k < cells; ++k)
    from[k] = -1;
  Vector<i32> queue;
  const i32 start = sj * g.nx + si, goal = tj * g.nx + ti;
  from[static_cast<u32>(start)] = start;
  queue.push_back(start);
  const i32 di[8] = {1, -1, 0, 0, 1, 1, -1, -1};
  const i32 dj[8] = {0, 0, 1, -1, 1, -1, 1, -1};
  for (u32 head = 0; head < queue.size() && from[static_cast<u32>(goal)] < 0; ++head) {
    const i32 c = queue[head];
    const i32 ci = c % g.nx, cj = c / g.nx;
    for (u32 n = 0; n < 8; ++n) {
      const i32 ni = ci + di[n], nj = cj + dj[n];
      if (!g.at(ni, nj)) continue;
      // A diagonal only between two open sides: no corner is cut.
      if (n >= 4 && (!g.at(ci + di[n], cj) || !g.at(ci, cj + dj[n]))) continue;
      const i32 next = nj * g.nx + ni;
      if (from[static_cast<u32>(next)] >= 0) continue;
      from[static_cast<u32>(next)] = c;
      queue.push_back(next);
    }
  }
  if (from[static_cast<u32>(goal)] < 0) return false;
  Vector<i32> chain;  // goal back to start
  for (i32 c = goal; c != start; c = from[static_cast<u32>(c)])
    chain.push_back(c);
  chain.push_back(start);
  std::reverse(chain.begin(), chain.end());
  u32 k = 0;
  while (k + 1 < chain.size()) {
    u32 reach = k + 1;
    for (u32 q = chain.size() - 1; q > k + 1; --q)
      if (clear_line(g, chain[k] % g.nx, chain[k] / g.nx, chain[q] % g.nx, chain[q] / g.nx)) {
        reach = q;
        break;
      }
    k = reach;
    // The last point is the one asked for, not its cell's middle, when the cell holds it.
    const bool last = k + 1 == chain.size();
    const f64 px = last ? bx : g.x(chain[k] % g.nx), pz = last ? bz : g.z(chain[k] / g.nx);
    out.push_back(RoutePoint{scene_gen::nearest_mm(px), y_mm, scene_gen::nearest_mm(pz)});
  }
  return true;
}

// ---- the rooms ----------------------------------------------------------------------------------

// An opening's middle on its wall's centre line and the unit normal toward `room` (the wall's
// left holds `room_a`).
void opening_frame(const WalkOpening& o, i32 toward_room, f64& cx, f64& cz, f64& nx, f64& nz) {
  cx = (static_cast<f64>(o.a.x) + static_cast<f64>(o.b.x)) * 0.5;
  cz = (static_cast<f64>(o.a.y) + static_cast<f64>(o.b.y)) * 0.5;
  const f64 dx = static_cast<f64>(o.b.x) - static_cast<f64>(o.a.x);
  const f64 dz = static_cast<f64>(o.b.y) - static_cast<f64>(o.a.y);
  const f64 len = std::sqrt(dx * dx + dz * dz);
  // The left of (dx, dz), seen from above with y up, is (dz, -dx).
  nx = len > 0.0 ? dz / len : 0.0;
  nz = len > 0.0 ? -dx / len : 0.0;
  if (o.room_a != toward_room) {
    nx = -nx;
    nz = -nz;
  }
}

// The openings a walker passes (doors and doorways) from `from_room` to `to_room`, fewest first;
// false when the rooms are not joined on their storey.
bool room_chain(const Walk& walk, i32 from_room, i32 to_room, Vector<u32>& out) {
  out.clear();
  if (from_room == to_room) return true;
  // Breadth-first over rooms; `via[k]` is the opening room `seen[k]` was entered by.
  Vector<i32> seen;
  Vector<i32> parent;
  Vector<u32> via;
  seen.push_back(from_room);
  parent.push_back(-1);
  via.push_back(0);
  for (u32 head = 0; head < seen.size(); ++head) {
    const i32 room = seen[head];
    if (room == to_room) {
      for (i32 k = static_cast<i32>(head); parent[static_cast<u32>(k)] >= 0;
           k = parent[static_cast<u32>(k)])
        out.push_back(via[static_cast<u32>(k)]);
      std::reverse(out.begin(), out.end());
      return true;
    }
    for (u32 k = 0; k < walk.openings.size(); ++k) {
      const WalkOpening& o = walk.openings[k];
      if (o.kind == structure::OpeningKind::window) continue;
      if (o.room_a != room && o.room_b != room) continue;
      const i32 other = o.room_a == room ? o.room_b : o.room_a;
      if (other < 0) continue;
      bool known = false;
      for (const i32 s : seen)
        known = known || s == other;
      if (known) continue;
      seen.push_back(other);
      parent.push_back(static_cast<i32>(head));
      via.push_back(k);
    }
  }
  return false;
}

// **What an opening's leaves take of it** (structures.md, "Leaves"; `Walk::leaves`). A leaf open a
// quarter turn lies beside its hinge jamb, its thickness inside the frame: the clear way is that
// much narrower and its middle has moved half of it along the wall, away from the hinge. A walker
// sent through the opening's own middle meets the leaf's edge where the way is tight (a 73 cm
// frame and a 5.5 cm leaf leave 31 cm on the hinge side of the middle, and the capsule with its
// padding is 32), which is what stopped the first walk through a house with doors. A leaf less
// than a quarter turn open stands across the way.
struct LeafToll {
  f64 taken = 0.0;                   // of the clear width, metres
  f64 shift_x = 0.0, shift_z = 0.0;  // of the clear way's middle from the opening's
  bool across = false;
};
LeafToll leaf_toll(const Walk& walk, const WalkOpening& o) {
  LeafToll toll;
  const f64 mx = (static_cast<f64>(o.a.x) + static_cast<f64>(o.b.x)) * 0.5;
  const f64 mz = (static_cast<f64>(o.a.y) + static_cast<f64>(o.b.y)) * 0.5;
  const f64 dx = static_cast<f64>(o.b.x) - static_cast<f64>(o.a.x);
  const f64 dz = static_cast<f64>(o.b.y) - static_cast<f64>(o.a.y);
  const f64 len = std::sqrt(dx * dx + dz * dz);
  if (!(len > 0.0)) return toll;
  for (const structures::WalkLeaf& leaf : walk.leaves) {
    if (leaf.opening != o.opening) continue;
    if (leaf.open_deg < 90) {
      toll.across = true;
      continue;
    }
    const f64 t = static_cast<f64>(leaf.thickness);
    const f64 along =
        ((static_cast<f64>(leaf.hinge.x) - mx) * dx + (static_cast<f64>(leaf.hinge.y) - mz) * dz) /
        len;
    const f64 away = along >= 0.0 ? -1.0 : 1.0;
    toll.taken += t;
    toll.shift_x += away * 0.5 * t * dx / len;
    toll.shift_z += away * 0.5 * t * dz / len;
  }
  return toll;
}

bool passable(const Walk& walk, const WalkOpening& o, const char* what, std::string& why) {
  const f64 ax = static_cast<f64>(o.a.x), az = static_cast<f64>(o.a.y);
  const f64 bx = static_cast<f64>(o.b.x), bz = static_cast<f64>(o.b.y);
  const LeafToll toll = leaf_toll(walk, o);
  if (toll.across) {
    why = std::string(what) + " (opening " + std::to_string(o.opening) +
          ") has a leaf standing less than a quarter turn open, across the walker's way";
    return false;
  }
  const f64 clear =
      std::sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az)) - 2.0 * k_frame_m - toll.taken;
  // Clear of the frame's head (6 cm, as its jambs) and the threshold (2 cm): the first walk of
  // this row stood at the example's 1.90 m door, whose 1.82 m clear the 1.8 m capsule and its
  // 2 cm of padding do not enter.
  const f64 height = static_cast<f64>(o.head) - static_cast<f64>(o.sill) - k_frame_m - 0.02;
  char text[200];
  if (clear < k_min_clear_m) {
    std::snprintf(text, sizeof(text), "%s (opening %u) is %.2f m clear, under the walker's %.2f m",
                  what, o.opening, clear, k_min_clear_m);
    why = text;
    return false;
  }
  if (height < k_min_head_m) {
    std::snprintf(text, sizeof(text), "%s (opening %u) stands %.2f m high: the walker needs %.2f m",
                  what, o.opening, height, k_min_head_m);
    why = text;
    return false;
  }
  return true;
}

// The route (village.md, "The way through a house"): in at `entrance`, to the stair if the house
// has one its rooms reach, up it, and to a window of the storey it ends on.
bool route_through(const structure::Description& d, const Walk& walk, const WalkOpening& entrance,
                   House& out, std::string& why) {
  const i32 hall = entrance.room_a >= 0 ? entrance.room_a : entrance.room_b;
  u32 storey = 0;
  i64 y_mm = static_cast<i64>(d.storeys[0].elevation) * 10;
  Grid grid;
  if (!storey_grid(d, walk, storey, grid)) {
    why = "the ground storey has no floor to walk";
    return false;
  }
  f64 cx = 0, cz = 0, nx = 0, nz = 0;
  opening_frame(entrance, hall, cx, cz, nx, nz);
  // In through the middle of what the door's own leaf leaves clear (the plan's door is there too).
  const LeafToll entrance_toll = leaf_toll(walk, entrance);
  cx += entrance_toll.shift_x;
  cz += entrance_toll.shift_z;
  f64 at_x = cx + nx * (static_cast<f64>(entrance.thickness) * 0.5 + k_through_m);
  f64 at_z = cz + nz * (static_cast<f64>(entrance.thickness) * 0.5 + k_through_m);
  out.route.push_back(RoutePoint{scene_gen::nearest_mm(at_x), y_mm, scene_gen::nearest_mm(at_z)});
  i32 room = hall;
  // Through the doorways from the room the walker is in to `to_room`, each crossed straight.
  const auto cross_to = [&](i32 to_room) {
    Vector<u32> chain;
    if (!room_chain(walk, room, to_room, chain)) {
      why = "room " + std::to_string(to_room) + " is not reached from room " + std::to_string(room);
      return false;
    }
    for (const u32 k : chain) {
      const WalkOpening& o = walk.openings[k];
      if (!passable(walk, o, "a doorway on the way", why)) return false;
      const i32 next = o.room_a == room ? o.room_b : o.room_a;
      opening_frame(o, next, cx, cz, nx, nz);
      // Through the middle of what its leaves leave clear, not of the opening.
      const LeafToll toll = leaf_toll(walk, o);
      cx += toll.shift_x;
      cz += toll.shift_z;
      const f64 reach = static_cast<f64>(o.thickness) * 0.5 + k_through_m;
      const f64 bx = cx - nx * reach, bz = cz - nz * reach;
      if (!way(grid, at_x, at_z, bx, bz, y_mm, out.route)) {
        why = "no way across room " + std::to_string(room) + " to its doorway (opening " +
              std::to_string(o.opening) + ")";
        return false;
      }
      at_x = cx + nx * reach;
      at_z = cz + nz * reach;
      out.route.push_back(
          RoutePoint{scene_gen::nearest_mm(at_x), y_mm, scene_gen::nearest_mm(at_z)});
      room = next;
    }
    return true;
  };
  // The first stair that rises from this storey out of a room the hall reaches.
  for (u32 guard = 0; guard < structures::k_max_storeys; ++guard) {
    const structures::WalkFlight* flight = nullptr;
    for (const structures::WalkFlight& f : walk.flights) {
      if (f.stair >= d.stairs.size() || d.stairs[f.stair].storey != storey) continue;
      Vector<u32> chain;
      if (flight == nullptr && room_chain(walk, room, d.stairs[f.stair].from_room, chain))
        flight = &f;
    }
    if (flight == nullptr) break;
    const structure::Stair& stair = d.stairs[flight->stair];
    char text[200];
    if (static_cast<f64>(flight->rise) > k_max_rise_m ||
        static_cast<f64>(flight->going) < k_min_going_m) {
      std::snprintf(text, sizeof(text),
                    "stair %u rises %.3f m a step on a going of %.3f m: the walker's are at most "
                    "%.2f and at least %.2f",
                    flight->stair, static_cast<f64>(flight->rise), static_cast<f64>(flight->going),
                    k_max_rise_m, k_min_going_m);
      why = text;
      return false;
    }
    if (!cross_to(stair.from_room)) return false;
    const f64 fx = static_cast<f64>(flight->foot.x), fz = static_cast<f64>(flight->foot.z);
    const f64 dx = static_cast<f64>(flight->dir.x), dz = static_cast<f64>(flight->dir.y);
    // To a step in front of the first riser, then straight up the flight to a step past its top.
    if (!way(grid, at_x, at_z, fx - dx * k_through_m, fz - dz * k_through_m, y_mm, out.route)) {
      why = "no way across room " + std::to_string(room) + " to the foot of stair " +
            std::to_string(flight->stair);
      return false;
    }
    const f64 run = static_cast<f64>(flight->risers > 0 ? flight->risers - 1 : 0) *
                        static_cast<f64>(flight->going) +
                    k_through_m;
    at_x = fx + dx * run;
    at_z = fz + dz * run;
    storey = stair.storey + 1;
    if (storey >= d.storeys.size()) break;
    y_mm = scene_gen::nearest_mm(static_cast<f64>(flight->top));
    out.route.push_back(RoutePoint{scene_gen::nearest_mm(at_x), y_mm, scene_gen::nearest_mm(at_z)});
    room = stair.to_room;
    if (!storey_grid(d, walk, storey, grid)) {
      why = "storey " + std::to_string(storey) + " has no floor to walk";
      return false;
    }
  }
  // A window of this storey in a room the walker reaches: the first in the description's order.
  for (const WalkOpening& o : walk.openings) {
    if (o.kind != structure::OpeningKind::window) continue;
    if (o.wall >= d.walls.size() || d.walls[o.wall].storey != storey) continue;
    if ((o.room_a >= 0) == (o.room_b >= 0)) continue;  // an outer wall's
    const i32 within = o.room_a >= 0 ? o.room_a : o.room_b;
    Vector<u32> chain;
    if (!room_chain(walk, room, within, chain)) continue;
    if (!cross_to(within)) return false;
    opening_frame(o, within, cx, cz, nx, nz);
    const f64 reach = static_cast<f64>(o.thickness) * 0.5 + k_through_m;
    if (!way(grid, at_x, at_z, cx + nx * reach, cz + nz * reach, y_mm, out.route)) continue;
    // What a person there looks at: twenty-five metres out, a little under the eye's height.
    out.look = RoutePoint{scene_gen::nearest_mm(cx - nx * 25.0), y_mm + 1400,
                          scene_gen::nearest_mm(cz - nz * 25.0)};
    return true;
  }
  // No window: the way ends where it is, looking back out of the door.
  opening_frame(entrance, hall, cx, cz, nx, nz);
  out.look = RoutePoint{scene_gen::nearest_mm(cx - nx * 25.0), y_mm + 1500,
                        scene_gen::nearest_mm(cz - nz * 25.0)};
  return true;
}

// ---- the description a plot gets
// ------------------------------------------------------------------

// **What the filler is asked for a plot** (dwellings.md, "The rural filler"): the rectangle the
// plot leaves a house — the frontage less half a metre each side, the depth behind the setback —
// and a household drawn from the plot's seed: three to eight persons, and its means **drawn per
// plot** (G208; docs/experiments/houses-that-differ.md, "Means"): where the entry allows two
// storeys, a quarter of the households have no means for a second hearth or a floor over (0),
// three eighths one hearth (1) and three eighths two (2), and the storeys follow from the
// household as the filler asks them; where it allows one, 0 or 1 as before. A plot the household's
// house does not fit gets the filler's smaller type, which is the filler's rule, and then **the
// household is the one that house sleeps** (`fitted_house`). One function, so the plan and
// `describe_house` cannot draw two households.
struct Draw {
  dwellings::RuralPlot plot;
  dwellings::Household household;
  u8 drawn_persons = 0;  // before the household was fitted to its house
  u64 seed = 0;
};
// The name carries no plot: two plots given one household and one frontage are one description,
// so they share one mesh, when the filler's own seed is the household's and not the plot's.
u64 filler_seed(const Draw& w) noexcept {
  return hash_combine(hash_combine(w.household.persons, w.household.means),
                      static_cast<u64>(w.plot.frontage_cm));
}
Draw house_draw(const HouseAsk& ask) noexcept {
  Draw w;
  w.plot.frontage_cm = static_cast<i32>((ask.frontage_mm - 1000) / 10);
  w.plot.depth_cm = static_cast<i32>((ask.depth_mm - ask.setback_mm) / 10);
  // The storeys asked are the most the house may have (a cottage's one), never a count it must.
  w.plot.storeys_max = ask.storeys >= 2 ? u8{2} : u8{1};
  w.household.persons = static_cast<u8>(3 + mix64(ask.seed) % 6);
  w.drawn_persons = w.household.persons;
  // Sixteenths, so a one-storey ask (r % 2) keeps the draw it had.
  const u64 r = mix64(ask.seed ^ 0x9E3779B97F4A7C15ull) % 16;
  w.household.means = ask.storeys >= 2 ? (r < 4 ? u8{0} : (r < 10 ? u8{1} : u8{2}))
                                       : static_cast<u8>(r % 2);
  w.seed = filler_seed(w);
  return w;
}

// **The house filler**: the rural filler's choice and plan for the plot's ask, and the description
// it becomes (what `dwellings::fill_rural` does, with the choice and the plan kept).
dwellings::Kit kit_of(const detail::HouseKit* hk) noexcept {
  if (hk == nullptr || hk->rows.empty()) return {};
  return dwellings::Kit{hk->rows.data(), static_cast<u32>(hk->rows.size())};
}

bool house_plan(const Draw& w, const dwellings::Kit& kit, dwellings::RuralChoice& choice,
                dwellings::InteriorPlan& plan, structure::Description& d, std::string& why) {
  const dwellings::RuralStyle style;
  if (!dwellings::plan_rural(w.plot, w.household, style, w.seed, choice, plan, &why)) return false;
  dwellings::describe(plan, choice, "house", w.seed, d, kit);
  return true;
}

// **The household fitted to its house** (need 134: an eight-person household drawn into a
// one-room cot): when the plot holds only a smaller house than the household asked, the household
// is the one that house sleeps (`dwellings::persons_housed`), drawn again with it, so its seed and
// its house are the fitted household's. The fitted household asks the house it gets or a larger
// one the plot still does not hold, so one more plan settles it.
bool fitted_house(const HouseAsk& ask, const dwellings::Kit& kit, Draw& w,
                  dwellings::RuralChoice& choice, dwellings::InteriorPlan& plan,
                  structure::Description& d, std::string& why) {
  w = house_draw(ask);
  if (!house_plan(w, kit, choice, plan, d, why)) return false;
  if (!choice.demoted) return true;
  const u8 room = dwellings::persons_housed(choice.interior);
  if (w.household.persons <= room) return true;
  w.household.persons = room;
  w.seed = filler_seed(w);
  return house_plan(w, kit, choice, plan, d, why);
}

}  // namespace

namespace detail {
i64 least_house_frontage_mm() noexcept {
  const dwellings::RuralStyle style;
  // The filler's rectangle is the frontage less half a metre each side (`house_draw`).
  return i64{dwellings::least_rural_frontage_cm(style)} * 10 + 1000;
}
}  // namespace detail

namespace {

// The furnishing `describe` wrote (the same call, so the same pieces and kit pieces).
void house_furnishing(const Draw& w, const dwellings::Kit& kit,
                      const dwellings::RuralChoice& choice, const dwellings::InteriorPlan& plan,
                      dwellings::Furnishing& f) {
  if (choice.timber) dwellings::timber_interior(plan, choice.ridge_along_front, f);
  if (choice.furniture) dwellings::furnish_interior(plan, w.seed, f, kit);
}

// **Where a kit piece's mesh stands** (village.md, "A kit"): its front turned by quarters from the
// way its mesh faces to the way into the room from the wall its piece backs to, its foot's middle
// on the piece's footprint's middle, its lowest point on its storey's floor. House frame, mm.
bool kit_place(const VillageLayer& layer, const dwellings::Piece& piece, u32 index, i64 off_mm,
               i64 floor_mm, KitPlace& out) {
  const u32 row = static_cast<u32>(piece.kit) - 1;
  if (row >= layer.kit.size()) return false;
  const KitPiece& k = layer.kit[row];
  i64 fx = 0, fz = 0;  // the mesh's front, a unit axis
  switch (k.front) {
    case KitFront::PosX: fx = 1; break;
    case KitFront::NegX: fx = -1; break;
    case KitFront::PosZ: fz = 1; break;
    default: fz = -1; break;
  }
  i64 tx = 0, tz = 0;  // into the room from the wall behind the piece
  switch (piece.against) {
    case dwellings::Side::Front: tz = 1; break;
    case dwellings::Side::Back: tz = -1; break;
    case dwellings::Side::Left: tx = 1; break;
    default: tx = -1; break;
  }
  const f64 sc = static_cast<f64>(k.scale);
  const bool no_foot =
      k.foot[0] == 0.0f && k.foot[1] == 0.0f && k.foot[2] == 0.0f && k.foot[3] == 0.0f;
  f64 cx = 0.5 *
           (static_cast<f64>(no_foot ? k.bounds_min[0] : k.foot[0]) +
            static_cast<f64>(no_foot ? k.bounds_max[0] : k.foot[2])) *
           sc;
  f64 cz = 0.5 *
           (static_cast<f64>(no_foot ? k.bounds_min[2] : k.foot[1]) +
            static_cast<f64>(no_foot ? k.bounds_max[2] : k.foot[3])) *
           sc;
  u8 turns = 0;
  while (!(fx == tx && fz == tz) && turns < 4) {
    // A quarter turn about y by the engine's rotation (x' = z, z' = -x for a quarter).
    const i64 nx = fz, nz = -fx;
    fx = nx;
    fz = nz;
    const f64 ncx = cz, ncz = -cx;
    cx = ncx;
    cz = ncz;
    ++turns;
  }
  if (turns == 4) return false;
  const dwellings::RectCm& r = piece.footprint;
  const f64 mid_x = static_cast<f64>(static_cast<i64>(r.x0 + r.x1) * 5 + off_mm) / 1000.0;
  const f64 mid_z = static_cast<f64>(static_cast<i64>(r.z0 + r.z1) * 5 + off_mm) / 1000.0;
  out.row = row;
  out.piece = index;
  out.x_mm = scene_gen::nearest_mm(mid_x - cx);
  out.z_mm = scene_gen::nearest_mm(mid_z - cz);
  out.y_mm = floor_mm - scene_gen::nearest_mm(static_cast<f64>(k.bounds_min[1]) * sc);
  out.quarter_turns = turns;
  return true;
}

bool house_for(void* context, const HouseAsk& ask, House& out, std::string& why) {
  out = {};
  const auto* hk = static_cast<const detail::HouseKit*>(context);
  const dwellings::Kit kit = kit_of(hk);
  structure::Description d;
  Draw drawn;
  dwellings::RuralChoice choice;
  dwellings::InteriorPlan interior;
  if (!fitted_house(ask, kit, drawn, choice, interior, d, why)) return false;
  if (!structures::validate(d, why)) {
    why = "rural filler: the mesher refuses the description: " + why;
    return false;
  }
  out.persons = drawn.household.persons;
  out.means = drawn.household.means;
  out.storeys = interior.params.storeys;
  out.filler_seed = drawn.seed;
  out.type = dwellings::plan_type_name(interior.params.type);
  Walk walk;
  if (!structures::walk_records(d, walk, why)) return false;
  // **The kit pieces** (village.md, "A kit"): where each mesh stands, and its foot as a box the
  // way through the house keeps off as it keeps off the furniture's solid members, since a kit
  // piece states no box of the description.
  if (kit.count > 0 && hk != nullptr && hk->layer != nullptr) {
    dwellings::Furnishing f;
    house_furnishing(drawn, kit, choice, interior, f);
    // Read back by the furnisher's own validator, which repairs nothing.
    if (!dwellings::validate_furnishing(interior, f, &why)) {
      why = "the kit's furnishing: " + why;
      return false;
    }
    const i64 off_mm = static_cast<i64>(dwellings::description_offset_cm(choice)) * 10;
    for (u32 k = 0; k < f.pieces.size(); ++k) {
      const dwellings::Piece& piece = f.pieces[k];
      if (piece.kit == 0) continue;
      const i64 floor_mm = piece.storey < d.storeys.size()
                               ? static_cast<i64>(d.storeys[piece.storey].elevation) * 10
                               : 0;
      KitPlace place;
      if (!kit_place(*hk->layer, piece, k, off_mm, floor_mm, place)) {
        why = "kit row " + std::to_string(piece.kit - 1) + " faces no way the engine turns a piece";
        return false;
      }
      out.kit.push_back(place);
      structures::WalkBox b;
      b.box = ~0u;
      const dwellings::RectCm& r = piece.footprint;
      b.min = Vec3{static_cast<f32>(static_cast<f64>(i64{r.x0} * 10 + off_mm) / 1000.0),
                   static_cast<f32>(static_cast<f64>(floor_mm) / 1000.0),
                   static_cast<f32>(static_cast<f64>(i64{r.z0} * 10 + off_mm) / 1000.0)};
      b.max =
          Vec3{static_cast<f32>(static_cast<f64>(i64{r.x1} * 10 + off_mm) / 1000.0),
               static_cast<f32>(static_cast<f64>(floor_mm + i64{piece.height_cm} * 10) / 1000.0),
               static_cast<f32>(static_cast<f64>(i64{r.z1} * 10 + off_mm) / 1000.0)};
      walk.boxes.push_back(b);
    }
  }
  // The box: the outer faces of the ground storey's walls.
  f64 lo_x = 1e30, lo_z = 1e30, hi_x = -1e30, hi_z = -1e30;
  for (const structures::WalkWall& w : walk.walls) {
    if (w.wall >= d.walls.size() || d.walls[w.wall].storey != 0 ||
        d.walls[w.wall].kind != structure::WallKind::exterior)
      continue;
    const f64 half = static_cast<f64>(w.thickness) * 0.5;
    lo_x = std::min(lo_x, std::min(static_cast<f64>(w.from.x), static_cast<f64>(w.to.x)) - half);
    hi_x = std::max(hi_x, std::max(static_cast<f64>(w.from.x), static_cast<f64>(w.to.x)) + half);
    lo_z = std::min(lo_z, std::min(static_cast<f64>(w.from.y), static_cast<f64>(w.to.y)) - half);
    hi_z = std::max(hi_z, std::max(static_cast<f64>(w.from.y), static_cast<f64>(w.to.y)) + half);
  }
  if (!(hi_x > lo_x) || !(hi_z > lo_z)) {
    why = "the house has no outer walls on its ground storey";
    return false;
  }
  out.min_x_mm = scene_gen::nearest_mm(lo_x);
  out.max_x_mm = scene_gen::nearest_mm(hi_x);
  out.min_z_mm = scene_gen::nearest_mm(lo_z);
  out.max_z_mm = scene_gen::nearest_mm(hi_z);
  // The entrance: the first door between the outside and a ground-storey room, in a wall along an
  // axis.
  const WalkOpening* entrance = nullptr;
  for (const WalkOpening& o : walk.openings) {
    if (o.kind != structure::OpeningKind::door || (o.room_a >= 0) == (o.room_b >= 0)) continue;
    if (o.wall >= d.walls.size() || d.walls[o.wall].storey != 0) continue;
    if (entrance == nullptr) entrance = &o;
  }
  if (entrance == nullptr) {
    why = "the house has no door from outside into a ground-storey room";
    return false;
  }
  if (!passable(walk, *entrance, "the entrance", why)) return false;
  f64 cx = 0, cz = 0, nx = 0, nz = 0;
  opening_frame(*entrance, -1, cx, cz, nx, nz);  // the normal toward the outside
  // The door the plan knows is the middle of the clear way beside the open leaf, so the path, the
  // gateway and the walker all come to where a person would come in.
  const LeafToll door_toll = leaf_toll(walk, *entrance);
  cx += door_toll.shift_x;
  cz += door_toll.shift_z;
  out.out_x = nx > 0.5 ? 1 : (nx < -0.5 ? -1 : 0);
  out.out_z = nz > 0.5 ? 1 : (nz < -0.5 ? -1 : 0);
  if ((out.out_x == 0) == (out.out_z == 0)) {
    why = "the entrance's wall is not along an axis";
    return false;
  }
  // **The eaves over the entrance** (S89; plan 14 §14.4, "a roof's thatch overhang ... should not
  // be walls"): the proxy keeps the roof, so where the door is under an eaves side the roof's
  // underside at its overhang's edge must clear the walker, or it never reaches the door.
  for (const structure::Roof& roof : d.roofs) {
    const bool eaves_side = roof.ridge_along_x ? out.out_z != 0 : out.out_x != 0;
    const f64 edge = (static_cast<f64>(roof.eaves) -
                      static_cast<f64>(roof.overhang) * static_cast<f64>(roof.pitch) / 1000.0) /
                     100.0;
    if (eaves_side && edge < k_min_head_m - 0.03) {
      char text[200];
      std::snprintf(text, sizeof(text),
                    "the eaves over the entrance stand %.2f m over the floor at their edge: the "
                    "walker needs %.2f m to come in under them",
                    edge, k_min_head_m - 0.03);
      why = text;
      return false;
    }
  }
  const f64 half = static_cast<f64>(entrance->thickness) * 0.5;
  out.door_x_mm = scene_gen::nearest_mm(cx + nx * half);
  out.door_z_mm = scene_gen::nearest_mm(cz + nz * half);
  const f64 ax = static_cast<f64>(entrance->a.x), az = static_cast<f64>(entrance->a.y);
  const f64 bx = static_cast<f64>(entrance->b.x), bz = static_cast<f64>(entrance->b.y);
  out.door_width_mm =
      scene_gen::nearest_mm(std::sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az)));
  // The ground floor's top stands 3 cm over the pad, so the threshold (2 cm more) is a 5 cm lip:
  // the capsule's round foot meets its edge at 34 degrees from level and walks up it as a slope
  // (the walker's limit is 40), with no step-up under the lintel; and the foundation's depth keeps
  // the walls' feet in the ground.
  out.floor_mm = std::min<i64>(30, static_cast<i64>(d.foundation) * 5);
  if (!route_through(d, walk, *entrance, out, why)) return false;

  structure::Recipe recipe;
  recipe.description = d;
  recipe.scope = structure::MeshScope::whole;
  out.recipe = structures::canonical_json(recipe);
  char file[64];
  std::snprintf(file, sizeof(file), "village/%s-%016llx.structure.json",
                d.name.empty() ? "house" : d.name.c_str(),
                static_cast<unsigned long long>(structures::source_hash(recipe)));
  out.path = file;
  out.name = d.name.empty() ? std::string("house") : d.name;
  // The walls' furthest corner from the frame's origin, and a metre for the eaves.
  const f64 fx = std::max(std::fabs(lo_x), std::fabs(hi_x)),
            fz = std::max(std::fabs(lo_z), std::fabs(hi_z));
  out.reach = static_cast<f32>(std::sqrt(fx * fx + fz * fz) + 1.0);
  return true;
}

}  // namespace

namespace detail {

namespace {
// Up from a positive length: a kit piece's foot is never rounded smaller than its mesh.
i32 ceil_cm(f64 cm) noexcept {
  if (!(cm > 0.0)) return 0;
  if (cm > 100000.0) return 100000;
  const i32 whole = static_cast<i32>(cm);
  return static_cast<f64>(whole) < cm ? whole + 1 : whole;
}
}  // namespace

void house_kit(const VillageLayer& layer, HouseKit& out) {
  out.layer = &layer;
  out.rows.clear();
  for (u32 r = 0; r < layer.kit.size(); ++r) {
    const KitPiece& k = layer.kit[r];
    dwellings::KitRow row;
    row.id = r;
    switch (k.replaces) {
      case KitReplaces::TableSet: row.replaces = dwellings::PieceKind::TableSet; break;
      case KitReplaces::Settle: row.replaces = dwellings::PieceKind::Settle; break;
      case KitReplaces::Bed: row.replaces = dwellings::PieceKind::Bed; break;
      case KitReplaces::Chest: row.replaces = dwellings::PieceKind::Chest; break;
      default: continue;
    }
    const bool no_foot =
        k.foot[0] == 0.0f && k.foot[1] == 0.0f && k.foot[2] == 0.0f && k.foot[3] == 0.0f;
    const f64 sc = static_cast<f64>(k.scale) * 100.0;
    const f64 ex = (static_cast<f64>(no_foot ? k.bounds_max[0] : k.foot[2]) -
                    static_cast<f64>(no_foot ? k.bounds_min[0] : k.foot[0])) *
                   sc;
    const f64 ez = (static_cast<f64>(no_foot ? k.bounds_max[2] : k.foot[3]) -
                    static_cast<f64>(no_foot ? k.bounds_min[2] : k.foot[1])) *
                   sc;
    const bool front_x = k.front == KitFront::PosX || k.front == KitFront::NegX;
    row.along_cm = ceil_cm(front_x ? ez : ex);
    row.out_cm = ceil_cm(front_x ? ex : ez);
    row.height_cm =
        ceil_cm((static_cast<f64>(k.bounds_max[1]) - static_cast<f64>(k.bounds_min[1])) * sc);
    out.rows.push_back(row);
  }
}

HouseFn house_filler(const HouseKit& kit, void*& context) noexcept {
  context = const_cast<HouseKit*>(&kit);
  return &house_for;
}

}  // namespace detail

bool plan_scene_village(const VillageLayer& layer, const scene_gen::Context& context,
                        VillagePlan& out, std::string& error) {
  detail::HouseKit kit;
  detail::house_kit(layer, kit);
  void* house_context = nullptr;
  const HouseFn house = detail::house_filler(kit, house_context);
  return plan_village(layer, context.uncut(), context, house, house_context, out, error);
}

scene_gen::Placement kit_placement(const VillageLayer& layer, const Plot& plot, const KitPlace& k) {
  scene_gen::Placement out = house_placement(plot);
  out.position = house_point(plot, k.x_mm, k.y_mm, k.z_mm);
  // A quarter turn about +y, by halves of the angle: 0, 90, 180 and 270 degrees.
  constexpr f32 h = 0.70710678118654752f;
  const Quat turn[4] = {Quat{0.0f, 0.0f, 0.0f, 1.0f}, Quat{0.0f, h, 0.0f, h},
                        Quat{0.0f, 1.0f, 0.0f, 0.0f}, Quat{0.0f, h, 0.0f, -h}};
  out.rotation = out.rotation * turn[k.quarter_turns & 3u];
  const f32 s = k.row < layer.kit.size() ? layer.kit[k.row].scale : 1.0f;
  out.scale = Vec3{s, s, s};
  return out;
}

namespace {

// Whole millimetres as metres, and three of them as a JSON point.
JsonValue point_mm(i64 x_mm, i64 y_mm, i64 z_mm) {
  JsonValue a = JsonValue::array();
  a.push_back(JsonValue(static_cast<f64>(x_mm) / 1000.0));
  a.push_back(JsonValue(static_cast<f64>(y_mm) / 1000.0));
  a.push_back(JsonValue(static_cast<f64>(z_mm) / 1000.0));
  return a;
}
JsonValue pair_mm(i64 x_mm, i64 z_mm) {
  JsonValue a = JsonValue::array();
  a.push_back(JsonValue(static_cast<f64>(x_mm) / 1000.0));
  a.push_back(JsonValue(static_cast<f64>(z_mm) / 1000.0));
  return a;
}
const char* side_name(dwellings::Side s) noexcept {
  switch (s) {
    case dwellings::Side::Front: return "front";
    case dwellings::Side::Right: return "right";
    case dwellings::Side::Back: return "back";
    default: return "left";
  }
}
// The schema's names for a kit row's enums, as an entry's JSON spells them.
const char* kit_class_name(KitClass c) noexcept {
  switch (c) {
    case KitClass::Wall: return "Wall";
    case KitClass::Free: return "Free";
    case KitClass::Top: return "Top";
    case KitClass::Bed: return "Bed";
    case KitClass::Hung: return "Hung";
    case KitClass::Joist: return "Joist";
    case KitClass::HearthIn: return "HearthIn";
    case KitClass::HearthBy: return "HearthBy";
    default: return "Yard";
  }
}
const char* kit_front_name(KitFront f) noexcept {
  switch (f) {
    case KitFront::PosX: return "PosX";
    case KitFront::NegX: return "NegX";
    case KitFront::PosZ: return "PosZ";
    default: return "NegZ";
  }
}
const char* kit_replaces_name(KitReplaces r) noexcept {
  switch (r) {
    case KitReplaces::TableSet: return "TableSet";
    case KitReplaces::Settle: return "Settle";
    case KitReplaces::Bed: return "Bed";
    case KitReplaces::Chest: return "Chest";
    default: return "None";
  }
}

}  // namespace

bool describe_house(const VillageLayer& layer, const VillagePlan& plan, u32 house, JsonValue& out,
                    std::string& plan_text, std::string& error) {
  (void)layer;
  out = JsonValue::object();
  plan_text.clear();
  if (house >= plan.houses.size()) {
    error = "house " + std::to_string(house) + " is not built: the plan has " +
            std::to_string(plan.houses.size()) + " houses (`--house` counts built houses)";
    return false;
  }
  const Plot* plot = nullptr;
  u32 plot_index = 0;
  for (u32 k = 0; k < plan.plots.size(); ++k)
    if (plan.plots[k].state == PlotState::built && plan.plots[k].house == house) {
      plot = &plan.plots[k];
      plot_index = k;
    }
  if (plot == nullptr) {
    error = "house " + std::to_string(house) + " has no built plot";
    return false;
  }
  const House& h = plan.houses[house];
  // **The plot's own ask, again** (village.md, "A house as data"): the same numbers the plan asked
  // its filler, so the house planned here is the house the plan holds, byte for byte, or nothing.
  HouseAsk ask;
  ask.frontage_mm = plot->frontage_mm;
  ask.depth_mm = plot->depth_mm;
  ask.setback_mm = plot->setback_mm;
  ask.seed = plot->seed;
  ask.storeys = plot->storeys;
  ask.index = plot->order;
  detail::HouseKit hk;
  detail::house_kit(layer, hk);
  const dwellings::Kit kit = kit_of(&hk);
  Draw w;
  dwellings::RuralChoice choice;
  dwellings::InteriorPlan interior;
  structure::Description d;
  std::string why;
  if (!fitted_house(ask, kit, w, choice, interior, d, why)) {
    error = "house " + std::to_string(house) + ": " + why;
    return false;
  }
  structure::Recipe recipe;
  recipe.description = d;
  recipe.scope = structure::MeshScope::whole;
  if (structures::canonical_json(recipe) != h.recipe) {
    error = "house " + std::to_string(house) +
            ": the rural filler's house for its plot's ask is not the one the plan holds (a plan "
            "made with another house filler has no description to give)";
    return false;
  }
  const i64 off_mm = static_cast<i64>(dwellings::description_offset_cm(choice)) * 10;
  const auto elevation_mm = [&](u8 storey) -> i64 {
    return storey < d.storeys.size() ? static_cast<i64>(d.storeys[storey].elevation) * 10 : 0;
  };
  // A plan point (centimetres, between the outer walls' inner faces) in the house's frame, mm.
  const auto plan_x = [&](i32 x_cm) { return static_cast<i64>(x_cm) * 10 + off_mm; };
  const auto world = [&](i64 x_mm, i64 y_mm, i64 z_mm) {
    const WorldPos p = house_point(*plot, x_mm, y_mm, z_mm);
    return point_mm(scene_gen::nearest_mm(p.x), scene_gen::nearest_mm(p.y),
                    scene_gen::nearest_mm(p.z));
  };

  out["format"] = JsonValue("engine.village.house.v1");
  out["house"] = JsonValue(house);
  out["plot"] = JsonValue(plot_index);
  out["order"] = JsonValue(plot->order);
  out["lane"] = JsonValue(plot->lane);
  out["kind"] = JsonValue(plot->kind == PlotKind::cottage ? "cottage" : "tenement");
  JsonValue a = JsonValue::object();
  a["frontage_m"] = JsonValue(static_cast<f64>(ask.frontage_mm) / 1000.0);
  a["depth_m"] = JsonValue(static_cast<f64>(ask.depth_mm) / 1000.0);
  a["setback_m"] = JsonValue(static_cast<f64>(ask.setback_mm) / 1000.0);
  a["storeys"] = JsonValue(ask.storeys);
  a["seed"] = JsonValue(ask.seed);
  out["ask"] = std::move(a);
  JsonValue hh = JsonValue::object();
  hh["persons"] = JsonValue(static_cast<u32>(w.household.persons));
  hh["means"] = JsonValue(static_cast<u32>(w.household.means));
  // The household as drawn, before it was fitted to the house its plot holds.
  hh["drawn_persons"] = JsonValue(static_cast<u32>(w.drawn_persons));
  out["household"] = std::move(hh);
  JsonValue f = JsonValue::object();
  f["seed"] = JsonValue(w.seed);
  f["type"] = JsonValue(dwellings::plan_type_name(interior.params.type));
  f["storeys"] = JsonValue(static_cast<u32>(interior.params.storeys));
  f["hearths"] = JsonValue(static_cast<u32>(interior.hearths.size()));
  f["demoted"] = JsonValue(choice.demoted);
  f["mirror"] = JsonValue(interior.params.mirror);
  f["plot_cm"] =
      pair_mm(static_cast<i64>(w.plot.frontage_cm) * 10, static_cast<i64>(w.plot.depth_cm) * 10);
  f["clear_m"] = pair_mm(static_cast<i64>(interior.params.width_cm) * 10,
                         static_cast<i64>(interior.params.depth_cm) * 10);
  f["outer_m"] = pair_mm(static_cast<i64>(choice.outer_width_cm) * 10,
                         static_cast<i64>(choice.outer_depth_cm) * 10);
  f["outer_wall_m"] = JsonValue(static_cast<f64>(choice.outer_wall_cm) / 100.0);
  f["ridge_along_front"] = JsonValue(choice.ridge_along_front);
  out["filler"] = std::move(f);

  // **The frame**: a point (x, y, z) of the house's frame, metres, is `origin + x * x_axis + y * up
  // + z * z_axis` in the world. The house's frame is its description's: the plan's frame (every
  // `plan_*` number of the dwellings capability) moved by `plan_offset_m` on x and z, y up from the
  // ground floor's top, which stands `floor_m` over the pad.
  f64 c = plot->along_x, s = -plot->along_z;
  for (u8 q = 0; q < plot->quarter_turns; ++q) {
    const f64 nc = -s, ns = c;
    c = nc;
    s = ns;
  }
  JsonValue frame = JsonValue::object();
  frame["origin"] = world(0, 0, 0);
  JsonValue xa = JsonValue::array(), za = JsonValue::array();
  xa.push_back(JsonValue(c));
  xa.push_back(JsonValue(0.0));
  xa.push_back(JsonValue(-s));
  za.push_back(JsonValue(s));
  za.push_back(JsonValue(0.0));
  za.push_back(JsonValue(c));
  frame["x_axis"] = std::move(xa);
  frame["z_axis"] = std::move(za);
  frame["quarter_turns"] = JsonValue(static_cast<u32>(plot->quarter_turns));
  frame["pad_m"] = JsonValue(static_cast<f64>(plot->pad_mm) / 1000.0);
  frame["floor_m"] = JsonValue(static_cast<f64>(plot->floor_mm) / 1000.0);
  frame["plan_offset_m"] = JsonValue(static_cast<f64>(off_mm) / 1000.0);
  JsonValue floors = JsonValue::array();
  for (u8 st = 0; st < d.storeys.size(); ++st)
    floors.push_back(JsonValue(static_cast<f64>(elevation_mm(st)) / 1000.0));
  frame["storey_floors_m"] = std::move(floors);
  out["frame"] = std::move(frame);

  // The door the plan knows: the threshold's middle on the wall's outer face, beside the open leaf.
  JsonValue door = JsonValue::object();
  door["house"] = point_mm(h.door_x_mm, 0, h.door_z_mm);
  door["world"] = world(h.door_x_mm, 0, h.door_z_mm);
  JsonValue out_dir = JsonValue::array();
  out_dir.push_back(JsonValue(static_cast<i64>(h.out_x)));
  out_dir.push_back(JsonValue(static_cast<i64>(h.out_z)));
  door["out"] = std::move(out_dir);
  door["width_m"] = JsonValue(static_cast<f64>(h.door_width_mm) / 1000.0);
  out["door"] = std::move(door);
  JsonValue box = JsonValue::object();
  box["low"] = pair_mm(h.min_x_mm, h.min_z_mm);
  box["high"] = pair_mm(h.max_x_mm, h.max_z_mm);
  out["footprint"] = std::move(box);

  JsonValue rooms = JsonValue::array();
  for (u32 r = 0; r < interior.rooms.size(); ++r) {
    const dwellings::Room& room = interior.rooms[r];
    JsonValue o = JsonValue::object();
    o["id"] = JsonValue(r);
    o["kind"] = JsonValue(dwellings::room_kind_name(room.kind));
    o["storey"] = JsonValue(static_cast<u32>(room.storey));
    o["floor_m"] = JsonValue(static_cast<f64>(elevation_mm(room.storey)) / 1000.0);
    o["low"] = pair_mm(plan_x(room.rect.x0), plan_x(room.rect.z0));
    o["high"] = pair_mm(plan_x(room.rect.x1), plan_x(room.rect.z1));
    o["habitable"] = JsonValue((room.flags & dwellings::k_room_habitable) != 0);
    o["heated"] = JsonValue((room.flags & dwellings::k_room_heated) != 0);
    o["entrance"] = JsonValue((room.flags & dwellings::k_room_entrance) != 0);
    rooms.push_back(std::move(o));
  }
  out["rooms"] = std::move(rooms);

  JsonValue openings = JsonValue::array();
  for (const dwellings::OuterOpening& o : interior.openings) {
    JsonValue q = JsonValue::object();
    q["kind"] = JsonValue(o.door() ? "door" : "window");
    q["room"] = JsonValue(o.room);
    q["storey"] = JsonValue(static_cast<u32>(o.storey()));
    q["side"] = JsonValue(side_name(o.side));
    q["at_m"] = JsonValue(static_cast<f64>(o.at_cm) / 100.0);
    q["width_m"] = JsonValue(static_cast<f64>(o.width_cm) / 100.0);
    q["sill_m"] = JsonValue(static_cast<f64>(o.sill_cm) / 100.0);
    q["head_m"] = JsonValue(static_cast<f64>(o.head_cm) / 100.0);
    openings.push_back(std::move(q));
  }
  out["openings"] = std::move(openings);

  // The timber and the furniture as `describe` wrote them: a piece's footprint and height.
  dwellings::Furnishing furnishing;
  house_furnishing(w, kit, choice, interior, furnishing);
  JsonValue pieces = JsonValue::array();
  for (u32 k = 0; k < furnishing.pieces.size(); ++k) {
    const dwellings::Piece& p = furnishing.pieces[k];
    JsonValue q = JsonValue::object();
    q["index"] = JsonValue(k);
    q["kind"] = JsonValue(dwellings::piece_kind_name(p.kind));
    q["room"] = JsonValue(p.room);
    q["storey"] = JsonValue(static_cast<u32>(p.storey));
    q["against"] = JsonValue(side_name(p.against));
    q["low"] = point_mm(plan_x(p.footprint.x0), elevation_mm(p.storey), plan_x(p.footprint.z0));
    q["high"] = point_mm(plan_x(p.footprint.x1), elevation_mm(p.storey) + i64{p.height_cm} * 10,
                         plan_x(p.footprint.z1));
    q["members"] = JsonValue(p.member_count);
    if (p.kit != 0) q["kit_row"] = JsonValue(static_cast<u32>(p.kit - 1));
    pieces.push_back(std::move(q));
  }
  out["pieces"] = std::move(pieces);

  // **The kit**: every row of the entry's, and the pieces of this house that stand in the box
  // furniture's place, where their meshes stand (`kit_placement` composes the same numbers).
  // **Every row as the entry states it** (need 153 (e)): its class, the way it faces, the heights
  // its foot may stand at, its scale, bounds and foot, and the piece it stands in for by name; and,
  // where it stands in for one, the foot and the height the furnisher searched with, centimetres.
  JsonValue kit_rows = JsonValue::array();
  const auto f32s = [](const auto& v, u32 n) {
    JsonValue out = JsonValue::array();
    for (u32 i = 0; i < n; ++i)
      out.push_back(JsonValue(static_cast<f64>(v[i])));
    return out;
  };
  for (u32 r = 0; r < layer.kit.size(); ++r) {
    const KitPiece& piece = layer.kit[r];
    JsonValue q = JsonValue::object();
    q["row"] = JsonValue(r);
    q["name"] = JsonValue(piece.name);
    q["mesh"] = JsonValue(piece.mesh);
    q["placement"] = JsonValue(kit_class_name(piece.placement));
    q["front"] = JsonValue(kit_front_name(piece.front));
    q["height_min_m"] = JsonValue(static_cast<f64>(piece.height_min_m));
    q["height_max_m"] = JsonValue(static_cast<f64>(piece.height_max_m));
    q["scale"] = JsonValue(static_cast<f64>(piece.scale));
    q["bounds_min"] = f32s(piece.bounds_min, 3);
    q["bounds_max"] = f32s(piece.bounds_max, 3);
    q["foot"] = f32s(piece.foot, 4);
    q["replaces"] = JsonValue(kit_replaces_name(piece.replaces));
    for (const dwellings::KitRow& k : hk.rows)
      if (k.id == r) {
        q["foot_cm"] =
            pair_mm(static_cast<i64>(k.along_cm) * 1000, static_cast<i64>(k.out_cm) * 1000);
        q["height_cm"] = JsonValue(static_cast<i64>(k.height_cm));
      }
    kit_rows.push_back(std::move(q));
  }
  out["kit_rows"] = std::move(kit_rows);
  JsonValue kit_json = JsonValue::array();
  for (const KitPlace& k : h.kit) {
    JsonValue q = JsonValue::object();
    q["row"] = JsonValue(k.row);
    q["piece"] = JsonValue(k.piece);
    q["house"] = point_mm(k.x_mm, k.y_mm, k.z_mm);
    q["world"] = world(k.x_mm, k.y_mm, k.z_mm);
    q["quarter_turns"] = JsonValue(static_cast<u32>(k.quarter_turns));
    const scene_gen::Placement placed = kit_placement(layer, *plot, k);
    JsonValue rot = JsonValue::array();
    rot.push_back(JsonValue(placed.rotation.x));
    rot.push_back(JsonValue(placed.rotation.y));
    rot.push_back(JsonValue(placed.rotation.z));
    rot.push_back(JsonValue(placed.rotation.w));
    q["rotation"] = std::move(rot);
    q["scale"] = JsonValue(placed.scale.x);
    kit_json.push_back(std::move(q));
  }
  out["kit"] = std::move(kit_json);

  // **The places the plan already knows** (furnish.h, `house_places`): the hearth's recess, mouth,
  // cheeks, the floor beside its stack and its face outside, the furniture's tops, seats, lid and
  // mattress (a kit chest's lid too), the joists' and tie beams' undersides, the doors' and
  // doorways' approaches and the windows' light, each a box in the house's frame (a surface where
  // `low` and `high` share their height). A stack's outside face is in no room (-1).
  Vector<dwellings::NamedPlace> named;
  dwellings::house_places(interior, furnishing, choice.outer_wall_cm, named);
  JsonValue places = JsonValue::array();
  for (const dwellings::NamedPlace& n : named) {
    JsonValue q = JsonValue::object();
    q["kind"] = JsonValue(dwellings::place_kind_name(n.kind));
    q["room"] =
        n.room == dwellings::k_no_room ? JsonValue(static_cast<i64>(-1)) : JsonValue(n.room);
    q["storey"] = JsonValue(static_cast<u32>(n.storey));
    q["faces"] = JsonValue(side_name(n.faces));
    q["of"] = JsonValue(n.index);
    q["low"] = point_mm(plan_x(n.rect.x0), elevation_mm(n.storey) + n.y0_mm, plan_x(n.rect.z0));
    q["high"] = point_mm(plan_x(n.rect.x1), elevation_mm(n.storey) + n.y1_mm, plan_x(n.rect.z1));
    places.push_back(std::move(q));
  }
  out["places"] = std::move(places);

  // The plan's own walk (the tour through every room and a view a room, as `engine-content
  // dwelling` writes them) and the walker's route the plan stands the house by.
  Vector<dwellings::TourPoint> tour;
  dwellings::tour_interior(interior, i32{choice.outer_wall_cm} + 150, tour);
  JsonValue tour_json = JsonValue::array();
  for (const dwellings::TourPoint& t : tour) {
    JsonValue q = JsonValue::object();
    q["house"] = point_mm(plan_x(t.x_cm), t.y_mm, plan_x(t.z_cm));
    q["room"] =
        t.room == dwellings::k_no_room ? JsonValue(static_cast<i64>(-1)) : JsonValue(t.room);
    tour_json.push_back(std::move(q));
  }
  out["tour"] = std::move(tour_json);
  Vector<dwellings::RoomView> views;
  dwellings::room_views(interior, 1650, views);
  JsonValue views_json = JsonValue::array();
  for (const dwellings::RoomView& v : views) {
    JsonValue q = JsonValue::object();
    q["room"] = JsonValue(v.room);
    q["eye"] = point_mm(plan_x(v.eye_x_cm), v.eye_y_mm, plan_x(v.eye_z_cm));
    q["at"] = point_mm(plan_x(v.at_x_cm), v.at_y_mm, plan_x(v.at_z_cm));
    views_json.push_back(std::move(q));
  }
  out["views"] = std::move(views_json);
  JsonValue route = JsonValue::array();
  for (const RoutePoint& p : h.route) {
    JsonValue q = JsonValue::object();
    q["house"] = point_mm(p.x_mm, p.y_mm, p.z_mm);
    q["world"] = world(p.x_mm, p.y_mm, p.z_mm);
    route.push_back(std::move(q));
  }
  out["route"] = std::move(route);
  JsonValue look = JsonValue::object();
  look["house"] = point_mm(h.look.x_mm, h.look.y_mm, h.look.z_mm);
  look["world"] = world(h.look.x_mm, h.look.y_mm, h.look.z_mm);
  out["look"] = std::move(look);

  for (u8 storey = 0; storey < interior.params.storeys; ++storey) {
    plan_text += std::string(dwellings::plan_type_name(interior.params.type)) + ", storey " +
                 std::to_string(storey) + ", 20 cm a character\n";
    plan_text += dwellings::draw_interior(interior, storey, 20);
    plan_text += "\n";
  }
  return true;
}

}  // namespace engine::village
