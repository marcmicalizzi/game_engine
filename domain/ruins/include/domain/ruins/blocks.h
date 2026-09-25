#pragma once

// The block-by-block representation of a ruined building (docs/subsystems/ruins.md, "The block
// layer"; plan 07 §7.6, the direction note of 2026-09-24): the building the section assembler
// makes — the same footprint, yaw, base, ruin state per wall and openings — laid instead as
// blocks, each an instance of one of a block kit's dozen meshes. Courses run in running bond
// along each wall's centre line; the two walls at a corner take its square in turn, course by
// course, with a quoin whose length past the wall's thickness is the bond's offset, so the
// courses of every wall alternate by it; an opening is a gap in the courses it crosses, a lintel
// over it and a sill under a window; the ruin rule's line decides which blocks stand, broken per
// block by a seeded draw either side of it, and a block stands only where the course under it
// carries it; and the blocks that did not stand are the rubble, one on each of the building's
// rubble sites (`Assembler::rubble`) — the sites the assembler heaps its debris members on — and
// the rest under the sand.
//
// **Why the section assembler decides the building.** One seed has to be one building whichever
// way it is drawn, or a handover between the two representations (the open question, ruins.md)
// would swap one ruin for another. So this layer takes the assembler's walls, frame, ruin line and
// rubble field (`Assembler::sides`, `frame`, `height_q`, `rubble`) and adds only what blocks need:
// courses, the bond, the broken top per block, support, and which fallen block lies on each site.
//
// **Pure, and integer in every decision**, as the assembler is: positions along a wall are
// centimetres, fractions are Q10, every draw is the engine's hash of (building seed, purpose,
// index), and the world placement is the assembler's Q14 turn. A building is the same blocks on
// every toolchain, pinned by a golden hash in apps/engine_content/tests/determinism_tests.cpp
// beside the assembler's.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/ruins/assembler.h>
#include <domain/ruins/kit.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::scene {
struct RuinBlockKit;
}

namespace engine::jobs {
class JobSystem;
}

namespace engine::ruins {

enum class BlockRole : u8 { stretcher = 0, half = 1, quoin = 2, lintel = 3, sill = 4 };
inline constexpr u32 k_block_roles = 5;
const char* block_role_name(BlockRole role) noexcept;

// One block mesh of a kit, in centimetres.
struct KitBlock {
  std::string name;
  std::string mesh;  // resolved against the block kit file's directory
  u64 hash = 0;      // assets::source_mesh_hash the mesh must have; 0 is unchecked
  BlockRole role = BlockRole::stretcher;
  i32 length_cm = 0;
  i32 height_cm = 0;
  i32 depth_cm = 0;
  i32 radius_cm = 0;  // half its diagonal in plan: how far past its centre it reaches lying down
  bool eroded = false;
  u32 weight = 1;
  Vec3 offset{};    // the mesh-to-block transform, as for a kit member: rendering reads it, never a
  u8 yaw_step = 0;  // decision
  u32 mesh_index = 0;
};

// The rules, converted: fractions Q10, lengths centimetres. How much rubble lies beside a wall is
// not the block kit's to say: it is the section kit's rubble rule, which both representations lay
// (the file's `debris_kept` is read and ignored).
struct BlockRules {
  i32 top_drop_q = 512;
  i32 eroded_exposed_q = 768;
  i32 eroded_q = 205;
  i32 support_q = 512;
  i32 lintel_bearing_cm = 20;
};

struct BlockKit {
  std::string name;
  std::string profile;
  std::string path;
  i32 course_cm = 30;
  i32 thickness_cm = 64;
  Vector<KitBlock> blocks;
  Vector<std::string> meshes;  // distinct mesh files, in first-use order
  Vector<u64> mesh_hashes;     // parallel to `meshes`
  BlockRules rules;

