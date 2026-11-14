// The tier rule by what the eye can see (visibility.h; docs/subsystems/animation.md, "The LOD
// policy: what the eye can see"; roadmap A44): the bands are pixels of limb travel, the rule moves
// with the lens, a bias is a factor on it, and the budget holds the actors it cannot pay for a tier
// coarser — the nearest and the largest on screen keeping theirs — through the tier code's own
// hysteresis.
#include <domain/sim/tiers.h>
#include <systems/animation/animation.h>
#include <systems/animation/visibility.h>

#include <doctest/doctest.h>

#include <cmath>

using namespace engine;
using namespace engine::animation;

namespace {

// cot(fov / 2) * rows / 2 at the renderer's default 55 degrees (`renderer::View::proj_scale`).
f32 lens(u32 rows) noexcept {
  return static_cast<f32>(static_cast<f64>(rows) * 0.5 / std::tan(0.9599310886 * 0.5));
}

// The tier one actor lands in from the coarsest tier (the tight boundaries), alone in the set.
u8 tier_at(f32 distance_m, f32 travel_m, f32 px_per_rad, f32 bias, const sim::TierParams& params) {
  sim::ObserverSet observers;
  observers.add(WorldPos::origin(), 1.0f);
  const WorldPos at{0.0, 0.0, -static_cast<f64>(distance_m)};
  const f32 score = sim::TierAssignment::score(at, motion_importance(travel_m, px_per_rad, bias),
                                               observers, params);
  return sim::TierAssignment::tier_of(score, params);
}

}  // namespace

TEST_CASE("animation visibility: a band of D metres is 1000 / D pixels of limb travel") {
  const sim::TierParams params = tier_params();
  CHECK(boundary_px(params.boundaries[0]) == doctest::Approx(4.0f));
  CHECK(boundary_px(params.boundaries[1]) == doctest::Approx(1.0f));
  CHECK(boundary_px(params.boundaries[2]) == doctest::Approx(1.0f / 3.0f));
  // The reference actor through the reference lens scores its plain distance.
  CHECK(motion_importance(k_reference_travel_m, k_reference_px_per_rad, 1.0f) == 1.0f);

  // Walking a half-metre limb away at 2160 rows: each tier changes exactly where its travel on the
  // screen crosses the band's pixels, a part in a thousand either side.
  const f32 p = lens(2160);
  const f32 travel = 0.5f;
  const f32 thresholds[3] = {4.0f, 1.0f, 1.0f / 3.0f};
  for (u8 band = 0; band < 3; ++band) {
    const f32 at = travel * p / thresholds[band];  // the distance where it subtends the threshold
    CHECK(tier_at(at * 0.999f, travel, p, 1.0f, params) == band);
    CHECK(tier_at(at * 1.001f, travel, p, 1.0f, params) == band + 1);
  }
  // Which is what the page says the bands come to at the owner's target: 259 / 1,037 / 3,112 m.
  CHECK(travel * p / 4.0f == doctest::Approx(259.3f).epsilon(0.001));
  CHECK(travel * p / 1.0f == doctest::Approx(1037.3f).epsilon(0.001));
  CHECK(travel * p * 3.0f == doctest::Approx(3112.0f).epsilon(0.001));
}

TEST_CASE("animation visibility: the same actor at 100 m moves with the lens and with its bias") {
  // A small actor — limbs that travel 7.5 cm — 100 m away: 0.78 px of travel at 1080 rows, under
  // the skinned band's pixel, so it draws its rest pose (LOD2); at 2160 rows the same actor is
  // 1.56 px and skinned (LOD1); at 1080 rows with a bias of 4 it is 3.1 px and skinned again.
  const sim::TierParams params = tier_params();
  const f32 travel = 0.075f;
  CHECK(travel_px(travel, 100.0f, lens(1080)) == doctest::Approx(0.778f).epsilon(0.002));
  CHECK(tier_at(100.0f, travel, lens(1080), 1.0f, params) == 2);
  CHECK(tier_at(100.0f, travel, lens(2160), 1.0f, params) == 1);
  CHECK(tier_at(100.0f, travel, lens(1080), 4.0f, params) == 1);
  // A bias of 0 is the coarsest tier at any distance, and zero travel is too (the floor).
  CHECK(tier_at(1.0f, travel, lens(2160), 0.0f, params) == k_tier_count - 1);
  CHECK(tier_at(1.0f, 0.0f, lens(2160), 1.0f, params) == k_tier_count - 1);
  // The floor is low enough that a real small actor far away is scored by the rule, not by it: a
  // 10 cm wing at a kilometre through 1080 rows is 1e-4 of the reference.
  CHECK(motion_importance(0.1f, lens(1080), 1.0f) > params.min_importance);
}

