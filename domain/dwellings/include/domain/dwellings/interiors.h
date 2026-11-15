#pragma once

// A house's interior as a plan (docs/subsystems/dwellings.md; plan 14 §14.3, "The
// fillers", step 6; roadmap G66): from a shell's clear inner rectangle, a plan type, a storey count
// and a seed, the rooms as a graph a person recognises — a one-room cot, a hall and parlour, a
// cross-passage house, a two-up two-down — with the inner walls between them, a doorway between
// every pair of rooms the graph joins, an enclosed straight stair a walker can climb, the hearths
// and their stacks, and where the outer doors and the windows sit in each room.
//
// It writes no mesh and names no ground: the plan is plain records in integer centimetres, in the
// building's own frame (x along the front wall, z away from the lane, the inner faces of the outer
// walls at x = 0, x = width, z = 0 and z = depth). `interior_parts.h` hands it to the mesher's seam
// and the rural filler (`rural_filler.h`) asks for it; a hand-authored shell asks for it the same
// way.
//
// Determinism (ADR-0027's stance, plan 14 §14.7): every decision is integer centimetres or
// millimetres and the engine's hash of the seed, a purpose and an index; no floating point and no C
// library call anywhere in this file's implementation. The same parameters give the same plan on
// every toolchain, and `hash_interior` is the number the determinism test pins.
//
// LOD policy: none of its own. The plan is description data, a few hundred bytes a house; what is
// drawn at a distance is the mesher's interior mesh and the cluster DAG's cut of it.

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <string>

namespace engine::dwellings {

// Moves with any change to what `plan_interior` writes for the same parameters.
inline constexpr u32 k_interior_version = 1;

enum class PlanType : u8 {
  Cot,           // one room, one storey, a gable hearth
  HallParlour,   // two rooms side by side, the door into the hall, a gable hearth
  CrossPassage,  // (inner room,) hall, a passage with opposed doors, service; the stack backs
                 // onto the passage
  TwoUpTwoDown,  // one room wide and two deep, the stair between front and back, two chambers
};
inline constexpr u32 k_plan_types = 4;
const char* plan_type_name(PlanType type) noexcept;

enum class RoomKind : u8 { Hall, Parlour, Passage, Service, Inner, Kitchen, Chamber, Stair };
const char* room_kind_name(RoomKind kind) noexcept;

// The four outer walls, by the inner face each room meets: Front is z = 0 (the lane's side), Right
// x = width, Back z = depth, Left x = 0. A position along a side is x for Front and Back, z for
// Left and Right.
enum class Side : u8 { Front, Right, Back, Left };

struct RectCm {
  i32 x0 = 0;
  i32 z0 = 0;
  i32 x1 = 0;
  i32 z1 = 0;
};

inline constexpr u32 k_no_room = 0xffffffffu;

inline constexpr u8 k_room_habitable = 1;  // wants a window
inline constexpr u8 k_room_heated = 2;     // holds a hearth
inline constexpr u8 k_room_entrance = 4;   // an outer door opens into it

// One room on one storey. Rooms tile their storey's inner rectangle: a room's rectangle runs to
// the centre line of each inner wall round it and to the inner face of each outer wall. 20 bytes.
struct Room {
  RectCm rect;
  RoomKind kind = RoomKind::Hall;
  u8 storey = 0;
  u8 flags = 0;
  u8 reserved = 0;
};

// An inner wall's centre line, axis-aligned, (x0, z0) its low end. `a` is the room on its low side
// (smaller z for a wall along x, smaller x for a wall along z), `b` the room on its high side.
// 28 bytes.
struct InnerWall {
  i32 x0 = 0;
  i32 z0 = 0;
  i32 x1 = 0;
  i32 z1 = 0;
  u32 a = k_no_room;
  u32 b = k_no_room;
  u16 thickness_cm = 0;
  u8 storey = 0;
  u8 reserved = 0;
};

// An open doorway through an inner wall, between the wall's two rooms. 12 bytes.
struct Doorway {
  u32 wall = 0;
  i32 at_cm = 0;  // from the wall's low end to the doorway's centre
  u16 width_cm = 0;
  u16 head_cm = 0;  // above its storey's floor
};

// A door or a window in an outer wall. 16 bytes.
struct OuterOpening {
  i32 at_cm = 0;  // along the side, to the opening's centre
  u32 room = k_no_room;
  u16 width_cm = 0;
  u16 sill_cm = 0;  // 0 for a door
  u16 head_cm = 0;
  Side side = Side::Front;
  u8 storey_door = 0;  // the storey in the low 7 bits, bit 7 set for a door