  // Derived by `block_kit_from_schema`. The blocks of one role and one length are a **group**;
  // a slot the layer lays asks for a group, and the block is drawn from its crisp or its eroded
  // members by the place's exposure.
  struct Group {
    BlockRole role = BlockRole::stretcher;
    i32 length_cm = 0;
    Vector<u32> crisp;
    Vector<u32> eroded;
  };
  Vector<Group> groups;
  // The stretcher and half groups by length ascending (what a run is laid in), and the lintel and
  // sill groups the same way.
  Vector<u32> straight;
  Vector<u32> lintels;
  Vector<u32> sills;
  i32 stretcher_cm = 0;  // the bond's block: the stretcher length with the most weight
  // Its group: what a rubble site takes when its wall has no fallen block left to give it.
  i32 stretcher_group = -1;
  i32 quoin_group = -1;  // the quoins' group (every quoin is one length), or none
  i32 quoin_cm = 0;
};

// Converts and validates. Refuses, with a sentence naming the block, a kit with no stretcher,
// quoins of two lengths or one no longer than the wall is thick (it would lay the courses in stack
// bond), a yaw that is not a sixteenth of a turn, a size that is not positive, a bond other than
// running, and rules out of range.
bool block_kit_from_schema(const scene::RuinBlockKit& file, std::string_view dir, BlockKit& out,
                           std::string& error);
bool read_block_kit_file(const std::string& path, BlockKit& out, std::string& error);

// One placed block: 28 bytes, 4-aligned (tests/size_table.cpp). A thousand ruins laid block by
// block are some hundreds of thousands of these, so they are the representation's hot type.
struct Block {
  Vec3 position{};   // metres: the block frame's origin — its start end on the wall's centre line,
                     // at its bottom — x and z from integer centimetres, y from the base or ground
  u32 block = 0;     // index into BlockKit::blocks
  u32 building = 0;  // index into BlockOutput::sites
  u16 wall = 0;      // the wall it stands in or fell from
  u16 index = 0;     // its place along its course, or the rubble site it lies on (RubbleSite)
  u8 course = 0;     // counted from the ground
  u8 role = 0;       // BlockRole
  u8 yaw = 0;        // sixteenths of a turn about +y
  u8 flags = 0;      // k_block_fallen, k_block_eroded
};
inline constexpr u8 k_block_fallen = 1;
inline constexpr u8 k_block_eroded = 2;

// The buildings laid block by block. A site's `first_instance` and `instance_count` index
// `blocks`; its drifts are the assembler's, unchanged.
struct BlockOutput {
  Vector<Block> blocks;
  Vector<Drift> drifts;
  Vector<Site> sites;
  void clear() noexcept {
    blocks.clear();
    drifts.clear();
    sites.clear();
  }
};

// Lays buildings one tile at a time, reusing its assembler and scratch: one per thread, the kits
// read only.
class BlockAssembler {
 public:
  BlockAssembler(const Kit& kit, const BlockKit& blocks) noexcept;

  // The building on `tile`, appended to `out`. False, with nothing appended and `error` set, when
  // the assembler finds no footprint.
  bool assemble(const Placement& placement, TileCoord tile, BlockOutput& out, std::string* error);

  // The section assembler it drew the building with: the footprint's walls and frame.
  const Assembler& assembler() const noexcept { return assembler_; }
  // Courses in an intact wall: the section kit's wall height in the block kit's courses.
  u32 courses() const noexcept { return courses_; }

 private:
  // One block's place in a course, before the ruin rule decides it.
  struct Slot {
    i32 u0;  // along the wall from its start vertex, centimetres
    i32 u1;
    u32 group;   // BlockKit::groups
    u8 role;     // BlockRole
    u8 bearing;  // a lintel: it needs both of its bearings carried, not a share of its length
    u8 stands;
    u8 reserved;
  };
  // A standing block's extent along its wall: what carries the course above.
  struct Span {
    i32 u0;
    i32 u1;
  };
  // A block that did not stand, in the order the courses were laid: what the rubble sites are
  // filled from.
  struct Fallen {
    u32 wall;
    u32 group;
    i32 centre_cm;
    u32 course;
  };

  void fill(i32 a, i32 b);

  const Kit& kit_;
  const BlockKit& blocks_;
  Assembler assembler_;
  Output scratch_;  // the assembler's own output for the building: its site and drifts
  u32 courses_ = 1;
  Vector<Slot> slots_;
  // The course under this one and this one, as standing spans per wall (`*_first_` indexes them),
  // and whether each wall's start corner square stood in it.
  Vector<Span> below_;
  Vector<u32> below_first_;
  Vector<Span> here_;
  Vector<u32> here_first_;
  Vector<u8> corner_below_;
  Vector<u8> corner_here_;
  Vector<Fallen> fallen_;
  Vector<u32> next_wall_;
  // The fallen blocks by wall (`fallen_first_` indexes `fallen_order_`, which indexes `fallen_`),
  // and which of them a rubble site has taken.
  Vector<u32> fallen_first_;
  Vector<u32> fallen_order_;
  Vector<u8> fallen_taken_;
};

// Many tiles, on the job system's performance pool when one is given; the output is in the order
// of `tiles` whatever the thread count, as `assemble_tiles`'s is.
bool assemble_block_tiles(const Kit& kit, const BlockKit& blocks, const Placement& placement,
                          std::span<const TileCoord> tiles, jobs::JobSystem* jobs, BlockOutput& out,
                          std::string* error);

// A hash over every field of every record, in order, field by field: the number the determinism
// test pins.
u64 hash_blocks(const BlockOutput& out) noexcept;

// The world translation and yaw of a block's *mesh*: its position and yaw with the kit block's
// own offset and yaw folded in, as `instance_translation` does for a member.
Vec3 block_translation(const BlockKit& kit, const Block& block) noexcept;
u32 block_yaw_step(const BlockKit& kit, const Block& block) noexcept;

}  // namespace engine::ruins
