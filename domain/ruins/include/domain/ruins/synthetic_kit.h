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
#include <string_view>

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

// ---- The synthetic block kit (docs/subsystems/ruins.md, "The synthetic block kit")
// ---------------
//
// Fourteen block meshes for the block layer (blocks.h) at one of three fidelities, which bracket
// what a block costs the cull pass in pairs — the number the block representation's comparison
// with sections turns on:
//
//   low   a chamfered box: 44 triangles, one cluster, one pair an instance
//   mid   a rounded, subdivided block with seeded erosion: about 5,000 triangles, tens of clusters
//   high  the same at about 50,000 triangles: hundreds of clusters, the bracket for a scan-quality
//         block such as E33's image-to-3D bricks
//
// Stretchers at three lengths (0.75, 1 and 1.25 of the stretcher) and a half, a quoin as long as
// the wall is thick plus half a stretcher (so the courses alternate by half a block at every
// corner), a lintel and a sill; crisp and eroded variants of each but the sill. Every block is a
// course high and a wall thick. Deterministic from the options, with no C library call a toolchain
// could round differently, and written as GLBs so the scene reader clusters them through the
// derived-data cache as it does the kit of boxes.
enum class BlockFidelity : u8 { low = 0, mid = 1, high = 2 };
const char* block_fidelity_name(BlockFidelity fidelity) noexcept;
bool parse_block_fidelity(std::string_view text, BlockFidelity& out) noexcept;

struct SyntheticBlockOptions {
  BlockFidelity fidelity = BlockFidelity::low;
  f32 thickness = 0.6f;  // the kit of boxes' wall; E33's ashlar is 0.64
  f32 course = 0.3f;
  f32 stretcher = 0.6f;  // E33's ashlar: blocks of 0.60 m +- 0.10
  f32 lintel = 1.5f;     // spans a 1.1 m doorway with 0.2 m bearings
  f32 sill = 1.0f;
  u32 mid_triangles = 5000;
  u32 high_triangles = 50000;
  u64 seed = 1;
  std::string name;  // empty: "synthetic-blocks-<fidelity>"
};

struct SyntheticBlockKit {
  scene::RuinBlockKit kit;         // what block-kit.json holds; blocks name "<mesh_names[i]>"
  Vector<std::string> mesh_names;  // file names, one per mesh
  Vector<Vector<u8>> glb;          // parallel to mesh_names
  Vector<u32> triangles;           // parallel to mesh_names
};

void make_synthetic_block_kit(const SyntheticBlockOptions& options, SyntheticBlockKit& out);

// Writes the block kit's GLBs and `block-kit.json` into `dir`.
bool write_synthetic_block_kit(const std::string& dir, const SyntheticBlockOptions& options,
                               std::string* error, std::string* kit_path = nullptr);

}  // namespace engine::ruins
