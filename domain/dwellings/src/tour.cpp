// The tour and the views (tour.h; docs/subsystems/dwellings.md, "The tour and the views"). Integer
// centimetres; a depth-first walk over the plan's graph.

#include <domain/dwellings/tour.h>

namespace engine::dwellings {

namespace {

template <class T>
u32 count_of(const Vector<T>& v) noexcept {
  return static_cast<u32>(v.size());
}

constexpr i32 k_door_step_cm = 45;  // a door point's distance from its wall's line

struct P {
  i32 x = 0;
  i32 z = 0;
};

struct Edge {
  u32 a = k_no_room;
  u32 b = k_no_room;
  u32 record = 0;  // a doorway, or a stair
  bool stair = false;
};

i32 floor_mm(const InteriorPlan& plan, u8 storey) noexcept {
  return storey == 0 ? 0 : i32{plan.params.storey_cm} * 10;
}

P centre(const RectCm& r) noexcept { return P{(r.x0 + r.x1) / 2, (r.z0 + r.z1) / 2}; }

// The flight's two ends on the strip's centre line: where it leaves the foot landing, and where it
// lands on the head.
void flight_ends(const Stair& s, P& low, P& high) noexcept {
  const P mid = centre(s.strip);
  switch (s.rises) {
    case Side::Front:
      low = P{mid.x, s.flight.z1};
      high = P{mid.x, s.flight.z0};
      break;
    case Side::Back:
      low = P{mid.x, s.flight.z0};
      high = P{mid.x, s.flight.z1};
      break;
    case Side::Left:
      low = P{s.flight.x1, mid.z};
      high = P{s.flight.x0, mid.z};
      break;
    default:
      low = P{s.flight.x0, mid.z};
      high = P{s.flight.x1, mid.z};
      break;
  }
}

// A doorway's two door points: on its wall's low side and on its high side.
void door_points(const InteriorPlan& plan, const Doorway& d, P& low, P& high) noexcept {
  const InnerWall& w = plan.walls[d.wall];
  if (w.x0 == w.x1) {
    const i32 at = w.z0 + d.at_cm;
    low = P{w.x0 - k_door_step_cm, at};
    high = P{w.x0 + k_door_step_cm, at};
  } else {
    const i32 at = w.x0 + d.at_cm;
    low = P{at, w.z0 - k_door_step_cm};
    high = P{at, w.z0 + k_door_step_cm};
  }
}

// Just inside an outer opening, and `outside_cm` before its wall's inner face.
void opening_points(const InteriorPlan& plan, const OuterOpening& o, i32 inside_cm, i32 outside_cm,
                    P& inside, P& outside) noexcept {
  const i32 width = plan.params.width_cm;
  const i32 depth = plan.params.depth_cm;
  switch (o.side) {
    case Side::Front:
      inside = P{o.at_cm, inside_cm};
      outside = P{o.at_cm, -outside_cm};
      break;
    case Side::Back:
      inside = P{o.at_cm, depth - inside_cm};
      outside = P{o.at_cm, depth + outside_cm};
      break;
    case Side::Left:
      inside = P{inside_cm, o.at_cm};
      outside = P{-outside_cm, o.at_cm};
      break;
    default:
      inside = P{width - inside_cm, o.at_cm};
      outside = P{width + outside_cm, o.at_cm};
      break;
  }
}

// Whether the leg from a to b, parallel to a wall, passes within the clearance of a stack that
// stands on this storey.
bool leg_hits(const InteriorPlan& plan, u8 storey, P a, P b) noexcept {
  const i32 x0 = a.x < b.x ? a.x : b.x;
  const i32 x1 = a.x < b.x ? b.x : a.x;
  const i32 z0 = a.z < b.z ? a.z : b.z;
  const i32 z1 = a.z < b.z ? b.z : a.z;
  for (const Hearth& h : plan.hearths) {
    if (h.storey > storey) continue;
    const RectCm s{h.stack.x0 - k_tour_clear_cm, h.stack.z0 - k_tour_clear_cm,
                   h.stack.x1 + k_tour_clear_cm, h.stack.z1 + k_tour_clear_cm};
    if (x0 < s.x1 && s.x0 < x1 + 1 && z0 < s.z1 && s.z0 < z1 + 1) return true;
  }
  return false;
}

const Stair* stair_of(const InteriorPlan& plan, u32 room) noexcept {
  for (const Stair& s : plan.stairs) {
    if (s.lower_room == room || s.upper_room == room) return &s;
  }
  return nullptr;
}

// Where a walker stands in a room: a landing's middle, or the room's, moved a quarter of the room
// aside when a stack stands there.
P anchor(const InteriorPlan& plan, u32 room) noexcept {
  const Room& r = plan.rooms[room];
  if (const Stair* s = stair_of(plan, room)) {
    return centre(s->lower_room == room ? s->foot : s->head);
  }
  const P mid = centre(r.rect);
  const i32 qx = (r.rect.x1 - r.rect.x0) / 4;
  const i32 qz = (r.rect.z1 - r.rect.z0) / 4;
  const P tries[5] = {mid, P{mid.x - qx, mid.z}, P{mid.x + qx, mid.z}, P{mid.x, mid.z - qz},
                      P{mid.x, mid.z + qz}};
  for (const P& t : tries) {
    if (!leg_hits(plan, r.storey, t, t)) return t;
  }
  return mid;
}

void edges_of(const InteriorPlan& plan, Vector<Edge>& edges) {
  edges.clear();
  for (u32 i = 0; i < count_of(plan.doorways); ++i) {
    const InnerWall& w = plan.walls[plan.doorways[i].wall];
    Edge e;
    e.a = w.a;
    e.b = w.b;
    e.record = i;
    edges.push_back(e);
  }
  for (u32 i = 0; i < count_of(plan.stairs); ++i) {
    Edge e;
    e.a = plan.stairs[i].lower_room;
    e.b = plan.stairs[i].upper_room;
    e.record = i;
    e.stair = true;
    edges.push_back(e);
  }
}

struct Walker {
  const InteriorPlan& plan;
  Vector<TourPoint>& out;
  Vector<Edge> edges;
  Vector<u8> visited;
  u32 seen = 0;
  P at;

