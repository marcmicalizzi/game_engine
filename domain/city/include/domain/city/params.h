#pragma once

// An island's parameters, converted (docs/subsystems/city.md, "Parameters and style tables"): the
// authored `IslandParams` (metres, degrees and fractions, schemas/city.schema) turned into integer
// centimetres, 64ths of a turn and Q16 fractions once, when read, so that no decision of the plan
// or the grammar ever reads a float. The conversion is the only place a parsed number becomes an
// integer, and it rounds the same way on every toolchain (`std::llround` of the f32 widened to
// f64, which is exact for the magnitudes a city has).

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <schemas/city.h>
#include <string>
#include <string_view>

namespace engine::city {

inline constexpr i32 k_q16 = 65536;
inline constexpr u32 k_district_kinds = 7;
inline constexpr u32 k_street_classes = 5;
inline constexpr u32 k_archetypes = 7;
inline constexpr u32 k_work_kinds = 7;
inline constexpr u32 k_unit_kinds = 9;
inline constexpr u32 k_space_kinds = 21;
inline constexpr u32 k_rules = 23;

const char* district_kind_name(DistrictKind kind) noexcept;
const char* street_class_name(StreetClass cls) noexcept;
const char* archetype_name(Archetype archetype) noexcept;
const char* park_kind_name(ParkKind kind) noexcept;
const char* civic_kind_name(CivicKind kind) noexcept;
const char* space_kind_name(SpaceKind kind) noexcept;
const char* unit_kind_name(UnitKind kind) noexcept;
const char* work_kind_name(WorkKind kind) noexcept;
const char* rule_name(Rule rule) noexcept;
const char* stage_name(Stage stage) noexcept;
bool parse_archetype(std::string_view text, Archetype& out) noexcept;
bool parse_stage(std::string_view text, Stage& out) noexcept;

// Metres to centimetres, degrees to 64ths of a turn, a fraction to Q16: the conversions every
// authored number goes through once.
i32 to_cm(f32 metres) noexcept;
u32 to_step64(f32 degrees) noexcept;
i32 to_q16(f32 fraction) noexcept;

// One district kind's style, converted.
struct Style {
  DistrictKind kind = DistrictKind::Residential;
  StreetPattern pattern = StreetPattern::Grid;
  LotLayout lot_layout = LotLayout::Perimeter;
  i32 block_min_cm = 0;
  i32 block_max_cm = 0;
  i32 lot_width_min_cm = 0;
  i32 lot_width_max_cm = 0;
  i32 lot_depth_min_cm = 0;
  i32 lot_depth_max_cm = 0;
  i32 setback_front_cm = 0;
  i32 setback_side_cm = 0;
  i32 setback_rear_cm = 0;
  i32 coverage_q = 0;
  u32 floors_min = 1;
  u32 floors_max = 1;
  i32 height_max_cm = 0;
  i32 park_share_q = 0;
  i32 square_q = 0;
  i32 lane_q = 0;
  i32 merge_q = 0;
  // Archetype weights, Q16, indexed by Archetype; the dwelling mix, Q16, studio to three bedrooms.
  i32 archetype_q[k_archetypes] = {};
  i32 unit_mix_q[4] = {};
};

// The building rules, converted.
struct Rules {
  i32 module_cm = 200;
  i32 floor_cm = 320;
  i32 ground_floor_cm = 420;
  i32 exterior_cm = 30;
  i32 core_wall_cm = 25;
  i32 party_cm = 20;
  i32 partition_cm = 10;
  i32 corridor_cm = 180;
  i32 civic_corridor_cm = 280;
  i32 service_band_cm = 240;
  i32 unit_depth_min_cm = 640;
  i32 unit_depth_max_cm = 880;
  u32 two_stair_floors = 4;
  i32 stair_separation_q = 0;
  i32 max_travel_cm = 3000;
  i32 stair_cm = 280;
  i32 elevator_cm = 240;
  i32 shaft_cm = 140;
  u32 elevator_floors = 5;
  u32 units_per_elevator = 60;
  u32 workplaces_per_elevator = 250;
  i32 max_span_cm = 900;
  i32 max_span_house_cm = 700;
  i32 max_span_hall_cm = 3000;
  i32 door_cm = 90;
  i32 entrance_cm = 140;
  i32 loading_cm = 400;
  i32 window_cm = 140;
  i32 window_sill_cm = 90;
  i32 window_head_cm = 230;
  i32 bed_width_cm = 160;
  i32 bed_length_cm = 200;
  i32 bed_clearance_cm = 60;
  i64 office_area_cm2 = 0;
  i64 retail_area_cm2 = 0;
  i64 industrial_area_cm2 = 0;
  i64 civic_area_cm2 = 0;
  // Minimum clear width (the smaller side) and depth per space kind; zero where there is none.
  i32 min_width_cm[k_space_kinds] = {};
  i32 min_depth_cm[k_space_kinds] = {};
};

struct Hours {
  u16 open = 0;
  u16 close = 0;
  u8 days = 0;
  bool set = false;
};

// An authored pin, as read.
struct Pin {
  OverrideKind what = OverrideKind::District;
  u32 id = 0;
  DistrictKind district_kind = DistrictKind::Residential;
  StreetClass street_class = StreetClass::Local;
  ParkKind park = ParkKind::None;
  CivicKind civic = CivicKind::None;
};

// Everything the plan is a function of, in integers. `source` is the authored form, kept so the
// plan file can carry it and the key can hash it.
struct Params {
  IslandParams source;
  std::string name;
  u64 seed = 1;
  i32 radius_cm = 0;
  i32 roughness_q = 0;
  Vector<i32> coastline_cm;  // x0 z0 x1 z1 ..., empty for the seeded outline
  u32 mountain_step = 0;     // 64ths of a turn about +y from +x
  i32 mountain_share_q = 0;
  i32 greenbelt_cm = 0;
  u32 harbour_step = 0;
  i32 harbour_width_cm = 0;
  i32 harbour_depth_cm = 0;
  i32 tile_cm = 6400;
  i32 arterial_spacing_cm = 0;
  i32 arterial_jitter_q = 0;
  i32 district_size_cm = 0;
  i32 district_share_q[k_district_kinds] = {};
  Style styles[k_district_kinds];
  i32 street_width_cm[k_street_classes] = {};
  i32 walk_distance_cm = 0;
  i32 central_share_q = 0;
  u32 civic_count[5] = {};  // by CivicKind; [0] unused
  Rules rules;
  Hours hours[k_work_kinds];
  Vector<Pin> pins;
};

// The default island (docs/subsystems/city.md, "Parameters and style tables"): every table filled,
// so a file written from it names every number an owner might want to change. `params_from_schema`
// fills whatever a file leaves empty from the same defaults.
IslandParams default_island_params();
DistrictStyle default_style(DistrictKind kind);
BuildingRules default_building_rules();

// Converts and checks. False with a sentence naming the field when a number is out of range or a
// table names the same kind twice.
bool params_from_schema(const IslandParams& source, Params& out, std::string& error);
bool read_params_file(const std::string& path, Params& out, std::string& error);
bool write_params_file(const std::string& path, const IslandParams& params, std::string* error);

}  // namespace engine::city
