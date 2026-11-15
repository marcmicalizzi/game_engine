#pragma once

// What a room shows besides its walls (docs/subsystems/dwellings.md, "Timber and furniture";
// the numbers and their sources, docs/experiments/vernacular-interiors.md, "Timber" and
// "Furniture"; roadmap G66): the timber under an upper floor (a spine beam and common joists a
// room), a stair's nosings and half-newels, and the least furniture that says what a room is — a
// hall's table between two benches and a settle by its hearth, a parlour's and a chamber's bed and
// chest.
//
// Every piece is **made of members**: axis-aligned boards, posts and rails in integer centimetres
// (a table is a top, four legs, four rails and three stretchers, never one box), which reach the
// building description as its plain `Box` members (describe.h) and are meshed with the shell. So
// a hand-authored description states the same pieces the filler does: `add_piece` builds any of
// them into a rectangle its author names, and nothing here is the filler's alone.
//
// Furniture stands **never in the tour's way**: `furnish_interior` places each piece against a
// wall of its room, clear of the tour's line by the tour's own clearance, of every doorway's and
// outer door's approach, of the hearth's, of every stack, of the windows a walker is led to, and
// of the pieces before it; a piece that fits nowhere is left out (a room keeps its way through
// before it keeps its table). `validate_furnishing` reads that back and repairs nothing. One
// member of each piece is `solid`, the one that covers its footprint (a table's top, a bench's
// seat, a bed's mattress, a chest's body): it is what the mesher's walk records carry, so a walker
// meets the furniture.
//
// Determinism: integer centimetres, the engine's hash of the seed and a purpose, one draw a
// statement; no floating point and no C library call. LOD policy: none of its own; members are
// description data, and the mesher's cluster DAG is what a distance changes.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/dwellings/interiors.h>

#include <string>

namespace engine::dwellings {

// Moves with any change to what `furnish_interior` or `timber_interior` writes for the same plan
// and seed.
inline constexpr u32 k_furnish_version = 1;

enum class PieceKind : u8 {
  TableSet,  // a table between two benches, one against the wall
  Settle,    // a high-backed bench with ends, by the hearth
  Bed,       // a bedstead, its head to the wall, with its bedding
  Chest,     // a boarded chest
  Joists,    // the common joists under an upper floor, one room's
  Beam,      // the spine beam they cross
  Nosings,   // a stair's tread edges
  Newel,     // a half-newel against the stair's wall, at its foot or its head
};
const char* piece_kind_name(PieceKind kind) noexcept;

// What a member is drawn with: the description's timber, its boards, or its plaster white (a
// bed's linen: the description has no cloth surface).
enum class MemberSurface : u8 { Timber, Boards, Linen };

// One board, post or rail: a box in the plan's frame (x and z as the plan's, y centimetres above
// its storey's floor). 32 bytes.
struct Member {
  i32 x0 = 0;
  i32 y0 = 0;
  i32 z0 = 0;
  i32 x1 = 0;
  i32 y1 = 0;
  i32 z1 = 0;
  u32 room = k_no_room;
  MemberSurface surface = MemberSurface::Timber;
  u8 storey = 0;
  bool solid = false;
  u8 reserved = 0;
};

// One piece: its footprint on the floor, the side of its room its back is to, and its members'
// range. 36 bytes.
struct Piece {
  RectCm footprint;
  u32 room = k_no_room;
  u32 first_member = 0;
  u32 member_count = 0;
  u16 height_cm = 0;
  PieceKind kind = PieceKind::TableSet;
  Side against = Side::Front;
  u8 storey = 0;
  u8 reserved[3] = {0, 0, 0};
};

struct Furnishing {
  Vector<Piece> pieces;
  Vector<Member> members;

  void clear() noexcept;
};

// How far each piece stands out from the wall behind it; its length along the wall is its
// author's (a bed's is its mattress's width and 10 cm, and it stands 200 out from its head wall).
inline constexpr i32 k_table_set_out_cm = 121;
inline constexpr i32 k_settle_out_cm = 58;
inline constexpr i32 k_bed_out_cm = 200;
inline constexpr i32 k_chest_out_cm = 45;
// The gap a piece keeps from the face of the wall behind it, so no face of it lies in the wall's.
inline constexpr i32 k_wall_gap_cm = 2;

// Builds a furniture piece's members into `footprint` (the plan's frame), its back to the side
// `against` of the footprint, on `storey` in `room`. The footprint's size along and out from that
// side is the piece's size; false when it is under the kind's least (a table set 120 by 121, a
// settle 120 by 58, a bed 101 by 200, a chest 70 by 45) or the kind is not furniture.
bool add_piece(Furnishing& out, PieceKind kind, const RectCm& footprint, Side against, u8 storey,
               u32 room);

// The furniture of every habitable room of the plan, by its use, drawn from `seed`. Appends.
void furnish_interior(const InteriorPlan& plan, u64 seed, Furnishing& out);

// The timber: under every upper floor a spine beam along the ridge and joists across it, a room
// at a time (never over a stair's compartment), and each stair's nosings and half-newels.
// `ridge_along_x`: the ridge runs along the front wall. Appends.
void timber_interior(const InteriorPlan& plan, bool ridge_along_x, Furnishing& out);

// Every furniture piece inside its room's free floor, clear of the tour, the approaches, the
// stacks and the other pieces, with a solid member; every member a real box. False with a
// sentence naming the first piece that is not; nothing is repaired.
bool validate_furnishing(const InteriorPlan& plan, const Furnishing& furnishing, std::string* why);

// Every field of every piece and member, in order.
u64 hash_furnishing(const Furnishing& furnishing) noexcept;

}  // namespace engine::dwellings