  void emit(P p, u32 room) {
    TourPoint t;
    t.x_cm = p.x;
    t.z_cm = p.z;
    t.room = room;
    t.y_mm = room == k_no_room ? 0 : floor_mm(plan, plan.rooms[room].storey);
    if (!out.empty()) {
      const TourPoint& last = out.back();
      if (last.x_cm == t.x_cm && last.z_cm == t.z_cm && last.y_mm == t.y_mm) {
        at = p;
        return;
      }
    }
    out.push_back(t);
    at = p;
  }

  // Inside one room, by one leg or two, the corner chosen clear of the stacks.
  void move_to(u32 room, P to) {
    const u8 storey = plan.rooms[room].storey;
    if (at.x != to.x && at.z != to.z) {
      const P x_first{to.x, at.z};
      const P z_first{at.x, to.z};
      const bool x_clear = !leg_hits(plan, storey, at, x_first) && !leg_hits(plan, storey, x_first, to);
      const bool z_clear = !leg_hits(plan, storey, at, z_first) && !leg_hits(plan, storey, z_first, to);
      emit(x_clear || !z_clear ? x_first : z_first, room);
    }
    emit(to, room);
  }

  void cross(const Edge& e, u32 from, u32 to) {
    P leave;
    P arrive;
    if (e.stair) {
      const Stair& s = plan.stairs[e.record];
      P low;
      P high;
      flight_ends(s, low, high);
      leave = from == s.lower_room ? low : high;
      arrive = from == s.lower_room ? high : low;
    } else {
      P low;
      P high;
      door_points(plan, plan.doorways[e.record], low, high);
      leave = from == e.a ? low : high;
      arrive = from == e.a ? high : low;
    }
    move_to(from, leave);
    emit(arrive, to);
  }

  void visit(u32 room) {
    visited[room] = 1;
    ++seen;
    move_to(room, anchor(plan, room));
    // Rooms before stairs, so the ground storey is seen before the climb.
    for (u32 pass = 0; pass < 3; ++pass) {
      for (u32 i = 0; i < count_of(edges); ++i) {
        const Edge e = edges[i];
        if (e.a != room && e.b != room) continue;
        const u32 other = e.a == room ? e.b : e.a;
        const bool to_stair_room = plan.rooms[other].kind == RoomKind::Stair;
        const u32 rank = e.stair ? 2u : (to_stair_room ? 1u : 0u);
        if (rank != pass || visited[other] != 0) continue;
        cross(e, room, other);
        visit(other);
        if (seen == count_of(plan.rooms)) return;
        cross(e, other, room);
        move_to(room, anchor(plan, room));
      }
    }
  }

