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

// The terrain's height at (x, z), metres: a plain function and a context, because the assembler
// is in the domain layer and the terrain in the renderer, and because it is called a few dozen
// times a building, never in an inner loop over instances. No `fn` is flat ground at 0.
using HeightFn = f32 (*)(const void* context, f32 x, f32 z) noexcept;
struct Ground {
  HeightFn fn = nullptr;
  const void* context = nullptr;
  f32 at(f32 x, f32 z) const noexcept { return fn != nullptr ? fn(context, x, z) : 0.0f; }
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
};

// One placed piece: 28 bytes, 4-aligned (tests/size_table.cpp). The world transform is `position`
// and `yaw` sixteenths of a turn about +y, applied to the member's frame; `instance_translation`
// folds the member's own mesh offset in.
struct Instance {
  Vec3 position{};   // metres: x and z from integer centimetres, y from the ground query
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
  Vec3 origin{};  // the footprint's first vertex, on the ground, metres
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
  };
  const Vector<Side>& sides() const noexcept { return sides_; }
  Shape shape() const noexcept { return shape_; }

 private:
  // What one corner, section or gap lost to the ruin rule: the debris pass's input.
  struct Loss {
    u32 wall;
    u32 slot;
    i32 at_cm;  // along the wall, from its start vertex
    i32 length_cm;
    i32 removed_q;  // the intact height less the height asked of it, Q10
  };

  bool build_footprint(u64 seed, i32 tile_cm, u32 attempt);
  bool ring_from(const u8* turns, const i32* lengths, u32 count, i32 x0, i32 z0, u8 ring);
  bool footprint_valid() const noexcept;
  i32 profile_q(const Side& side, u32 wall, u64 seed, i32 at_cm) const noexcept;

  const Kit& kit_;
  Vector<Side> sides_;
  Vector<Loss> losses_;
  Vector<u32> candidates_;
  Shape shape_ = Shape::rectangle;
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
// from the table). What a scene file writes and a renderer draws.
Vec3 instance_translation(const Kit& kit, const Instance& instance) noexcept;
u32 instance_yaw_step(const Kit& kit, const Instance& instance) noexcept;

}  // namespace engine::ruins
