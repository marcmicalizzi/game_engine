// The interior plan (interiors.h; docs/subsystems/dwellings.md). Integer centimetres,
// the engine's hash, and no floating point: the same plan on every toolchain.
//
// The layout is written once, in a local frame of bands: u runs along the row of rooms, v across
// it. A cot, a hall and parlour and a cross-passage house lay their bands along the front (u = x);
// a two-up two-down lays them away from the lane (u = z). The stair, when there is one, is a band
// of its own, the strip, with its flight along v.

#include <domain/dwellings/interiors.h>

#include <core/hash/hash.h>

namespace engine::dwellings {

namespace {

template <class T>
u32 count_of(const Vector<T>& v) noexcept {
  return static_cast<u32>(v.size());
}

// Least sizes (docs/experiments/vernacular-interiors.md, "Room sizes").
constexpr i32 k_min_cot_length_cm = 400;
constexpr i32 k_min_depth_cm = 330;
constexpr i32 k_min_pair_cm = 640;      // a hall and its second room together
constexpr i32 k_min_hall_cm = 350;
constexpr i32 k_min_second_cm = 240;    // a parlour, a service room
constexpr i32 k_min_inner_cm = 270;
constexpr i32 k_min_two_up_room_cm = 300;
constexpr i32 k_min_axial_depth_cm = 400;

// Where openings sit (the same page, "Where doors and windows sit").
constexpr i32 k_corner_cm = 40;       // an opening's edge to its room's corner
constexpr i32 k_pier_cm = 60;         // between two openings in one wall
constexpr i32 k_stack_pier_cm = 20;   // a window's edge to a stack that stands against its wall
constexpr i32 k_door_margin_cm = 25;  // a partition doorway's jamb to the outer wall's face
constexpr i32 k_landing_pier_cm = 15;  // a landing doorway's jamb to the outer wall's face
constexpr i32 k_landing_end_cm = 5;    // and its other jamb to the first riser's line
constexpr i32 k_approach_cm = 90;     // the floor kept clear before a doorway (plan 14 §14.4)
constexpr i32 k_stair_window_cm = 60;
constexpr u32 k_max_windows = 4;

// Hearths ("The hearth and its stack").
constexpr i32 k_gable_stack_cm = 40;   // a wall hearth's breast, into the room
constexpr i32 k_axial_stack_cm = 100;  // an axial stack, along the house
constexpr i32 k_stack_jamb_cm = 30;    // each side of the opening
constexpr i32 k_hall_opening_cm = 120;
constexpr i32 k_small_opening_cm = 90;
constexpr i32 k_lintel_cm = 130;

constexpr u64 k_draw_split = 1;
constexpr u64 k_draw_walk = 2;
constexpr u64 k_draw_door = 3;

u64 draw(u64 seed, u64 purpose) noexcept {
  return mix64(hash_combine(hash_combine(k_hash_seed, seed), purpose));
}

// A whole number in [lo, hi].
i32 draw_range(u64 seed, u64 purpose, i32 lo, i32 hi) noexcept {
  if (hi <= lo) return lo;
  const u64 span = static_cast<u64>(hi - lo) + 1;
  return lo + static_cast<i32>(draw(seed, purpose) % span);
}

i32 round10(i32 v) noexcept { return (v + 5) / 10 * 10; }

bool fail(std::string* error, const char* what) {
  if (error != nullptr) *error = what;
  return false;
}

enum class Local : u8 { Near, Far, Low, High };  // v = 0, v = V, u = 0, u = U

struct Frame {
  bool swap = false;  // u = z, v = x
  i32 u_len = 0;
  i32 v_len = 0;

  RectCm rect(i32 u0, i32 v0, i32 u1, i32 v1) const noexcept {
    return swap ? RectCm{v0, u0, v1, u1} : RectCm{u0, v0, u1, v1};
  }
  // The outer wall on a local side, which is also the direction that looks toward it.
  Side side(Local s) const noexcept {
    switch (s) {
      case Local::Near: return swap ? Side::Left : Side::Front;
      case Local::Far: return swap ? Side::Right : Side::Back;
      case Local::Low: return swap ? Side::Front : Side::Left;
      default: return swap ? Side::Back : Side::Right;
    }
  }
};

struct Band {
  RoomKind kind = RoomKind::Hall;
  i32 u0 = 0;
  i32 u1 = 0;
  u8 flags = 0;
  u32 room = k_no_room;
};

struct Builder {
  InteriorPlan& plan;
  Frame frame;

  u32 room(RoomKind kind, u8 storey, i32 u0, i32 u1, u8 flags) {
    Room r;
    r.rect = frame.rect(u0, 0, u1, frame.v_len);
    r.kind = kind;
    r.storey = storey;
    r.flags = flags;
    plan.rooms.push_back(r);
    return count_of(plan.rooms) - 1;
  }

  // The wall across v at `u`, between the band below it and the band above.
  u32 wall(u8 storey, i32 u, u32 a, u32 b) {
    InnerWall w;
    const RectCm line = frame.rect(u, 0, u, frame.v_len);
    w.x0 = line.x0;
    w.z0 = line.z0;
    w.x1 = line.x1;
    w.z1 = line.z1;
    w.a = a;
    w.b = b;
    w.thickness_cm = plan.params.inner_wall_cm;
    w.storey = storey;
    plan.walls.push_back(w);
    return count_of(plan.walls) - 1;
  }

  void doorway(u32 wall_index, i32 v_centre, i32 width) {
    Doorway d;
    d.wall = wall_index;
    d.at_cm = v_centre;
    d.width_cm = static_cast<u16>(width);
    d.head_cm = plan.params.head_cm;
    plan.doorways.push_back(d);
  }

  void outer_door(Local side, i32 at, u32 room_index) {
    OuterOpening o;
    o.at_cm = at;
    o.room = room_index;
    o.width_cm = plan.params.door_cm;
    o.sill_cm = 0;
    o.head_cm = plan.params.head_cm;
    o.side = frame.side(side);
    o.storey_door = 0x80u;
    plan.openings.push_back(o);
    plan.rooms[room_index].flags = static_cast<u8>(plan.rooms[room_index].flags | k_room_entrance);
  }