  u8 storey() const noexcept { return static_cast<u8>(storey_door & 0x7fu); }
  bool door() const noexcept { return (storey_door & 0x80u) != 0; }
};

// A hearth: the stack's mass in plan (floor to roof, through every storey above it), and the fire's
// opening in the face that looks into its room. 28 bytes.
struct Hearth {
  RectCm stack;
  u32 room = k_no_room;
  u16 opening_cm = 0;        // the opening's width, centred on the stack's face
  u16 lintel_cm = 0;         // the opening's height
  Side faces = Side::Front;  // the direction the opening looks: Front is toward -z
  u8 storey = 0;
  u16 reserved = 0;
};

// One stair between a storey and the one above: an enclosed straight flight in a strip between two
// walls, a level foot landing at one end entered through doorways in the strip's sides, a level
// head landing at the other on the upper floor. The flight's (risers - 1) treads share
// `flight` equally along `rises`; the last riser lands on the head landing. The upper floor has no
// slab over `well`. 80 bytes.
struct Stair {
  RectCm strip;   // the whole compartment, centre line to centre line of its side walls
  RectCm foot;    // the foot landing, at the lower floor's level
  RectCm flight;  // the treads
  RectCm head;    // the head landing, at the upper floor's level
  u32 lower_room = k_no_room;  // the stair's room on the lower storey
  u32 upper_room = k_no_room;  // and on the upper
  i32 rise_mm = 0;             // floor to floor
  u8 risers = 0;
  Side rises = Side::Front;  // the direction of travel going up
  u8 from_storey = 0;
  u8 reserved = 0;

  // The opening in the upper floor: the strip less the head landing.
  RectCm well() const noexcept;
  // The top of tread `i` (0 is the first step up) above the lower floor, millimetres.
  i32 tread_mm(u32 i) const noexcept { return rise_mm * static_cast<i32>(i + 1) / risers; }
  // Tread `i`'s rectangle in plan.
  RectCm tread(u32 i) const noexcept;
};

// What the generator is asked. Lengths are centimetres.
struct InteriorParams {
  PlanType type = PlanType::Cot;
  u8 storeys = 1;            // 1 or 2; a cot is always 1, a two-up two-down always 2
  bool mirror = false;       // the plan flipped left to right
  bool inner_room = false;   // a cross-passage house's room beyond the hall
  u8 hearths = 1;            // 1, or 2 for a second hearth (the parlour's, the kitchen's)
  i32 width_cm = 0;          // the clear inner rectangle, along the front
  i32 depth_cm = 0;          // and away from the lane
  u16 storey_cm = 240;       // floor to floor, the ground storey
  u16 upper_clear_cm = 250;  // the upper storey's floor to its ceiling
  u16 floor_cm = 20;         // the upper floor's thickness
  u16 inner_wall_cm = 10;
  u16 door_cm = 95;     // an outer door's width
  u16 doorway_cm = 85;  // an inner doorway's
  u16 head_cm = 200;    // a door's and a window's head above the floor
  u16 window_cm = 90;   // a window's width
  u16 sill_cm = 90;
  u64 seed = 0;
};

struct InteriorPlan {
  InteriorParams params;
  Vector<Room> rooms;
  Vector<InnerWall> walls;
  Vector<Doorway> doorways;
  Vector<OuterOpening> openings;
  Vector<Hearth> hearths;
  Vector<Stair> stairs;

