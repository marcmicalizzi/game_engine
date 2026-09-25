#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/city/params.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace engine::city {

namespace {

constexpr const char* k_params_format = "engine.city-params.v1";

template <class E, usize N>
const char* name_of(E value, const char* const (&names)[N]) noexcept {
  const auto i = static_cast<usize>(value);
  return i < N ? names[i] : "unknown";
}

constexpr const char* k_district_names[] = {"downtown",   "mixed_use",  "residential", "old_town",
                                            "industrial", "waterfront", "campus"};
constexpr const char* k_class_names[] = {"arterial", "collector", "local", "alley", "pedestrian"};
constexpr const char* k_archetype_names[] = {"apartment_tower", "midrise_over_shops", "office",
                                             "terraced_house",  "detached_house",     "warehouse",
                                             "civic_shell"};
constexpr const char* k_park_names[] = {"none",   "central",    "district", "pocket",
                                        "square", "waterfront", "greenbelt"};
constexpr const char* k_civic_names[] = {"none", "hospital", "university", "station", "government"};
constexpr const char* k_space_names[] = {
    "corridor",       "lobby",     "stair",       "elevator", "shaft",     "entry",
    "living_kitchen", "bedroom",   "bathroom",    "wc",       "landing",   "kitchen",
    "shop_floor",     "back_room", "open_office", "meeting",  "reception", "hall",
    "department",     "concourse", "storage"};
constexpr const char* k_unit_names[] = {"studio", "one_bed",      "two_bed",  "three_bed", "house",
                                        "shop",   "office_suite", "workshop", "department"};
constexpr const char* k_work_names[] = {"retail",    "office", "industrial", "medical",
                                        "education", "civic",  "transport"};
constexpr const char* k_rule_names[] = {"plan.connectivity",
                                        "plan.dead_end_arterial",
                                        "plan.block_size",
                                        "plan.park_share",
                                        "plan.park_distance",
                                        "plan.frontage",
                                        "reach.room",
                                        "reach.unit",
                                        "reach.core",
                                        "egress.stairs",
                                        "egress.separation",
                                        "egress.travel",
                                        "daylight.window",
                                        "fit.room",
                                        "fit.bed",
                                        "fit.corridor",
                                        "services.elevators",
                                        "services.shaft",
                                        "structure.span",
                                        "site.setback",
                                        "site.coverage",
                                        "site.height",
                                        "site.frontage"};
constexpr const char* k_stage_names[] = {"massing", "floors", "rooms"};

static_assert(sizeof(k_district_names) / sizeof(k_district_names[0]) == k_district_kinds);
static_assert(sizeof(k_class_names) / sizeof(k_class_names[0]) == k_street_classes);
static_assert(sizeof(k_archetype_names) / sizeof(k_archetype_names[0]) == k_archetypes);
static_assert(sizeof(k_space_names) / sizeof(k_space_names[0]) == k_space_kinds);
static_assert(sizeof(k_unit_names) / sizeof(k_unit_names[0]) == k_unit_kinds);
static_assert(sizeof(k_work_names) / sizeof(k_work_names[0]) == k_work_kinds);
static_assert(sizeof(k_rule_names) / sizeof(k_rule_names[0]) == k_rules);

ArchetypeWeight weight(Archetype a, f32 w) {
  ArchetypeWeight out;
  out.archetype = a;
  out.weight = w;
  return out;
}

bool positive(f32 v) { return v > 0.0f && std::isfinite(v); }
bool fraction(f32 v) { return v >= 0.0f && v <= 1.0f; }

}  // namespace

const char* district_kind_name(DistrictKind kind) noexcept {
  return name_of(kind, k_district_names);
}
const char* street_class_name(StreetClass cls) noexcept { return name_of(cls, k_class_names); }
const char* archetype_name(Archetype archetype) noexcept {
  return name_of(archetype, k_archetype_names);
}
const char* park_kind_name(ParkKind kind) noexcept { return name_of(kind, k_park_names); }
const char* civic_kind_name(CivicKind kind) noexcept { return name_of(kind, k_civic_names); }
const char* space_kind_name(SpaceKind kind) noexcept { return name_of(kind, k_space_names); }
const char* unit_kind_name(UnitKind kind) noexcept { return name_of(kind, k_unit_names); }
const char* work_kind_name(WorkKind kind) noexcept { return name_of(kind, k_work_names); }
const char* rule_name(Rule rule) noexcept { return name_of(rule, k_rule_names); }
const char* stage_name(Stage stage) noexcept { return name_of(stage, k_stage_names); }

