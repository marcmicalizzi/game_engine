#pragma once

// The whole-island plan (docs/subsystems/city.md, "The plan"): a pure function of an island's
// parameters, computed once and cached as a derived node — coastline, districts, the street graph,
// blocks, lots, parks, plazas and civic reservations, every coordinate an integer centimetre.
//
// **Why a whole-island plan and not a per-tile street generator.** Island City is finite (plan 13
// §13.2) and its streets are a property of the island: an arterial crosses many tiles, a district
// is a region, a park rule ("a park within walking distance of every dwelling") is a question about
// the whole city, and a street graph assembled tile by tile has no way to be connected or free of
// dead-end arterials except by luck. So the plan is made once, from (seed, parameters), and every
// **building** is then a function of (plan, lot) that a tile materializes on demand
// (building.h): a tile's content is the lots that intersect it (`lots_in_tile`). ADR-0044 records
// the decision and the alternatives.
//
// **Stable ids.** Ids are composed from the arterial grid, which no pin changes, so an authored pin
// names the same thing whatever else is pinned: a superblock is the cell between two arterials; a
// block is `superblock * 256 + its index in the superblock`; a lot is `block * 1024 + its index in
// the block`; a street is an arterial line (`axis * 512 + line`) or `(superblock + 1) * 1024 +` a
// line or an interior street of that superblock. A district pin changes the pattern of the
// superblocks it covers and renumbers what lies inside them, and nothing else.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/city/params.h>

#include <schemas/city.h>
#include <span>
#include <string>

namespace engine::jobs {
class JobSystem;
}

namespace engine::city {

// Bumped whenever a change makes a plan or a building the same parameters used to give come out
// different: it is part of the plan's derived-data key and of every building's identity.
inline constexpr u32 k_generator_version = 1;

inline constexpr u32 k_no_id = 0xffffffffu;

// An axis-aligned rectangle, centimetres: [x0, x1) x [z0, z1).
struct Rect {
  i32 x0 = 0;
  i32 z0 = 0;
  i32 x1 = 0;
  i32 z1 = 0;
  constexpr i32 width() const noexcept { return x1 - x0; }
  constexpr i32 depth() const noexcept { return z1 - z0; }
  constexpr i64 area() const noexcept { return i64{x1 - x0} * i64{z1 - z0}; }
  constexpr bool operator==(const Rect&) const = default;
};

struct TileCoord {
  i32 x = 0;
  i32 z = 0;
  constexpr bool operator==(const TileCoord&) const = default;
};

struct District {
  u32 id = 0;
  DistrictKind kind = DistrictKind::Residential;
  u8 pinned = 0;
  u16 superblocks = 0;
  i32 cx = 0;  // the seed cell's centre
  i32 cz = 0;
  i64 area_cm2 = 0;  // kept blocks
};

// One named line: an arterial, or a line or interior street of one superblock. 32 bytes.
struct Street {
  u32 id = 0;
  StreetClass cls = StreetClass::Local;
  u8 along_x = 1;  // runs along x (its line is a z), or along z
  u8 pinned = 0;
  u8 interior = 0;  // a close, an alley or a promenade inside a block
  i32 line_cm = 0;
  i32 from_cm = 0;  // where its kept segments begin and end on the line
  i32 to_cm = 0;
  i32 width_cm = 0;
  u32 superblock = k_no_id;
  u32 reserved = 0;
};

// A node of the street graph and an edge between two: a street's stretch between two consecutive
// nodes on its line.
struct Node {
  i32 x = 0;
  i32 z = 0;
};
struct Segment {
  u32 street = 0;  // index into Plan::streets
  u32 a = 0;       // nodes, a before b along the line
  u32 b = 0;
  StreetClass cls = StreetClass::Local;
  u8 reserved[3] = {0, 0, 0};
};

// 72 bytes.
struct Block {
  u32 id = 0;
  u32 district = 0;  // index into Plan::districts
  u32 superblock = 0;
  Rect rect;       // on the street centre lines
  Rect buildable;  // what the streets' halves leave
  ParkKind park = ParkKind::None;
  CivicKind civic = CivicKind::None;
  u8 pin_park = 0;   // an authored park pin (or a pin keeping the rules off it)
  u8 pin_civic = 0;  // an authored civic pin keeps the civic rules off it
  u32 first_lot = 0;
  u32 lot_count = 0;
  u32 streets[4] = {k_no_id, k_no_id, k_no_id, k_no_id};  // on its sides, quarter-turn order
};

// One lot: 36 bytes, 4-aligned (tests/size_table.cpp). A building is a function of the plan and
// the lot alone.
struct Lot {
  u32 id = 0;
  u32 block = 0;  // index into Plan::blocks
  Rect rect;
  u32 street = 0;  // index into Plan::streets: the street it fronts
  u16 district = 0;
  u8 front = 0;  // quarter turn from the lot toward its street: 0 +x, 1 -z, 2 -x, 3 +z
  LotUse use = LotUse::Building;
  Archetype archetype = Archetype::MidRiseOverShops;
  ParkKind park = ParkKind::None;
  CivicKind civic = CivicKind::None;
  u8 reserved = 0;
};

struct Park {
  ParkKind kind = ParkKind::District;
  u32 block = 0;      // index into Plan::blocks
  u32 lot = k_no_id;  // index into Plan::lots for a pocket park
  u32 district = 0;
  Rect rect;
};

// One entry of the per-tile index: a tile a lot's rectangle overlaps.
struct TileEntry {
  u64 key = 0;
  u32 lot = 0;  // index into Plan::lots
  u32 reserved = 0;
};

struct Plan {
  Params params;
  u64 key = 0;
  Vector<i32> coastline;  // x0 z0 x1 z1 ..., centimetres, counter-clockwise from above
  Vector<i32> arterials_x;
  Vector<i32> arterials_z;
  i32 mountain_ux = 0;  // Q14
  i32 mountain_uz = 0;
  i32 city_limit_cm = 0;      // along the mountain axis: the city ends here
  i32 mountain_start_cm = 0;  // and the mountain begins here
  Vector<District> districts;
  Vector<Street> streets;  // sorted by id
  Vector<Node> nodes;      // sorted by (z, x)
  Vector<Segment> segments;
  Vector<Block> blocks;  // sorted by id
  Vector<Lot> lots;      // sorted by id
  Vector<Park> parks;
  // The per-tile index, sorted by (key, lot). Rebuilt whenever a plan is made or read.
  Vector<TileEntry> tile_index;

