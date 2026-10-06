#pragma once

// The ruin assembler (docs/subsystems/ruins.md; plan 07 §7.6, direction note of 2026-09-24): from
// a kit, a world seed, a tile coordinate and a height query, one ruined building as instances of
// the kit's members — wall sections at right angles on a footprint, a corner member at every
// join, openings with their lintels on the walls still standing, each wall's ruin state, the
// debris its fallen stones make, and the sand drift each wall asks the terrain for.
//
// **Pure, and integer in every decision.** The building is a function of (kit, world seed, tile,
// wind) and nothing else: the seed mixing is the engine's `hash_combine`, every draw is a hash of
// the building's seed, what the draw is for and an index — so a draw added in one place moves
// nothing elsewhere — and every choice the grammar and the ruin rule make is integer arithmetic on
// centimetres and Q10 fractions. Floats appear only where a result is not a decision: the height
// the terrain query returns, and the metres an instance is written in. So one seed is one ruin on
// MSVC, GCC and Clang, at x86-64-v2 and v3, and the golden hash in
// `apps/engine_content/tests/determinism_tests.cpp` holds on all of them. The height query is the
// caller's: a building stands on whatever ground that machine's terrain function gives, and no
// decision ever reads it.
//
// **Output is instances of kit meshes**, never geometry: a thousand ruins are instances of a few
// dozen meshes, which is what the scene's instance-of-mesh model and the cull pass's pairs are
// for, and nothing per building has to be built or stored.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/ruins/kit.h>

#include <span>
#include <string>

namespace engine::jobs {
class JobSystem;
}