bool parse_archetype(std::string_view text, Archetype& out) noexcept {
  for (u32 i = 0; i < k_archetypes; ++i) {
    if (text == k_archetype_names[i]) {
      out = static_cast<Archetype>(i);
      return true;
    }
  }
  return false;
}

bool parse_stage(std::string_view text, Stage& out) noexcept {
  for (u32 i = 0; i < 3; ++i) {
    if (text == k_stage_names[i]) {
      out = static_cast<Stage>(i);
      return true;
    }
  }
  return false;
}

// f32 widened to f64 is exact, and a city's metres times 100 stay far inside f64's 53 bits, so
// `llround` sees the same number on every toolchain and rounds it the same way.
i32 to_cm(f32 metres) noexcept {
  return static_cast<i32>(std::llround(static_cast<f64>(metres) * 100.0));
}
u32 to_step64(f32 degrees) noexcept {
  const i64 steps = std::llround(static_cast<f64>(degrees) / 5.625);
  return static_cast<u32>(((steps % 64) + 64) % 64);
}
i32 to_q16(f32 fraction) noexcept {
  return static_cast<i32>(std::llround(static_cast<f64>(fraction) * 65536.0));
}

DistrictStyle default_style(DistrictKind kind) {
  DistrictStyle s;
  s.kind = kind;
  switch (kind) {
    case DistrictKind::Downtown:
      s.pattern = StreetPattern::Grid;
      s.block_min = 80.0f;
      s.block_max = 140.0f;
      s.lot_layout = LotLayout::AlleyRows;
      s.lot_width_min = 26.0f;
      s.lot_width_max = 44.0f;
      s.lot_depth_min = 24.0f;
      s.lot_depth_max = 64.0f;
      s.setback_front = 0.0f;
      s.setback_side = 0.0f;
      s.setback_rear = 3.0f;
      s.coverage_max = 0.7f;
      s.floors_min = 10;
      s.floors_max = 40;
      s.height_max = 140.0f;
      s.park_share = 0.04f;
      s.archetypes = {weight(Archetype::ApartmentTower, 5.0f), weight(Archetype::Office, 4.0f),
                      weight(Archetype::MidRiseOverShops, 1.0f)};
      s.unit_mix = {0.25f, 0.40f, 0.25f, 0.10f};
      break;
    case DistrictKind::MixedUse:
      s.pattern = StreetPattern::CoarseGrid;
      s.block_min = 90.0f;
      s.block_max = 170.0f;
      s.lot_layout = LotLayout::Perimeter;
      s.lot_width_min = 14.0f;
      s.lot_width_max = 26.0f;
      s.lot_depth_min = 16.0f;
      s.lot_depth_max = 22.0f;
      s.setback_rear = 2.0f;
      s.coverage_max = 0.8f;
      s.floors_min = 4;
      s.floors_max = 8;
      s.height_max = 30.0f;
      s.park_share = 0.06f;
      s.archetypes = {weight(Archetype::MidRiseOverShops, 6.0f), weight(Archetype::Office, 1.0f)};
      s.unit_mix = {0.20f, 0.35f, 0.30f, 0.15f};
      break;
    case DistrictKind::Residential:
      s.pattern = StreetPattern::Closes;
      s.block_min = 60.0f;
      s.block_max = 240.0f;
      s.lot_layout = LotLayout::CloseRows;
      s.lot_width_min = 7.0f;
      s.lot_width_max = 18.0f;
      s.lot_depth_min = 26.0f;
      s.lot_depth_max = 32.0f;
      s.setback_front = 4.0f;
      s.setback_side = 1.5f;
      s.setback_rear = 6.0f;
      s.coverage_max = 0.5f;
      s.floors_min = 2;
      s.floors_max = 3;
      s.height_max = 12.0f;
      s.park_share = 0.08f;
      s.archetypes = {weight(Archetype::TerracedHouse, 5.0f),
                      weight(Archetype::DetachedHouse, 4.0f)};
      s.unit_mix = {0.0f, 0.0f, 0.0f, 0.0f};
      break;
    case DistrictKind::OldTown:
      s.pattern = StreetPattern::Organic;
      s.block_min = 36.0f;
      s.block_max = 130.0f;
      s.lot_layout = LotLayout::Perimeter;
      s.lot_width_min = 9.0f;
      s.lot_width_max = 16.0f;
      s.lot_depth_min = 13.0f;
      s.lot_depth_max = 18.0f;
      s.setback_rear = 2.0f;
      s.coverage_max = 0.85f;
      s.floors_min = 3;
      s.floors_max = 5;
      s.height_max = 20.0f;
      s.park_share = 0.05f;
      s.square_chance = 0.12f;
      s.lane_chance = 0.3f;
      s.merge_chance = 0.15f;
      s.archetypes = {weight(Archetype::MidRiseOverShops, 6.0f),
                      weight(Archetype::TerracedHouse, 2.0f)};
      s.unit_mix = {0.30f, 0.40f, 0.20f, 0.10f};
      break;
    case DistrictKind::Industrial:
      s.pattern = StreetPattern::Wide;
      s.block_min = 120.0f;
      s.block_max = 260.0f;
      s.lot_layout = LotLayout::Whole;
      s.lot_width_min = 40.0f;
      s.lot_width_max = 80.0f;
      s.lot_depth_min = 40.0f;
      s.lot_depth_max = 240.0f;
      s.setback_front = 6.0f;
      s.setback_side = 4.0f;
      s.setback_rear = 6.0f;
      s.coverage_max = 0.6f;
      s.floors_min = 1;
      s.floors_max = 3;
      s.height_max = 16.0f;
      s.park_share = 0.03f;
      s.archetypes = {weight(Archetype::Warehouse, 8.0f), weight(Archetype::Office, 1.0f)};
      s.unit_mix = {0.0f, 0.0f, 0.0f, 0.0f};
      break;
    case DistrictKind::Waterfront:
      s.pattern = StreetPattern::CoarseGrid;
      s.block_min = 70.0f;
      s.block_max = 160.0f;
      s.lot_layout = LotLayout::Perimeter;
      s.lot_width_min = 14.0f;
      s.lot_width_max = 26.0f;
      s.lot_depth_min = 16.0f;
      s.lot_depth_max = 22.0f;
      s.setback_front = 2.0f;
      s.setback_rear = 2.0f;
      s.coverage_max = 0.75f;
      s.floors_min = 4;
      s.floors_max = 10;
      s.height_max = 40.0f;
      s.park_share = 0.12f;
      s.archetypes = {weight(Archetype::MidRiseOverShops, 6.0f), weight(Archetype::Office, 1.0f)};
      s.unit_mix = {0.15f, 0.35f, 0.35f, 0.15f};
      break;
    case DistrictKind::Campus:
      s.pattern = StreetPattern::Wide;
      s.block_min = 120.0f;
      s.block_max = 260.0f;
      s.lot_layout = LotLayout::Whole;
      s.lot_width_min = 50.0f;
      s.lot_width_max = 120.0f;
      s.lot_depth_min = 50.0f;
      s.lot_depth_max = 240.0f;
      s.setback_front = 10.0f;
      s.setback_side = 8.0f;
      s.setback_rear = 10.0f;
      s.coverage_max = 0.45f;
      s.floors_min = 3;
      s.floors_max = 6;
      s.height_max = 30.0f;
      s.park_share = 0.12f;
      s.archetypes = {weight(Archetype::CivicShell, 1.0f)};
      s.unit_mix = {0.0f, 0.0f, 0.0f, 0.0f};
      break;
  }
  return s;
}

