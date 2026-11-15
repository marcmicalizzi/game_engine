// The rural filler's choice (rural_filler.h). Integers and the engine's hash; one draw a
// statement, because a call's arguments are evaluated in the compiler's order.

#include <core/hash/hash.h>
#include <domain/dwellings/describe.h>
#include <domain/dwellings/rural_filler.h>

namespace engine::dwellings {

namespace {

constexpr u64 k_draw_width = 101;
constexpr u64 k_draw_depth = 102;
constexpr u64 k_draw_mirror = 103;
constexpr u64 k_draw_interior = 104;

u64 draw(u64 seed, u64 purpose) noexcept {
  return mix64(hash_combine(hash_combine(k_hash_seed, seed), purpose));
}

i32 up10(i32 v) noexcept { return (v + 9) / 10 * 10; }
i32 down10(i32 v) noexcept { return v / 10 * 10; }

// A length on the 10 cm module in [lo, hi].
i32 draw_module(u64 seed, u64 purpose, i32 lo, i32 hi) noexcept {
  if (hi <= lo) return lo;
  const u64 steps = static_cast<u64>((hi - lo) / 10) + 1;
  return lo + 10 * static_cast<i32>(draw(seed, purpose) % steps);
}

// How far past its least size a type's houses run (docs/experiments/vernacular-interiors.md,
// "Room sizes"): the measured ranges, less the least the interior needs.
struct Range {
  i32 width_cm = 0;
  i32 depth_cm = 0;
};

Range range_of(const InteriorParams& p) noexcept {
  switch (p.type) {
    case PlanType::Cot: return Range{120, 100};
    case PlanType::HallParlour: return Range{260, p.storeys == 2 ? 40 : 170};
    case PlanType::CrossPassage: return Range{210, p.storeys == 2 ? 40 : 100};
    default: return Range{40, 120};
  }
}

// The least clear rectangle, on the module. A cot's grows by its stack so one bay stays clear.
InteriorExtent least_of(const InteriorParams& p) noexcept {
  InteriorExtent e = interior_min_extent(p);
  if (p.type == PlanType::Cot) {
    e.min_width_cm += 50;
    e.min_depth_cm += 30;
  }
  e.min_width_cm = up10(e.min_width_cm);
  e.min_depth_cm = up10(e.min_depth_cm);
  return e;
}

void apply(const RuralStyle& style, InteriorParams& p) noexcept {
  p.storey_cm = style.storey_cm;
  p.upper_clear_cm = style.upper_clear_cm;
  p.floor_cm = style.floor_cm;
  p.inner_wall_cm = style.inner_wall_cm;
  p.door_cm = style.door_cm;
  p.doorway_cm = style.doorway_cm;
  p.head_cm = style.head_cm;
  p.window_cm = style.window_cm;
  p.sill_cm = style.sill_cm;
}

bool fits(const InteriorParams& p, i32 clear_width, i32 clear_depth) noexcept {
  const InteriorExtent least = least_of(p);
  return least.min_width_cm <= clear_width && least.min_depth_cm <= clear_depth;
}

}  // namespace

void household_asks(const Household& household, InteriorParams& out) noexcept {
  const u32 persons = household.persons < 1 ? 1u : household.persons;
  const u32 means = household.means > 2 ? 2u : household.means;
  out.inner_room = false;
  out.hearths = means == 2 ? u8{2} : u8{1};
  if (persons <= 2) {
    out.type = PlanType::Cot;
    out.storeys = 1;
    out.hearths = 1;
  } else if (persons <= 4) {
    out.type = PlanType::HallParlour;
    out.storeys = means == 2 ? u8{2} : u8{1};
  } else if (persons <= 6) {
    // Five or six sleep in two chambers over a hall and parlour, or, with no means for an upper
    // floor, along a cross-passage house's three rooms.
    out.type = means == 0 ? PlanType::CrossPassage : PlanType::HallParlour;
    out.storeys = means == 0 ? u8{1} : u8{2};
  } else {
    out.type = PlanType::CrossPassage;
    out.storeys = means == 0 ? u8{1} : u8{2};
    out.inner_room = means != 1;
  }
}

bool choose_rural(const RuralPlot& plot, const Household& household, const RuralStyle& style,
                  u64 seed, RuralChoice& out, std::string* error) {
  out = RuralChoice{};
  const i32 walls = 2 * i32{style.outer_wall_cm};
  // What the plot holds, and never more than a description reaches.
  const i32 reach = max_clear_cm(style.outer_wall_cm);
  i32 clear_width = down10(plot.frontage_cm - walls);
  if (clear_width > reach) clear_width = reach;
  i32 clear_depth = down10(plot.depth_cm - walls);
  if (clear_depth > reach) clear_depth = reach;

  InteriorParams asked;
  apply(style, asked);
  household_asks(household, asked);
  if (asked.storeys == 1) asked.storey_cm = style.single_storey_cm;

  // The household's house, then what a smaller plot still holds, largest first: the same rooms on
  // two storeys, the plan one room wide, the two rooms on one floor, the cot.
  InteriorParams candidates[5];
  u32 count = 0;
  candidates[count++] = asked;
  auto add = [&](PlanType type, u8 storeys) {
    InteriorParams c = asked;
    c.type = type;
    c.storeys = storeys;
    c.storey_cm = storeys == 1 ? style.single_storey_cm : style.storey_cm;
    c.inner_room = false;
    candidates[count++] = c;
  };
  const bool two_storey_pair = asked.type == PlanType::HallParlour && asked.storeys == 2;
  if (asked.type == PlanType::CrossPassage) add(PlanType::HallParlour, 2);
  if (asked.type != PlanType::Cot) add(PlanType::TwoUpTwoDown, 2);
  if (asked.type == PlanType::CrossPassage || two_storey_pair) add(PlanType::HallParlour, 1);
  if (asked.type != PlanType::Cot) add(PlanType::Cot, 1);

  u32 chosen = count;
  for (u32 i = 0; i < count; ++i) {
    if (fits(candidates[i], clear_width, clear_depth)) {
      chosen = i;
      break;
    }
  }
  if (chosen == count) {
    if (error != nullptr) *error = "rural filler: the plot holds no house, not even a cot";
    return false;
  }

  InteriorParams p = candidates[chosen];
  if (p.type == PlanType::Cot) p.hearths = 1;
  const InteriorExtent least = least_of(p);
  const Range range = range_of(p);
  i32 width_hi = least.min_width_cm + range.width_cm;
  if (width_hi > clear_width) width_hi = clear_width;
  i32 depth_hi = least.min_depth_cm + range.depth_cm;
  if (depth_hi > clear_depth) depth_hi = clear_depth;
  p.width_cm = draw_module(seed, k_draw_width, least.min_width_cm, width_hi);
  p.depth_cm = draw_module(seed, k_draw_depth, least.min_depth_cm, depth_hi);
  p.mirror = (draw(seed, k_draw_mirror) & 1) != 0;
  p.seed = draw(seed, k_draw_interior);

  out.interior = p;
  out.outer_wall_cm = style.outer_wall_cm;
  out.outer_width_cm = p.width_cm + walls;
  out.outer_depth_cm = p.depth_cm + walls;
  out.ridge_along_front = out.outer_width_cm >= out.outer_depth_cm;
  out.demoted = chosen != 0;
  return true;
}

bool plan_rural(const RuralPlot& plot, const Household& household, const RuralStyle& style,
                u64 seed, RuralChoice& choice, InteriorPlan& plan, std::string* error) {
  if (!choose_rural(plot, household, style, seed, choice, error)) {
    plan.clear();
    return false;
  }
  return plan_interior(choice.interior, plan, error);
}

}  // namespace engine::dwellings
