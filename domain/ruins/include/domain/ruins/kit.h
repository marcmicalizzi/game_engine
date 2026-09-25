#pragma once

// A ruin kit in the assembler's units (docs/subsystems/ruins.md, "The kit"). The file is the
// schema's `engine.scene.RuinKit` (schemas/scene.schema): members with a mesh, their sockets and
// heights, and the profile's rules, all in metres and fractions. `kit_from_schema` validates it
// and converts every number the grammar decides with, **once**, to integer centimetres and Q10
// fractions, so that no seeded decision the assembler makes touches a float: the same seed gives
// the same building on MSVC, GCC and Clang, at x86-64-v2 and v3, whatever the C library.
//
// **Why the sockets are data and the lengths are derived from them.** A member's length is where
// its neighbour joins it, not the extent of its mesh: a ruined section's mesh carries rubble and a
// sand skirt past its ends (E33's 2 m section is 5.1 m of mesh), so a bounding box would say
// nothing about where the next member goes. The kit says it, as a pair of sockets on the wall's
// centre line, and the loader checks the pair is the shape its kind claims.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>

#include <string>
#include <string_view>

namespace engine::scene {
struct RuinKit;
}

namespace engine::ruins {

// One is 1,024: a height fraction or a probability, as every rule reads it.
inline constexpr i32 k_q_one = 1024;

enum class PieceKind : u8 {
  section = 0,
  corner = 1,
  doorway = 2,
  window = 3,
  debris = 4,
  inside_corner = 5,
};
inline constexpr u32 k_piece_kinds = 6;
const char* piece_kind_name(PieceKind kind) noexcept;

// One member, in centimetres. A straight member (section, doorway, window) is `length_cm` long
// between its sockets; a corner joins the incoming wall `arm_in_cm` before its centre-line vertex
// and hands the outgoing wall on `arm_out_cm` after it.
struct Member {
  std::string name;
  std::string mesh;  // resolved against the kit file's directory
  u64 hash = 0;      // assets::source_mesh_hash the mesh must have; 0 is unchecked
  PieceKind kind = PieceKind::section;
  i32 length_cm = 0;
  i32 arm_in_cm = 0;
  i32 arm_out_cm = 0;
  i32 height_cm = 0;
  i32 opening_start_cm = 0;
  i32 opening_end_cm = 0;
  i32 sill_cm = 0;
  i32 head_cm = 0;
  i32 radius_cm = 0;
  u32 weight = 1;
  // The mesh-to-member transform: translate by `offset`, then turn `yaw_step` sixteenths of a turn
  // about +y. Only rendering reads it, never a decision.
  Vec3 offset{};
  u8 yaw_step = 0;
  u32 mesh_index = 0;  // which of `Kit::meshes` it draws: members may share a file
};

// The rules, converted: probabilities and fractions Q10, lengths centimetres.
struct Rules {
  u32 shape_weight[4] = {4, 2, 1, 1};  // rectangle, L, U, courtyard
  u32 min_fill = 1;                    // modules
  u32 max_fill = 5;
  u32 yaw_steps = 16;
  i32 collapse_q = 307;
  i32 breach_q = 256;
  i32 windward_q = 205;
  i32 collapse_min_q = 154;
  i32 collapse_max_q = 614;
  i32 breach_min_cm = 150;
  i32 breach_max_cm = 400;
  i32 corner_drop_q = 205;
  i32 corner_reach_cm = 300;
  i32 min_height_q = 123;
  i32 max_sink_q = 512;
  i32 opening_q = 614;
  i32 debris_per_module_q = 3072;  // pieces per fully fallen module, Q10
  i32 debris_spread_cm = 150;
  i32 debris_outward_q = 614;
  i32 drift_windward_cm = 34;
  i32 drift_lee_cm = 22;
  i32 drift_reach_cm = 160;
  i32 embed_cm = 5;
};

struct Kit {
  std::string name;
  std::string profile;
  std::string path;  // the kit file, when it came from one
  i32 module_cm = 200;
  i32 thickness_cm = 64;
  i32 wall_height_cm = 240;
  Vector<Member> members;
  Vector<std::string> meshes;  // distinct mesh files, in first-use order
  Vector<u64> mesh_hashes;     // parallel to `meshes`
  Rules rules;

  // Derived by `kit_from_schema`, so that a building costs no search of the member list.
  // Members by kind, in file order.
  Vector<u32> by_kind[k_piece_kinds];
  // The distinct section lengths, in modules, ascending.
  Vector<u32> section_modules;
  // `fillable[n]`: n modules can be filled with sections alone. Up to `max_fill_modules`.
  Vector<u8> fillable;
  u32 max_fill_modules = 0;
  // The corners' arms: the first outside corner's, and the first inside corner's (or the outside
  // corner turned half round, which swaps its arms).
  i32 corner_in_cm = 0;
  i32 corner_out_cm = 0;
  i32 reflex_in_cm = 0;
  i32 reflex_out_cm = 0;
  bool has_inside_corner = false;
  // The largest reach of anything past the wall's centre line: debris and drift. A building
  // keeps this much clear of its tile's edge.
  i32 margin_cm = 0;
};

// Converts and validates. `dir` is what relative mesh paths resolve against. Refuses, with a
// sentence naming the member, a straight member that is not a whole number of modules, sockets
// that are not the shape the kind claims, a yaw that is not a sixteenth of a turn, no section one
// or more modules long, no corner, and rules out of range.
bool kit_from_schema(const scene::RuinKit& file, std::string_view dir, Kit& out,
                     std::string& error);

// Reads a kit file (`engine.scene.RuinKit` as JSON) and converts it.
bool read_kit_file(const std::string& path, Kit& out, std::string& error);

// Metres to centimetres and fractions to Q10, rounded to nearest, in double so the conversion is
// the same on every toolchain for every float the JSON reader can produce.
i32 to_cm(f32 metres) noexcept;
i32 to_q(f32 fraction) noexcept;

}  // namespace engine::ruins