BuildingRules default_building_rules() {
  BuildingRules r;
  auto minimum = [&](SpaceKind kind, f32 w, f32 d) {
    RoomMinimum m;
    m.kind = kind;
    m.width = w;
    m.depth = d;
    r.minimums.push_back(m);
  };
  minimum(SpaceKind::LivingKitchen, 3.2f, 4.0f);
  minimum(SpaceKind::Bedroom, 2.6f, 2.8f);
  minimum(SpaceKind::Bathroom, 1.6f, 2.0f);
  minimum(SpaceKind::Wc, 0.9f, 1.4f);
  minimum(SpaceKind::Entry, 1.2f, 1.2f);
  minimum(SpaceKind::Kitchen, 2.2f, 2.8f);
  minimum(SpaceKind::ShopFloor, 3.0f, 4.0f);
  minimum(SpaceKind::BackRoom, 1.4f, 1.8f);
  minimum(SpaceKind::OpenOffice, 3.6f, 4.0f);
  minimum(SpaceKind::Meeting, 2.6f, 3.0f);
  minimum(SpaceKind::Reception, 2.0f, 2.4f);
  minimum(SpaceKind::Hall, 10.0f, 10.0f);
  minimum(SpaceKind::Department, 4.0f, 4.0f);
  minimum(SpaceKind::Concourse, 8.0f, 8.0f);
  minimum(SpaceKind::Lobby, 2.4f, 3.0f);
  minimum(SpaceKind::Landing, 0.9f, 0.9f);
  minimum(SpaceKind::Stair, 1.0f, 2.6f);
  minimum(SpaceKind::Elevator, 1.6f, 1.6f);
  return r;
}

