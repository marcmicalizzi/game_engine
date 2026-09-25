#pragma once

// The building grammar (docs/subsystems/city.md, "The building grammar"; plan 13 §13.2): one lot of
// the plan to one building, as a **description** — floors, cores, spaces, units, walls as centre
// lines with their thickness and openings, doors and windows, furniture zones — and its occupancy
// summary. No meshes: the kits attach later by the ruins' socket convention, and until then the
// proxy fragment (fragment.h) draws the description with a dozen box meshes.
//
// The stages, each fixed before the next reads it, as plan 13 §13.2 writes them:
//
//   footprint -> structural grid -> building type -> floor count -> vertical cores (stairs,
//   elevators) -> entrances -> service shafts -> floor plans -> units -> room graphs ->
//   doors and windows -> furniture zones
//
// A building is a pure function of (plan, lot): every draw is the engine's hash of the lot's seed,
// a purpose and an index, every decision integer centimetres and Q16, and nothing reads a clock, a
// thread or a global. So a tile materialized twice holds the same buildings, and a building's
// identity is (generator version, plan key, lot) — its units and rooms are numbered from it and
// name the same apartment whenever it is generated again (plan 13 §13.2, "Persistent interior
// identity").
//
// Coordinates are the building's own frame: x along the front facade, z away from the street, the
// footprint at [0, width) x [0, depth). `to_plan` puts a point of it on the plan.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/city/plan.h>

#include <schemas/city.h>
#include <span>
#include <string>

namespace engine::jobs {
class JobSystem;
}

namespace engine::city {

inline constexpr u32 k_outside = 0xffffffffu;

struct Floor {
  i32 elevation_cm = 0;
  i32 height_cm = 0;
  u32 first_space = 0;
  u32 space_count = 0;
  u32 first_wall = 0;
  u32 wall_count = 0;
};

struct Core {
  Rect rect;
  u16 floor_from = 0;
  u16 floor_to = 0;
  u8 stair = 0;
  u8 elevators = 0;
  u8 shaft = 0;
  u8 main = 0;
};

// One space on one floor: a room, a corridor, a lobby, a stair. 28 bytes.
struct Space {
  Rect rect;
  u32 unit = k_no_id;  // index into Building::units, or none for circulation and cores
  u16 floor = 0;
  SpaceKind kind = SpaceKind::Corridor;
  u8 flags = 0;        // k_space_*
  u32 core = k_no_id;  // index into Building::cores for a core's spaces
};
inline constexpr u8 k_space_habitable = 1;  // needs daylight
inline constexpr u8 k_space_vertical = 2;   // continues to the floors above and below

struct Unit {
  Rect rect;
  u32 first_space = 0;
  u32 space_count = 0;
  u32 entry = 0;  // index into Building::spaces
  u16 floor = 0;
  UnitKind kind = UnitKind::Studio;
  WorkKind work = WorkKind::Retail;
  u16 bedrooms = 0;
  u16 residents = 0;
  u16 workplaces = 0;
  u16 floor_to = 0;  // a house spans floors
};

// One wall: a centre line between two spaces (or a space and the outside). 32 bytes.
struct Wall {
  i32 x0 = 0;
  i32 z0 = 0;
  i32 x1 = 0;
  i32 z1 = 0;
  u32 a = k_outside;  // the space on its low side (smaller z for a wall along x, smaller x else)
  u32 b = k_outside;
  u16 floor = 0;
  u16 thickness_cm = 0;
  WallKind kind = WallKind::Partition;
  u8 reserved[3] = {0, 0, 0};
};

// A door or a window in a wall. 20 bytes.
struct Opening {
  u32 wall = 0;
  i32 at_cm = 0;  // from the wall's (x0, z0) end to the opening's centre
  u16 width_cm = 0;
  u16 sill_cm = 0;
  u16 head_cm = 0;
  OpeningKind kind = OpeningKind::Door;
  u8 reserved = 0;
  u32 reserved2 = 0;
};

struct Zone {
  Rect rect;
  u32 space = 0;
  ZoneKind kind = ZoneKind::Bed;
  u8 reserved[3] = {0, 0, 0};
};

// A link through a floor: a stair or an elevator space continuing to the same space on the floor
// above. The reachability validator walks them.
struct Link {
  u32 below = 0;  // spaces
  u32 above = 0;
};

// The building's occupancy summary: the fields the NPC routine work reads (docs/subsystems/city.md,
// "The occupancy summary").
struct OccupancySummary {
  u32 dwellings = 0;
  u32 dwellings_by_kind[5] = {};  // studio, one, two, three bedrooms, house
  u32 bedrooms = 0;
  u32 residents = 0;
  u32 workplaces[k_work_kinds] = {};
};

struct Building {
  u32 lot = k_no_id;  // index into Plan::lots
  u32 lot_id = k_no_id;
  u64 seed = 0;
  Archetype archetype = Archetype::MidRiseOverShops;
  CivicKind civic = CivicKind::None;
  Stage stage = Stage::Rooms;
  u8 front = 0;
  i32 origin_x = 0;  // the frame's origin on the plan
  i32 origin_z = 0;
  Rect footprint;    // in the building's frame
  Rect lot_rect;     // the lot, in the building's frame
  i32 bay_x_cm = 0;  // the structural grid
  i32 bay_z_cm = 0;
  i32 corridor_cm = 0;
  u8 exposed[4] = {1, 1, 1, 1};  // facades with daylight: front, right, back, left
  Vector<Floor> floors;
  Vector<Core> cores;
  Vector<Space> spaces;
  Vector<Unit> units;
  Vector<Wall> walls;
  Vector<Opening> openings;
  Vector<Zone> zones;
  Vector<Link> links;
  OccupancySummary occupancy;

  void clear() noexcept;
  i32 height_cm() const noexcept;
};

// The capability's LOD policy (ADR-0027): how far the grammar runs for a tile at this distance
// from the nearest observer, in tiles. A far tile gets massing only, a middle one cores and floors,
// a near one rooms.
Stage stage_for_distance(f32 distance_tiles) noexcept;

// Generates one building, reusing `out`'s arrays. False with a sentence when the lot is not a
// building lot or is too small for any archetype (the plan never gives one such a use).
class Grammar {
 public:
  explicit Grammar(const Plan& plan) noexcept : plan_(plan) {}
  bool generate(u32 lot, Stage stage, Building& out, std::string* error);

 private:
  const Plan& plan_;
  Vector<u32> scratch_;
};

// Many lots, on the job system's performance pool when one is given; the output is in the order of
// `lots` whatever the thread count, each building generated whole on one thread.
bool generate_buildings(const Plan& plan, std::span<const u32> lots, Stage stage,
                        jobs::JobSystem* jobs, Vector<Building>& out, std::string* error);

// A point of the building's frame on the plan, centimetres.
void to_plan(const Building& building, i64 x, i64 z, i64& px, i64& pz) noexcept;
// The footprint on the plan.
Rect footprint_on_plan(const Building& building) noexcept;

// Every field of every record, in order: the number the golden test pins.
u64 hash_building(const Building& building) noexcept;

// The building as its file (schemas/city.schema, BuildingFile), with what the validators found.
struct Report;
void building_to_schema(const Plan& plan, const Building& building, const Report* report,
                        BuildingFile& out);

}  // namespace engine::city
