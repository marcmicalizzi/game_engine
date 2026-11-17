// Timber and furniture as members (furnish.h; docs/subsystems/dwellings.md, "Timber and
// furniture"). Integer centimetres and the engine's hash; one draw a statement, because a call's
// arguments are evaluated in the compiler's order. Every size below is on the grounding page
// (docs/experiments/vernacular-interiors.md, "Timber" and "Furniture") with its source, or is
// labelled there as a choice.

#include <core/hash/hash.h>
#include <domain/dwellings/furnish.h>
#include <domain/dwellings/tour.h>

#include <initializer_list>

namespace engine::dwellings {

namespace {

template <class T>
u32 count_of(const Vector<T>& v) noexcept {
  return static_cast<u32>(v.size());
}

constexpr u64 k_draw_settle = 201;
constexpr u64 k_draw_table = 202;
constexpr u64 k_draw_bed_width = 203;
constexpr u64 k_draw_bed = 204;
constexpr u64 k_draw_chest_length = 205;
constexpr u64 k_draw_chest = 206;

u64 draw(u64 seed, u64 purpose, u32 room) noexcept {
  return mix64(hash_combine(hash_combine(hash_combine(k_hash_seed, seed), purpose), u64{room}));
}

// The floor kept clear (centimetres).
constexpr i32 k_door_side_cm = 20;      // each side of a door's or a doorway's span
constexpr i32 k_door_reach_cm = 100;    // before an outer door: its leaf's swing
constexpr i32 k_doorway_reach_cm = 90;  // each side of an inner doorway (interiors.cpp's approach)
constexpr i32 k_window_side_cm = 10;
constexpr i32 k_window_reach_cm = 100;  // before a window a walker is led to
constexpr i32 k_hearth_side_cm = 20;
constexpr i32 k_hearth_reach_cm = 110;  // before a hearth's opening
constexpr i32 k_stack_gap_cm = 2;
constexpr i32 k_piece_gap_cm = 10;    // between two pieces
constexpr i32 k_settle_far_cm = 320;  // a settle farther from its hearth than this is no settle

// The timber (centimetres).
constexpr i32 k_joist_centres_cm = 52;
constexpr i32 k_joist_width_cm = 12;
constexpr i32 k_joist_depth_cm = 10;
constexpr i32 k_beam_width_cm = 24;
constexpr i32 k_beam_depth_cm = 18;
constexpr i32 k_beam_least_span_cm = 300;  // joists alone span less
constexpr i32 k_into_cm = 2;               // a member's end runs this far into what bears it
constexpr i32 k_nosing_cm = 2;
constexpr i32 k_nosing_depth_cm = 3;
constexpr i32 k_newel_cm = 9;
constexpr i32 k_newel_out_cm = 5;
constexpr i32 k_newel_height_cm = 100;

enum class Keep : u8 { Tour, Door, Window, Led, Hearth, Stack };

struct KeepOut {
  RectCm r;
  u8 storey = 0;
  Keep kind = Keep::Tour;
};

bool overlaps(const RectCm& a, const RectCm& b) noexcept {
  return a.x0 < b.x1 && b.x0 < a.x1 && a.z0 < b.z1 && b.z0 < a.z1;
}

bool inside(const RectCm& inner, const RectCm& outer) noexcept {
  return inner.x0 >= outer.x0 && inner.x1 <= outer.x1 && inner.z0 >= outer.z0 &&
         inner.z1 <= outer.z1;
}

RectCm grown(const RectCm& r, i32 by) noexcept {
  return RectCm{r.x0 - by, r.z0 - by, r.x1 + by, r.z1 + by};
}

i32 apart(const RectCm& a, const RectCm& b) noexcept {
  const i32 dx = (a.x0 + a.x1) / 2 - (b.x0 + b.x1) / 2;
  const i32 dz = (a.z0 + a.z1) / 2 - (b.z0 + b.z1) / 2;
  return (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
}

bool is_furniture(PieceKind kind) noexcept {
  return kind == PieceKind::TableSet || kind == PieceKind::Settle || kind == PieceKind::Bed ||
         kind == PieceKind::Chest;
}

// A window's floor is kept clear of a settle, whose back stands over a sill; a table, a chest and a
// bed stand under windows as they do in a cottage. The one window every piece keeps clear of is
// `Keep::Led`: the first window of an upper storey, which the village's walker is led to.
bool window_keeps(PieceKind kind) noexcept { return kind == PieceKind::Settle; }

// The strip of floor before a span of an outer wall's inner face.
RectCm before_side(const InteriorPlan& plan, Side side, i32 lo, i32 hi, i32 reach) noexcept {
  const InteriorParams& p = plan.params;
  switch (side) {
    case Side::Front: return RectCm{lo, 0, hi, reach};
    case Side::Back: return RectCm{lo, p.depth_cm - reach, hi, p.depth_cm};
    case Side::Left: return RectCm{0, lo, reach, hi};
    default: return RectCm{p.width_cm - reach, lo, p.width_cm, hi};
  }
}

void keep_outs(const InteriorPlan& plan, Vector<KeepOut>& out) {
  out.clear();
  const InteriorParams& p = plan.params;
  Vector<TourPoint> tour;
  if (tour_interior(plan, 150, tour)) {
    for (u32 i = 0; i + 1 < count_of(tour); ++i) {
      const TourPoint& a = tour[i];
      const TourPoint& b = tour[i + 1];
      if (a.y_mm != b.y_mm) continue;  // the flight
      KeepOut k;
      k.r.x0 = (a.x_cm < b.x_cm ? a.x_cm : b.x_cm) - k_tour_clear_cm;
      k.r.x1 = (a.x_cm < b.x_cm ? b.x_cm : a.x_cm) + k_tour_clear_cm;
      k.r.z0 = (a.z_cm < b.z_cm ? a.z_cm : b.z_cm) - k_tour_clear_cm;
      k.r.z1 = (a.z_cm < b.z_cm ? b.z_cm : a.z_cm) + k_tour_clear_cm;
      k.storey = a.y_mm == 0 ? u8{0} : u8{1};
      k.kind = Keep::Tour;
      out.push_back(k);
    }
  }
  for (const Doorway& d : plan.doorways) {
    if (d.wall >= count_of(plan.walls)) continue;
    const InnerWall& w = plan.walls[d.wall];
    const bool along_z = w.x0 == w.x1;
    const i32 mid = (along_z ? w.z0 : w.x0) + d.at_cm;
    const i32 half = i32{d.width_cm} / 2 + k_door_side_cm;
    const i32 reach = k_doorway_reach_cm + i32{w.thickness_cm} / 2;
    KeepOut k;
    k.r = along_z ? RectCm{w.x0 - reach, mid - half, w.x0 + reach, mid + half}
                  : RectCm{mid - half, w.z0 - reach, mid + half, w.z0 + reach};
    k.storey = w.storey;
    k.kind = Keep::Door;
    out.push_back(k);
  }
  bool led = false;
  for (const OuterOpening& o : plan.openings) {
    const i32 side = o.door() ? k_door_side_cm : k_window_side_cm;
    const i32 half = i32{o.width_cm} / 2 + side;
    KeepOut k;
    k.r = before_side(plan, o.side, o.at_cm - half, o.at_cm + half,
                      o.door() ? k_door_reach_cm : k_window_reach_cm);
    k.storey = o.storey();
    k.kind = o.door() ? Keep::Door : Keep::Window;
    if (!o.door() && o.storey() > 0 && !led) {
      k.kind = Keep::Led;
      led = true;
    }
    out.push_back(k);
  }
  for (const Hearth& h : plan.hearths) {
    // The stack, on its hearth's storey and every storey over it.
    for (u8 s = h.storey; s < p.storeys; ++s) {
      KeepOut k;
      k.r = grown(h.stack, k_stack_gap_cm);
      k.storey = s;
      k.kind = Keep::Stack;
      out.push_back(k);
    }
    KeepOut k;
    const RectCm& st = h.stack;
    switch (h.faces) {
      case Side::Front:
        k.r = RectCm{st.x0 - k_hearth_side_cm, st.z0 - k_hearth_reach_cm, st.x1 + k_hearth_side_cm,
                     st.z0};
        break;
      case Side::Back:
        k.r = RectCm{st.x0 - k_hearth_side_cm, st.z1, st.x1 + k_hearth_side_cm,
                     st.z1 + k_hearth_reach_cm};
        break;
      case Side::Left:
        k.r = RectCm{st.x0 - k_hearth_reach_cm, st.z0 - k_hearth_side_cm, st.x0,
                     st.z1 + k_hearth_side_cm};
        break;
      default:
        k.r = RectCm{st.x1, st.z0 - k_hearth_side_cm, st.x1 + k_hearth_reach_cm,
                     st.z1 + k_hearth_side_cm};
        break;
    }
    k.storey = h.storey;
    k.kind = Keep::Hearth;
    out.push_back(k);
  }
}

// A room's free floor: its rectangle less half an inner wall on each side one bounds it, less the
// gap a piece keeps from a wall's face.
RectCm free_floor(const InteriorPlan& plan, const Room& room) noexcept {
  const InteriorParams& p = plan.params;
  const i32 half = i32{p.inner_wall_cm} / 2;
  RectCm f = room.rect;
  f.x0 += (f.x0 == 0 ? 0 : half) + k_wall_gap_cm;
  f.z0 += (f.z0 == 0 ? 0 : half) + k_wall_gap_cm;
  f.x1 -= (f.x1 == p.width_cm ? 0 : half) + k_wall_gap_cm;
  f.z1 -= (f.z1 == p.depth_cm ? 0 : half) + k_wall_gap_cm;
  return f;
}

// A footprint seen from the wall behind it: u along the wall, v out from it, y up.
struct Builder {
  Furnishing* f = nullptr;
  RectCm r;
  Side against = Side::Front;
  u8 storey = 0;
  u32 room = k_no_room;

  void put(i32 u0, i32 u1, i32 v0, i32 v1, i32 y0, i32 y1,
           MemberSurface surface = MemberSurface::Timber, bool solid = false) const {
    Member m;
    switch (against) {
      case Side::Front:
        m.x0 = r.x0 + u0, m.x1 = r.x0 + u1, m.z0 = r.z0 + v0, m.z1 = r.z0 + v1;
        break;
      case Side::Back:
        m.x0 = r.x0 + u0, m.x1 = r.x0 + u1, m.z0 = r.z1 - v1, m.z1 = r.z1 - v0;
        break;
      case Side::Left:
        m.x0 = r.x0 + v0, m.x1 = r.x0 + v1, m.z0 = r.z0 + u0, m.z1 = r.z0 + u1;
        break;
      default: m.x0 = r.x1 - v1, m.x1 = r.x1 - v0, m.z0 = r.z0 + u0, m.z1 = r.z0 + u1; break;
    }
    m.y0 = y0, m.y1 = y1;
    m.room = room;
    m.surface = surface;
    m.storey = storey;
    m.solid = solid;
    f->members.push_back(m);
  }
};

// A bench (a form): a seat on two slab ends with a stretcher between them.
void bench(const Builder& b, i32 length, i32 v) {
  b.put(4, length - 4, v, v + 28, 41, 45, MemberSurface::Timber, true);
  b.put(14, 18, v + 2, v + 26, 0, 41);
  b.put(length - 18, length - 14, v + 2, v + 26, 0, 41);
  b.put(18, length - 18, v + 12, v + 16, 18, 26);
}

// A joined table between two benches, one against the wall: a top, four legs, four rails under
// the top and an H of stretchers.
i32 table_set(const Builder& b, i32 length) {
  bench(b, length, 1);
  b.put(0, length, 24, 97, 71, 75, MemberSurface::Timber, true);
  for (const i32 u : {8, length - 15})
    for (const i32 v : {30, 84})
      b.put(u, u + 7, v, v + 7, 0, 71);
  b.put(15, length - 15, 31, 34, 61, 71);
  b.put(15, length - 15, 87, 90, 61, 71);
  b.put(10, 13, 37, 84, 61, 71);
  b.put(length - 13, length - 10, 37, 84, 61, 71);
  b.put(10, 13, 37, 84, 10, 16);
  b.put(length - 13, length - 10, 37, 84, 10, 16);
  b.put(13, length - 13, 59, 62, 10, 16);
  bench(b, length, 93);
  return 75;
}

// A settle: a boarded back, two ends, a seat and a board closing its front.
i32 settle(const Builder& b, i32 length) {
  b.put(0, length, 0, 4, 8, 105);
  b.put(0, 4, 4, 58, 0, 70);
  b.put(length - 4, length, 4, 58, 0, 70);
  b.put(4, length - 4, 4, 54, 41, 45, MemberSurface::Timber, true);
  b.put(4, length - 4, 50, 53, 6, 41);
  return 105;
}

// A bedstead, its head to the wall: four posts, a head and a foot board, two side rails, and its
// bedding in linen.
i32 bed(const Builder& b, i32 width) {
  b.put(0, 8, 0, 8, 0, 110);
  b.put(width - 8, width, 0, 8, 0, 110);
  b.put(0, 8, 192, 200, 0, 70);
  b.put(width - 8, width, 192, 200, 0, 70);
  b.put(8, width - 8, 2, 6, 35, 100);
  b.put(8, width - 8, 194, 198, 35, 60);
  b.put(2, 6, 8, 192, 28, 42);
  b.put(width - 6, width - 2, 8, 192, 28, 42);
  b.put(5, width - 5, 5, 195, 38, 56, MemberSurface::Linen, true);
  b.put(14, width - 14, 9, 36, 56, 63, MemberSurface::Linen);
  return 110;
}

// A boarded chest: two ends that reach the floor, a body and a lid that oversails it.
i32 chest(const Builder& b, i32 length) {
  b.put(2, 6, 2, 43, 0, 10);
  b.put(length - 6, length - 2, 2, 43, 0, 10);
  b.put(2, length - 2, 2, 43, 8, 48, MemberSurface::Timber, true);
  b.put(0, length, 0, 45, 48, 52);
  return 52;
}

i32 out_of(PieceKind kind) noexcept {
  switch (kind) {
    case PieceKind::TableSet: return k_table_set_out_cm;
    case PieceKind::Settle: return k_settle_out_cm;
    case PieceKind::Bed: return k_bed_out_cm;
    default: return k_chest_out_cm;
  }
}

i32 least_along(PieceKind kind) noexcept {
  switch (kind) {
    case PieceKind::TableSet: return 120;
    case PieceKind::Settle: return 120;
    case PieceKind::Bed: return 101;
    default: return 70;
  }
}

// One room being furnished.
struct Place {
  const InteriorPlan* plan = nullptr;
  const Vector<KeepOut>* keep = nullptr;
  Furnishing* out = nullptr;
  u32 room = 0;
  u32 first_piece = 0;  // the pieces this call has placed start here
  RectCm open_floor;

  bool fits(const RectCm& r, PieceKind kind) const {
    const Room& room_record = plan->rooms[room];
    if (!inside(r, open_floor)) return false;
    for (const KeepOut& k : *keep) {
      if (k.storey != room_record.storey) continue;
      if (k.kind == Keep::Window && !window_keeps(kind)) continue;
      if (overlaps(r, k.r)) return false;
    }
    for (u32 i = first_piece; i < count_of(out->pieces); ++i) {
      const Piece& other = out->pieces[i];
      if (other.storey != room_record.storey || !is_furniture(other.kind)) continue;
      if (overlaps(r, grown(other.footprint, k_piece_gap_cm))) return false;
    }
    return true;
  }
};

struct Candidate {
  RectCm r;
  Side against = Side::Front;
  i32 score = 0;
};

// Every place a piece `along` by `depth` fits against a wall of the room, on a 10 cm module, added
// to `found`.
void candidates(const Place& place, PieceKind kind, i32 along, i32 depth,
                Vector<Candidate>& found) {
  const RectCm& f = place.open_floor;
  constexpr Side k_sides[4] = {Side::Front, Side::Right, Side::Back, Side::Left};
  for (const Side side : k_sides) {
    const bool along_x = side == Side::Front || side == Side::Back;
    const i32 lo = along_x ? f.x0 : f.z0;
    const i32 hi = (along_x ? f.x1 : f.z1) - along;
    if (hi < lo) continue;
    for (i32 t = lo;; t += 10) {
      if (t > hi) t = hi;
      Candidate c;
      c.against = side;
      switch (side) {
        case Side::Front: c.r = RectCm{t, f.z0, t + along, f.z0 + depth}; break;
        case Side::Back: c.r = RectCm{t, f.z1 - depth, t + along, f.z1}; break;
        case Side::Left: c.r = RectCm{f.x0, t, f.x0 + depth, t + along}; break;
        default: c.r = RectCm{f.x1 - depth, t, f.x1, t + along}; break;
      }
      if (place.fits(c.r, kind)) found.push_back(c);
      if (t == hi) break;
    }
  }
}

// The best-scored candidate, a draw among equals. False when there is none.
bool choose(const Vector<Candidate>& found, u64 drawn, Candidate& best) noexcept {
  if (found.empty()) return false;
  i32 top = found[0].score;
  for (const Candidate& c : found)
    if (c.score > top) top = c.score;
  u32 equals = 0;
  for (const Candidate& c : found)
    equals += c.score == top ? 1u : 0u;
  u32 pick = static_cast<u32>(drawn % u64{equals});
  for (const Candidate& c : found) {
    if (c.score != top) continue;
    if (pick == 0) {
      best = c;
      return true;
    }
    --pick;
  }
  return false;
}

const Hearth* hearth_of(const InteriorPlan& plan, u32 room) noexcept {
  for (const Hearth& h : plan.hearths)
    if (h.room == room) return &h;
  return nullptr;
}

constexpr u64 k_draw_kit = 207;

// A kit piece in a found place: a piece of the row's kind with no members.
void add_kit_piece(Furnishing& out, const KitRow& row, const RectCm& footprint, Side against,
                   u8 storey, u32 room) {
  Piece piece;
  piece.footprint = footprint;
  piece.room = room;
  piece.first_member = count_of(out.members);
  piece.member_count = 0;
  piece.height_cm = static_cast<u16>(row.height_cm);
  piece.kind = row.replaces;
  piece.against = against;
  piece.storey = storey;
  piece.kit = static_cast<u16>(row.id + 1);
  out.pieces.push_back(piece);
}

void furnish_room(const InteriorPlan& plan, const Vector<KeepOut>& keep, u64 seed, u32 room_index,
                  const Kit& kit, Furnishing& out) {
  const Room& room = plan.rooms[room_index];
  // **The kit's row for a kind in this room** (furnish.h, "Kit pieces"): one of those that stand
  // under the room's joists or ceiling, by a draw on the seed, the kind and the room; none when the
  // kit has no row for the kind.
  const i32 clear = room_clear_cm(plan, room);
  const auto kit_row = [&](PieceKind kind) -> const KitRow* {
    u32 n = 0;
    for (u32 k = 0; k < kit.count; ++k)
      n += kit.rows[k].replaces == kind && kit.rows[k].height_cm + k_kit_head_gap_cm <= clear &&
                   kit.rows[k].along_cm > 0 && kit.rows[k].out_cm > 0
               ? 1u
               : 0u;
    if (n == 0) return nullptr;
    u32 pick = static_cast<u32>(
        draw(seed, k_draw_kit + u64{static_cast<u8>(kind)} * 16, room_index) % u64{n});
    for (u32 k = 0; k < kit.count; ++k) {
      const KitRow& r = kit.rows[k];
      if (r.replaces != kind || r.height_cm + k_kit_head_gap_cm > clear || r.along_cm <= 0 ||
          r.out_cm <= 0)
        continue;
      if (pick == 0) return &r;
      --pick;
    }
    return nullptr;
  };
  const bool cot = plan.params.type == PlanType::Cot;
  // A parlour is the best bedroom where the house has no chamber over it and a sitting room where
  // it has (the two-up two-down's front room); a cot's one room is lived and slept in.
  const bool sat_in = room.kind == RoomKind::Parlour && plan.params.type == PlanType::TwoUpTwoDown;
  const bool lived = room.kind == RoomKind::Hall || room.kind == RoomKind::Kitchen || sat_in;
  const bool slept = (room.kind == RoomKind::Parlour && !sat_in) || room.kind == RoomKind::Inner ||
                     room.kind == RoomKind::Chamber || cot;
  if (!lived && !slept) return;
  Place place;
  place.plan = &plan;
  place.keep = &keep;
  place.out = &out;
  place.room = room_index;
  place.first_piece = count_of(out.pieces);
  place.open_floor = free_floor(plan, room);
  Vector<Candidate> found;
  Candidate best;

  const auto live_in = [&]() {
    // A settle by the hearth: the nearest place to the stack, along any wall.
    const Hearth* hearth = hearth_of(plan, room_index);
    bool kit_settle = false;
    if (hearth != nullptr) {
      const u64 drawn = draw(seed, k_draw_settle, room_index);
      if (const KitRow* row = kit_row(PieceKind::Settle); row != nullptr) {
        found.clear();
        candidates(place, PieceKind::Settle, row->along_cm, row->out_cm, found);
        for (Candidate& c : found)
          c.score = -apart(c.r, hearth->stack);
        if (choose(found, drawn, best) && -best.score <= k_settle_far_cm) {
          add_kit_piece(out, *row, best.r, best.against, room.storey, room_index);
          kit_settle = true;
        }
      }
    }
    if (hearth != nullptr && !kit_settle) {
      const u64 drawn = draw(seed, k_draw_settle, room_index);
      for (const i32 along : {180, 150, 120}) {
        found.clear();
        candidates(place, PieceKind::Settle, along, k_settle_out_cm, found);
        for (Candidate& c : found)
          c.score = -apart(c.r, hearth->stack);
        if (choose(found, drawn, best) && -best.score <= k_settle_far_cm) {
          add_piece(out, PieceKind::Settle, best.r, best.against, room.storey, room_index);
          break;
        }
      }
    }
    // The table between its benches: under a window where one is free, else along any wall.
    const u64 drawn = draw(seed, k_draw_table, room_index);
    const auto under_window = [&]() {
      for (Candidate& c : found) {
        for (const OuterOpening& o : plan.openings) {
          if (o.door() || o.storey() != room.storey || o.side != c.against) continue;
          const bool along_x = o.side == Side::Front || o.side == Side::Back;
          const i32 lo = along_x ? c.r.x0 : c.r.z0;
          const i32 hi = along_x ? c.r.x1 : c.r.z1;
          if (o.at_cm >= lo + 30 && o.at_cm <= hi - 30) c.score = 1;
        }
      }
    };
    if (const KitRow* row = kit_row(PieceKind::TableSet); row != nullptr) {
      found.clear();
      candidates(place, PieceKind::TableSet, row->along_cm, row->out_cm, found);
      under_window();
      if (choose(found, drawn, best)) {
        add_kit_piece(out, *row, best.r, best.against, room.storey, room_index);
        return;
      }
    }
    for (const i32 along : {180, 150, 120}) {
      found.clear();
      candidates(place, PieceKind::TableSet, along, k_table_set_out_cm, found);
      under_window();
      if (choose(found, drawn, best)) {
        add_piece(out, PieceKind::TableSet, best.r, best.against, room.storey, room_index);
        break;
      }
    }
  };
  const auto sleep_in = [&]() {
    // A bed in the corner farthest from the room's way in, then a chest as near it as it fits.
    RectCm way_in = room.rect;
    i32 nearest = -1;
    for (const KeepOut& k : keep) {
      if (k.kind != Keep::Door || k.storey != room.storey || !overlaps(k.r, room.rect)) continue;
      const i32 d = apart(k.r, room.rect);
      if (nearest < 0 || d < nearest) {
        nearest = d;
        way_in = k.r;
      }
    }
    const u64 width_drawn = draw(seed, k_draw_bed_width, room_index);
    const u64 bed_drawn = draw(seed, k_draw_bed, room_index);
    // A mattress of 137, 122 or 91 cm in a frame 10 cm wider; a cot's one room takes the narrow.
    const i32 first = cot ? 101 : (width_drawn % 2 == 0 ? 147 : 132);
    const i32 second = cot ? 101 : (first == 147 ? 132 : 147);
    bool bedded = false;
    RectCm bed_at;
    const RectCm& f = place.open_floor;
    // A kit bed: its foot board is its front, so its head is to the wall it backs to.
    if (const KitRow* row = kit_row(PieceKind::Bed); row != nullptr) {
      found.clear();
      candidates(place, PieceKind::Bed, row->along_cm, row->out_cm, found);
      for (Candidate& c : found) {
        const bool along_x = c.against == Side::Front || c.against == Side::Back;
        const bool at_low = along_x ? c.r.x0 == f.x0 : c.r.z0 == f.z0;
        const bool at_high = along_x ? c.r.x1 == f.x1 : c.r.z1 == f.z1;
        c.score = apart(c.r, way_in) + ((at_low || at_high) ? 100000 : 0);
      }
      if (choose(found, bed_drawn, best)) {
        add_kit_piece(out, *row, best.r, best.against, room.storey, room_index);
        bed_at = best.r;
        bedded = true;
      }
    }
    for (const i32 width : {first, second, 101}) {
      if (bedded) break;
      // Its head to a wall, or its side along one with its head toward the nearer end; a corner
      // before the middle of a wall.
      found.clear();
      candidates(place, PieceKind::Bed, width, k_bed_out_cm, found);
      const u32 head_on = count_of(found);
      candidates(place, PieceKind::Bed, k_bed_out_cm, width, found);
      for (u32 i = 0; i < count_of(found); ++i) {
        Candidate& c = found[i];
        const bool along_x = c.against == Side::Front || c.against == Side::Back;
        const bool at_low = along_x ? c.r.x0 == f.x0 : c.r.z0 == f.z0;
        const bool at_high = along_x ? c.r.x1 == f.x1 : c.r.z1 == f.z1;
        if (i >= head_on) {
          // The side along the wall: the head is toward the end the bed touches, else the low.
          c.against = along_x ? (at_high && !at_low ? Side::Right : Side::Left)
                              : (at_high && !at_low ? Side::Back : Side::Front);
        }
        c.score = apart(c.r, way_in) + ((at_low || at_high) ? 100000 : 0);
      }
      if (choose(found, bed_drawn, best)) {
        add_piece(out, PieceKind::Bed, best.r, best.against, room.storey, room_index);
        bed_at = best.r;
        bedded = true;
        break;
      }
    }
    const u64 length_drawn = draw(seed, k_draw_chest_length, room_index);
    const u64 chest_drawn = draw(seed, k_draw_chest, room_index);
    const i32 longest = 90 + 10 * static_cast<i32>(length_drawn % 4);  // 90 to 120
    if (const KitRow* row = kit_row(PieceKind::Chest); row != nullptr) {
      found.clear();
      candidates(place, PieceKind::Chest, row->along_cm, row->out_cm, found);
      for (Candidate& c : found)
        c.score = bedded ? -apart(c.r, bed_at) : 0;
      if (choose(found, chest_drawn, best)) {
        add_kit_piece(out, *row, best.r, best.against, room.storey, room_index);
        return;
      }
    }
    for (i32 along = longest; along >= 70; along -= 10) {
      found.clear();
      candidates(place, PieceKind::Chest, along, k_chest_out_cm, found);
      for (Candidate& c : found)
        c.score = bedded ? -apart(c.r, bed_at) : 0;
      if (choose(found, chest_drawn, best)) {
        add_piece(out, PieceKind::Chest, best.r, best.against, room.storey, room_index);
        break;
      }
    }
  };
  // A cot's one room takes its bed before its table: a household sleeps before it sits down.
  if (cot) {
    sleep_in();
    live_in();
    return;
  }
  if (lived) live_in();
  if (slept) sleep_in();
}

void whole_piece(Furnishing& out, PieceKind kind, const RectCm& footprint, u8 storey, u32 room,
                 u32 first_member, i32 height) {
  Piece piece;
  piece.footprint = footprint;
  piece.room = room;
  piece.first_member = first_member;
  piece.member_count = count_of(out.members) - first_member;
  piece.height_cm = static_cast<u16>(height);
  piece.kind = kind;
  piece.storey = storey;
  out.pieces.push_back(piece);
}

void timber_member(Furnishing& out, const RectCm& r, i32 y0, i32 y1, u8 storey, u32 room,
                   MemberSurface surface) {
  Member m;
  m.x0 = r.x0, m.x1 = r.x1, m.z0 = r.z0, m.z1 = r.z1;
  m.y0 = y0, m.y1 = y1;
  m.room = room;
  m.surface = surface;
  m.storey = storey;
  out.members.push_back(m);
}

}  // namespace

const char* piece_kind_name(PieceKind kind) noexcept {
  switch (kind) {
    case PieceKind::TableSet: return "table-set";
    case PieceKind::Settle: return "settle";
    case PieceKind::Bed: return "bed";
    case PieceKind::Chest: return "chest";
    case PieceKind::Joists: return "joists";
    case PieceKind::Beam: return "beam";
    case PieceKind::Nosings: return "nosings";
    default: return "newel";
  }
}

void Furnishing::clear() noexcept {
  pieces.clear();
  members.clear();
}

bool add_piece(Furnishing& out, PieceKind kind, const RectCm& footprint, Side against, u8 storey,
               u32 room) {
  if (!is_furniture(kind)) return false;
  const bool along_x = against == Side::Front || against == Side::Back;
  const i32 along = along_x ? footprint.x1 - footprint.x0 : footprint.z1 - footprint.z0;
  const i32 depth = along_x ? footprint.z1 - footprint.z0 : footprint.x1 - footprint.x0;
  if (along < least_along(kind) || depth != out_of(kind)) return false;
  Builder b;
  b.f = &out;
  b.r = footprint;
  b.against = against;
  b.storey = storey;
  b.room = room;
  const u32 first_member = count_of(out.members);
  i32 height = 0;
  switch (kind) {
    case PieceKind::TableSet: height = table_set(b, along); break;
    case PieceKind::Settle: height = settle(b, along); break;
    case PieceKind::Bed: height = bed(b, along); break;
    default: height = chest(b, along); break;
  }
  whole_piece(out, kind, footprint, storey, room, first_member, height);
  out.pieces[count_of(out.pieces) - 1].against = against;
  return true;
}

void furnish_interior(const InteriorPlan& plan, u64 seed, Furnishing& out, const Kit& kit) {
  Vector<KeepOut> keep;
  keep_outs(plan, keep);
  for (u32 r = 0; r < count_of(plan.rooms); ++r)
    furnish_room(plan, keep, seed, r, kit, out);
}

i32 room_clear_cm(const InteriorPlan& plan, const Room& room) noexcept {
  const InteriorParams& p = plan.params;
  if (room.storey + 1 < p.storeys) return i32{p.storey_cm} - i32{p.floor_cm} - k_joist_depth_cm;
  return p.storeys > 1 ? i32{p.upper_clear_cm} : i32{p.storey_cm};
}

void timber_interior(const InteriorPlan& plan, bool ridge_along_x, Furnishing& out) {
  const InteriorParams& p = plan.params;
  // Under an upper floor: joists across the ridge at their centres, a room at a time, their ends
  // in the walls, and where they span more than a beam's least a spine beam under their middle.
  const i32 ceiling = i32{p.storey_cm} - i32{p.floor_cm};
  for (u32 r = 0; r < count_of(plan.rooms); ++r) {
    const Room& room = plan.rooms[r];
    if (room.storey + 1 >= p.storeys || room.kind == RoomKind::Stair) continue;
    const RectCm& rect = room.rect;
    const i32 length = ridge_along_x ? rect.x1 - rect.x0 : rect.z1 - rect.z0;
    const i32 span = ridge_along_x ? rect.z1 - rect.z0 : rect.x1 - rect.x0;
    const i32 count = length / k_joist_centres_cm;
    if (count < 1) continue;
    const i32 low = ridge_along_x ? rect.x0 : rect.z0;
    const i32 first = low + (length - (count - 1) * k_joist_centres_cm) / 2;
    const u32 first_member = count_of(out.members);
    for (i32 j = 0; j < count; ++j) {
      const i32 c = first + j * k_joist_centres_cm;
      const RectCm joist = ridge_along_x ? RectCm{c - k_joist_width_cm / 2, rect.z0 - k_into_cm,
                                                  c + k_joist_width_cm / 2, rect.z1 + k_into_cm}
                                         : RectCm{rect.x0 - k_into_cm, c - k_joist_width_cm / 2,
                                                  rect.x1 + k_into_cm, c + k_joist_width_cm / 2};
      timber_member(out, joist, ceiling - k_joist_depth_cm, ceiling + 1, room.storey, r,
                    MemberSurface::Timber);
    }
    whole_piece(out, PieceKind::Joists, rect, room.storey, r, first_member, ceiling);
    if (span < k_beam_least_span_cm) continue;
    const u32 beam_member = count_of(out.members);
    const i32 mid = ridge_along_x ? (rect.z0 + rect.z1) / 2 : (rect.x0 + rect.x1) / 2;
    const RectCm beam = ridge_along_x ? RectCm{rect.x0 - k_into_cm, mid - k_beam_width_cm / 2,
                                               rect.x1 + k_into_cm, mid + k_beam_width_cm / 2}
                                      : RectCm{mid - k_beam_width_cm / 2, rect.z0 - k_into_cm,
                                               mid + k_beam_width_cm / 2, rect.z1 + k_into_cm};
    timber_member(out, beam, ceiling - k_beam_depth_cm, ceiling + 1, room.storey, r,
                  MemberSurface::Timber);
    whole_piece(out, PieceKind::Beam, beam, room.storey, r, beam_member, ceiling);
  }

  // A stair's nosings, one a riser, the head landing's edge among them, and a half-newel against
  // the wall on the climber's right at the first riser and at the last.
  const i32 half_wall = i32{p.inner_wall_cm} / 2;
  for (const Stair& s : plan.stairs) {
    const bool along_z = s.rises == Side::Front || s.rises == Side::Back;
    const bool toward_low = s.rises == Side::Front || s.rises == Side::Left;
    const i32 across_lo = (along_z ? s.strip.x0 : s.strip.z0) + half_wall;
    const i32 across_hi = (along_z ? s.strip.x1 : s.strip.z1) - half_wall;
    const u32 first_member = count_of(out.members);
    for (u32 i = 0; i < s.risers; ++i) {
      const RectCm tread = s.tread(i);
      // The riser's line: the tread's edge on the side the climber comes from.
      const i32 line =
          along_z ? (toward_low ? tread.z1 : tread.z0) : (toward_low ? tread.x1 : tread.x0);
      const i32 lo = toward_low ? line : line - k_nosing_cm;
      const i32 hi = toward_low ? line + k_nosing_cm : line;
      const i32 top = (s.tread_mm(i) + 5) / 10;
      const RectCm nosing = along_z ? RectCm{across_lo - 1, lo, across_hi + 1, hi}
                                    : RectCm{lo, across_lo - 1, hi, across_hi + 1};
      timber_member(out, nosing, top - k_nosing_depth_cm, top, s.from_storey, s.lower_room,
                    MemberSurface::Boards);
    }
    whole_piece(out, PieceKind::Nosings, s.flight, s.from_storey, s.lower_room, first_member,
                s.rise_mm / 10);
    // The climber's right: +x going toward -z, -x toward +z, -z toward -x, +z toward +x.
    const bool right_high = s.rises == Side::Front || s.rises == Side::Right;
    const i32 wall_lo = right_high ? across_hi - k_newel_out_cm : across_lo - 1;
    const i32 wall_hi = right_high ? across_hi + 1 : across_lo + k_newel_out_cm;
    const i32 first_line = along_z ? (toward_low ? s.flight.z1 : s.flight.z0)
                                   : (toward_low ? s.flight.x1 : s.flight.x0);
    const i32 last_line = along_z ? (toward_low ? s.flight.z0 : s.flight.z1)
                                  : (toward_low ? s.flight.x0 : s.flight.x1);
    const i32 rise_cm = s.rise_mm / 10;
    const i32 step_cm = (s.rise_mm / i32{s.risers} + 5) / 10;
    for (u32 end = 0; end < 2; ++end) {
      // Astride its riser's line: five centimetres of wall stand between a landing's doorway and
      // the flight, and a post must not stand in the doorway.
      const i32 line = end == 0 ? first_line : last_line;
      const i32 toward_foot = end == 0 ? 5 : 4;
      const i32 lo = toward_low ? line - (k_newel_cm - toward_foot) : line - toward_foot;
      const i32 hi = lo + k_newel_cm;
      const RectCm post =
          along_z ? RectCm{wall_lo, lo, wall_hi, hi} : RectCm{lo, wall_lo, hi, wall_hi};
      const i32 base = end == 0 ? 0 : rise_cm - step_cm - 2;
      const i32 top = (end == 0 ? step_cm : rise_cm) + k_newel_height_cm;
      const u32 newel_member = count_of(out.members);
      timber_member(out, post, base, top, s.from_storey, s.lower_room, MemberSurface::Timber);
      whole_piece(out, PieceKind::Newel, post, s.from_storey, s.lower_room, newel_member, top);
    }
  }
}

bool validate_furnishing(const InteriorPlan& plan, const Furnishing& furnishing, std::string* why) {
  const auto fail = [why](u32 piece, const char* kind, const char* what) {
    if (why != nullptr) {
      *why = "piece " + std::to_string(piece) + " (" + kind + "): " + what;
    }
    return false;
  };
  Vector<KeepOut> keep;
  keep_outs(plan, keep);
  for (u32 i = 0; i < count_of(furnishing.pieces); ++i) {
    const Piece& piece = furnishing.pieces[i];
    const char* name = piece_kind_name(piece.kind);
    const bool kit = piece.kit != 0;
    if (kit && (!is_furniture(piece.kind) || piece.member_count != 0)) {
      return fail(i, name, "a kit piece stands in a furniture piece's place and has no members");
    }
    if (!kit && (piece.first_member + piece.member_count > count_of(furnishing.members) ||
                 piece.member_count == 0)) {
      return fail(i, name, "its members are not in the list");
    }
    u32 solids = 0;
    for (u32 m = piece.first_member; m < piece.first_member + piece.member_count; ++m) {
      const Member& member = furnishing.members[m];
      if (member.x0 >= member.x1 || member.y0 >= member.y1 || member.z0 >= member.z1) {
        return fail(i, name, "a member is empty");
      }
      solids += member.solid ? 1u : 0u;
    }
    if (!is_furniture(piece.kind)) {
      if (solids != 0) return fail(i, name, "timber is not walked into");
      continue;
    }
    if (!kit && solids == 0) return fail(i, name, "no member of it is solid");
    if (piece.room >= count_of(plan.rooms)) return fail(i, name, "its room does not exist");
    const Room& room = plan.rooms[piece.room];
    if (room.storey != piece.storey) return fail(i, name, "its room is on another storey");
    if (kit && i32{piece.height_cm} + k_kit_head_gap_cm > room_clear_cm(plan, room)) {
      return fail(i, name, "a kit piece stands into its room's joists or ceiling");
    }
    if (!inside(piece.footprint, free_floor(plan, room))) {
      return fail(i, name, "it does not stand on its room's free floor");
    }
    for (const KeepOut& k : keep) {
      if (k.storey != piece.storey || !overlaps(piece.footprint, k.r)) continue;
      switch (k.kind) {
        case Keep::Tour: return fail(i, name, "it stands in the tour's way");
        case Keep::Door: return fail(i, name, "it stands in a door's or a doorway's approach");
        case Keep::Hearth: return fail(i, name, "it stands before a hearth");
        case Keep::Stack: return fail(i, name, "it stands in a stack");
        case Keep::Led: return fail(i, name, "it stands before the window a walker is led to");
        default:
          if (window_keeps(piece.kind)) return fail(i, name, "it stands before a window");
          break;
      }
    }
    for (u32 j = 0; j < i; ++j) {
      const Piece& other = furnishing.pieces[j];
      if (!is_furniture(other.kind) || other.storey != piece.storey) continue;
      if (overlaps(piece.footprint, other.footprint)) {
        return fail(i, name, "it stands in another piece");
      }
    }
  }
  return true;
}

const char* place_kind_name(PlaceKind kind) noexcept {
  switch (kind) {
    case PlaceKind::HearthRecess: return "hearth-recess";
    case PlaceKind::HearthMouth: return "hearth-mouth";
    case PlaceKind::HearthCheek: return "hearth-cheek";
    case PlaceKind::StackSide: return "stack-side";
    case PlaceKind::HearthApron: return "hearth-apron";
    case PlaceKind::TableTop: return "table-top";
    case PlaceKind::BenchSeat: return "bench-seat";
    case PlaceKind::SettleSeat: return "settle-seat";
    case PlaceKind::ChestLid: return "chest-lid";
    case PlaceKind::Mattress: return "mattress";
    case PlaceKind::JoistUnderside: return "joist-underside";
    case PlaceKind::DoorApproach: return "door-approach";
    case PlaceKind::DoorwayApproach: return "doorway-approach";
    default: return "window-light";
  }
}

void house_places(const InteriorPlan& plan, const Furnishing& furnishing, Vector<NamedPlace>& out) {
  const auto add = [&out](PlaceKind kind, const RectCm& r, i32 y0_mm, i32 y1_mm, u32 room,
                          u8 storey, Side faces, u32 index) {
    NamedPlace n;
    n.rect = r;
    n.y0_mm = y0_mm;
    n.y1_mm = y1_mm;
    n.room = room;
    n.index = index;
    n.kind = kind;
    n.faces = faces;
    n.storey = storey;
    out.push_back(n);
  };
  // **The hearths**, in the frame of the way each faces: `along` across its opening, `into` the
  // stack from its face. The recess and the cheeks are the mesher's (structures.md, "Chimneys"):
  // a recess `k_hearth_recess_cm` deep and as wide as the opening, on a hearth raised 3 cm, and the
  // cheeks the face either side of it.
  for (u32 hi = 0; hi < count_of(plan.hearths); ++hi) {
    const Hearth& h = plan.hearths[hi];
    const RectCm& st = h.stack;
    const bool across_x = h.faces == Side::Front || h.faces == Side::Back;
    const i32 lo = across_x ? st.x0 : st.z0, hi_edge = across_x ? st.x1 : st.z1;
    const i32 mid = (lo + hi_edge) / 2, half = i32{h.opening_cm} / 2;
    // The face's line and the sense from it into the stack.
    i32 face = 0, sense = 0;
    switch (h.faces) {
      case Side::Front: face = st.z0, sense = 1; break;
      case Side::Back: face = st.z1, sense = -1; break;
      case Side::Left: face = st.x0, sense = 1; break;
      default: face = st.x1, sense = -1; break;
    }
    const auto box = [&](i32 a0, i32 a1, i32 b0, i32 b1) {
      const i32 v0 = b0 < b1 ? b0 : b1, v1 = b0 < b1 ? b1 : b0;
      return across_x ? RectCm{a0, v0, a1, v1} : RectCm{v0, a0, v1, a1};
    };
    const i32 lintel_mm = i32{h.lintel_cm} * 10;
    add(PlaceKind::HearthRecess,
        box(mid - half, mid + half, face, face + sense * k_hearth_recess_cm), k_raised_hearth_mm,
        lintel_mm, h.room, h.storey, h.faces, hi);
    add(PlaceKind::HearthMouth, box(mid - half, mid + half, face, face), k_raised_hearth_mm,
        lintel_mm, h.room, h.storey, h.faces, hi);
    if (mid - half > lo)
      add(PlaceKind::HearthCheek, box(lo, mid - half, face, face), 0, lintel_mm, h.room, h.storey,
          h.faces, hi);
    if (mid + half < hi_edge)
      add(PlaceKind::HearthCheek, box(mid + half, hi_edge, face, face), 0, lintel_mm, h.room,
          h.storey, h.faces, hi);
    // Beside the stack: the floor along its two sides, from its face back to its wall, 60 cm out,
    // inside the room's free floor (a bellows, a log basket, the wall a salt box hangs on).
    const RectCm room_floor =
        h.room < count_of(plan.rooms) ? free_floor(plan, plan.rooms[h.room]) : st;
    const i32 back = face + sense * (across_x ? st.z1 - st.z0 : st.x1 - st.x0);
    for (const i32 side : {-1, 1}) {
      const i32 a0 = side < 0 ? lo - 60 : hi_edge, a1 = side < 0 ? lo : hi_edge + 60;
      RectCm r = box(a0, a1, face, back);
      r.x0 = r.x0 < room_floor.x0 ? room_floor.x0 : r.x0;
      r.z0 = r.z0 < room_floor.z0 ? room_floor.z0 : r.z0;
      r.x1 = r.x1 > room_floor.x1 ? room_floor.x1 : r.x1;
      r.z1 = r.z1 > room_floor.z1 ? room_floor.z1 : r.z1;
      if (r.x1 - r.x0 >= 10 && r.z1 - r.z0 >= 10)
        add(PlaceKind::StackSide, r, 0, 0, h.room, h.storey, h.faces, hi);
    }
    add(PlaceKind::HearthApron,
        box(lo - k_hearth_side_cm, hi_edge + k_hearth_side_cm, face,
            face - sense * k_hearth_reach_cm),
        0, 0, h.room, h.storey, h.faces, hi);
  }
  // **The furniture's surfaces**, read from the members `add_piece` built (furnish.h): a table
  // set's top and its benches' seats, a settle's seat, the mattress, the chest's lid.
  for (u32 pi = 0; pi < count_of(furnishing.pieces); ++pi) {
    const Piece& piece = furnishing.pieces[pi];
    const u32 end = piece.first_member + piece.member_count;
    if (end > count_of(furnishing.members)) continue;
    if (piece.kind == PieceKind::Joists || piece.kind == PieceKind::Beam) {
      for (u32 m = piece.first_member; m < end; ++m) {
        const Member& mm = furnishing.members[m];
        add(PlaceKind::JoistUnderside, RectCm{mm.x0, mm.z0, mm.x1, mm.z1}, mm.y0 * 10, mm.y0 * 10,
            piece.room, piece.storey, piece.against, pi);
      }
      continue;
    }
    if (!is_furniture(piece.kind)) continue;
    i32 lid = -1;  // a chest's topmost member
    for (u32 m = piece.first_member; m < end; ++m) {
      const Member& mm = furnishing.members[m];
      const RectCm r{mm.x0, mm.z0, mm.x1, mm.z1};
      if (piece.kind == PieceKind::Chest) {
        if (lid < 0 || mm.y1 > furnishing.members[static_cast<u32>(lid)].y1)
          lid = static_cast<i32>(m);
        continue;
      }
      if (!mm.solid) continue;
      PlaceKind kind = PlaceKind::Mattress;
      if (piece.kind == PieceKind::TableSet)
        kind = mm.y1 > 60 ? PlaceKind::TableTop : PlaceKind::BenchSeat;
      else if (piece.kind == PieceKind::Settle)
        kind = PlaceKind::SettleSeat;
      add(kind, r, mm.y1 * 10, mm.y1 * 10, piece.room, piece.storey, piece.against, pi);
    }
    if (lid >= 0) {
      const Member& mm = furnishing.members[static_cast<u32>(lid)];
      add(PlaceKind::ChestLid, RectCm{mm.x0, mm.z0, mm.x1, mm.z1}, mm.y1 * 10, mm.y1 * 10,
          piece.room, piece.storey, piece.against, pi);
    }
  }
  // **The floors kept clear**: the furniture's own keep-outs, named.
  Vector<KeepOut> keep;
  keep_outs(plan, keep);
  u32 door = 0, window = 0, doorway = 0;
  for (const KeepOut& k : keep) {
    if (k.kind == Keep::Door) {
      // The doorways' come first in `keep_outs`, then the outer openings'.
      if (doorway < count_of(plan.doorways)) {
        const InnerWall& w = plan.walls[plan.doorways[doorway].wall];
        add(PlaceKind::DoorwayApproach, k.r, 0, 0, w.a, k.storey, Side::Front, doorway);
        ++doorway;
        continue;
      }
    }
    if (k.kind != Keep::Door && k.kind != Keep::Window && k.kind != Keep::Led) continue;
    // An outer opening's: find it in order.
    while (door + window < count_of(plan.openings)) {
      const OuterOpening& o = plan.openings[door + window];
      const u32 index = door + window;
      if (o.door()) {
        ++door;
        add(PlaceKind::DoorApproach, k.r, 0, 0, o.room, k.storey, o.side, index);
      } else {
        ++window;
        add(PlaceKind::WindowLight, k.r, i32{o.sill_cm} * 10, i32{o.sill_cm} * 10, o.room, k.storey,
            o.side, index);
      }
      break;
    }
  }
}

u64 hash_furnishing(const Furnishing& furnishing) noexcept {
  u64 h = hash_combine(k_hash_seed, u64{k_furnish_version});
  const auto pair = [&h](i32 a, i32 b) {
    h = hash_combine(h, u64{static_cast<u32>(a)} << 32 | static_cast<u32>(b));
  };
  for (const Piece& p : furnishing.pieces) {
    pair(p.footprint.x0, p.footprint.z0);
    pair(p.footprint.x1, p.footprint.z1);
    pair(static_cast<i32>(p.room), static_cast<i32>(p.first_member));
    pair(static_cast<i32>(p.member_count), i32{p.height_cm});
    // A kit piece's row in the word's high bits, so a furnishing with no kit hashes as it did.
    h = hash_combine(h, u64{p.kit} << 24 | u64{static_cast<u8>(p.kind)} << 16 |
                            u64{static_cast<u8>(p.against)} << 8 | u64{p.storey});
  }
  for (const Member& m : furnishing.members) {
    pair(m.x0, m.y0);
    pair(m.z0, m.x1);
    pair(m.y1, m.z1);
    h = hash_combine(h, u64{m.room} << 32 | u64{static_cast<u8>(m.surface)} << 16 |
                            u64{m.storey} << 8 | (m.solid ? 1u : 0u));
  }
  return h;
}

}  // namespace engine::dwellings