  // From the room the walk stands in to `target`, by the fewest crossings.
  void go(u32 from, u32 target) {
    const u32 rooms = count_of(plan.rooms);
    Vector<u32> via;  // the edge a room was reached through, from the target outward
    via.resize(rooms, k_no_room);
    Vector<u32> open;
    open.push_back(target);
    Vector<u8> reached;
    reached.resize(rooms, u8{0});
    reached[target] = 1;
    for (u32 next = 0; next < count_of(open); ++next) {
      const u32 r = open[next];
      for (u32 i = 0; i < count_of(edges); ++i) {
        const Edge& e = edges[i];
        if (e.a != r && e.b != r) continue;
        const u32 other = e.a == r ? e.b : e.a;
        if (reached[other] != 0) continue;
        reached[other] = 1;
        via[other] = i;
        open.push_back(other);
      }
    }
    u32 room = from;
    while (room != target && via[room] != k_no_room) {
      const Edge e = edges[via[room]];
      const u32 other = e.a == room ? e.b : e.a;
      move_to(room, anchor(plan, room));
      cross(e, room, other);
      room = other;
    }
    move_to(room, anchor(plan, room));
  }
};

}  // namespace

bool tour_interior(const InteriorPlan& plan, i32 outside_cm, Vector<TourPoint>& out) {
  out.clear();
  const OuterOpening* door = nullptr;
  for (const OuterOpening& o : plan.openings) {
    if (o.door() && o.room < count_of(plan.rooms)) {
      door = &o;
      break;
    }
  }
  if (door == nullptr) return false;

  Walker walker{plan, out, {}, {}, 0, P{}};
  edges_of(plan, walker.edges);
  walker.visited.resize(count_of(plan.rooms), u8{0});
  P inside;
  P outside;
  opening_points(plan, *door, k_door_step_cm, outside_cm, inside, outside);
  walker.emit(outside, k_no_room);
  walker.emit(inside, door->room);
  walker.visit(door->room);
  if (!plan.stairs.empty() && !out.empty()) {
    walker.go(out.back().room, plan.stairs[0].upper_room);
  }
  return true;
}

void room_views(const InteriorPlan& plan, i32 eye_height_mm, Vector<RoomView>& out) {
  out.clear();
  for (u32 r = 0; r < count_of(plan.rooms); ++r) {
    const Room& room = plan.rooms[r];
    const i32 ground = floor_mm(plan, room.storey);
    RoomView v;
    v.room = r;
    v.eye_y_mm = ground + eye_height_mm;
    if (const Stair* s = stair_of(plan, r)) {
      P low;
      P high;
      flight_ends(*s, low, high);
      if (s->lower_room == r) {
        // From the foot landing, up the flight.
        const P foot = centre(s->foot);
        v.eye_x_cm = foot.x;
        v.eye_z_cm = foot.z;
        v.at_x_cm = high.x;
        v.at_z_cm = high.z;
        v.at_y_mm = s->rise_mm + 1000;
      } else {
        // From the top of the flight, out of the landing's window.
        v.eye_x_cm = high.x;
        v.eye_z_cm = high.z;
        v.at_x_cm = high.x;
        v.at_z_cm = high.z;
        v.at_y_mm = v.eye_y_mm;
        for (const OuterOpening& o : plan.openings) {
          if (o.room != r || o.door()) continue;
          P inside;
          P outside;
          opening_points(plan, o, 0, 0, inside, outside);
          v.at_x_cm = inside.x;
          v.at_z_cm = inside.z;
          v.at_y_mm = ground + (i32{o.sill_cm} + o.head_cm) * 5;
        }
      }
      out.push_back(v);
      continue;
    }

    // The way in: an outer door, else the first doorway.
    P eye = anchor(plan, r);
    const OuterOpening* entered = nullptr;
    for (const OuterOpening& o : plan.openings) {
      if (o.room == r && o.door() && entered == nullptr) entered = &o;
    }
    if (entered != nullptr) {
      P outside;
      opening_points(plan, *entered, 60, 0, eye, outside);
    } else {
      for (u32 i = 0; i < count_of(plan.doorways); ++i) {
        const InnerWall& w = plan.walls[plan.doorways[i].wall];
        if (w.a != r && w.b != r) continue;
        P low;
        P high;
        door_points(plan, plan.doorways[i], low, high);
        eye = w.a == r ? low : high;
        break;
      }
    }
    v.eye_x_cm = eye.x;
    v.eye_z_cm = eye.z;

    // The farthest window; a room with none looks at its other door, or across itself.
    const P across = anchor(plan, r);
    v.at_x_cm = across.x;
    v.at_z_cm = across.z;
    v.at_y_mm = ground + 1200;
    i64 best = -1;
    for (const OuterOpening& o : plan.openings) {
      if (o.room != r || &o == entered) continue;
      P inside;
      P outside;
      opening_points(plan, o, 0, 0, inside, outside);
      const i64 dx = inside.x - eye.x;
      const i64 dz = inside.z - eye.z;
      // Any window beats a door.
      const i64 score = dx * dx + dz * dz + (o.door() ? 0 : i64{1} << 40);
      if (score <= best) continue;
      best = score;
      v.at_x_cm = inside.x;
      v.at_z_cm = inside.z;
      v.at_y_mm = ground + (o.door() ? 1500 : (i32{o.sill_cm} + o.head_cm) * 5);
    }
    out.push_back(v);
  }
}

}  // namespace engine::dwellings