  void clear() noexcept;
};

// The stair rule (docs/experiments/vernacular-interiors.md, "The stair"): a riser of at most
// 210 mm on a going of 280 mm, so twice the rise plus the going stays at or under 700 mm.
inline constexpr i32 k_stair_going_cm = 28;
inline constexpr i32 k_stair_max_rise_mm = 210;
inline constexpr i32 k_stair_strip_cm = 100;    // centre line to centre line
inline constexpr i32 k_stair_landing_cm = 100;  // each landing's least length
inline constexpr i32 k_headroom_cm = 210;       // plan 14 §14.4

// The risers a storey of `rise_mm` takes, and the strip's least length for them.
u32 stair_risers(i32 rise_mm) noexcept;
i32 stair_strip_length_cm(i32 rise_mm) noexcept;

// The least clear inner rectangle a plan needs, so a caller can size a shell before it asks.
struct InteriorExtent {
  i32 min_width_cm = 0;
  i32 min_depth_cm = 0;
};
InteriorExtent interior_min_extent(const InteriorParams& params) noexcept;

// Lays the plan out. False with a sentence when the rectangle is too small for the type (a room
// under its least size, a stair with no room for its landings) or a parameter is out of range;
// `out` is then empty.
bool plan_interior(const InteriorParams& params, InteriorPlan& out, std::string* error);

// Every field of every record, in order.
u64 hash_interior(const InteriorPlan& plan) noexcept;

// --- The graph ---------------------------------------------------------------------------------

// Rooms a walker reaches from an outer door through doorways and stairs: `reached[i]` is 1 for
// room i. Returns how many.
u32 reachable_rooms(const InteriorPlan& plan, Vector<u8>& reached);

// One storey as text, a character a cell of `cell_cm`, the lane below the picture: '#' an outer
// wall with 'D' and 'W' its doors and windows, '%' an inner wall and '.' a doorway through it, 'H'
// a hearth's stack ('c' its chimney on a storey above), a room's initial near its middle, and the
// stair seen from its lower storey as 'f' the foot landing, '-' and '=' the treads, 'x' where the
// flight has passed under head height, or from its upper as 'h' the head landing and ':' the well.
// For a person or an agent reviewing a plan in a terminal; nothing reads it back.
std::string draw_interior(const InteriorPlan& plan, u8 storey, i32 cell_cm);

// --- Validators (plan 14 §14.4's table, read from the plan) --------------------------------------

enum class InteriorRule : u8 {
  RoomsTile,      // a storey's rooms cover its rectangle once
  RoomReachable,  // every room is reached from an outer door
  DoorwayClear,   // a doorway is at least 0.8 m wide and 2.0 m high, inside its wall
  DoorwayJoins,   // a doorway lies on the boundary its wall's two rooms share
  StairRise,      // a riser at most 0.25 m
  StairGoing,     // a going at least 0.28 m
  StairWidth,     // at least 0.8 m clear between the strip's faces
  StairLanding,   // each landing at least 0.9 m
  StairHeadroom,  // at least 2.1 m clear over every tread and both landings
  StairLands,     // the head landing lies on the upper floor and a doorway leaves it
  WindowInWall,   // a window's span, with its pier, meets no inner wall, stack or opening
  WindowHeight,   // a sill and a head inside the storey
  RoomLit,        // every habitable room has a window
  HearthClear,    // a stack inside its room, off every doorway's approach and every stair
};
const char* interior_rule_name(InteriorRule rule) noexcept;

struct InteriorFinding {
  InteriorRule rule = InteriorRule::RoomsTile;
  u32 index = 0;     // the record the rule names: a room, a doorway, a stair, an opening, a hearth
  i32 measured = 0;  // what was measured, in the rule's unit (cm, or mm for a rise)
  i32 limit = 0;
};

// Every rule over every record. Returns true when nothing was found; a finding is never repaired.
bool validate_interior(const InteriorPlan& plan, Vector<InteriorFinding>& findings);

// The clear height over a point of a stair at `height_mm` above the lower floor: to the upper
// floor's underside where the point lies outside the well, to the upper storey's ceiling inside it.
i32 stair_headroom_cm(const InteriorPlan& plan, const Stair& stair, i32 x_cm, i32 z_cm,
                      i32 height_mm) noexcept;

}  // namespace engine::dwellings
