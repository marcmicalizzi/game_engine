// LOD tier assignment (docs/plan/05-simulation.md §5.4): the observer function, hysteresis, the
// per-tick rate limits, deterministic change order, and independence from the worker count.

#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <domain/sim/tiers.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

using namespace engine;
using namespace engine::sim;

namespace {

TierParams four_tiers() {
  TierParams params;
  params.tier_count = 4;
  params.boundaries[0] = 32.0f;
  params.boundaries[1] = 128.0f;
  params.boundaries[2] = 1024.0f;
  params.hysteresis = 0.25f;
  params.max_promotions = 1'000'000;
  params.max_demotions = 1'000'000;
  return params;
}

struct Population {
  Vector<WorldPos> positions;
  Vector<f32> importance;
  Vector<u8> tiers;

  void add(WorldPos position, f32 weight, u8 tier) {
    positions.push_back(position);
    importance.push_back(weight);
    tiers.push_back(tier);
  }

  TierInput input() {
    TierInput in;
    in.positions = std::span<const WorldPos>(positions.data(), positions.size());
    in.importance = std::span<const f32>(importance.data(), importance.size());
    in.tiers = std::span<u8>(tiers.data(), tiers.size());
    return in;
  }
};

// A deterministic spread, so every run of these tests sees the same world.
Population spread(u32 count) {
  Population population;
  u64 state = 0x243F6A8885A308D3ull;
  for (u32 i = 0; i < count; ++i) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    const f64 x = static_cast<f64>((state >> 33) % 4096u);
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    const f64 z = static_cast<f64>((state >> 33) % 4096u);
    const f32 weight = 1.0f + static_cast<f32>(i % 4u);
    population.add(WorldPos{x, 0.0, z}, weight, 3);
  }
  return population;
}

}  // namespace

TEST_CASE("tiers: the score is the minimum over observers of distance over weight and importance") {
  ObserverSet observers;
  observers.add(WorldPos{0.0, 0.0, 0.0}, 1.0f);
  observers.add(WorldPos{1000.0, 0.0, 0.0}, 1.0f);
  const TierParams params = four_tiers();

  // Equidistant from both observers: the minimum is either, and it is 500.
  CHECK(TierAssignment::score(WorldPos{500.0, 0.0, 0.0}, 1.0f, observers, params) ==
        doctest::Approx(500.0f));
  // Near the second observer: the first one does not get a say.
  CHECK(TierAssignment::score(WorldPos{990.0, 0.0, 0.0}, 1.0f, observers, params) ==
        doctest::Approx(10.0f));
  // Importance divides the distance, so an important entity is "nearer" to every observer.
  CHECK(TierAssignment::score(WorldPos{200.0, 0.0, 0.0}, 4.0f, observers, params) ==
        doctest::Approx(50.0f));
  // So does a heavier observer.
  ObserverSet heavy;
  heavy.add(WorldPos{0.0, 0.0, 0.0}, 2.0f);
  CHECK(TierAssignment::score(WorldPos{200.0, 0.0, 0.0}, 1.0f, heavy, params) ==
        doctest::Approx(100.0f));
  // With nobody watching, everything falls to the last tier.
  ObserverSet nobody;
  CHECK(TierAssignment::tier_of(TierAssignment::score(WorldPos{}, 1.0f, nobody, params), params) ==
        3);
}