  // Index of the record with this id, or k_no_id.
  u32 find_block(u32 id) const noexcept;
  u32 find_lot(u32 id) const noexcept;
  u32 find_street(u32 id) const noexcept;
};

// The plan of an island. False with a sentence when the parameters leave no city (the island is
// all mountain) or an authored pin names an id the plan does not have. `jobs` generates the
// superblocks' lots on the performance pool when given; the plan is the same on any thread count.
bool generate_plan(const Params& params, Plan& out, std::string& error,
                   jobs::JobSystem* jobs = nullptr);

// Is (x, z) inside the coastline? Integer crossing test.
bool on_land(const Plan& plan, i64 x, i64 z) noexcept;
// Where (x, z) lies along the mountain axis, centimetres.
i64 along_mountain(const Plan& plan, i64 x, i64 z) noexcept;

// ---- the per-tile query ------------------------------------------------------------------------

u64 tile_key(TileCoord tile) noexcept;
TileCoord tile_of(const Plan& plan, i64 x_cm, i64 z_cm) noexcept;
Rect tile_rect(const Plan& plan, TileCoord tile) noexcept;
// The lots whose closed rectangle touches the tile, in lot order (their indices into Plan::lots):
// closed, because a building's facade stands on its lot's line, and what stands on a tile's edge
// belongs to the tile beyond it.
void lots_in_tile(const Plan& plan, TileCoord tile, Vector<u32>& out);
// The one tile a lot belongs to: the one holding its centre. What a consumer that must see each
// building exactly once (an NPC census, a save) keys by.
TileCoord owner_tile(const Plan& plan, const Lot& lot) noexcept;
// Rebuilds `tile_index` from the lots.
void build_tile_index(Plan& plan);

// ---- identity ----------------------------------------------------------------------------------

// The plan's derived-data key: the canonical JSON of the parameters, hashed with the generator
// version. Nothing about the machine: the same parameters are the same key everywhere.
u64 plan_key(const IslandParams& params);
// "<ddc_root>/city/<key as 16 hex digits>".
std::string plan_cache_dir(std::string_view ddc_root, u64 key);
// Every field of every record, in order: the number the golden test pins.
u64 hash_plan(const Plan& plan) noexcept;
// A lot's seed: the island's seed and the lot's id through the engine's hash.
u64 lot_seed(const Plan& plan, const Lot& lot) noexcept;

// ---- the file ----------------------------------------------------------------------------------

void plan_to_schema(const Plan& plan, PlanFile& out);
// Reads a plan file back; the graph and the tile index are rebuilt from its streets and lots. False
// when the file's generator version is not this build's, or its hash does not match what it holds.
bool plan_from_schema(const PlanFile& file, Plan& out, std::string& error);
bool write_plan_file(const std::string& path, const Plan& plan, std::string* error);
bool read_plan_file(const std::string& path, Plan& out, std::string& error);

// ---- statistics --------------------------------------------------------------------------------

struct PlanStats {
  u32 districts_by_kind[k_district_kinds] = {};
  i64 district_area_cm2[k_district_kinds] = {};
  u32 streets_by_class[k_street_classes] = {};
  i64 street_length_cm[k_street_classes] = {};
  u32 blocks = 0;
  u32 lots = 0;
  u32 lots_by_archetype[k_archetypes] = {};
  u32 parks_by_kind[7] = {};
  i64 park_area_cm2 = 0;
  u32 civic_by_kind[5] = {};
  i64 city_area_cm2 = 0;
  u32 nodes = 0;
  u32 segments = 0;
};
void plan_stats(const Plan& plan, PlanStats& out) noexcept;

}  // namespace engine::city