  void hearth(u32 room_index, u8 storey, i32 u0, i32 v0, i32 u1, i32 v1, Local looks,
              i32 opening) {
    Hearth h;
    h.stack = frame.rect(u0, v0, u1, v1);
    h.room = room_index;
    h.opening_cm = static_cast<u16>(opening);
    h.lintel_cm = static_cast<u16>(k_lintel_cm);
    h.faces = frame.side(looks);
    h.storey = storey;
    plan.hearths.push_back(h);
    plan.rooms[room_index].flags = static_cast<u8>(plan.rooms[room_index].flags | k_room_heated);
  }
};

i32 span_x(const RectCm& r) noexcept { return r.x1 - r.x0; }
i32 span_z(const RectCm& r) noexcept { return r.z1 - r.z0; }

bool overlaps(const RectCm& a, const RectCm& b) noexcept {
  return a.x0 < b.x1 && b.x0 < a.x1 && a.z0 < b.z1 && b.z0 < a.z1;
}

bool inside(const RectCm& inner, const RectCm& outer) noexcept {
  return inner.x0 >= outer.x0 && inner.x1 <= outer.x1 && inner.z0 >= outer.z0 &&
         inner.z1 <= outer.z1;
}

Side flip(Side s) noexcept {
  if (s == Side::Left) return Side::Right;
  if (s == Side::Right) return Side::Left;
  return s;
}

RectCm flip(const RectCm& r, i32 width) noexcept {
  return RectCm{width - r.x1, r.z0, width - r.x0, r.z1};
}

// The plan flipped left to right: x becomes width - x.
void mirror_plan(InteriorPlan& plan) noexcept {
  const i32 width = plan.params.width_cm;
  for (Room& r : plan.rooms) r.rect = flip(r.rect, width);
  for (u32 i = 0; i < count_of(plan.walls); ++i) {
    InnerWall& w = plan.walls[i];
    if (w.x0 == w.x1) {
      // Along z: its low side is now its high side.
      w.x0 = width - w.x0;
      w.x1 = w.x0;
      const u32 a = w.a;
      w.a = w.b;
      w.b = a;
    } else {
      const i32 length = w.x1 - w.x0;
      const i32 x0 = width - w.x1;
      w.x1 = width - w.x0;
      w.x0 = x0;
      for (Doorway& d : plan.doorways) {
        if (d.wall == i) d.at_cm = length - d.at_cm;
      }
    }
  }
  for (OuterOpening& o : plan.openings) {
    if (o.side == Side::Front || o.side == Side::Back) o.at_cm = width - o.at_cm;
    o.side = flip(o.side);
  }
  for (Hearth& h : plan.hearths) {
    h.stack = flip(h.stack, width);
    h.faces = flip(h.faces);
  }
  for (Stair& s : plan.stairs) {
    s.strip = flip(s.strip, width);
    s.foot = flip(s.foot, width);
    s.flight = flip(s.flight, width);
    s.head = flip(s.head, width);
    s.rises = flip(s.rises);
  }
}

// --- Windows -------------------------------------------------------------------------------------

struct Interval {
  i32 lo = 0;
  i32 hi = 0;
};

struct Free {
  Interval at[16];
  u32 count = 0;

  void subtract(i32 lo, i32 hi) noexcept {
    Interval kept[16];
    u32 n = 0;
    for (u32 i = 0; i < count; ++i) {
      const Interval f = at[i];
      if (hi <= f.lo || lo >= f.hi) {
        if (n < 16) kept[n++] = f;
        continue;
      }
      if (lo > f.lo && n < 16) kept[n++] = Interval{f.lo, lo};
      if (hi < f.hi && n < 16) kept[n++] = Interval{hi, f.hi};
    }
    count = n;
    for (u32 i = 0; i < n; ++i) at[i] = kept[i];
  }