IslandParams default_island_params() {
  IslandParams p;
  p.format = k_params_format;
  p.name = "island";
  auto share = [&](DistrictKind kind, f32 s) {
    DistrictShare d;
    d.kind = kind;
    d.share = s;
    p.district_mix.push_back(d);
  };
  share(DistrictKind::Downtown, 0.08f);
  share(DistrictKind::Industrial, 0.12f);
  share(DistrictKind::OldTown, 0.07f);
  share(DistrictKind::Waterfront, 0.09f);
  share(DistrictKind::Campus, 0.06f);
  share(DistrictKind::MixedUse, 0.24f);
  share(DistrictKind::Residential, 0.34f);
  for (u32 k = 0; k < k_district_kinds; ++k)
    p.styles.push_back(default_style(static_cast<DistrictKind>(k)));
  auto width = [&](StreetClass cls, f32 w) {
    StreetWidth s;
    s.street_class = cls;
    s.width = w;
    p.street_widths.push_back(s);
  };
  width(StreetClass::Arterial, 32.0f);
  width(StreetClass::Collector, 22.0f);
  width(StreetClass::Local, 14.0f);
  width(StreetClass::Alley, 6.0f);
  width(StreetClass::Pedestrian, 8.0f);
  p.building = default_building_rules();
  auto hours = [&](WorkKind kind, u32 open, u32 close, u8 days) {
    OpeningHours h;
    h.kind = kind;
    h.open = open;
    h.close = close;
    h.days = days;
    p.hours.push_back(h);
  };
  hours(WorkKind::Retail, 9 * 60, 19 * 60, 0x3f);
  hours(WorkKind::Office, 8 * 60, 18 * 60, 0x1f);
  hours(WorkKind::Industrial, 6 * 60, 22 * 60, 0x1f);
  hours(WorkKind::Medical, 0, 24 * 60, 0x7f);
  hours(WorkKind::Education, 8 * 60, 17 * 60, 0x1f);
  hours(WorkKind::Civic, 8 * 60 + 30, 16 * 60 + 30, 0x1f);
  hours(WorkKind::Transport, 5 * 60, 60, 0x7f);
  return p;
}

