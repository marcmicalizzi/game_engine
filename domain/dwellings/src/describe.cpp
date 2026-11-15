// A plan as a description (describe.h). Integer centimetres throughout; the mesher's conventions
// are structures.md's: walls on centre lines, a wall's `a` the room on the left of its direction
// seen from above (the left of +x is -z, the left of +z is +x), an opening's `at` along its wall
// from the wall's `from`.

#include <core/hash/hash.h>
#include <domain/dwellings/describe.h>
#include <domain/dwellings/furnish.h>
#include <domain/structures/structures.h>

namespace engine::dwellings {

namespace {

template <class T>
u32 count_of(const Vector<T>& v) noexcept {
  return static_cast<u32>(v.size());
}

constexpr i32 k_stack_proud_cm = 8;    // a wall stack stands this far out of its wall's outer face
constexpr i32 k_over_roof_cm = 75;     // a stack's top over the roof where it comes through
constexpr i32 k_ground_floor_cm = 20;  // the ground storey's slab
constexpr i32 k_eave_clear_cm = 2;     // an eave's edge over the head of a door under it

// The exterior walls of a storey, in the footprint's order: counter-clockwise seen from above.
constexpr u32 k_wall_left = 0;
constexpr u32 k_wall_back = 1;
constexpr u32 k_wall_right = 2;
constexpr u32 k_wall_front = 3;

u32 wall_of(Side side) noexcept {
  switch (side) {
    case Side::Left: return k_wall_left;
    case Side::Back: return k_wall_back;
    case Side::Right: return k_wall_right;
    default: return k_wall_front;
  }
}

// A direction as the description's quarter turn: 0 +x, 1 +z, 2 -x, 3 -z.
u8 quarter_of(Side looks) noexcept {
  switch (looks) {
    case Side::Right: return 0;
    case Side::Back: return 1;
    case Side::Left: return 2;
    default: return 3;
  }
}

structure::RoomKind kind_of(RoomKind kind) noexcept {
  switch (kind) {
    case RoomKind::Hall: return structure::RoomKind::hall;
    case RoomKind::Parlour: return structure::RoomKind::parlour;
    case RoomKind::Inner: return structure::RoomKind::parlour;
    case RoomKind::Kitchen: return structure::RoomKind::kitchen;
    case RoomKind::Chamber: return structure::RoomKind::chamber;
    case RoomKind::Service: return structure::RoomKind::service;
    default: return structure::RoomKind::passage;  // a passage, and a stair's compartment
  }
}

// What one house's shell differs from its neighbour's in, drawn from the house's seed: one draw
// a statement, a purpose each (dwellings.md, "What a house's seed varies in its shell").
constexpr u64 k_draw_wave = 301;
constexpr u64 k_draw_batter = 303;
constexpr u64 k_draw_arris = 304;
constexpr u64 k_draw_plinth = 305;
constexpr u64 k_draw_windows = 306;
constexpr u64 k_draw_inner_leaf = 307;
constexpr u64 k_draw_inner_side = 308;

u64 draw(u64 seed, u64 purpose, u32 index = 0) noexcept {
  return mix64(hash_combine(hash_combine(hash_combine(k_hash_seed, seed), purpose), u64{index}));
}

struct Dress {
  i32 wave_mm = 0;            // the exterior walls' faces, hollow to swell
  i32 partition_wave_mm = 0;  // a thin partition's
  i32 top_thickness = 0;      // a batter: the exterior walls' thickness at the plate
  i32 arris = 0;              // the outer corners' radius
  i32 plinth_proud = 0;
  bool mullions = false;  // unglazed windows with mullions; else glazing bars
  bool shutters = false;  // a shutter or a pair on every window 70 cm wide or more
};

Dress dress_of(u64 seed, i32 wall, u8 storeys) noexcept {
  Dress d;
  // Hand-laid walls under a render: 8, 12 or 16 mm, 12 the commonest.
  constexpr i32 k_waves[4] = {8, 12, 12, 16};
  d.wave_mm = k_waves[draw(seed, k_draw_wave) % 4];
  // A partition stays flat: waved, its two faces cost a house of nine rooms two thousand
  // triangles for 6 mm on a board 10 cm thick.
  d.partition_wave_mm = 0;
  // A batter is a wall of one storey's (the mesher carries a lean cleanly on one): two houses of
  // three, three quarters of the foot's thickness at the plate, as cob's.
  if (storeys == 1 && draw(seed, k_draw_batter) % 3 != 0) {
    d.top_thickness = wall * 3 / 4 < 25 ? 25 : wall * 3 / 4;
  }
  // Corners as round as cob's under a batter, else by the draw.
  d.arris = d.top_thickness != 0 ? 12 : (draw(seed, k_draw_arris) % 2 == 0 ? 6 : 12);
  constexpr i32 k_proud[4] = {0, 3, 4, 5};
  d.plinth_proud = k_proud[draw(seed, k_draw_plinth) % 4];
  const u64 windows = draw(seed, k_draw_windows) % 4;
  d.mullions = windows == 1 || windows == 2;
  d.shutters = windows == 1 || windows == 3;
  return d;
}

bool touches(const InteriorPlan& plan, const Room& room, Side side) noexcept {
  switch (side) {
    case Side::Front: return room.rect.z0 == 0;
    case Side::Back: return room.rect.z1 == plan.params.depth_cm;
    case Side::Left: return room.rect.x0 == 0;
    default: return room.rect.x1 == plan.params.width_cm;
  }
}

}  // namespace

i32 max_clear_cm(u16 outer_wall_cm) noexcept {
  const i32 wall = i32{outer_wall_cm};
  return (structures::k_reach_cm - wall - wall / 2 - k_stack_proud_cm) / 10 * 10;
}

void describe(const InteriorPlan& plan, const RuralChoice& choice, std::string_view name, u64 seed,
              structure::Description& out) {
  const InteriorParams& p = plan.params;
  out = structure::Description{};
  const i32 wall = i32{choice.outer_wall_cm};
  const i32 off = wall / 2;
  const i32 a = p.width_cm + wall;  // the centre lines' rectangle
  const i32 b = p.depth_cm + wall;
  out.name = std::string(name);
  out.style = std::string("rural/") + plan_type_name(p.type);
  const Dress dress = choice.dressed ? dress_of(seed, wall, p.storeys) : Dress{};
  out.arris = dress.arris;
  out.plinth_proud = dress.plinth_proud;
  const i32 corners[4][2] = {{0, 0}, {0, b}, {a, b}, {a, 0}};
  for (const auto& c : corners) {
    out.footprint.push_back(c[0]);
    out.footprint.push_back(c[1]);
  }

  // Storeys. A one-room house is open to its roof; rooms divided by walls are ceiled.
  structure::Storey ground;
  ground.elevation = 0;
  ground.height = p.storey_cm;
  ground.floor_thickness = k_ground_floor_cm;
  ground.ceiling = p.storeys == 1 && count_of(plan.rooms) > 1;
  out.storeys.push_back(ground);
  if (p.storeys == 2) {
    structure::Storey above;
    above.elevation = p.storey_cm;
    above.height = p.upper_clear_cm;
    above.floor_thickness = p.floor_cm;
    above.ceiling = true;
    out.storeys.push_back(above);
  }
  const structure::Storey& top = out.storeys[count_of(out.storeys) - 1];
  const i32 eaves = top.elevation + top.height;

  // Rooms, on the walls' centre lines.
  auto on_lines_x = [&](i32 x) { return x == 0 ? 0 : (x == p.width_cm ? a : x + off); };
  auto on_lines_z = [&](i32 z) { return z == 0 ? 0 : (z == p.depth_cm ? b : z + off); };
  for (u32 i = 0; i < count_of(plan.rooms); ++i) {
    const Room& r = plan.rooms[i];
    structure::Room room;
    room.id = i;
    room.storey = r.storey;
    room.kind = kind_of(r.kind);
    room.name = room_kind_name(r.kind);
    const i32 x0 = on_lines_x(r.rect.x0);
    const i32 x1 = on_lines_x(r.rect.x1);
    const i32 z0 = on_lines_z(r.rect.z0);
    const i32 z1 = on_lines_z(r.rect.z1);
    const i32 outline[8] = {x0, z0, x0, z1, x1, z1, x1, z0};
    for (const i32 v : outline)
      room.outline.push_back(v);
    out.rooms.push_back(room);
  }

  // Exterior walls: four a storey. A wall names one room: the one its door opens into, else the
  // first that meets it.
  for (u8 storey = 0; storey < p.storeys; ++storey) {
    constexpr Side k_sides[4] = {Side::Left, Side::Back, Side::Right, Side::Front};
    for (u32 i = 0; i < 4; ++i) {
      structure::Wall w;
      w.storey = storey;
      w.from[0] = corners[i][0];
      w.from[1] = corners[i][1];
      w.to[0] = corners[(i + 1) % 4][0];
      w.to[1] = corners[(i + 1) % 4][1];
      w.thickness = wall;
      w.kind = structure::WallKind::exterior;
      w.b = -1;
      w.wave = dress.wave_mm;
      w.top_thickness = dress.top_thickness;
      for (u32 r = 0; r < count_of(plan.rooms) && w.a < 0; ++r) {
        if (plan.rooms[r].storey == storey && touches(plan, plan.rooms[r], k_sides[i])) {
          w.a = static_cast<i32>(r);
        }
      }
      for (const OuterOpening& o : plan.openings) {
        if (o.door() && o.storey() == storey && o.side == k_sides[i]) {
          w.a = static_cast<i32>(o.room);
        }
      }
      out.walls.push_back(w);
    }
  }
  const u32 first_inner = count_of(out.walls);
  for (const InnerWall& iw : plan.walls) {
    structure::Wall w;
    w.storey = iw.storey;
    w.thickness = iw.thickness_cm;
    w.kind = structure::WallKind::inner;
    w.wave = dress.partition_wave_mm;
    if (iw.x0 == iw.x1) {
      // Along +z, from the front wall's line to the back's: the left of +z is +x.
      w.from[0] = iw.x0 + off;
      w.from[1] = 0;
      w.to[0] = iw.x0 + off;
      w.to[1] = b;
      w.a = static_cast<i32>(iw.b);
      w.b = static_cast<i32>(iw.a);
    } else {
      // Along +x, from the left wall's line to the right's: the left of +x is -z.
      w.from[0] = 0;
      w.from[1] = iw.z0 + off;
      w.to[0] = a;
      w.to[1] = iw.z0 + off;
      w.a = static_cast<i32>(iw.a);
      w.b = static_cast<i32>(iw.b);
    }
    out.walls.push_back(w);
  }

  // Openings.
  for (const OuterOpening& o : plan.openings) {
    structure::Opening opening;
    opening.wall = u32{o.storey()} * 4 + wall_of(o.side);
    opening.kind = o.door() ? structure::OpeningKind::door : structure::OpeningKind::window;
    switch (o.side) {
      case Side::Left:
      case Side::Back: opening.at = off + o.at_cm; break;
      case Side::Right: opening.at = p.depth_cm + off - o.at_cm; break;
      default: opening.at = p.width_cm + off - o.at_cm; break;
    }
    opening.width = o.width_cm;
    opening.sill = o.sill_cm;
    opening.head = o.head_cm;
    if (choice.dressed && o.door()) {
      // A ledged leaf, open a quarter turn (the walk goes through it), opening inward, hung on
      // the jamb nearer its room's corner so the open leaf stands by a wall.
      opening.leaf = structure::LeafKind::ledged;
      opening.leaf_open = 90;
      const Room& room = plan.rooms[o.room];
      const bool along_x = o.side == Side::Front || o.side == Side::Back;
      const i32 low_gap = o.at_cm - i32{o.width_cm} / 2 - (along_x ? room.rect.x0 : room.rect.z0);
      const i32 high_gap = (along_x ? room.rect.x1 : room.rect.z1) - o.at_cm - i32{o.width_cm} / 2;
      const bool hinge_low = low_gap <= high_gap;
      // The left and the back walls run toward high z and high x; the right and the front back.
      const bool wall_rises = o.side == Side::Left || o.side == Side::Back;
      opening.hinge_at_to = wall_rises ? !hinge_low : hinge_low;
    } else if (choice.dressed) {
      // One mullion to about every 17 cm of light; a shutter on a window 70 cm wide or more (a
      // narrower one's would stand out of its reveal), a pair on one of 80, folded back.
      const i32 light = i32{o.width_cm} - 10;
      if (dress.mullions && light >= 12 * (light / 17 + 1)) {
        opening.mullions = static_cast<u32>(light / 17);
      }
      if (dress.shutters && o.width_cm >= 70) {
        opening.leaf = structure::LeafKind::ledged;
        opening.leaves = o.width_cm >= 80 ? u8{2} : u8{1};
        opening.leaf_open = 90;
      }
    }
    out.openings.push_back(opening);
  }
  for (const Doorway& d : plan.doorways) {
    structure::Opening opening;
    opening.wall = first_inner + d.wall;
    opening.kind = structure::OpeningKind::doorway;
    opening.at = off + d.at_cm;
    opening.width = d.width_cm;
    opening.sill = 0;
    opening.head = d.head_cm;
    // A partition's doorway, two of three, is a door: a frame and a ledged leaf open a quarter
    // turn, hung on the jamb nearer the outer wall, opening into either room by the draw. A
    // landing's doorway stays bare: its 80 cm less a frame is under what the walker passes.
    const u32 index = count_of(out.openings);
    if (choice.dressed && d.width_cm >= p.doorway_cm && p.doorway_cm >= 85 &&
        draw(seed, k_draw_inner_leaf, index) % 3 != 0) {
      const InnerWall& iw = plan.walls[d.wall];
      const i32 length = iw.x0 == iw.x1 ? p.depth_cm : p.width_cm;
      opening.kind = structure::OpeningKind::door;
      opening.leaf = structure::LeafKind::ledged;
      opening.leaf_open = 90;
      opening.hinge_at_to = d.at_cm > length / 2;
      opening.opens_right = draw(seed, k_draw_inner_side, index) % 2 == 0;
    }
    out.openings.push_back(opening);
  }

  // The stair: its steps run into the strip's two walls by a centimetre, so no face of a step lies
  // in a wall's face.
  for (const Stair& s : plan.stairs) {
    structure::Stair stair;
    stair.kind = structure::StairKind::straight;
    stair.storey = s.from_storey;
    const bool along_z = s.rises == Side::Front || s.rises == Side::Back;
    const i32 mid_x = (s.strip.x0 + s.strip.x1) / 2;
    const i32 mid_z = (s.strip.z0 + s.strip.z1) / 2;
    switch (s.rises) {
      case Side::Front:
        stair.foot[0] = mid_x + off;
        stair.foot[1] = s.flight.z1 + off;
        break;
      case Side::Back:
        stair.foot[0] = mid_x + off;
        stair.foot[1] = s.flight.z0 + off;
        break;
      case Side::Left:
        stair.foot[0] = s.flight.x1 + off;
        stair.foot[1] = mid_z + off;
        break;
      default:
        stair.foot[0] = s.flight.x0 + off;
        stair.foot[1] = mid_z + off;
        break;
    }
    stair.dir = quarter_of(s.rises);
    const i32 across = along_z ? s.strip.x1 - s.strip.x0 : s.strip.z1 - s.strip.z0;
    stair.width = across - p.inner_wall_cm + 2;
    stair.risers = s.risers;
    stair.going = k_stair_going_cm;
    const RectCm well = s.well();
    stair.well[0] = well.x0 + off;
    stair.well[1] = well.z0 + off;
    stair.well[2] = well.x1 + off;
    stair.well[3] = well.z1 + off;
    stair.from_room = static_cast<i32>(s.lower_room);
    stair.to_room = static_cast<i32>(s.upper_room);
    out.stairs.push_back(stair);
  }

  // One gable roof, its ridge along the longer side; the covering's numbers are the mesher's
  // example's (docs/experiments/vernacular-shell.md).
  structure::Roof roof = structures::example_cot().roofs[0];
  roof.form = structure::RoofForm::gable;
  roof.rect[0] = -off;
  roof.rect[1] = -off;
  roof.rect[2] = a + off;
  roof.rect[3] = b + off;
  roof.ridge_along_x = choice.ridge_along_front;
  roof.eaves = eaves;
  // A way out stands under no eave lower than its own head (the mesher's validator refuses one):
  // the eave's edge hangs pitch * overhang under the plate, so where an outer door stands in a wall
  // the eave runs along, on the storey the roof sits on, the overhang is cut back until the edge
  // clears the door's head by `k_eave_clear_cm`. A house of one storey is the case: at a 2.4 m
  // plate under a fifty degree thatch a 2.0 m head leaves 32 cm of overhang.
  for (const OuterOpening& o : plan.openings) {
    if (!o.door() || o.storey() + 1 != p.storeys) continue;
    const bool under_eave = (o.side == Side::Front || o.side == Side::Back) == roof.ridge_along_x;
    if (!under_eave) continue;
    const i32 drop = eaves - (top.elevation + i32{o.head_cm}) - k_eave_clear_cm;
    const i32 most = drop > 0 ? drop * 1000 / roof.pitch + 1 : 0;
    if (roof.overhang > most) roof.overhang = most;
  }
  out.roofs.push_back(roof);

  // A chimney a hearth: a wall stack runs through its wall and stands proud of it outside; every
  // stack rises from the footing to 75 cm over the roof where it comes through.
  for (const Hearth& h : plan.hearths) {
    structure::Chimney c;
    c.rect[0] = h.stack.x0 == 0 ? -off - k_stack_proud_cm : h.stack.x0 + off;
    c.rect[1] = h.stack.z0 == 0 ? -off - k_stack_proud_cm : h.stack.z0 + off;
    c.rect[2] = h.stack.x1 == p.width_cm ? a + off + k_stack_proud_cm : h.stack.x1 + off;
    c.rect[3] = h.stack.z1 == p.depth_cm ? b + off + k_stack_proud_cm : h.stack.z1 + off;
    c.base = -out.foundation;
    // The roof's height over the stack's highest part: its run in from the nearer eave.
    const i32 lo = roof.ridge_along_x ? c.rect[1] : c.rect[0];
    const i32 hi = roof.ridge_along_x ? c.rect[3] : c.rect[2];
    const i32 eave_lo = roof.ridge_along_x ? roof.rect[1] : roof.rect[0];
    const i32 eave_hi = roof.ridge_along_x ? roof.rect[3] : roof.rect[2];
    const i32 half = (eave_hi - eave_lo) / 2;
    const i32 ridge = eave_lo + half;
    i32 run = half;
    if (hi < ridge) run = hi - eave_lo;
    if (lo > ridge) run = eave_hi - lo;
    c.top = eaves + roof.pitch * run / 1000 + 2 * roof.thickness + k_over_roof_cm;
    c.hearth_side = quarter_of(h.faces);
    c.hearth_storey = h.storey;
    c.hearth_width = h.opening_cm;
    c.hearth_head = h.lintel_cm;
    // The recess, the raised hearth and the timber over the opening are the mesher's, from these
    // fields: its default recess of 40 cm is the depth a wall stack stands into its room.
    out.chimneys.push_back(c);
  }

  // The timber a room shows and its furniture (furnish.h), as plain members: each a box in the
  // room it stands in, the one that covers its piece's footprint solid, so the walk records carry
  // the furniture and a walker meets it.
  Furnishing furnishing;
  if (choice.timber) timber_interior(plan, choice.ridge_along_front, furnishing);
  if (choice.furniture) furnish_interior(plan, seed, furnishing);
  for (const Member& m : furnishing.members) {
    structure::Box box;
    const i32 floor_top = out.storeys[m.storey].elevation;
    box.min[0] = m.x0 + off;
    box.min[1] = floor_top + m.y0;
    box.min[2] = m.z0 + off;
    box.max[0] = m.x1 + off;
    box.max[1] = floor_top + m.y1;
    box.max[2] = m.z1 + off;
    switch (m.surface) {
      case MemberSurface::Boards: box.surface = structure::Surface::boards; break;
      case MemberSurface::Linen: box.surface = structure::Surface::ceiling; break;
      default: box.surface = structure::Surface::timber; break;
    }
    box.solid = m.solid;
    box.room = static_cast<i32>(m.room);
    out.boxes.push_back(box);
  }

  out.provenance.filler = "rural";
  out.provenance.version = k_rural_filler_version;
  out.provenance.seed = seed;
}

bool fill_rural(const RuralPlot& plot, const Household& household, const RuralStyle& style,
                u64 seed, std::string_view name, structure::Description& out, std::string* error) {
  RuralChoice choice;
  InteriorPlan plan;
  if (!plan_rural(plot, household, style, seed, choice, plan, error)) return false;
  describe(plan, choice, name, seed, out);
  std::string why;
  if (!structures::validate(out, why)) {
    if (error != nullptr) *error = "rural filler: the mesher refuses the description: " + why;
    return false;
  }
  return true;
}

}  // namespace engine::dwellings
