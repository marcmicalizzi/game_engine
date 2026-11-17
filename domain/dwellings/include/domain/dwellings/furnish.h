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
// range. A **kit piece** (`kit` non-zero: the row `kit - 1` of the caller's kit, "Kit pieces"
// below) stands in its kind's place with no members: a mesh of the scene's is drawn there, not
// boxes. 36 bytes.
struct Piece {
  RectCm footprint;
  u32 room = k_no_room;
  u32 first_member = 0;
  u32 member_count = 0;
  u16 height_cm = 0;
  PieceKind kind = PieceKind::TableSet;
  Side against = Side::Front;
  u8 storey = 0;
  u8 reserved = 0;
  u16 kit = 0;
};

// ---- kit pieces -------------------------------------------------------------------------------
//
// **Kit pieces** (dwellings.md, "Kit pieces"; the forest session's need 113 (e) and (f)): a scene's
// own meshes in place of the box furniture. A row says which kind of piece it stands in for and the
// box its foot and its height take, its back to the wall and its front into the room; the
// furnisher searches for it with the same clearances it keeps for its own pieces (the tour's line,
// the approaches, the hearth's apron, the stacks, the led window, the gap between pieces) and the
// same preference (a settle by the stack, a table under a window, a bed in the corner far from the
// way in, a chest by the bed), and only where no row of a kind finds a place does the box piece
// stand. A row is chosen a room and a kind by a draw on the house's seed, among the rows that stand
// under the room's ceiling or joists.
struct KitRow {
  u32 id = 0;  // the caller's name for it (a kit table's row); `Piece::kit` is id + 1
  PieceKind replaces = PieceKind::TableSet;
  i32 along_cm = 0;  // the foot's length across its front, along the wall it backs to
  i32 out_cm = 0;    // its depth, from its back to its front
  i32 height_cm = 0;
};
struct Kit {
  const KitRow* rows = nullptr;
  u32 count = 0;
};
// How far a kit piece stands under the joists or a ceiling, centimetres.
inline constexpr i32 k_kit_head_gap_cm = 2;

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

// The furniture of every habitable room of the plan, by its use, drawn from `seed`, a kit row in a
// box piece's place wherever `kit` has one that fits. Appends. With an empty kit the furnishing
// is the same bytes it was before kits existed (the pins hold it).
void furnish_interior(const InteriorPlan& plan, u64 seed, Furnishing& out, const Kit& kit = {});

// The clear height over a room's floor a piece stands under: the joists' undersides under an upper
// floor, else the storey's ceiling or plate. Centimetres.
i32 room_clear_cm(const InteriorPlan& plan, const Room& room) noexcept;

// The timber: under every upper floor a spine beam along the ridge and joists across it, a room
// at a time (never over a stair's compartment), and each stair's nosings and half-newels.
// `ridge_along_x`: the ridge runs along the front wall. Appends.
void timber_interior(const InteriorPlan& plan, bool ridge_along_x, Furnishing& out);

// Every furniture piece inside its room's free floor, clear of the tour, the approaches, the
// stacks and the other pieces, with a solid member; every member a real box; every kit piece the
// same clearances, no members, and its height under the room's joists or ceiling by
// `k_kit_head_gap_cm`. False with a sentence naming the first piece that is not; nothing is
// repaired.
bool validate_furnishing(const InteriorPlan& plan, const Furnishing& furnishing, std::string* why);

// Every field of every piece and member, in order.
u64 hash_furnishing(const Furnishing& furnishing) noexcept;

// ---- the places the plan already knows
// ------------------------------------------------------------
//
// **Named places** (dwellings.md, "The places a plan knows"; the forest session's need 113 (d)):
// the surfaces and slots a plan and its furniture already decide, said once, so whoever lays props
// into a house reads them instead of finding them again from a description's boxes in the pieces'
// order. Each is a box in the plan's frame — x and z centimetres as the plan's, y millimetres above
// its storey's floor — of which a surface is flat (`y0_mm == y1_mm`: what a thing stands on, or
// what hangs from it) and a face is thin across `faces` (a hearth's cheek, its mouth).
enum class PlaceKind : u8 {
  HearthRecess,     // the fire's recess in the stack: its floor (the raised hearth) to the lintel
  HearthMouth,      // the opening's line on the stack's face, the raised hearth to the lintel
  HearthCheek,      // the stack's face beside the opening, floor to lintel, one each side
  StackSide,        // the floor beside the stack along its wall, as deep as the stack stands proud
  HearthApron,      // the floor before the opening that the furniture keeps clear
  TableTop,         // a table's top
  BenchSeat,        // a bench's or a form's seat
  SettleSeat,       // a settle's seat
  ChestLid,         // a chest's lid
  Mattress,         // a bed's mattress
  JoistUnderside,   // one joist's underside (or the spine beam's), where a thing is hung from
  DoorApproach,     // the floor before an outer door that its leaf and its way in keep
  DoorwayApproach,  // the floor each side of an inner doorway
  WindowLight,      // the floor before a window, at its sill: where its light falls first
};
const char* place_kind_name(PlaceKind kind) noexcept;

// One named place. `faces` is the way a face looks (a hearth's, its opening's) or, for a surface,
// the side of the room the thing it belongs to backs to; `index` the record it was read from (the
// hearth, the piece, the opening, the doorway). 36 bytes.
struct NamedPlace {
  RectCm rect;
  i32 y0_mm = 0;
  i32 y1_mm = 0;
  u32 room = k_no_room;
  u32 index = 0;
  PlaceKind kind = PlaceKind::TableTop;
  Side faces = Side::Front;
  u8 storey = 0;
  u8 reserved = 0;
};

// How deep the mesher cuts a hearth's recess into its stack and how high it raises the hearth's
// floor (structures.md, "Chimneys": `Chimney::hearth_depth`'s default and the shell's 3 cm), which
// `describe` leaves at the mesher's own.
inline constexpr i32 k_hearth_recess_cm = 40;
inline constexpr i32 k_raised_hearth_mm = 30;

// Every named place of a plan and its furnishing, in a fixed order: the hearths' (recess, mouth,
// cheeks, the stack's sides, the apron), the furniture's surfaces in the pieces' order, the joists'
// undersides, then the doors', the doorways' and the windows' floors. Appends.
void house_places(const InteriorPlan& plan, const Furnishing& furnishing, Vector<NamedPlace>& out);

}  // namespace engine::dwellings