namespace {

bool convert_style(const DistrictStyle& s, Style& out, std::string& error) {
  const std::string where = std::string("styles[") + district_kind_name(s.kind) + "]";
  if (!positive(s.block_min) || s.block_max < s.block_min) {
    error = where + ": block_min must be positive and block_max at least block_min";
    return false;
  }
  if (!positive(s.lot_width_min) || s.lot_width_max < s.lot_width_min ||
      !positive(s.lot_depth_min) || s.lot_depth_max < s.lot_depth_min) {
    error = where + ": lot widths and depths must be positive ranges";
    return false;
  }
  if (s.setback_front < 0.0f || s.setback_side < 0.0f || s.setback_rear < 0.0f) {
    error = where + ": setbacks must not be negative";
    return false;
  }
  if (!fraction(s.coverage_max) || s.coverage_max < 0.05f || !fraction(s.park_share) ||
      !fraction(s.square_chance) || !fraction(s.lane_chance) || !fraction(s.merge_chance)) {
    error = where + ": coverage_max, park_share and the chances are fractions (coverage >= 0.05)";
    return false;
  }
  if (s.floors_min < 1 || s.floors_max < s.floors_min || s.floors_max > 200 ||
      !positive(s.height_max)) {
    error = where + ": floors are 1 to 200, floors_max at least floors_min, height_max positive";
    return false;
  }
  out.kind = s.kind;
  out.pattern = s.pattern;
  out.lot_layout = s.lot_layout;
  out.block_min_cm = to_cm(s.block_min);
  out.block_max_cm = to_cm(s.block_max);
  out.lot_width_min_cm = to_cm(s.lot_width_min);
  out.lot_width_max_cm = to_cm(s.lot_width_max);
  out.lot_depth_min_cm = to_cm(s.lot_depth_min);
  out.lot_depth_max_cm = to_cm(s.lot_depth_max);
  out.setback_front_cm = to_cm(s.setback_front);
  out.setback_side_cm = to_cm(s.setback_side);
  out.setback_rear_cm = to_cm(s.setback_rear);
  out.coverage_q = to_q16(s.coverage_max);
  out.floors_min = s.floors_min;
  out.floors_max = s.floors_max;
  out.height_max_cm = to_cm(s.height_max);
  out.park_share_q = to_q16(s.park_share);
  out.square_q = to_q16(s.square_chance);
  out.lane_q = to_q16(s.lane_chance);
  out.merge_q = to_q16(s.merge_chance);
  f64 total = 0.0;
  for (const ArchetypeWeight& w : s.archetypes) {
    if (!(w.weight >= 0.0f) || static_cast<u32>(w.archetype) >= k_archetypes) {
      error = where + ": an archetype weight is negative or names no archetype";
      return false;
    }
    total += static_cast<f64>(w.weight);
  }
  if (!(total > 0.0)) {
    error = where + ": archetypes must give some archetype a positive weight";
    return false;
  }
  for (u32 a = 0; a < k_archetypes; ++a)
    out.archetype_q[a] = 0;
  for (const ArchetypeWeight& w : s.archetypes)
    out.archetype_q[static_cast<u32>(w.archetype)] +=
        static_cast<i32>(std::llround(static_cast<f64>(w.weight) / total * 65536.0));
  f64 mix = 0.0;
  for (const f32 m : s.unit_mix) {
    if (!(m >= 0.0f)) {
      error = where + ": unit_mix must not be negative";
      return false;
    }
    mix += static_cast<f64>(m);
  }
  for (u32 i = 0; i < 4; ++i) {
    out.unit_mix_q[i] =
        mix > 0.0 ? static_cast<i32>(std::llround(static_cast<f64>(s.unit_mix[i]) / mix * 65536.0))
                  : (i == 1 ? k_q16 : 0);
  }
  return true;
}

bool convert_rules(const BuildingRules& r, Rules& out, std::string& error) {
  const f32 lengths[] = {r.module,          r.floor_height,   r.ground_floor_height,
                         r.exterior_wall,   r.core_wall,      r.party_wall,
                         r.partition,       r.corridor_width, r.civic_corridor_width,
                         r.service_band,    r.unit_depth_min, r.unit_depth_max,
                         r.max_travel,      r.stair_width,    r.elevator_width,
                         r.shaft_width,     r.max_span,       r.max_span_house,
                         r.max_span_hall,   r.door_width,     r.entrance_width,
                         r.loading_width,   r.window_width,   r.window_sill,
                         r.window_head,     r.bed_width,      r.bed_length,
                         r.bed_clearance,   r.office_area,    r.retail_area,
                         r.industrial_area, r.civic_area};
  for (const f32 v : lengths) {
    if (!positive(v)) {
      error = "building: every length and area must be positive";
      return false;
    }
  }
  if (r.unit_depth_max < r.unit_depth_min || r.window_head <= r.window_sill ||
      !fraction(r.stair_separation) || r.units_per_elevator == 0 ||
      r.workplaces_per_elevator == 0 || r.two_stair_floors < 2) {
    error =
        "building: unit_depth_max below unit_depth_min, a window head below its sill, a "
        "stair_separation that is not a fraction, a zero per-elevator count, or two_stair_floors "
        "below 2";
    return false;
  }
  out.module_cm = to_cm(r.module);
  if (out.module_cm < 50) {
    error = "building: module must be at least 0.5 m";
    return false;
  }
  out.floor_cm = to_cm(r.floor_height);
  out.ground_floor_cm = to_cm(r.ground_floor_height);
  out.exterior_cm = to_cm(r.exterior_wall);
  out.core_wall_cm = to_cm(r.core_wall);
  out.party_cm = to_cm(r.party_wall);
  out.partition_cm = to_cm(r.partition);
  out.corridor_cm = to_cm(r.corridor_width);
  out.civic_corridor_cm = to_cm(r.civic_corridor_width);
  out.service_band_cm = to_cm(r.service_band);
  out.unit_depth_min_cm = to_cm(r.unit_depth_min);
  out.unit_depth_max_cm = to_cm(r.unit_depth_max);
  out.two_stair_floors = r.two_stair_floors;
  out.stair_separation_q = to_q16(r.stair_separation);
  out.max_travel_cm = to_cm(r.max_travel);
  out.stair_cm = to_cm(r.stair_width);
  out.elevator_cm = to_cm(r.elevator_width);
  out.shaft_cm = to_cm(r.shaft_width);
  out.elevator_floors = r.elevator_floors;
  out.units_per_elevator = r.units_per_elevator;
  out.workplaces_per_elevator = r.workplaces_per_elevator;
  out.max_span_cm = to_cm(r.max_span);
  out.max_span_house_cm = to_cm(r.max_span_house);
  out.max_span_hall_cm = to_cm(r.max_span_hall);
  out.door_cm = to_cm(r.door_width);
  out.entrance_cm = to_cm(r.entrance_width);
  out.loading_cm = to_cm(r.loading_width);
  out.window_cm = to_cm(r.window_width);
  out.window_sill_cm = to_cm(r.window_sill);
  out.window_head_cm = to_cm(r.window_head);
  out.bed_width_cm = to_cm(r.bed_width);
  out.bed_length_cm = to_cm(r.bed_length);
  out.bed_clearance_cm = to_cm(r.bed_clearance);
  auto area = [](f32 m2) { return static_cast<i64>(std::llround(static_cast<f64>(m2) * 1.0e4)); };
  out.office_area_cm2 = area(r.office_area);
  out.retail_area_cm2 = area(r.retail_area);
  out.industrial_area_cm2 = area(r.industrial_area);
  out.civic_area_cm2 = area(r.civic_area);
  for (u32 k = 0; k < k_space_kinds; ++k) {
    out.min_width_cm[k] = 0;
    out.min_depth_cm[k] = 0;
  }
  for (const RoomMinimum& m : r.minimums) {
    const u32 k = static_cast<u32>(m.kind);
    if (k >= k_space_kinds || !positive(m.width) || m.depth < m.width) {
      error = "building.minimums: a minimum names no space kind, or its depth is below its width";
      return false;
    }
    out.min_width_cm[k] = to_cm(m.width);
    out.min_depth_cm[k] = to_cm(m.depth);
  }
  return true;
}

}  // namespace