namespace engine::ruins {

struct TileCoord {
  i32 x = 0;
  i32 z = 0;
  constexpr bool operator==(const TileCoord&) const = default;
};

// The terrain's height in metres under a point given in **whole millimetres from the world's
// origin**: a plain function and a context, because the assembler is in the domain layer and the
// terrain in the renderer, and because it is called a few dozen times a building, never in an
// inner loop over instances. No `fn` is flat ground at 0. The place is integers (ADR-0053;
// scene_gen.md, "Placements far from the origin"): the building's points are integer centimetres
// of the world, so they reach the ground exactly anywhere; until 2026-10-06 they went through
// float32 metres, which 419 km out is a 3.1 cm grid. The signature is `scene_gen::HeightFn`'s, so
// the scene's ground is handed over as it is.
using HeightFn = f64 (*)(const void* context, i64 x_mm, i64 z_mm) noexcept;
struct Ground {
  HeightFn fn = nullptr;
  const void* context = nullptr;
  f64 at_mm(i64 x_mm, i64 z_mm) const noexcept {
    return fn != nullptr ? fn(context, x_mm, z_mm) : 0.0;
  }
  // Under a point of whole centimetres, in the float32 every decision reads a height in (the
  // building's base, a piece's bed), as it always did: every ground here answers a float, so the
  // narrowing is exact and the decisions by the origin are the ones they were.
  f32 at_cm(i64 x_cm, i64 z_cm) const noexcept {
    return static_cast<f32>(at_mm(x_cm * 10, z_cm * 10));
  }
};

enum class Shape : u8 { rectangle = 0, l_shape = 1, u_shape = 2, courtyard = 3 };
const char* shape_name(Shape shape) noexcept;

enum class WallState : u8 { intact = 0, breached = 1, collapsed = 2 };

// The capability's LOD policy (ADR-0027): how much of a building a tile materializes. `full` is
// everything; `walls` leaves the debris out — the most numerous pieces and the smallest, a few
// pixels each at a few hundred metres — and changes nothing else, because debris draws from its
// own streams after every wall is decided. `detail_for_distance` is the policy's function.
enum class Detail : u8 { full = 0, walls = 1 };
Detail detail_for_distance(f32 distance_m, f32 tile_m) noexcept;

// What one call asks for, besides the tile.
struct Placement {
  u64 world_seed = 1;
  i32 tile_cm = 3200;  // a tile's edge; tile (x, z) covers [x, x + 1) * tile_cm in x and z
  u8 wind_step = 0;    // where the wind blows from, sixteenths of a turn about +y from +x
  Detail detail = Detail::full;
  Ground ground;
  // Where tile (0, 0)'s corner is, centimetres of the world: zero for every scene entry, whose
  // tiles are the world's. A building's seed is its tile's (`building_seed`), so its shape is a
  // function of where it stands; this moves the grid under the same seeds, which is how a test
  // puts the same building 10,000 km out and holds that it is placed there exactly as by the
  // origin (ruins.md, "Far from the origin").
  i64 origin_x_cm = 0;
  i64 origin_z_cm = 0;
};

// One placed piece: 40 bytes, 8-aligned (tests/size_table.cpp). The world transform is `position`
// and `yaw` sixteenths of a turn about +y, applied to the member's frame; `instance_translation`
// folds the member's own mesh offset in.
struct Instance {
  // Metres in f64, each the integer centimetres of the world it was decided in, divided once
  // (ADR-0053): x and z the piece's, y the base's or the ground's under it. Exact to a nanometre
  // anywhere, where the float32 it was until 2026-10-06 was a 3.1 cm grid at 419 km.
  WorldPos position{};
  u32 member = 0;    // index into Kit::members
  u32 building = 0;  // index into Output::sites
  u16 wall = 0;      // the wall it stands on or fell from; a corner is its outgoing wall's
  u16 slot = 0;      // its place along the wall, 0 for a corner
  u16 height_q = 0;  // the height the ruin rule asked of it, Q10 of the intact wall
  u8 kind = 0;       // PieceKind
  u8 yaw = 0;        // sixteenths of a turn about +y
};

// The sand one wall face asks the terrain for (plan 05 §5.13): a declaration, not sand. 40 bytes.
struct Drift {
  Vec2 from{};  // the face's ends, x and z, metres
  Vec2 to{};
  Vec2 normal{};  // the face's outward normal, x and z
  f32 height = 0.0f;
  f32 reach = 0.0f;
  u32 building = 0;
  u16 wall = 0;
  u8 windward = 0;
  u8 reserved = 0;
};

// One building.
struct Site {
  u64 seed = 0;  // building_seed(world seed, tile)
  TileCoord tile;
  WorldPos origin{};  // the footprint's first vertex, on the ground, metres from integer cm
  u32 first_instance = 0;
  u32 instance_count = 0;
  u32 first_drift = 0;
  u32 drift_count = 0;
  u16 walls = 0;
  u8 shape = 0;  // Shape
  u8 yaw = 0;    // sixteenths of a turn
};

struct Output {
  Vector<Instance> instances;
  Vector<Drift> drifts;
  Vector<Site> sites;
  void clear() noexcept {
    instances.clear();
    drifts.clear();
    sites.clear();
  }
};

// One place a building's rubble lies (docs/subsystems/ruins.md, "The rubble rule"): a block's worth
// of a wall that came down — the kit's profile block, `Kit::rubble_cm3` — beside the wall it came
// from, within `Kit::rubble_radius_cm` of its centre. The sites are the building's, drawn from its
// seed and the wall, and both representations lay their rubble on them: the block layer one fallen
// block a site, at its centre and turned by its yaw, and the section assembler a heap of the kit's
// debris members as large as the block. So a building's two forms have their rubble in the same
// places and in the same amount, and a handover between them does not move what lies on the
// ground. 20 bytes (tests/size_table.cpp).
struct RubbleSite {
  i32 x_cm = 0;  // the site's centre, in the building's frame (Assembler::Frame)
  i32 z_cm = 0;
  i32 along_cm = 0;  // where along its wall the material it stands for came down
  u16 wall = 0;
  u16 index = 0;   // its draw among its wall's sites; a site that could not be put down is skipped
  u8 yaw = 0;      // how the piece on it lies: sixteenths of a turn about +y, in the world
  u8 outward = 0;  // outside the wall, or 0 inside the footprint
  u8 reserved[2] = {0, 0};
};

// Assembles buildings one tile at a time into an `Output`, reusing its scratch across calls, so a
// building allocates nothing but what the output arrays grow by. One per thread; the kit is read
// only.
class Assembler {
 public:
  explicit Assembler(const Kit& kit) noexcept : kit_(kit) {}

  // The building on `tile`, appended to `out` (whether or not the world seed gives the tile one:
  // choosing tiles is `choose_tiles`'s business). False, with nothing appended and `error` set,
  // when no footprint of this kit fits a tile of this size.
  bool assemble(const Placement& placement, TileCoord tile, Output& out, std::string* error);

  // The scratch a building uses, public so the tests can look at a footprint's walls.
  struct Side {
    i32 x0 = 0;  // start vertex, centimetres in the building's frame
    i32 z0 = 0;
    i32 length_cm = 0;  // vertex to vertex, on the centre line
    i32 start_cm = 0;   // what the corner at the start takes of it
    i32 fill_cm = 0;    // what the sections, openings and gaps fill: a whole number of modules
    i32 end_cm = 0;     // what the corner at the end takes of it
    u8 dir = 0;         // quarter turns: 0 +x, 1 -z, 2 -x, 3 +z
    u8 ring = 0;        // 0 the outer wall, 1 a courtyard's inner wall
    u8 start_convex = 1;
    u8 end_convex = 1;
    u8 facing = 1;    // 0 windward, 1 side-on, 2 lee
    u8 state = 0;     // WallState
    i32 level_q = 0;  // a collapsed wall's height fraction
    i32 breach_at_cm = 0;
    i32 breach_half_cm = 0;
    // The opening the wall got, if any: a kit member (~0u for none) whose first socket is
    // `opening_at_cm` along the wall from its start vertex. The block layer lays its gap, sill
    // and lintel from these.
    u32 opening = ~0u;
    i32 opening_at_cm = 0;
  };
  const Vector<Side>& sides() const noexcept { return sides_; }
  Shape shape() const noexcept { return shape_; }