TEST_CASE("animation visibility: the budget keeps the nearest and largest at the finest tier") {
  sim::TierParams params = tier_params();
  params.max_promotions = 1024;
  params.max_demotions = 1024;
  sim::ObserverSet observers;
  observers.add(WorldPos::origin(), 1.0f);
  // Twelve actors in a row, every one wanting LOD0: rows 0..11 at 10 m to 21 m, the same travel,
  // except row 7, whose travel is ten times the rest — largest on screen, so it ranks first.
  constexpr u32 k_rows = 12;
  WorldPos positions[k_rows];
  f32 base[k_rows];
  for (u32 i = 0; i < k_rows; ++i) {
    positions[i] = WorldPos{0.0, 0.0, -10.0 - static_cast<f64>(i)};
    base[i] = motion_importance(i == 7 ? 5.0f : 0.5f, lens(2160), 1.0f);
  }
  TierBudget budget;
  budget.full = 4;
  budget.skinned = 0;
  sim::TierAssignment tiers;
  u8 state[k_rows] = {};  // every one at LOD0, as `attach` puts them
  Vector<u32> order;
  order.reserve(k_rows);
  Vector<sim::TierChange> changes;
  f32 scores[k_rows];
  f32 importance[k_rows];
  TierBudgetStats stats;
  for (u32 step = 0; step < 6; ++step) {
    for (u32 i = 0; i < k_rows; ++i) {
      importance[i] = base[i];
      scores[i] = sim::TierAssignment::score(positions[i], importance[i], observers, params);
    }
    stats = cap_tiers(std::span<const f32>(scores, k_rows), std::span<const u8>(state, k_rows),
                      params, budget, std::span<f32>(importance, k_rows), order);
    sim::TierInput input;
    input.positions = {positions, k_rows};
    input.importance = {importance, k_rows};
    input.tiers = {state, k_rows};
    changes.clear();
    (void)tiers.assign_tiers(input, observers, params, changes);
  }
  CHECK(stats.wanted_full == k_rows);
  CHECK(stats.fell_back_full == k_rows - budget.full);
  CHECK(stats.fell_back_skinned == 0);
  // The largest on screen and the three nearest keep LOD0; the rest are held at LOD1, not lower.
  for (u32 i = 0; i < k_rows; ++i) {
    const bool kept = i == 7 || i < 3;
    CHECK_MESSAGE(state[i] == (kept ? 0 : 1), "row " << i);
  }
  // Steady: a sixth step changed nothing (the held rows stay held, the kept stay kept).
  CHECK(changes.empty());

  // The skinned ceiling holds the farthest of what is left at LOD2, where the rest pose is drawn.
  budget.skinned = 6;
  for (u32 step = 0; step < 6; ++step) {
    for (u32 i = 0; i < k_rows; ++i) {
      importance[i] = base[i];
      scores[i] = sim::TierAssignment::score(positions[i], importance[i], observers, params);
    }
    stats = cap_tiers(std::span<const f32>(scores, k_rows), std::span<const u8>(state, k_rows),
                      params, budget, std::span<f32>(importance, k_rows), order);
    sim::TierInput input;
    input.positions = {positions, k_rows};
    input.importance = {importance, k_rows};
    input.tiers = {state, k_rows};
    changes.clear();
    (void)tiers.assign_tiers(input, observers, params, changes);
  }
  CHECK(stats.fell_back_skinned == k_rows - budget.skinned);
  u32 at[k_tier_count] = {};
  for (const u8 t : state)
    ++at[t];
  CHECK(at[0] == budget.full);
  CHECK(at[1] == budget.skinned - budget.full);
  CHECK(at[2] == k_rows - budget.skinned);
  CHECK(state[11] == 2);  // the farthest
  CHECK(state[7] == 0);   // the largest

  // No ceiling: nothing is held, and the importance is left exactly as it was.
  for (u32 i = 0; i < k_rows; ++i) {
    importance[i] = base[i];
    scores[i] = sim::TierAssignment::score(positions[i], importance[i], observers, params);
  }
  stats = cap_tiers(std::span<const f32>(scores, k_rows), std::span<const u8>(state, k_rows),
                    params, TierBudget{}, std::span<f32>(importance, k_rows), order);
  CHECK(stats.fell_back_full == 0);
  CHECK(stats.fell_back_skinned == 0);
  for (u32 i = 0; i < k_rows; ++i)
    CHECK(importance[i] == base[i]);
}

TEST_CASE("animation visibility: two actors at the budget's margin do not trade the tier") {
  // Two actors 0.5 % apart in score with room for one at LOD0: the one that has it keeps it while
  // the other walks nearer, until the newcomer is nearer by more than the hysteresis.
  sim::TierParams params = tier_params();
  sim::ObserverSet observers;
  observers.add(WorldPos::origin(), 1.0f);
  TierBudget budget;
  budget.full = 1;
  const f32 importance_base = motion_importance(0.5f, lens(2160), 1.0f);
  WorldPos positions[2] = {WorldPos{0.0, 0.0, -50.0}, WorldPos{0.0, 0.0, -50.25}};
  u8 state[2] = {0, 1};
  Vector<u32> order;
  order.reserve(2);
  Vector<sim::TierChange> changes;
  sim::TierAssignment tiers;
  u32 swaps = 0;
  for (u32 step = 0; step < 40; ++step) {
    // Row 1 walks from 0.5 % farther to about a fifth nearer than row 0 over the forty steps.
    positions[1].z = -50.25 + 0.3 * static_cast<f64>(step);
    f32 importance[2] = {importance_base, importance_base};
    f32 scores[2];
    for (u32 i = 0; i < 2; ++i)
      scores[i] = sim::TierAssignment::score(positions[i], importance[i], observers, params);
    (void)cap_tiers(std::span<const f32>(scores, 2), std::span<const u8>(state, 2), params, budget,
                    std::span<f32>(importance, 2), order);
    sim::TierInput input;
    input.positions = {positions, 2};
    input.importance = {importance, 2};
    input.tiers = {state, 2};
    const u8 before = state[0];
    changes.clear();
    (void)tiers.assign_tiers(input, observers, params, changes);
    swaps += state[0] != before ? 1u : 0u;
    CHECK(static_cast<u32>(state[0] == 0) + static_cast<u32>(state[1] == 0) <= 1);
  }
  // Exactly one hand-over in forty steps, after row 1 came nearer by more than the hysteresis.
  CHECK(swaps == 1);
  CHECK(state[1] == 0);
}