bool params_from_schema(const IslandParams& source, Params& out, std::string& error) {
  if (!source.format.empty() && source.format != k_params_format) {
    error = "format is '" + source.format + "', not " + k_params_format;
    return false;
  }
  out = Params{};
  out.source = source;
  if (out.source.format.empty()) out.source.format = k_params_format;
  // Whatever the file leaves empty comes from the defaults, and the source kept for the key and
  // the plan file is the filled one: two files that differ only by spelling a default out are the
  // same island and the same key.
  const IslandParams defaults = default_island_params();
  IslandParams& p = out.source;
  if (p.district_mix.empty()) p.district_mix = defaults.district_mix;
  for (const StreetWidth& w : defaults.street_widths) {
    bool found = false;
    for (const StreetWidth& mine : p.street_widths)
      found = found || mine.street_class == w.street_class;
    if (!found) p.street_widths.push_back(w);
  }
  // Tables keyed by a kind merge with the defaults kind by kind: a file that sets the bedroom's
  // minimum keeps every other room's.
  for (const OpeningHours& h : defaults.hours) {
    bool found = false;
    for (const OpeningHours& mine : p.hours)
      found = found || mine.kind == h.kind;
    if (!found) p.hours.push_back(h);
  }
  for (const RoomMinimum& m : defaults.building.minimums) {
    bool found = false;
    for (const RoomMinimum& mine : p.building.minimums)
      found = found || mine.kind == m.kind;
    if (!found) p.building.minimums.push_back(m);
  }
  for (u32 k = 0; k < k_district_kinds; ++k) {
    bool found = false;
    for (const DistrictStyle& s : p.styles)
      found = found || static_cast<u32>(s.kind) == k;
    if (!found) p.styles.push_back(default_style(static_cast<DistrictKind>(k)));
  }
  // And each table in the order of its kind, so the canonical JSON the key hashes is the island's
  // and not the order a file happened to list it in. (Overrides keep their order: a later pin of
  // the same id wins.)
  auto by_kind = [](auto& table, auto key) {
    std::stable_sort(table.begin(), table.end(),
                     [&](const auto& a, const auto& b) { return key(a) < key(b); });
  };
  by_kind(p.district_mix, [](const DistrictShare& d) { return static_cast<u32>(d.kind); });
  by_kind(p.styles, [](const DistrictStyle& d) { return static_cast<u32>(d.kind); });
  by_kind(p.street_widths, [](const StreetWidth& w) { return static_cast<u32>(w.street_class); });
  by_kind(p.hours, [](const OpeningHours& h) { return static_cast<u32>(h.kind); });
  by_kind(p.building.minimums, [](const RoomMinimum& m) { return static_cast<u32>(m.kind); });

  if (!positive(p.radius) || p.radius < 200.0f || p.radius > 50000.0f) {
    error = "radius must be 200 m to 50 km";
    return false;
  }
  if (!fraction(p.coast_roughness) || p.coast_roughness > 0.4f || !fraction(p.mountain_share) ||
      p.mountain_share > 0.9f || !fraction(p.arterial_jitter) || p.arterial_jitter > 0.3f) {
    error =
        "coast_roughness (to 0.4), mountain_share (to 0.9) and arterial_jitter (to 0.3) are "
        "fractions";
    return false;
  }
  if (p.greenbelt < 0.0f || p.harbour_width < 0.0f || p.harbour_depth < 0.0f ||
      !positive(p.tile_size) || p.tile_size < 8.0f || !positive(p.arterial_spacing) ||
      p.arterial_spacing < 120.0f || !positive(p.district_size)) {
    error =
        "greenbelt and the harbour must not be negative; tile_size at least 8 m, "
        "arterial_spacing at least 120 m, district_size positive";
    return false;
  }
  if (p.coastline.size() != 0 && p.coastline.size() < 3) {
    error = "an authored coastline needs three points or more";
    return false;
  }
  out.name = p.name;
  out.seed = p.seed;
  out.radius_cm = to_cm(p.radius);
  out.roughness_q = to_q16(p.coast_roughness);
  for (const Vec2& v : p.coastline) {
    out.coastline_cm.push_back(to_cm(v.x));
    out.coastline_cm.push_back(to_cm(v.y));
  }
  out.mountain_step = to_step64(p.mountain_deg);
  out.mountain_share_q = to_q16(p.mountain_share);
  out.greenbelt_cm = to_cm(p.greenbelt);
  out.harbour_step = to_step64(p.harbour_deg);
  out.harbour_width_cm = to_cm(p.harbour_width);
  out.harbour_depth_cm = to_cm(p.harbour_depth);
  out.tile_cm = to_cm(p.tile_size);
  out.arterial_spacing_cm = to_cm(p.arterial_spacing);
  out.arterial_jitter_q = to_q16(p.arterial_jitter);
  out.district_size_cm = to_cm(p.district_size);

  f64 total = 0.0;
  for (const DistrictShare& d : p.district_mix) {
    if (static_cast<u32>(d.kind) >= k_district_kinds || !(d.share >= 0.0f)) {
      error = "district_mix: a share is negative or names no district kind";
      return false;
    }
    total += static_cast<f64>(d.share);
  }
  if (!(total > 0.0)) {
    error = "district_mix: every share is zero";
    return false;
  }
  for (const DistrictShare& d : p.district_mix)
    out.district_share_q[static_cast<u32>(d.kind)] +=
        static_cast<i32>(std::llround(static_cast<f64>(d.share) / total * 65536.0));

  bool seen[k_district_kinds] = {};
  for (const DistrictStyle& s : p.styles) {
    const u32 k = static_cast<u32>(s.kind);
    if (k >= k_district_kinds || seen[k]) {
      error = "styles: a kind appears twice or is not a district kind";
      return false;
    }
    seen[k] = true;
    if (!convert_style(s, out.styles[k], error)) return false;
  }
  bool widths[k_street_classes] = {};
  for (const StreetWidth& w : p.street_widths) {
    const u32 c = static_cast<u32>(w.street_class);
    if (c >= k_street_classes || !positive(w.width) || w.width > 80.0f) {
      error = "street_widths: a width is not 0 to 80 m or names no class";
      return false;
    }
    widths[c] = true;
    out.street_width_cm[c] = to_cm(w.width);
  }
  const IslandParams fallback = default_island_params();
  for (const StreetWidth& w : fallback.street_widths) {
    const u32 c = static_cast<u32>(w.street_class);
    if (!widths[c]) out.street_width_cm[c] = to_cm(w.width);
  }
  if (!positive(p.parks.walk_distance) || !fraction(p.parks.central_share)) {
    error = "parks: walk_distance must be positive and central_share a fraction";
    return false;
  }
  out.walk_distance_cm = to_cm(p.parks.walk_distance);
  out.central_share_q = to_q16(p.parks.central_share);
  out.civic_count[static_cast<u32>(CivicKind::Hospital)] = p.civic.hospitals;
  out.civic_count[static_cast<u32>(CivicKind::University)] = p.civic.universities;
  out.civic_count[static_cast<u32>(CivicKind::Station)] = p.civic.stations;
  out.civic_count[static_cast<u32>(CivicKind::Government)] = p.civic.government;
  for (u32 c = 1; c < 5; ++c) {
    if (out.civic_count[c] > 64) {
      error = "civic: at most 64 of a kind";
      return false;
    }
  }
  if (!convert_rules(p.building, out.rules, error)) return false;
  for (const OpeningHours& h : p.hours) {
    const u32 k = static_cast<u32>(h.kind);
    if (k >= k_work_kinds || h.open > 1440 || h.close > 1440 || h.days > 0x7f) {
      error = "hours: minutes are 0 to 1440, days a 7-bit mask";
      return false;
    }
    out.hours[k] = Hours{static_cast<u16>(h.open), static_cast<u16>(h.close), h.days, true};
  }
  for (const Override& o : p.overrides) {
    Pin pin;
    pin.what = o.what;
    pin.id = o.id;
    pin.district_kind = o.district_kind;
    pin.street_class = o.street_class;
    pin.park = o.park;
    pin.civic = o.civic;
    if (static_cast<u32>(o.what) > 3 || static_cast<u32>(o.district_kind) >= k_district_kinds ||
        static_cast<u32>(o.street_class) >= k_street_classes ||
        static_cast<u32>(o.park) > static_cast<u32>(ParkKind::Greenbelt) ||
        static_cast<u32>(o.civic) > static_cast<u32>(CivicKind::Government)) {
      error = "overrides: an override names no kind";
      return false;
    }
    out.pins.push_back(pin);
  }
  return true;
}