TEST_CASE("tiers: hysteresis stops a crowd on a boundary from thrashing") {
  ObserverSet observers;
  observers.add(WorldPos{0.0, 0.0, 0.0}, 1.0f);
  TierParams params = four_tiers();  // boundary 0 at 32, widened to 40 by 25% hysteresis

  Population population;
  population.add(WorldPos{31.0, 0.0, 0.0}, 1.0f, 3);
  TierAssignment assignment;
  Vector<TierChange> changes;

  assignment.assign_tiers(population.input(), observers, params, changes);
  REQUIRE(changes.size() == 1);
  CHECK(changes[0].from == 3);
  CHECK(changes[0].to == 0);

  // Drifting out to 35 is past the boundary but inside the band: no change at all.
  changes.clear();
  population.positions[0] = WorldPos{35.0, 0.0, 0.0};
  assignment.assign_tiers(population.input(), observers, params, changes);
  CHECK(changes.empty());
  CHECK(population.tiers[0] == 0);

  // The same position with no band demotes at once, and a step back inside would promote at
  // once: that is the oscillation the band exists to stop, shown rather than asserted about.
  {
    TierParams sharp = params;
    sharp.hysteresis = 0.0f;
    Population copy;
    copy.add(WorldPos{35.0, 0.0, 0.0}, 1.0f, 0);
    TierAssignment sharp_assignment;
    Vector<TierChange> sharp_changes;
    sharp_assignment.assign_tiers(copy.input(), observers, sharp, sharp_changes);
    REQUIRE(sharp_changes.size() == 1);
    CHECK(sharp_changes[0].to == 1);
    sharp_changes.clear();
    copy.positions[0] = WorldPos{31.0, 0.0, 0.0};
    sharp_assignment.assign_tiers(copy.input(), observers, sharp, sharp_changes);
    REQUIRE(sharp_changes.size() == 1);
    CHECK(sharp_changes[0].to == 0);
  }

  // 45 is outside the widened band, so it demotes.
  changes.clear();
  population.positions[0] = WorldPos{45.0, 0.0, 0.0};
  assignment.assign_tiers(population.input(), observers, params, changes);
  REQUIRE(changes.size() == 1);
  CHECK(changes[0].from == 0);
  CHECK(changes[0].to == 1);

  // And coming back to 35 does not promote either: the band is symmetric about the boundary.
  changes.clear();
  population.positions[0] = WorldPos{35.0, 0.0, 0.0};
  assignment.assign_tiers(population.input(), observers, params, changes);
  CHECK(changes.empty());
  CHECK(population.tiers[0] == 1);
}

TEST_CASE("tiers: the rate limits spread a crowd's promotion over ticks, nearest first") {
  ObserverSet observers;
  observers.add(WorldPos{0.0, 0.0, 0.0}, 1.0f);
  TierParams params = four_tiers();
  params.max_promotions = 4;

  Population population;
  for (u32 i = 0; i < 20; ++i) {
    population.add(WorldPos{static_cast<f64>(20 - i), 0.0, 0.0}, 1.0f, 3);
  }

  TierAssignment assignment;
  Vector<TierChange> changes;
  const TierStats stats = assignment.assign_tiers(population.input(), observers, params, changes);
  CHECK(stats.evaluated == 20);
  CHECK(stats.promoted == 4);
  CHECK(stats.deferred_promotions == 16);
  REQUIRE(changes.size() == 4);
  // Nearest first: entities 19, 18, 17, 16 are at x = 1, 2, 3, 4. Emitted in index order.
  CHECK(changes[0].index == 16);
  CHECK(changes[3].index == 19);
  for (u32 i = 0; i < changes.size(); ++i)
    CHECK(changes[i].to == 0);

  // The rest arrive on later ticks rather than being forgotten.
  changes.clear();
  const TierStats second = assignment.assign_tiers(population.input(), observers, params, changes);
  CHECK(second.promoted == 4);
  CHECK(second.deferred_promotions == 12);
}

TEST_CASE("tiers: changes come out in entity order, whatever order they were selected in") {
  ObserverSet observers;
  observers.add(WorldPos{0.0, 0.0, 0.0}, 1.0f);
  TierParams params = four_tiers();

  Population population;
  // Deliberately in decreasing distance, so selection order is the reverse of index order.
  for (u32 i = 0; i < 16; ++i) {
    population.add(WorldPos{static_cast<f64>(30 - i), 0.0, 0.0}, 1.0f, 3);
  }
  TierAssignment assignment;
  Vector<TierChange> changes;
  assignment.assign_tiers(population.input(), observers, params, changes);
  REQUIRE(changes.size() == 16);
  for (u32 i = 0; i < changes.size(); ++i)
    CHECK(changes[i].index == i);
}