  // The widest interval, the lowest of equals; false when none holds `width`.
  bool widest(i32 width, Interval& out) const noexcept {
    bool found = false;
    for (u32 i = 0; i < count; ++i) {
      const i32 length = at[i].hi - at[i].lo;
      if (length < width) continue;
      if (!found || length > out.hi - out.lo) {
        out = at[i];
        found = true;
      }
    }
    return found;
  }
};

// A room's stretch of an outer wall, as positions along that side; false when it has none.
bool room_segment(const InteriorPlan& plan, const Room& room, Side side, Interval& out) noexcept {
  const RectCm& r = room.rect;
  switch (side) {
    case Side::Front:
      if (r.z0 != 0) return false;
      out = Interval{r.x0, r.x1};
      return true;
    case Side::Back:
      if (r.z1 != plan.params.depth_cm) return false;
      out = Interval{r.x0, r.x1};
      return true;
    case Side::Left:
      if (r.x0 != 0) return false;
      out = Interval{r.z0, r.z1};
      return true;
    default:
      if (r.x1 != plan.params.width_cm) return false;
      out = Interval{r.z0, r.z1};
      return true;
  }
}

// A stack's stretch of an outer wall it stands against; false when it stands against none.
bool stack_segment(const InteriorPlan& plan, const Hearth& hearth, Side side,
                   Interval& out) noexcept {
  const RectCm& r = hearth.stack;
  switch (side) {
    case Side::Front:
      if (r.z0 != 0) return false;
      out = Interval{r.x0, r.x1};
      return true;
    case Side::Back:
      if (r.z1 != plan.params.depth_cm) return false;
      out = Interval{r.x0, r.x1};
      return true;
    case Side::Left:
      if (r.x0 != 0) return false;
      out = Interval{r.z0, r.z1};
      return true;
    default:
      if (r.x1 != plan.params.width_cm) return false;
      out = Interval{r.z0, r.z1};
      return true;
  }
}

void add_window(InteriorPlan& plan, u32 room_index, Side side, i32 at, i32 width) {
  OuterOpening o;
  o.at_cm = at;
  o.room = room_index;
  o.width_cm = static_cast<u16>(width);
  o.sill_cm = plan.params.sill_cm;
  o.head_cm = plan.params.head_cm;
  o.side = side;
  o.storey_door = plan.rooms[room_index].storey;
  plan.openings.push_back(o);
}

// The free stretches of a room's wall on one side: its segment less its corners, the openings
// already in it with their piers, and every stack standing against it.
bool free_of(const InteriorPlan& plan, u32 room_index, Side side, Free& out) noexcept {
  const Room& room = plan.rooms[room_index];
  Interval segment;
  if (!room_segment(plan, room, side, segment)) return false;
  out.count = 1;
  out.at[0] = Interval{segment.lo + k_corner_cm, segment.hi - k_corner_cm};
  if (out.at[0].hi <= out.at[0].lo) {
    out.count = 0;
    return false;
  }
  for (const OuterOpening& o : plan.openings) {
    if (o.side != side || o.storey() != room.storey) continue;
    const i32 half = o.width_cm / 2;
    out.subtract(o.at_cm - half - k_pier_cm, o.at_cm + half + k_pier_cm + (o.width_cm & 1));
  }
  for (const Hearth& h : plan.hearths) {
    if (h.storey > room.storey) continue;
    Interval stack;
    if (!stack_segment(plan, h, side, stack)) continue;
    out.subtract(stack.lo - k_stack_pier_cm, stack.hi + k_stack_pier_cm);
  }
  return out.count != 0;
}

void place_windows(InteriorPlan& plan) {
  const InteriorParams& p = plan.params;
  const i32 width = p.window_cm;
  const i64 glass = i64{width} * (p.head_cm - p.sill_cm);
  constexpr Side k_order[4] = {Side::Front, Side::Back, Side::Left, Side::Right};
  const u32 rooms = count_of(plan.rooms);
  for (u32 r = 0; r < rooms; ++r) {
    const Room room = plan.rooms[r];
    if ((room.flags & k_room_habitable) == 0) continue;
    // The old byelaw's rule: a room's glass at least a tenth of its floor.
    const i64 area = i64{span_x(room.rect)} * span_z(room.rect);
    u32 need = static_cast<u32>((area + 10 * glass - 1) / (10 * glass));
    if (need < 1) need = 1;
    if (need > k_max_windows) need = k_max_windows;
    u32 placed = 0;
    bool any = true;
    while (placed < need && any) {
      any = false;
      for (const Side side : k_order) {
        if (placed >= need) break;
        Free free_runs;
        if (!free_of(plan, r, side, free_runs)) continue;
        Interval run;
        if (!free_runs.widest(width, run)) continue;
        add_window(plan, r, side, (run.lo + run.hi) / 2, width);
        ++placed;
        any = true;
      }
    }
  }
  // A stair's head landing looks out through the wall the flight climbs toward.
  for (const Stair& s : plan.stairs) {
    const bool along_x = s.rises == Side::Front || s.rises == Side::Back;
    const i32 at = along_x ? (s.strip.x0 + s.strip.x1) / 2 : (s.strip.z0 + s.strip.z1) / 2;
    add_window(plan, s.upper_room, s.rises, at, k_stair_window_cm);
  }
}

// --- The layout ----------------------------------------------------------------------------------

struct Layout {
  Band ground[5];
  u32 ground_count = 0;
  i32 strip_u0 = -1;  // the strip's band, when there is a stair
  i32 strip_u1 = -1;
  i32 inner_u1 = 0;   // the inner room's far edge, 0 with none
};

void push(Layout& l, RoomKind kind, i32 length, u8 flags) noexcept {
  Band b;
  b.kind = kind;
  b.u0 = l.ground_count == 0 ? 0 : l.ground[l.ground_count - 1].u1;
  b.u1 = b.u0 + length;
  b.flags = flags;
  if (kind == RoomKind::Stair) {
    l.strip_u0 = b.u0;
    l.strip_u1 = b.u1;
  }
  l.ground[l.ground_count++] = b;
}

}  // namespace

const char* plan_type_name(PlanType type) noexcept {
  switch (type) {
    case PlanType::Cot: return "cot";
    case PlanType::HallParlour: return "hall-parlour";
    case PlanType::CrossPassage: return "cross-passage";
    default: return "two-up-two-down";
  }
}

const char* room_kind_name(RoomKind kind) noexcept {
  switch (kind) {
    case RoomKind::Hall: return "hall";
    case RoomKind::Parlour: return "parlour";
    case RoomKind::Passage: return "passage";
    case RoomKind::Service: return "service";
    case RoomKind::Inner: return "inner";
    case RoomKind::Kitchen: return "kitchen";
    case RoomKind::Chamber: return "chamber";
    default: return "stair";
  }
}

const char* interior_rule_name(InteriorRule rule) noexcept {
  switch (rule) {
    case InteriorRule::RoomsTile: return "rooms-tile";
    case InteriorRule::RoomReachable: return "room-reachable";
    case InteriorRule::DoorwayClear: return "doorway-clear";
    case InteriorRule::DoorwayJoins: return "doorway-joins";
    case InteriorRule::StairRise: return "stair-rise";
    case InteriorRule::StairGoing: return "stair-going";
    case InteriorRule::StairWidth: return "stair-width";
    case InteriorRule::StairLanding: return "stair-landing";
    case InteriorRule::StairHeadroom: return "stair-headroom";
    case InteriorRule::StairLands: return "stair-lands";
    case InteriorRule::WindowInWall: return "window-in-wall";
    case InteriorRule::WindowHeight: return "window-height";
    case InteriorRule::RoomLit: return "room-lit";
    default: return "hearth-clear";
  }
}

void InteriorPlan::clear() noexcept {
  rooms.clear();
  walls.clear();
  doorways.clear();
  openings.clear();
  hearths.clear();
  stairs.clear();
}

RectCm Stair::well() const noexcept {
  switch (rises) {
    case Side::Front: return RectCm{strip.x0, head.z1, strip.x1, strip.z1};
    case Side::Back: return RectCm{strip.x0, strip.z0, strip.x1, head.z0};
    case Side::Left: return RectCm{head.x1, strip.z0, strip.x1, strip.z1};
    default: return RectCm{strip.x0, strip.z0, head.x0, strip.z1};
  }
}

RectCm Stair::tread(u32 i) const noexcept {
  const i32 k = static_cast<i32>(i) * k_stair_going_cm;
  RectCm r = flight;
  switch (rises) {
    case Side::Front:
      r.z1 = flight.z1 - k;
      r.z0 = r.z1 - k_stair_going_cm;
      break;
    case Side::Back:
      r.z0 = flight.z0 + k;
      r.z1 = r.z0 + k_stair_going_cm;
      break;
    case Side::Left:
      r.x1 = flight.x1 - k;
      r.x0 = r.x1 - k_stair_going_cm;
      break;
    default:
      r.x0 = flight.x0 + k;
      r.x1 = r.x0 + k_stair_going_cm;
      break;
  }
  return r;
}

u32 stair_risers(i32 rise_mm) noexcept {
  if (rise_mm <= 0) return 0;
  return static_cast<u32>((rise_mm + k_stair_max_rise_mm - 1) / k_stair_max_rise_mm);
}

i32 stair_strip_length_cm(i32 rise_mm) noexcept {
  const i32 risers = static_cast<i32>(stair_risers(rise_mm));
  if (risers < 2) return 0;
  return (risers - 1) * k_stair_going_cm + 2 * k_stair_landing_cm;
}

InteriorExtent interior_min_extent(const InteriorParams& params) noexcept {
  InteriorExtent e;
  const bool upper = params.type == PlanType::TwoUpTwoDown ||
                     (params.type != PlanType::Cot && params.storeys >= 2);
  const i32 strip = upper ? k_stair_strip_cm : 0;
  const i32 flight = upper ? stair_strip_length_cm(i32{params.storey_cm} * 10) : 0;
  switch (params.type) {
    case PlanType::Cot:
      e.min_width_cm = k_min_cot_length_cm;
      e.min_depth_cm = k_min_depth_cm;
      break;
    case PlanType::HallParlour:
      e.min_width_cm = k_min_pair_cm + strip;
      e.min_depth_cm = flight > k_min_depth_cm ? flight : k_min_depth_cm;
      break;
    case PlanType::CrossPassage:
      e.min_width_cm = k_min_pair_cm + 200 + strip + (params.inner_room ? k_min_inner_cm + 80 : 0);
      e.min_depth_cm = flight > k_min_axial_depth_cm ? flight : k_min_axial_depth_cm;
      break;
    default:
      e.min_width_cm = flight > k_min_depth_cm ? flight : k_min_depth_cm;
      e.min_depth_cm = 2 * k_min_two_up_room_cm + 60 + strip;
      break;
  }
  return e;
}

bool plan_interior(const InteriorParams& params, InteriorPlan& out, std::string* error) {
  out.clear();
  out.params = params;
  InteriorParams& p = out.params;
  if (p.type == PlanType::Cot) p.storeys = 1;
  if (p.type == PlanType::TwoUpTwoDown) p.storeys = 2;
  if (p.type != PlanType::CrossPassage) p.inner_room = false;
  if (p.hearths != 2 || p.type == PlanType::Cot) p.hearths = 1;
  if (p.storeys < 1 || p.storeys > 2) return fail(error, "interior: storeys must be 1 or 2");
  if (p.doorway_cm < 80 || p.door_cm < 80 || p.head_cm < 200) {
    return fail(error, "interior: a doorway is at least 80 cm wide and 200 cm high");
  }
  if (i32{p.storey_cm} - p.floor_cm < p.head_cm || p.upper_clear_cm < k_headroom_cm ||
      p.sill_cm >= p.head_cm || p.window_cm < 40 || p.inner_wall_cm < 4 || p.inner_wall_cm > 30) {
    return fail(error, "interior: a storey, a window or a wall thickness is out of range");
  }

  Frame frame;
  frame.swap = p.type == PlanType::TwoUpTwoDown;
  frame.u_len = frame.swap ? p.depth_cm : p.width_cm;
  frame.v_len = frame.swap ? p.width_cm : p.depth_cm;
  const i32 u_len = frame.u_len;
  const i32 v_len = frame.v_len;
  const bool upper = p.storeys == 2;
  const i32 strip = upper ? k_stair_strip_cm : 0;
  const u8 lived = k_room_habitable;

  // The stair first: it sets what the plan needs across v.
  const i32 rise_mm = i32{p.storey_cm} * 10;
  const i32 risers = static_cast<i32>(stair_risers(rise_mm));
  const i32 run = (risers - 1) * k_stair_going_cm;
  i32 head_len = 0;
  i32 foot_len = 0;
  if (upper) {
    if (v_len < run + 2 * k_stair_landing_cm) {
      return fail(error, "interior: the stair's flight and its two landings do not fit the house");
    }
    head_len = (v_len - run) / 2;
    foot_len = v_len - run - head_len;
  }
  if (v_len < k_min_depth_cm) return fail(error, "interior: the house is too shallow for a room");

  // The bands of the ground storey.
  Layout l;
  switch (p.type) {
    case PlanType::Cot: {
      if (u_len < k_min_cot_length_cm) return fail(error, "interior: a cot under 4 m long");
      push(l, RoomKind::Hall, u_len, lived);
      break;
    }
    case PlanType::HallParlour: {
      const i32 avail = u_len - strip;
      if (avail < k_min_pair_cm) {
        return fail(error, "interior: a hall and parlour need 6.4 m beside the stair");
      }
      const i32 percent = draw_range(p.seed, k_draw_split, 56, 62);
      const i32 hall = round10(avail * percent / 100);
      push(l, RoomKind::Hall, hall, lived);
      if (upper) push(l, RoomKind::Stair, strip, 0);
      push(l, RoomKind::Parlour, avail - hall, lived);
      break;
    }
    case PlanType::CrossPassage: {
      const i32 walk = 160 + 10 * draw_range(p.seed, k_draw_walk, 0, 4);
      i32 rest = u_len - walk - strip;
      i32 inner = 0;
      if (p.inner_room) {
        inner = round10(rest * 22 / 100);
        if (inner < k_min_inner_cm) inner = k_min_inner_cm;
        rest -= inner;
      }
      if (rest < k_min_pair_cm) {
        return fail(error, "interior: a cross-passage house is too short for its rooms");
      }
      if (v_len < k_min_axial_depth_cm) {
        return fail(error, "interior: an axial stack and a doorway beside it need 4 m of depth");
      }
      const i32 percent = draw_range(p.seed, k_draw_split, 56, 60);
      const i32 hall = round10(rest * percent / 100);
      if (inner != 0) {
        push(l, RoomKind::Inner, inner, lived);
        l.inner_u1 = inner;
      }
      push(l, RoomKind::Hall, hall, lived);
      push(l, RoomKind::Passage, walk, 0);
      if (upper) push(l, RoomKind::Stair, strip, 0);
      push(l, RoomKind::Service, rest - hall, lived);
      break;
    }
    default: {
      const i32 avail = u_len - strip;
      if (avail < 2 * k_min_two_up_room_cm + 60) {
        return fail(error, "interior: a two-up two-down needs two 3 m rooms and its stair");
      }
      const i32 percent = draw_range(p.seed, k_draw_split, 48, 54);
      const i32 front = round10(avail * percent / 100);
      push(l, RoomKind::Parlour, front, lived);
      push(l, RoomKind::Stair, strip, 0);
      push(l, RoomKind::Kitchen, avail - front, lived);
      break;
    }
  }
  for (u32 i = 0; i < l.ground_count; ++i) {
    const Band& b = l.ground[i];
    const i32 length = b.u1 - b.u0;
    const i32 least = b.kind == RoomKind::Hall
                          ? k_min_hall_cm
                          : (b.kind == RoomKind::Passage || b.kind == RoomKind::Stair
                                 ? 100
                                 : k_min_second_cm);
    if (length < least) return fail(error, "interior: a room came out under its least size");
  }

  Builder build{out, frame};

  // Rooms and the walls between them, the ground storey.
  for (u32 i = 0; i < l.ground_count; ++i) {
    Band& b = l.ground[i];
    b.room = build.room(b.kind, 0, b.u0, b.u1, b.flags);
  }
  const i32 partition_at = k_door_margin_cm + p.doorway_cm / 2;
  i32 landing_door = p.doorway_cm;
  if (upper) {
    const i32 least_landing = head_len < foot_len ? head_len : foot_len;
    if (landing_door > least_landing - k_landing_pier_cm - k_landing_end_cm) {
      landing_door = least_landing - k_landing_pier_cm - k_landing_end_cm;
    }
    if (landing_door < 80) {
      return fail(error, "interior: a landing too short for a doorway beside it");
    }
  }
  for (u32 i = 0; i + 1 < l.ground_count; ++i) {
    const Band& a = l.ground[i];
    const Band& b = l.ground[i + 1];
    const u32 w = build.wall(0, a.u1, a.room, b.room);
    if (a.kind == RoomKind::Stair || b.kind == RoomKind::Stair) {
      build.doorway(w, v_len - k_landing_pier_cm - (landing_door - landing_door / 2), landing_door);
    } else {
      build.doorway(w, partition_at, p.doorway_cm);
    }
  }

  // The upper storey: a chamber (or two) on each side of the strip.
  u32 upper_stair_room = k_no_room;
  if (upper) {
    u32 before = k_no_room;
    if (l.inner_u1 != 0) {
      const u32 far_chamber = build.room(RoomKind::Chamber, 1, 0, l.inner_u1, lived);
      before = build.room(RoomKind::Chamber, 1, l.inner_u1, l.strip_u0, lived);
      const u32 w = build.wall(1, l.inner_u1, far_chamber, before);
      build.doorway(w, v_len - partition_at, p.doorway_cm);
    } else {
      before = build.room(RoomKind::Chamber, 1, 0, l.strip_u0, lived);
    }
    upper_stair_room = build.room(RoomKind::Stair, 1, l.strip_u0, l.strip_u1, 0);
    const u32 after = build.room(RoomKind::Chamber, 1, l.strip_u1, u_len, lived);
    const u32 w0 = build.wall(1, l.strip_u0, before, upper_stair_room);
    build.doorway(w0, k_landing_pier_cm + landing_door / 2, landing_door);
    const u32 w1 = build.wall(1, l.strip_u1, upper_stair_room, after);
    build.doorway(w1, k_landing_pier_cm + landing_door / 2, landing_door);

    Stair s;
    s.strip = frame.rect(l.strip_u0, 0, l.strip_u1, v_len);
    s.head = frame.rect(l.strip_u0, 0, l.strip_u1, head_len);
    s.flight = frame.rect(l.strip_u0, head_len, l.strip_u1, v_len - foot_len);
    s.foot = frame.rect(l.strip_u0, v_len - foot_len, l.strip_u1, v_len);
    for (u32 i = 0; i < l.ground_count; ++i) {
      if (l.ground[i].kind == RoomKind::Stair) s.lower_room = l.ground[i].room;
    }
    s.upper_room = upper_stair_room;
    s.rise_mm = rise_mm;
    s.risers = static_cast<u8>(risers);
    s.rises = frame.side(Local::Near);
    s.from_storey = 0;
    out.stairs.push_back(s);
  }

  // Hearths and outer doors, by type.
  const i32 v_mid = v_len / 2;
  auto band_of = [&](RoomKind kind) -> const Band* {
    for (u32 i = 0; i < l.ground_count; ++i) {
      if (l.ground[i].kind == kind) return &l.ground[i];
    }
    return nullptr;
  };
  auto gable_low = [&](const Band& b, i32 opening) {
    const i32 half = (opening + 2 * k_stack_jamb_cm) / 2;
    build.hearth(b.room, 0, 0, v_mid - half, k_gable_stack_cm, v_mid + half, Local::High, opening);
  };
  auto gable_high = [&](const Band& b, i32 opening) {
    const i32 half = (opening + 2 * k_stack_jamb_cm) / 2;
    build.hearth(b.room, 0, u_len - k_gable_stack_cm, v_mid - half, u_len, v_mid + half,
                 Local::Low, opening);
  };
  switch (p.type) {
    case PlanType::Cot: {
      const Band& hall = l.ground[0];
      gable_high(hall, k_hall_opening_cm);
      const i32 percent = draw_range(p.seed, k_draw_door, 28, 40);
      i32 at = round10(u_len * percent / 100);
      const i32 least = k_corner_cm + p.door_cm / 2 + 1;
      if (at < least) at = least;
      build.outer_door(Local::Near, at, hall.room);
      break;
    }
    case PlanType::HallParlour: {
      const Band& hall = *band_of(RoomKind::Hall);
      const Band& parlour = *band_of(RoomKind::Parlour);
      gable_low(hall, k_hall_opening_cm);
      if (p.hearths == 2) gable_high(parlour, k_small_opening_cm);
      build.outer_door(Local::Near, hall.u1 - k_corner_cm - p.door_cm / 2 - 1, hall.room);
      break;
    }
    case PlanType::CrossPassage: {
      const Band& hall = *band_of(RoomKind::Hall);
      const Band& passage = *band_of(RoomKind::Passage);
      const Band& service = *band_of(RoomKind::Service);
      // The stack backs onto the passage; the hall's doorway from the passage stands in front of
      // it, so the opening narrows where the house is shallow.
      i32 opening = k_hall_opening_cm;
      const i32 room_for_door = k_door_margin_cm + p.doorway_cm + 15;
      if (v_mid - (opening + 2 * k_stack_jamb_cm) / 2 < room_for_door) {
        opening = k_small_opening_cm;
      }
      const i32 half = (opening + 2 * k_stack_jamb_cm) / 2;
      if (v_mid - half < room_for_door) {
        return fail(error, "interior: the hall's doorway does not clear the axial stack");
      }
      build.hearth(hall.room, 0, hall.u1 - k_axial_stack_cm, v_mid - half, hall.u1, v_mid + half,
                   Local::Low, opening);
      if (p.hearths == 2) gable_high(service, k_small_opening_cm);
      const i32 mid = (passage.u0 + passage.u1) / 2;
      build.outer_door(Local::Near, mid, passage.room);
      build.outer_door(Local::Far, mid, passage.room);
      break;
    }
    default: {
      const Band& front = *band_of(RoomKind::Parlour);
      const Band& back = *band_of(RoomKind::Kitchen);
      // Against the side wall the head landing is on, clear of the foot landing's doorways and,
      // above, of the head landing's: the stack keeps a doorway's approach off the strip.
      const i32 half = (k_small_opening_cm + 2 * k_stack_jamb_cm) / 2;
      const i32 front_mid = (front.u0 + front.u1 - k_approach_cm - 10) / 2;
      build.hearth(front.room, 0, front_mid - half, 0, front_mid + half, k_gable_stack_cm,
                   Local::Far, k_small_opening_cm);
      if (p.hearths == 2) {
        const i32 back_mid = (back.u0 + k_approach_cm + 10 + back.u1) / 2;
        build.hearth(back.room, 0, back_mid - half, 0, back_mid + half, k_gable_stack_cm,
                     Local::Far, k_small_opening_cm);
      }
      build.outer_door(Local::Low, v_len - k_corner_cm - p.door_cm / 2 - 1, front.room);
      break;
    }
  }

  if (p.mirror) mirror_plan(out);
  place_windows(out);

  Vector<InteriorFinding> findings;
  if (!validate_interior(out, findings)) {
    if (error != nullptr) {
      *error = "interior: the plan broke its own rule ";
      *error += interior_rule_name(findings[0].rule);
    }
    out.clear();
    return false;
  }
  return true;
}

u64 hash_interior(const InteriorPlan& plan) noexcept {
  const InteriorParams& p = plan.params;
  u64 h = hash_combine(k_hash_seed, u64{k_interior_version});
  auto pair = [&](i32 a, i32 b) {
    h = hash_combine(h, u64{static_cast<u32>(a)} << 32 | static_cast<u32>(b));
  };
  auto rect = [&](const RectCm& r) {
    pair(r.x0, r.z0);
    pair(r.x1, r.z1);
  };
  h = hash_combine(h, u64{static_cast<u8>(p.type)} << 32 | u64{p.storeys} << 24 |
                          u64{p.mirror ? 1u : 0u} << 16 | u64{p.inner_room ? 1u : 0u} << 8 |
                          p.hearths);
  pair(p.width_cm, p.depth_cm);
  h = hash_combine(h, u64{p.storey_cm} << 48 | u64{p.upper_clear_cm} << 32 |
                          u64{p.floor_cm} << 16 | p.inner_wall_cm);
  h = hash_combine(h, u64{p.door_cm} << 48 | u64{p.doorway_cm} << 32 | u64{p.head_cm} << 16 |
                          p.window_cm);
  h = hash_combine(h, p.sill_cm);
  h = hash_combine(h, p.seed);
  h = hash_combine(h, plan.rooms.size());
  for (const Room& r : plan.rooms) {
    rect(r.rect);
    h = hash_combine(h, u64{static_cast<u8>(r.kind)} << 16 | u64{r.storey} << 8 | r.flags);
  }
  h = hash_combine(h, plan.walls.size());
  for (const InnerWall& w : plan.walls) {
    pair(w.x0, w.z0);
    pair(w.x1, w.z1);
    h = hash_combine(h, u64{w.a} << 32 | w.b);
    h = hash_combine(h, u64{w.thickness_cm} << 8 | w.storey);
  }
  h = hash_combine(h, plan.doorways.size());
  for (const Doorway& d : plan.doorways) {
    h = hash_combine(h, u64{d.wall} << 32 | static_cast<u32>(d.at_cm));
    h = hash_combine(h, u64{d.width_cm} << 16 | d.head_cm);
  }
  h = hash_combine(h, plan.openings.size());
  for (const OuterOpening& o : plan.openings) {
    h = hash_combine(h, u64{static_cast<u32>(o.at_cm)} << 32 | o.room);
    h = hash_combine(h, u64{o.width_cm} << 48 | u64{o.sill_cm} << 32 | u64{o.head_cm} << 16 |
                            u64{static_cast<u8>(o.side)} << 8 | o.storey_door);
  }
  h = hash_combine(h, plan.hearths.size());
  for (const Hearth& x : plan.hearths) {
    rect(x.stack);
    h = hash_combine(h, u64{x.room} << 32 | u64{x.opening_cm} << 16 | x.lintel_cm);
    h = hash_combine(h, u64{static_cast<u8>(x.faces)} << 8 | x.storey);
  }
  h = hash_combine(h, plan.stairs.size());
  for (const Stair& s : plan.stairs) {
    rect(s.strip);
    rect(s.foot);
    rect(s.flight);
    rect(s.head);
    h = hash_combine(h, u64{s.lower_room} << 32 | s.upper_room);
    h = hash_combine(h, u64{static_cast<u32>(s.rise_mm)} << 32 | u64{s.risers} << 16 |
                            u64{static_cast<u8>(s.rises)} << 8 | s.from_storey);
  }
  return h;
}

u32 reachable_rooms(const InteriorPlan& plan, Vector<u8>& reached) {
  const u32 rooms = count_of(plan.rooms);
  reached.clear();
  reached.resize(rooms, u8{0});
  Vector<u32> open;
  for (u32 r = 0; r < rooms; ++r) {
    if ((plan.rooms[r].flags & k_room_entrance) != 0) {
      reached[r] = 1;
      open.push_back(r);
    }
  }
  auto visit = [&](u32 r) {
    if (r >= rooms || reached[r] != 0) return;
    reached[r] = 1;
    open.push_back(r);
  };
  u32 next = 0;
  while (next < count_of(open)) {
    const u32 r = open[next++];
    for (const Doorway& d : plan.doorways) {
      const InnerWall& w = plan.walls[d.wall];
      if (w.a == r) visit(w.b);
      if (w.b == r) visit(w.a);
    }
    for (const Stair& s : plan.stairs) {
      if (s.lower_room == r) visit(s.upper_room);
      if (s.upper_room == r) visit(s.lower_room);
    }
  }
  return count_of(open);
}

i32 stair_headroom_cm(const InteriorPlan& plan, const Stair& stair, i32 x_cm, i32 z_cm,
                      i32 height_mm) noexcept {
  const InteriorParams& p = plan.params;
  const RectCm well = stair.well();
  const bool in_well = x_cm >= well.x0 && x_cm < well.x1 && z_cm >= well.z0 && z_cm < well.z1;
  const i32 ceiling_mm = stair.rise_mm + i32{p.upper_clear_cm} * 10;
  const i32 slab_mm = stair.rise_mm - i32{p.floor_cm} * 10;
  const i32 over_mm = (in_well || height_mm >= stair.rise_mm) ? ceiling_mm : slab_mm;
  return (over_mm - height_mm) / 10;
}

bool validate_interior(const InteriorPlan& plan, Vector<InteriorFinding>& findings) {
  findings.clear();
  const InteriorParams& p = plan.params;
  const u32 rooms = count_of(plan.rooms);
  const RectCm whole{0, 0, p.width_cm, p.depth_cm};
  auto found = [&](InteriorRule rule, u32 record, i32 measured, i32 limit) {
    InteriorFinding f;
    f.rule = rule;
    f.index = record;
    f.measured = measured;
    f.limit = limit;
    findings.push_back(f);
  };

  // Rooms tile each storey.
  for (u8 storey = 0; storey < p.storeys; ++storey) {
    i64 area = 0;
    for (u32 a = 0; a < rooms; ++a) {
      const Room& ra = plan.rooms[a];
      if (ra.storey != storey) continue;
      if (!inside(ra.rect, whole) || span_x(ra.rect) <= 0 || span_z(ra.rect) <= 0) {
        found(InteriorRule::RoomsTile, a, 0, 0);
      }
      area += i64{span_x(ra.rect)} * span_z(ra.rect);
      for (u32 b = a + 1; b < rooms; ++b) {
        const Room& rb = plan.rooms[b];
        if (rb.storey == storey && overlaps(ra.rect, rb.rect)) {
          found(InteriorRule::RoomsTile, b, 0, 0);
        }
      }
    }
    const i64 want = i64{p.width_cm} * p.depth_cm;
    if (area != want) {
      found(InteriorRule::RoomsTile, storey, static_cast<i32>(area / 10000),
            static_cast<i32>(want / 10000));
    }
  }

  // Every room is reached.
  Vector<u8> reached;
  reachable_rooms(plan, reached);
  for (u32 r = 0; r < rooms; ++r) {
    if (reached[r] == 0) found(InteriorRule::RoomReachable, r, 0, 1);
  }

  // Doorways.
  for (u32 i = 0; i < count_of(plan.doorways); ++i) {
    const Doorway& d = plan.doorways[i];
    if (d.width_cm < 80) found(InteriorRule::DoorwayClear, i, d.width_cm, 80);
    if (d.head_cm < 200) found(InteriorRule::DoorwayClear, i, d.head_cm, 200);
    if (d.wall >= count_of(plan.walls)) {
      found(InteriorRule::DoorwayJoins, i, 0, 0);
      continue;
    }
    const InnerWall& w = plan.walls[d.wall];
    const bool along_z = w.x0 == w.x1;
    const i32 length = along_z ? w.z1 - w.z0 : w.x1 - w.x0;
    const i32 lo = d.at_cm - d.width_cm / 2;
    const i32 hi = lo + d.width_cm;
    if (lo < 5 || hi > length - 5) found(InteriorRule::DoorwayClear, i, lo, 5);
    if (w.a >= rooms || w.b >= rooms) {
      found(InteriorRule::DoorwayJoins, i, 0, 0);
      continue;
    }
    // The span lies on the line both rooms meet.
    const i32 base = along_z ? w.z0 : w.x0;
    for (u32 side = 0; side < 2; ++side) {
      const Room& room = plan.rooms[side == 0 ? w.a : w.b];
      const RectCm& r = room.rect;
      const i32 edge = along_z ? (side == 0 ? r.x1 : r.x0) : (side == 0 ? r.z1 : r.z0);
      const i32 line = along_z ? w.x0 : w.z0;
      const i32 r_lo = along_z ? r.z0 : r.x0;
      const i32 r_hi = along_z ? r.z1 : r.x1;
      if (room.storey != w.storey || edge != line || base + lo < r_lo || base + hi > r_hi) {
        found(InteriorRule::DoorwayJoins, i, edge, line);
      }
    }
  }

  // Stairs.
  for (u32 i = 0; i < count_of(plan.stairs); ++i) {
    const Stair& s = plan.stairs[i];
    if (s.risers < 2) {
      found(InteriorRule::StairRise, i, 0, 2);
      continue;
    }
    const i32 rise = (s.rise_mm + s.risers - 1) / s.risers;
    if (rise > 250) found(InteriorRule::StairRise, i, rise, 250);
    const bool along_z = s.rises == Side::Front || s.rises == Side::Back;
    const i32 run = along_z ? span_z(s.flight) : span_x(s.flight);
    const i32 going = run / (s.risers - 1);
    if (going < 28 || run != going * (s.risers - 1)) found(InteriorRule::StairGoing, i, going, 28);
    const i32 clear = (along_z ? span_x(s.strip) : span_z(s.strip)) - p.inner_wall_cm;
    if (clear < 80) found(InteriorRule::StairWidth, i, clear, 80);
    const i32 foot = along_z ? span_z(s.foot) : span_x(s.foot);
    const i32 head = along_z ? span_z(s.head) : span_x(s.head);
    if (foot < k_stair_landing_cm) found(InteriorRule::StairLanding, i, foot, k_stair_landing_cm);
    if (head < k_stair_landing_cm) found(InteriorRule::StairLanding, i, head, k_stair_landing_cm);
    // Headroom over every tread and both landings.
    i32 least = stair_headroom_cm(plan, s, (s.foot.x0 + s.foot.x1) / 2,
                                  (s.foot.z0 + s.foot.z1) / 2, 0);
    for (u32 t = 0; t + 1 < s.risers; ++t) {
      const RectCm tread = s.tread(t);
      const i32 h = stair_headroom_cm(plan, s, (tread.x0 + tread.x1) / 2,
                                      (tread.z0 + tread.z1) / 2, s.tread_mm(t));
      if (h < least) least = h;
    }
    const i32 over_head = stair_headroom_cm(plan, s, (s.head.x0 + s.head.x1) / 2,
                                            (s.head.z0 + s.head.z1) / 2, s.rise_mm);
    if (over_head < least) least = over_head;
    if (least < k_headroom_cm) found(InteriorRule::StairHeadroom, i, least, k_headroom_cm);
    // A doorway leaves each landing, inside the landing's length.
    bool leaves_head = false;
    bool leaves_foot = false;
    for (const Doorway& d : plan.doorways) {
      if (d.wall >= count_of(plan.walls)) continue;
      const InnerWall& w = plan.walls[d.wall];
      const bool wall_along_z = w.x0 == w.x1;
      if (wall_along_z != along_z) continue;
      const i32 lo = (wall_along_z ? w.z0 : w.x0) + d.at_cm - d.width_cm / 2;
      const i32 hi = lo + d.width_cm;
      const bool lower = w.storey == s.from_storey && (w.a == s.lower_room || w.b == s.lower_room);
      const bool above =
          w.storey == s.from_storey + 1 && (w.a == s.upper_room || w.b == s.upper_room);
      const i32 f_lo = along_z ? s.foot.z0 : s.foot.x0;
      const i32 f_hi = along_z ? s.foot.z1 : s.foot.x1;
      const i32 h_lo = along_z ? s.head.z0 : s.head.x0;
      const i32 h_hi = along_z ? s.head.z1 : s.head.x1;
      if (lower && lo >= f_lo && hi <= f_hi) leaves_foot = true;
      if (above && lo >= h_lo && hi <= h_hi) leaves_head = true;
    }
    if (!leaves_foot || !leaves_head || !inside(s.head, s.strip) || !inside(s.foot, s.strip) ||
        s.upper_room >= rooms || s.lower_room >= rooms) {
      found(InteriorRule::StairLands, i, leaves_head ? 1 : 0, 1);
    }
  }

  // Outer openings: none into an inner wall, a stack or another opening.
  const u32 openings = count_of(plan.openings);
  for (u32 i = 0; i < openings; ++i) {
    const OuterOpening& o = plan.openings[i];
    const i32 lo = o.at_cm - o.width_cm / 2;
    const i32 hi = lo + o.width_cm;
    const i32 clear_cm = o.storey() == 0 ? i32{p.storey_cm} - p.floor_cm : i32{p.upper_clear_cm};
    if (o.sill_cm >= o.head_cm || o.head_cm > clear_cm) {
      found(InteriorRule::WindowHeight, i, o.head_cm, clear_cm);
    }
    if (o.room >= rooms) {
      found(InteriorRule::WindowInWall, i, 0, 0);
      continue;
    }
    const Room& room = plan.rooms[o.room];
    Interval segment;
    if (room.storey != o.storey() || !room_segment(plan, room, o.side, segment) ||
        lo < segment.lo + 15 || hi > segment.hi - 15) {
      found(InteriorRule::WindowInWall, i, lo, 15);
    }
    // An inner wall's end on this side, within the span and its pier.
    const bool side_along_x = o.side == Side::Front || o.side == Side::Back;
    const i32 side_at = o.side == Side::Front || o.side == Side::Left
                            ? 0
                            : (side_along_x ? p.depth_cm : p.width_cm);
    for (const InnerWall& w : plan.walls) {
      if (w.storey != o.storey()) continue;
      const bool along_z = w.x0 == w.x1;
      if (along_z != side_along_x) continue;  // a wall parallel to the side meets it nowhere
      const i32 at = along_z ? w.x0 : w.z0;
      const i32 end_lo = along_z ? w.z0 : w.x0;
      const i32 end_hi = along_z ? w.z1 : w.x1;
      if (end_lo != side_at && end_hi != side_at) continue;
      const i32 pier = w.thickness_cm / 2 + 10;
      if (at > lo - pier && at < hi + pier) found(InteriorRule::WindowInWall, i, at - lo, pier);
    }
    for (const Hearth& x : plan.hearths) {
      if (x.storey > o.storey()) continue;
      Interval stack;
      if (!stack_segment(plan, x, o.side, stack)) continue;
      if (lo < stack.hi && stack.lo < hi) found(InteriorRule::WindowInWall, i, stack.lo, hi);
    }
    for (u32 j = i + 1; j < openings; ++j) {
      const OuterOpening& other = plan.openings[j];
      if (other.side != o.side || other.storey() != o.storey()) continue;
      const i32 other_lo = other.at_cm - other.width_cm / 2;
      const i32 other_hi = other_lo + other.width_cm;
      const i32 gap = other_lo >= hi ? other_lo - hi : lo - other_hi;
      if (gap < 30) found(InteriorRule::WindowInWall, j, gap, 30);
    }
  }
  for (u32 r = 0; r < rooms; ++r) {
    if ((plan.rooms[r].flags & k_room_habitable) == 0) continue;
    bool lit = false;
    for (const OuterOpening& o : plan.openings) {
      if (o.room == r && !o.door()) lit = true;
    }
    if (!lit) found(InteriorRule::RoomLit, r, 0, 1);
  }

  // Hearths: inside the room, off every doorway's approach, every outer door's and every stair.
  for (u32 i = 0; i < count_of(plan.hearths); ++i) {
    const Hearth& x = plan.hearths[i];
    if (x.room >= rooms || !inside(x.stack, plan.rooms[x.room].rect)) {
      found(InteriorRule::HearthClear, i, 0, 0);
      continue;
    }
    for (const Stair& s : plan.stairs) {
      if (overlaps(x.stack, s.strip)) found(InteriorRule::HearthClear, i, 1, 0);
    }
    for (const Doorway& d : plan.doorways) {
      if (d.wall >= count_of(plan.walls)) continue;
      const InnerWall& w = plan.walls[d.wall];
      if (w.storey < x.storey) continue;
      const bool along_z = w.x0 == w.x1;
      const i32 lo = (along_z ? w.z0 : w.x0) + d.at_cm - d.width_cm / 2;
      const i32 hi = lo + d.width_cm;
      const RectCm approach = along_z
                                  ? RectCm{w.x0 - k_approach_cm, lo, w.x0 + k_approach_cm, hi}
                                  : RectCm{lo, w.z0 - k_approach_cm, hi, w.z0 + k_approach_cm};
      if (overlaps(x.stack, approach)) found(InteriorRule::HearthClear, i, 2, 0);
    }
    for (const OuterOpening& o : plan.openings) {
      if (!o.door() || o.storey() < x.storey) continue;
      const i32 lo = o.at_cm - o.width_cm / 2;
      const i32 hi = lo + o.width_cm;
      RectCm approach;
      switch (o.side) {
        case Side::Front: approach = RectCm{lo, 0, hi, k_approach_cm}; break;
        case Side::Back:
          approach = RectCm{lo, p.depth_cm - k_approach_cm, hi, p.depth_cm};
          break;
        case Side::Left: approach = RectCm{0, lo, k_approach_cm, hi}; break;
        default: approach = RectCm{p.width_cm - k_approach_cm, lo, p.width_cm, hi}; break;
      }
      if (overlaps(x.stack, approach)) found(InteriorRule::HearthClear, i, 3, 0);
    }
  }
  return findings.empty();
}

std::string draw_interior(const InteriorPlan& plan, u8 storey, i32 cell_cm) {
  const InteriorParams& p = plan.params;
  if (cell_cm < 5) cell_cm = 5;
  const i32 cols = (p.width_cm + cell_cm - 1) / cell_cm;
  const i32 rows = (p.depth_cm + cell_cm - 1) / cell_cm;
  const i32 stride = cols + 3;  // a wall each side and the line's end
  std::string out(static_cast<usize>(stride) * static_cast<usize>(rows + 2), '#');
  // Row 0 is the back wall, the last row the front: the lane is below the picture.
  auto cell = [&](i32 col, i32 row) -> char& {
    return out[static_cast<usize>((rows - row) * stride + col + 1)];
  };
  for (i32 row = -1; row <= rows; ++row) out[static_cast<usize>((rows - row) * stride + cols + 2)] = '\n';
  for (i32 row = 0; row < rows; ++row) {
    for (i32 col = 0; col < cols; ++col) cell(col, row) = ' ';
  }
  auto fill = [&](const RectCm& r, char c) {
    for (i32 row = r.z0 / cell_cm; row * cell_cm < r.z1 && row < rows; ++row) {
      for (i32 col = r.x0 / cell_cm; col * cell_cm < r.x1 && col < cols; ++col) cell(col, row) = c;
    }
  };
  for (const Stair& s : plan.stairs) {
    if (storey != s.from_storey && storey != s.from_storey + 1) continue;
    const bool below = storey == s.from_storey;
    fill(s.foot, below ? 'f' : ':');
    fill(s.head, below ? 'x' : 'h');
    for (u32 t = 0; t + 1 < s.risers; ++t) {
      const char mark = below ? ((t & 1) != 0 ? '=' : '-') : ':';
      fill(s.tread(t), below && s.tread_mm(t) + i32{p.head_cm} * 10 > s.rise_mm ? 'x' : mark);
    }
  }
  for (const Hearth& h : plan.hearths) {
    if (h.storey <= storey) fill(h.stack, h.storey == storey ? 'H' : 'c');
  }
  for (u32 i = 0; i < count_of(plan.walls); ++i) {
    const InnerWall& w = plan.walls[i];
    if (w.storey != storey) continue;
    const bool along_z = w.x0 == w.x1;
    const i32 length = along_z ? w.z1 - w.z0 : w.x1 - w.x0;
    for (i32 at = 0; at < length; at += cell_cm) {
      bool open = false;
      for (const Doorway& d : plan.doorways) {
        const i32 lo = d.at_cm - d.width_cm / 2;
        if (d.wall == i && at + cell_cm / 2 >= lo && at + cell_cm / 2 < lo + d.width_cm) open = true;
      }
      const i32 col = (along_z ? w.x0 : w.x0 + at) / cell_cm;
      const i32 row = (along_z ? w.z0 + at : w.z0) / cell_cm;
      if (col < cols && row < rows) cell(col, row) = open ? '.' : '%';
    }
  }
  for (const Room& r : plan.rooms) {
    if (r.storey != storey || r.kind == RoomKind::Stair) continue;
    const char* name = room_kind_name(r.kind);
    const i32 col = (r.rect.x0 + r.rect.x1) / 2 / cell_cm;
    const i32 row = (r.rect.z0 + r.rect.z1) / 2 / cell_cm - 1;
    if (cell(col, row) == ' ') cell(col, row) = static_cast<char>(name[0] - 'a' + 'A');
  }
  for (const OuterOpening& o : plan.openings) {
    if (o.storey() != storey) continue;
    const i32 lo = o.at_cm - o.width_cm / 2;
    for (i32 at = lo; at < lo + o.width_cm; at += cell_cm) {
      const i32 along = at / cell_cm;
      const char mark = o.door() ? 'D' : 'W';
      switch (o.side) {
        case Side::Front: cell(along, -1) = mark; break;
        case Side::Back: cell(along, rows) = mark; break;
        case Side::Left: cell(-1, along) = mark; break;
        default: cell(cols, along) = mark; break;
      }
    }
  }
  return out;
}

}  // namespace engine::dwellings