bool read_params_file(const std::string& path, Params& out, std::string& error) {
  std::string text;
  const io::Status status = io::read_file(path, text);
  if (status != io::Status::Ok) {
    error = "cannot read " + path + ": " + io::status_name(status);
    return false;
  }
  JsonValue root;
  const JsonParseResult parsed = parse_json(text, root);
  if (!parsed.ok) {
    error = path + ":" + std::to_string(parsed.line) + ": " + parsed.message;
    return false;
  }
  IslandParams file;
  schema::ReadContext ctx;
  if (!schema::from_json(file, root, ctx) || !ctx.ok()) {
    std::string text_errors;
    for (const schema::Diagnostic& d : ctx.diagnostics) {
      if (!text_errors.empty()) text_errors += "; ";
      text_errors += d.path.empty() ? d.message : d.path + ": " + d.message;
    }
    error =
        path + ": " + (text_errors.empty() ? std::string("not island parameters") : text_errors);
    return false;
  }
  if (!params_from_schema(file, out, error)) {
    error = path + ": " + error;
    return false;
  }
  return true;
}

bool write_params_file(const std::string& path, const IslandParams& params, std::string* error) {
  const std::string text = write_json(schema::to_json(params)) + "\n";
  const std::string_view dir = io::parent_path(path);
  if (!dir.empty() && io::make_directories(dir) != io::Status::Ok) {
    if (error != nullptr) *error = "cannot create " + std::string(dir);
    return false;
  }
  const io::Status status = io::write_file(path, text);
  if (status != io::Status::Ok) {
    if (error != nullptr) *error = "cannot write " + path + ": " + io::status_name(status);
    return false;
  }
  return true;
}

}  // namespace engine::city
