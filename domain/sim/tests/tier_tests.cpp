// LOD tier assignment (docs/plan/05-simulation.md §5.4): the observer function, hysteresis, the
// per-tick rate limits, deterministic change order, and independence from the worker count.

#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <domain/sim/tiers.h>

#include <doctest/doctest.h>

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
  Vector<Vec3> positions;
  Vector<f32> importance;
  Vector<u8> tiers;

  void add(Vec3 position, f32 weight, u8 tier) {
    positions.push_back(position);
    importance.push_back(weight);
    tiers.push_back(tier);
  }

  TierInput input() {
    TierInput in;
    in.positions = std::span<const Vec3>(positions.data(), positions.size());
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
    const f32 x = static_cast<f32>((state >> 33) % 4096u);
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    const f32 z = static_cast<f32>((state >> 33) % 4096u);
    const f32 weight = 1.0f + static_cast<f32>(i % 4u);
    population.add(Vec3{x, 0.0f, z}, weight, 3);
  }
  return population;
}

}  // namespace

TEST_CASE("tiers: the score is the minimum over observers of distance over weight and importance") {
  ObserverSet observers;
  observers.add(Vec3{0.0f, 0.0f, 0.0f}, 1.0f);
  observers.add(Vec3{1000.0f, 0.0f, 0.0f}, 1.0f);
  const TierParams params = four_tiers();

  // Equidistant from both observers: the minimum is either, and it is 500.
  CHECK(TierAssignment::score(Vec3{500.0f, 0.0f, 0.0f}, 1.0f, observers, params) ==
        doctest::Approx(500.0f));
  // Near the second observer: the first one does not get a say.
  CHECK(TierAssignment::score(Vec3{990.0f, 0.0f, 0.0f}, 1.0f, observers, params) ==
        doctest::Approx(10.0f));
  // Importance divides the distance, so an important entity is "nearer" to every observer.
  CHECK(TierAssignment::score(Vec3{200.0f, 0.0f, 0.0f}, 4.0f, observers, params) ==
        doctest::Approx(50.0f));
  // So does a heavier observer.
  ObserverSet heavy;
  heavy.add(Vec3{0.0f, 0.0f, 0.0f}, 2.0f);
  CHECK(TierAssignment::score(Vec3{200.0f, 0.0f, 0.0f}, 1.0f, heavy, params) ==
        doctest::Approx(100.0f));
  // With nobody watching, everything falls to the last tier.
  ObserverSet nobody;
  CHECK(TierAssignment::tier_of(TierAssignment::score(Vec3{}, 1.0f, nobody, params), params) == 3);
}

TEST_CASE("tiers: hysteresis stops a crowd on a boundary from thrashing") {
  ObserverSet observers;
  observers.add(Vec3{0.0f, 0.0f, 0.0f}, 1.0f);
  TierParams params = four_tiers();  // boundary 0 at 32, widened to 40 by 25% hysteresis

  Population population;
  population.add(Vec3{31.0f, 0.0f, 0.0f}, 1.0f, 3);
  TierAssignment assignment;
  Vector<TierChange> changes;

  assignment.assign_tiers(population.input(), observers, params, changes);
  REQUIRE(changes.size() == 1);
  CHECK(changes[0].from == 3);
  CHECK(changes[0].to == 0);

  // Drifting out to 35 is past the boundary but inside the band: no change at all.
  changes.clear();
  population.positions[0] = Vec3{35.0f, 0.0f, 0.0f};
  assignment.assign_tiers(population.input(), observers, params, changes);
  CHECK(changes.empty());
  CHECK(population.tiers[0] == 0);

  // The same position with no band demotes at once, and a step back inside would promote at
  // once: that is the oscillation the band exists to stop, shown rather than asserted about.
  {
    TierParams sharp = params;
    sharp.hysteresis = 0.0f;
    Population copy;
    copy.add(Vec3{35.0f, 0.0f, 0.0f}, 1.0f, 0);
    TierAssignment sharp_assignment;
    Vector<TierChange> sharp_changes;
    sharp_assignment.assign_tiers(copy.input(), observers, sharp, sharp_changes);
    REQUIRE(sharp_changes.size() == 1);
    CHECK(sharp_changes[0].to == 1);
    sharp_changes.clear();
    copy.positions[0] = Vec3{31.0f, 0.0f, 0.0f};
    sharp_assignment.assign_tiers(copy.input(), observers, sharp, sharp_changes);
    REQUIRE(sharp_changes.size() == 1);
    CHECK(sharp_changes[0].to == 0);
  }

  // 45 is outside the widened band, so it demotes.
  changes.clear();
  population.positions[0] = Vec3{45.0f, 0.0f, 0.0f};
  assignment.assign_tiers(population.input(), observers, params, changes);
  REQUIRE(changes.size() == 1);
  CHECK(changes[0].from == 0);
  CHECK(changes[0].to == 1);

  // And coming back to 35 does not promote either: the band is symmetric about the boundary.
  changes.clear();
  population.positions[0] = Vec3{35.0f, 0.0f, 0.0f};
  assignment.assign_tiers(population.input(), observers, params, changes);
  CHECK(changes.empty());
  CHECK(population.tiers[0] == 1);
}

TEST_CASE("tiers: the rate limits spread a crowd's promotion over ticks, nearest first") {
  ObserverSet observers;
  observers.add(Vec3{0.0f, 0.0f, 0.0f}, 1.0f);
  TierParams params = four_tiers();
  params.max_promotions = 4;

  Population population;
  for (u32 i = 0; i < 20; ++i) {
    population.add(Vec3{static_cast<f32>(20 - i), 0.0f, 0.0f}, 1.0f, 3);
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
  observers.add(Vec3{0.0f, 0.0f, 0.0f}, 1.0f);
  TierParams params = four_tiers();

  Population population;
  // Deliberately in decreasing distance, so selection order is the reverse of index order.
  for (u32 i = 0; i < 16; ++i) {
    population.add(Vec3{static_cast<f32>(30 - i), 0.0f, 0.0f}, 1.0f, 3);
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
  observers.add(Vec3{512.0f, 0.0f, 512.0f}, 1.0f);
  observers.add(Vec3{3000.0f, 0.0f, 900.0f}, 2.0f);
  observers.add(Vec3{100.0f, 0.0f, 3500.0f}, 0.5f);
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

TEST_CASE("tiers: the tier count is configurable and out-of-range tiers are clamped") {
  ObserverSet observers;
  observers.add(Vec3{0.0f, 0.0f, 0.0f}, 1.0f);
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
  population.add(Vec3{100.0f, 0.0f, 0.0f}, 1.0f, 7);
  TierAssignment assignment;
  Vector<TierChange> changes;
  assignment.assign_tiers(population.input(), observers, params, changes);
  CHECK(population.tiers[0] == 1);
  CHECK(changes.empty());
}