  // Where the building it last assembled stands, in integer centimetres: its footprint's frame
  // is turned `yaw` sixteenths about +y and put at `origin`, and every wall stands on `base_cm`.
  // With `sides()` and `height_q` this is everything a second representation of the same building
  // needs — the block layer (blocks.h) lays its courses from it — and nothing in it is a float.
  struct Frame {
    u64 seed = 0;  // building_seed(world seed, tile)
    i64 origin_x_cm = 0;
    i64 origin_z_cm = 0;
    i32 base_cm = 0;
    u8 yaw = 0;
    // The tile it was fitted to: nothing it puts down, rubble included, leaves this square.
    i64 tile_x0_cm = 0;
    i64 tile_z0_cm = 0;
    i32 tile_cm = 0;
  };
  const Frame& frame() const noexcept { return frame_; }
  // The ruin rule's height at `at_cm` along wall `wall` of the building it last assembled, Q10 of
  // the intact wall: the line the sections were chosen by, and the one a block stands under.
  i32 height_q(u32 wall, i32 at_cm) const noexcept;
  // A point of the building's frame in the world, centimetres (the Q14 turn, then the origin).
  void to_world_cm(i64 x, i64 z, i64& wx, i64& wz) const noexcept;

  // The rubble field of the building it last assembled (ruins.md, "The rubble rule"), wall by
  // wall: how much of each wall came down is the ruin line integrated along it, the rule's
  // `debris_per_module` of it lies beside the wall in blocks' worth, and each site's place along
  // the wall follows where the wall came down. A function of the building alone, whatever detail
  // it was assembled at, worked out the first time it is asked for and kept until the next
  // building: the section assembler heaps its debris members on it, and the block layer, which
  // assembles the walls only, asks for it to lay its fallen blocks.
  const Vector<RubbleSite>& rubble();

 private:
  bool build_footprint(u64 seed, i32 tile_cm, u32 attempt);
  bool ring_from(const u8* turns, const i32* lengths, u32 count, i32 x0, i32 z0, u8 ring);
  bool footprint_valid() const noexcept;
  i32 profile_q(const Side& side, u32 wall, u64 seed, i32 at_cm) const noexcept;

  const Kit& kit_;
  Vector<Side> sides_;
  Vector<u32> candidates_;
  Shape shape_ = Shape::rectangle;
  Frame frame_;
  Vector<RubbleSite> rubble_;
  Vector<i64> removed_;  // one wall's removal, cumulative over its samples: the rubble field's
  bool rubble_ready_ = false;
};

// The building's seed: the world seed and the tile, through the engine's hash.
u64 building_seed(u64 world_seed, TileCoord tile) noexcept;
// Where a tile ranks under the seed, 24 bits: a tile holds a building at density d when its rank
// is below d * 2^24, and a scatter of `count` buildings takes the lowest-ranked tiles.
u32 tile_rank(u64 world_seed, TileCoord tile) noexcept;
bool tile_has_building(u64 world_seed, TileCoord tile, f32 density) noexcept;
// The tiles of the square [min, max] (both included) that hold a building: the `count`
// lowest-ranked (ties by z, then x), or with `count` zero every tile under `density`. In the
// order of their rank, which is the order the buildings are numbered in.
void choose_tiles(u64 world_seed, TileCoord min, TileCoord max, u32 count, f32 density,
                  Vector<TileCoord>& out);

// Many tiles, on the job system's performance pool when one is given. The output is in the
// order of `tiles` whatever the thread count: each job fills its own contiguous run of tiles, and
// the runs are joined in tile order.
bool assemble_tiles(const Kit& kit, const Placement& placement, std::span<const TileCoord> tiles,
                    jobs::JobSystem* jobs, Output& out, std::string* error);

// A hash over every field of every record, in order, field by field (no padding bytes): the
// number the determinism test pins.
u64 hash_output(const Output& out) noexcept;

// Sixteenths of a turn from degrees, rounded to nearest.
u8 yaw_step_from_degrees(f32 degrees) noexcept;
// cos and sin of a sixteenth-turn step, from a table rather than the C library.
f32 step_cos(u32 step) noexcept;
f32 step_sin(u32 step) noexcept;

// The world translation and yaw of an instance's *mesh*: the instance's position and yaw with the
// member's own offset and yaw folded in (translation = position + R(yaw + member yaw) * offset,
// from the table): the offset turned in float32, the size of a member, and added in f64. What the
// placement generator places and a renderer draws.
WorldPos instance_translation(const Kit& kit, const Instance& instance) noexcept;
u32 instance_yaw_step(const Kit& kit, const Instance& instance) noexcept;

}  // namespace engine::ruins