TEST_CASE("tiers: eight workers produce the same bytes as one, and as none") {
  ObserverSet observers;
  observers.add(WorldPos{512.0, 0.0, 512.0}, 1.0f);
  observers.add(WorldPos{3000.0, 0.0, 900.0}, 2.0f);
  observers.add(WorldPos{100.0, 0.0, 3500.0}, 0.5f);
  TierParams params = four_tiers();
  params.max_promotions = 512;
  params.max_demotions = 512;

  auto run = [&](jobs::JobSystem* system, u32 grain, Vector<u8>& tiers_out,
                 Vector<TierChange>& changes_out) {
    Population population = spread(20'000);
    TierAssignment assignment(system);
    assignment.set_grain(grain);
    Vector<TierChange> changes;
    for (u32 tick = 0; tick < 6; ++tick) {
      changes.clear();
      assignment.assign_tiers(population.input(), observers, params, changes);
    }
    tiers_out = population.tiers;
    changes_out = changes;
  };

  Vector<u8> serial_tiers;
  Vector<TierChange> serial_changes;
  run(nullptr, 2048, serial_tiers, serial_changes);

  jobs::JobSystemConfig config;
  config.performance_workers = 1;
  config.efficiency_workers = 1;
  jobs::JobSystem one(config);
  Vector<u8> one_tiers;
  Vector<TierChange> one_changes;
  run(&one, 512, one_tiers, one_changes);

  config.performance_workers = 8;
  jobs::JobSystem eight(config);
  Vector<u8> eight_tiers;
  Vector<TierChange> eight_changes;
  run(&eight, 512, eight_tiers, eight_changes);

  REQUIRE(serial_tiers.size() == 20'000);
  CHECK(std::memcmp(serial_tiers.data(), one_tiers.data(), serial_tiers.size()) == 0);
  CHECK(std::memcmp(serial_tiers.data(), eight_tiers.data(), serial_tiers.size()) == 0);
  REQUIRE(serial_changes.size() == one_changes.size());
  REQUIRE(serial_changes.size() == eight_changes.size());
  if (!serial_changes.empty()) {
    const usize bytes = static_cast<usize>(serial_changes.size()) * sizeof(TierChange);
    CHECK(std::memcmp(serial_changes.data(), one_changes.data(), bytes) == 0);
    CHECK(std::memcmp(serial_changes.data(), eight_changes.data(), bytes) == 0);
  }
}

// ADR-0053's far sites: the owner's 419 km (cell 6,548), 10,000 km and 1e8 m. Whole metres, so the
// sites themselves are exact in f64 and any offset in 1024ths of a metre added to them is too.
constexpr WorldPos k_far_sites[] = {
    WorldPos{419072.0, 0.0, -419072.0},
    WorldPos{10000000.0, 0.0, 10000000.0},
    WorldPos{100000000.0, 0.0, -100000000.0},
    // An observer 9 m short of a 64 m cell's edge, so every entity below straddles it.
    WorldPos{419063.0, 7.0, 419063.0},
};

TEST_CASE(
    "tiers: an entity 30 m from its observer is the same tier, at the same distance, far out") {
  // Displacements from the observer, metres. The first is 30 m exactly (18, 24); the second is
  // 30 m in a direction no axis holds, in 1024ths, so the displacement is exact at every site and
  // only its length rounds; the third is not in 1024ths, so the sites' own f64 rounding is in it;
  // the fourth is 31.5 m, half a metre inside the 32 m boundary, which a float32 world position
  // rounds onto the boundary at 10,000 km (a float steps by a metre there) and so put in tier 1.
  const DVec3 offsets[] = {
      DVec3{18.0, 0.0, 24.0},
      DVec3{-17.3203125, 9.0009765625, 22.80078125},
      DVec3{12.345678, -3.1415926, 27.182818},
      DVec3{31.5, 0.0, 0.0},
  };
  const TierParams params = four_tiers();
  for (const DVec3& offset : offsets) {
    const f64 truth = length(offset);
    ObserverSet origin;
    origin.add(WorldPos::origin(), 1.0f);
    const f32 by_origin_score =
        TierAssignment::score(WorldPos::origin() + offset, 1.0f, origin, params);
    CHECK(std::abs(static_cast<f64>(by_origin_score) - truth) <= 1.0e-6);
    for (u32 s = 0; s < 4; ++s) {
      CAPTURE(s);
      CAPTURE(truth);
      ObserverSet at_site;
      at_site.add(k_far_sites[s], 1.0f);
      const f32 there = TierAssignment::score(k_far_sites[s] + offset, 1.0f, at_site, params);
      // Within a micrometre of the true distance, as by the origin: the displacement is f64 and
      // only its length is narrowed, which rounds by half a float step at 30 m (0.95 um).
      CHECK(std::abs(static_cast<f64>(there) - truth) <= 1.0e-6);
      CHECK(TierAssignment::tier_of(there, params) ==
            TierAssignment::tier_of(by_origin_score, params));
    }
  }

  // The whole pass, with hysteresis and the rate limits, gives the same tiers and the same change
  // list far out as by the origin: a population round one observer, moved with it.
  // In 1024ths of a metre, so each is the same displacement from the observer at every site and the
  // scores are the same bits: what is compared below is the pass, not f64's rounding of the sites.
  const auto in_1024ths = [](f64 v) { return std::round(v * 1024.0) / 1024.0; };
  Population by_origin;
  for (u32 i = 0; i < 64; ++i) {
    const f64 r = 4.0 + 20.0 * static_cast<f64>(i);  // 4 m to 1,264 m: every tier
    const f64 a = 0.1 * static_cast<f64>(i);
    by_origin.add(WorldPos{in_1024ths(r * std::cos(a)), 0.5, in_1024ths(r * std::sin(a))},
                  1.0f + static_cast<f32>(i % 3), 3);
  }
  ObserverSet origin;
  origin.add(WorldPos::origin(), 1.0f);
  TierParams limited = params;
  limited.max_promotions = 16;
  TierAssignment reference;
  Vector<TierChange> reference_changes;
  for (u32 tick = 0; tick < 3; ++tick)
    reference.assign_tiers(by_origin.input(), origin, limited, reference_changes);
  for (u32 s = 0; s < 4; ++s) {
    CAPTURE(s);
    Population moved;
    for (u32 i = 0; i < 64; ++i)
      moved.add(k_far_sites[s] + (by_origin.positions[i] - WorldPos::origin()),
                by_origin.importance[i], 3);
    ObserverSet at_site;
    at_site.add(k_far_sites[s], 1.0f);
    TierAssignment assignment;
    Vector<TierChange> changes;
    for (u32 tick = 0; tick < 3; ++tick)
      assignment.assign_tiers(moved.input(), at_site, limited, changes);
    CHECK(moved.tiers == by_origin.tiers);
    REQUIRE(changes.size() == reference_changes.size());
    CHECK(std::memcmp(changes.data(), reference_changes.data(),
                      static_cast<usize>(changes.size()) * sizeof(TierChange)) == 0);
  }
}

TEST_CASE("tiers: the tier count is configurable and out-of-range tiers are clamped") {
  ObserverSet observers;
  observers.add(WorldPos{0.0, 0.0, 0.0}, 1.0f);
  TierParams params;
  params.tier_count = 8;
  for (u32 i = 0; i < 7; ++i) {
    params.boundaries[i] = static_cast<f32>(10 * (i + 1));
  }
  params.hysteresis = 0.0f;
  CHECK(TierAssignment::tier_of(5.0f, params) == 0);
  CHECK(TierAssignment::tier_of(15.0f, params) == 1);
  CHECK(TierAssignment::tier_of(65.0f, params) == 6);
  CHECK(TierAssignment::tier_of(1000.0f, params) == 7);

  params.tier_count = 2;
  params.boundaries[0] = 10.0f;
  CHECK(TierAssignment::tier_of(5.0f, params) == 0);
  CHECK(TierAssignment::tier_of(15.0f, params) == 1);

  // An entity carrying a tier the params no longer have is clamped rather than trusted.
  Population population;
  population.add(WorldPos{100.0, 0.0, 0.0}, 1.0f, 7);
  TierAssignment assignment;
  Vector<TierChange> changes;
  assignment.assign_tiers(population.input(), observers, params, changes);
  CHECK(population.tiers[0] == 1);
  CHECK(changes.empty());
}
