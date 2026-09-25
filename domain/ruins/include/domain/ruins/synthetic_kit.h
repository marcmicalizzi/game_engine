#pragma once

// A kit made of boxes, written at test time (docs/subsystems/ruins.md, "The synthetic kit"). Every
// member is a GLB of axis-aligned boxes already in its own frame — the wall's first socket at the
// origin, the wall along +x, the building's inside toward -z — so the kit needs no offsets: a
// section is one box, a corner an L of two, a doorway two jambs and a lintel, a window the same
// with a sill, debris a small block. Nothing binary is committed: the tests, the end-to-end test
// and the measurements all make it, in their own scratch directories, from these options.

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <schemas/scene.h>
#include <string>

namespace engine::ruins {

struct SyntheticKitOptions {
  f32 module = 2.0f;
  f32 thickness = 0.6f;
  f32 wall_height = 2.4f;
  // The straight sections, in modules: E33 laid 2 m and 10 m runs.
  u32 short_section = 1;
  u32 long_section = 5;
  // Sections at these fractions of the wall's height besides the intact one: the ruin rule's
  // height variants. Corners get the same.
  u32 height_variants = 3;  // 1: intact only; 2: and 0.55; 3: and 0.55 and 0.3
  // The corners' arms along the centre line, metres. The default, one module each, closes every
  // footprint; E33's L-corner (4 m and 3 m on the outer face, so 3.68 m and 2.68 m on the centre
  // line of a 0.64 m wall) closes only rectangles in a 2 m module.
  f32 corner_arm_in = 2.0f;
  f32 corner_arm_out = 2.0f;
  bool doorway = true;  // one module wide, a 1.1 m opening 2.1 m high
  bool window = true;   // one module wide, a 1.0 m opening from 0.9 m to 1.9 m
  bool inside_corner = false;
  u32 debris_variants = 2;
  std::string name = "synthetic-boxes";
};

// The E33 member sizes, as boxes: a 2 m module, 2 m and 10 m sections, a 0.64 m wall 2.4 m high,
// and the ashlar L-corner's arms. No doorway: E33's is 3.2 m long, which is not a whole number of
// the module (docs/subsystems/ruins.md, "The E33 kit as a kit").
SyntheticKitOptions e33_sized_options();

struct SyntheticKit {
  scene::RuinKit kit;              // what kit.json holds; members name "<mesh_names[i]>"
  Vector<std::string> mesh_names;  // file names, one per mesh
  Vector<Vector<u8>> glb;          // parallel to mesh_names
};

void make_synthetic_kit(const SyntheticKitOptions& options, SyntheticKit& out);

// Writes the kit's GLBs and `kit.json` into `dir` (created if it is not there). `kit_path`, when
// given, is set to the kit file's path.
bool write_synthetic_kit(const std::string& dir, const SyntheticKitOptions& options,
                         std::string* error, std::string* kit_path = nullptr);

}  // namespace engine::ruins
