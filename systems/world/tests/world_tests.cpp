// The tile ring and the consumer table (systems/world/tile_ring.h, world.h;
// docs/subsystems/world.md): tile order, the ring as `sim::TierAssignment` over tiles, the
// per-update budget, hysteresis, determinism from the observer set, and the order consumers are
// called in. No device, no files.
#include <domain/sim/tiers.h>
#include <systems/world/tile_ring.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::world;

namespace {

sim::ObserverSet one(Vec3 position, f32 weight = 1.0f) {
  sim::ObserverSet set;
  set.add(position, weight);
  return set;
}

RingParams unlimited_params() {
  RingParams p;
  p.max_activations = 0;
  p.max_deactivations = 0;
  return p;
}

// Brute force: the tiles whose centre is within the ring's radius of one observer.
u32 tiles_within(Vec3 observer, f32 radius_tiles, f32 tile_size) {
  const i32 reach = static_cast<i32>(std::ceil(radius_tiles)) + 2;
  const TileCoord at = tile_at(observer, tile_size);
  u32 n = 0;
  for (i32 x = at.x - reach; x <= at.x + reach; ++x) {
    for (i32 z = at.z - reach; z <= at.z + reach; ++z) {
      const Vec3 c = tile_center(TileCoord{x, z}, tile_size);
      const f32 d = std::sqrt((c.x - observer.x) * (c.x - observer.x) +
                              (c.z - observer.z) * (c.z - observer.z));
      n += d < radius_tiles * tile_size ? 1u : 0u;
    }
  }
  return n;
}

// A consumer that writes what it was called with into a shared log.
struct Recorder {
  std::string name;
  std::vector<std::string>* log = nullptr;
  bool refuse = false;
  u32 commits = 0;

  static std::string line(const std::string& who, const char* what, const TileEvent& e) {
    return who + " " + what + " " + std::to_string(e.tile.x) + "," + std::to_string(e.tile.z) +
           " " + std::to_string(e.from == k_inactive ? -1 : e.from) + ">" +
           std::to_string(e.to == k_inactive ? -1 : e.to);
  }
  static bool activate(void* c, const TileEvent& e) {
    auto* self = static_cast<Recorder*>(c);
    self->log->push_back(line(self->name, "activate", e));
    return !self->refuse;
  }
  static bool change(void* c, const TileEvent& e) {
    auto* self = static_cast<Recorder*>(c);
    self->log->push_back(line(self->name, "change", e));
    return true;
  }
  static void deactivate(void* c, const TileEvent& e) {
    auto* self = static_cast<Recorder*>(c);
    self->log->push_back(line(self->name, "deactivate", e));
  }
  static void commit(void* c) { ++static_cast<Recorder*>(c)->commits; }
  TileConsumer row(u8 rings) {
    TileConsumer out;
    out.name = name.c_str();
    out.context = this;
    out.rings = rings;
    out.activate = &activate;
    out.change_ring = &change;
    out.deactivate = &deactivate;
    out.commit = &commit;
    return out;
  }
};

}  // namespace

TEST_CASE("world: tile order is x then z, signed, and the store's id is the write-back packing") {
  const TileCoord tiles[] = {{-2, 5}, {-2, -7}, {0, 0}, {3, -1}, {-1, 100}, {3, -2}};
  std::vector<TileCoord> sorted(std::begin(tiles), std::end(tiles));
  std::sort(sorted.begin(), sorted.end(),
            [](TileCoord a, TileCoord b) { return tile_key(a) < tile_key(b); });
  const TileCoord expected[] = {{-2, -7}, {-2, 5}, {-1, 100}, {0, 0}, {3, -2}, {3, -1}};
  for (u32 i = 0; i < 6; ++i) {
    CHECK(sorted[i] == expected[i]);
    CHECK(tile_of_key(tile_key(expected[i])) == expected[i]);
  }
  CHECK(store_tile(TileCoord{1, 2}) == ((u64{1} << 32) | 2u));
  CHECK(store_tile(TileCoord{-1, 0}) == (u64{0xFFFFFFFFu} << 32));
  CHECK(tile_at(Vec3{-0.5f, 7.0f, 31.9f}, 32.0f) == TileCoord{-1, 0});
  CHECK(tile_at(Vec3{64.0f, 0.0f, -32.0f}, 32.0f) == TileCoord{2, -1});
}

TEST_CASE("world: ring parameters that do not make a ring are refused with a reason") {
  const char* why = nullptr;
  RingParams p;
  CHECK(valid_ring_params(p, &why));
  p.radius[1] = 1.0f;  // not increasing
  CHECK_FALSE(valid_ring_params(p, &why));
  CHECK(why != nullptr);
  p = RingParams{};
  p.tile_size = 0.0f;
  CHECK_FALSE(valid_ring_params(p));
  p = RingParams{};
  p.ring_count = 0;
  CHECK_FALSE(valid_ring_params(p));
  p = RingParams{};
  p.hysteresis = -0.1f;
  CHECK_FALSE(valid_ring_params(p));
  World world;
  RingParams bad;
  bad.ring_count = k_max_rings + 1;
  CHECK_FALSE(world.configure(bad, &why));
}

TEST_CASE("world: a still observer's rings are the tiles within each radius, in tile order") {
  const Vec3 observer{10.0f, 3.0f, -20.0f};
  TileRing ring(unlimited_params());
  Vector<TileEvent> events;
  ring.update(one(observer), events);
  const RingParams& p = ring.params();
  const u32 inner = tiles_within(observer, p.radius[0], p.tile_size);
  const u32 mid = tiles_within(observer, p.radius[1], p.tile_size);
  const u32 far = tiles_within(observer, p.radius[2], p.tile_size);
  CHECK(ring.stats().per_ring[0] == inner);
  CHECK(ring.stats().per_ring[1] == mid - inner);
  CHECK(ring.stats().per_ring[2] == far - mid);
  CHECK(ring.active_count() == far);
  REQUIRE(events.size() == far);
  for (u32 i = 0; i < events.size(); ++i) {
    CHECK(events[i].kind == TileEventKind::Activate);
    CHECK(events[i].from == k_inactive);
    if (i > 0) CHECK(tile_key(events[i - 1].tile) < tile_key(events[i].tile));
  }
  // The observer's own tile is in the inner ring, and a ring is sim's tier of the tile's score.
  CHECK(ring.ring_of(tile_at(observer, p.tile_size)) == 0);
  sim::TierParams tiers;
  tiers.tier_count = p.ring_count + 1;
  for (u32 r = 0; r < p.ring_count; ++r)
    tiers.boundaries[r] = p.radius[r] * p.tile_size;
  sim::ObserverSet ground = one(Vec3{observer.x, 0.0f, observer.z});
  for (const TileEvent& e : events) {
    const f32 score =
        sim::TierAssignment::score(tile_center(e.tile, p.tile_size), 1.0f, ground, tiers);
    CHECK(sim::TierAssignment::tier_of(score, tiers) == e.to);
  }
  // A second update from the same place changes nothing.
  events.clear();
  CHECK(ring.update(one(observer), events) == 0);
}

TEST_CASE("world: the budget spreads a first fill over updates, nearest first") {
  RingParams p;  // 8 activations an update
  TileRing ring(p);
  const Vec3 observer{0.0f, 0.0f, 0.0f};
  Vector<TileEvent> events;
  ring.update(one(observer), events);
  CHECK(events.size() == p.max_activations);
  // The first eight are the nearest: all of the inner ring's four tiles round the origin corner
  // and the next nearest after them.
  CHECK(ring.stats().deferred_promotions > 0);
  u32 inner = 0;
  for (const TileEvent& e : events)
    inner += e.to == 0 ? 1u : 0u;
  CHECK(inner == tiles_within(observer, p.radius[0], p.tile_size));
  u32 updates = 1;
  while (ring.stats().deferred_promotions > 0 || !events.empty()) {
    events.clear();
    ring.update(one(observer), events);
    CHECK(events.size() <= p.max_activations);
    ++updates;
    REQUIRE(updates < 10000);
  }
  CHECK(ring.active_count() == tiles_within(observer, p.radius[2], p.tile_size));
  MESSAGE("first fill at 8 an update: " << updates << " updates for " << ring.active_count()
                                        << " tiles");
}

TEST_CASE("world: the same observer path gives the same events, byte for byte") {
  auto run = [](Vector<TileEvent>& out) {
    World world;
    REQUIRE(world.configure(RingParams{}));
    Vector<u8> bytes;
    for (u32 t = 0; t < 400; ++t) {
      // A path that turns, speeds up and doubles back, with a second observer that comes and goes.
      const f32 s = static_cast<f32>(t);
      sim::ObserverSet observers;
      observers.add(Vec3{s * 3.1f - 200.0f, 2.0f, 40.0f * std::sin(s * 0.02f)}, 1.0f);
      if (t % 97 < 50) observers.add(Vec3{-150.0f, 0.0f, 300.0f - s}, 0.5f);
      world.update(observers, t);
      for (const TileEvent& e : world.last_events())
        out.push_back(e);
    }
  };
  Vector<TileEvent> a;
  Vector<TileEvent> b;
  run(a);
  run(b);
  REQUIRE(a.size() == b.size());
  CHECK(a.size() > 1000);
  CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(TileEvent)) == 0);
}

TEST_CASE("world: hysteresis crosses a ring boundary once where none would thrash") {
  // An observer walking back and forth by a metre across the inner ring's radius from one tile's
  // centre: 1.5 tiles is 48 m, so the tile at x + 1.5 tiles flips on every step with no band.
  auto flips = [](f32 hysteresis) {
    RingParams p = unlimited_params();
    p.hysteresis = hysteresis;
    TileRing ring(p);
    Vector<TileEvent> events;
    const TileCoord watched{1, 0};
    const Vec3 center = tile_center(watched, p.tile_size);
    u32 changes = 0;
    for (u32 t = 0; t < 40; ++t) {
      const f32 x = center.x - p.radius[0] * p.tile_size + (t % 2 == 0 ? -1.0f : 1.0f);
      events.clear();
      ring.update(one(Vec3{x, 0.0f, center.z}), events);
      for (const TileEvent& e : events)
        changes += e.tile == watched ? 1u : 0u;
    }
    return changes;
  };
  CHECK(flips(0.0f) >= 30);
  CHECK(flips(0.15f) <= 2);
}

TEST_CASE("world: a teleport activates the new place nearest first and lets the old go farthest") {
  RingParams p;
  p.max_activations = 8;
  p.max_deactivations = 16;
  TileRing ring(p);
  Vector<TileEvent> events;
  ring.update(one(Vec3{0, 0, 0}), events, /*unlimited=*/true);
  const u32 before = ring.active_count();
  events.clear();
  ring.update(one(Vec3{5000.0f, 0.0f, 0.0f}), events);
  u32 activated = 0;
  u32 activated_inner = 0;
  u32 deactivated = 0;
  for (const TileEvent& e : events) {
    activated += e.kind == TileEventKind::Activate ? 1u : 0u;
    activated_inner += e.kind == TileEventKind::Activate && e.to == 0 ? 1u : 0u;
    deactivated += e.kind == TileEventKind::Deactivate ? 1u : 0u;
  }
  // The whole inner ring is among the first eight: the nearest go first.
  const u32 inner = tiles_within(Vec3{5000.0f, 0.0f, 0.0f}, p.radius[0], p.tile_size);
  REQUIRE(inner <= p.max_activations);
  CHECK(activated_inner == inner);
  CHECK(activated == p.max_activations);
  CHECK(deactivated == p.max_deactivations);
  CHECK(ring.active_count() == before - deactivated + activated);
}

TEST_CASE("world: two observers take the nearer, and a heavier one reaches farther") {
  TileRing ring(unlimited_params());
  Vector<TileEvent> events;
  sim::ObserverSet two;
  two.add(Vec3{0, 0, 0}, 1.0f);
  two.add(Vec3{2000.0f, 0.0f, 0.0f}, 2.0f);
  ring.update(two, events);
  const RingParams& p = ring.params();
  CHECK(ring.ring_of(tile_at(Vec3{0, 0, 0}, p.tile_size)) == 0);
  CHECK(ring.ring_of(tile_at(Vec3{2000.0f, 0.0f, 0.0f}, p.tile_size)) == 0);
  // Two tiles along x from each: the light observer's is 82 m to its centre, past the inner ring's
  // 48 m; the heavy one's is 66 m, which its weight of two halves to 33, inside it.
  const TileCoord heavy_near = tile_at(Vec3{2000.0f + 64.0f, 0.0f, 0.0f}, p.tile_size);
  const TileCoord light_near = tile_at(Vec3{64.0f, 0.0f, 0.0f}, p.tile_size);
  CHECK(ring.ring_of(heavy_near) == 0);
  CHECK(ring.ring_of(light_near) == 1);
  CHECK(ring.active_count() > tiles_within(Vec3{0, 0, 0}, p.radius[2], p.tile_size));
}

TEST_CASE("world: consumers activate in registration order and deactivate in reverse") {
  std::vector<std::string> log;
  Recorder a{"a", &log};
  Recorder b{"b", &log};
  RingParams p = unlimited_params();
  p.ring_count = 2;
  p.radius[0] = 1.0f;
  p.radius[1] = 2.0f;
  p.hysteresis = 0.0f;
  World world(p);
  world.add_consumer(a.row(0x3));  // both rings
  world.add_consumer(b.row(0x1));  // the inner ring only
  // An observer at a tile's centre: that tile is inner (0 m), its four neighbours at one tile are
  // on the inner radius and so outer, the diagonals at 1.41 tiles outer too.
  const Vec3 center = tile_center(TileCoord{0, 0}, p.tile_size);
  world.update(one(center), 1);
  REQUIRE(!log.empty());
  // Tile (0, 0): a then b.
  const auto at = std::find(log.begin(), log.end(), "a activate 0,0 -1>0");
  REQUIRE(at != log.end());
  CHECK(*(at + 1) == "b activate 0,0 -1>0");
  // A neighbour only in the outer ring: a alone.
  CHECK(std::find(log.begin(), log.end(), "a activate 1,0 -1>1") != log.end());
  CHECK(std::find(log.begin(), log.end(), "b activate 1,0 -1>1") == log.end());
  CHECK(a.commits == 1);
  CHECK(b.commits == 1);

  // Move a tile over: (0, 0) goes from inner to outer — a moves it, b lets it go — and (1, 0)
  // comes in — a moves it, b takes it.
  log.clear();
  world.update(one(tile_center(TileCoord{1, 0}, p.tile_size)), 2);
  const auto b_off = std::find(log.begin(), log.end(), "b deactivate 0,0 0>-1");
  const auto a_move = std::find(log.begin(), log.end(), "a change 0,0 0>1");
  REQUIRE(b_off != log.end());
  REQUIRE(a_move != log.end());
  CHECK(b_off < a_move);  // letting go before taking
  CHECK(std::find(log.begin(), log.end(), "a change 1,0 1>0") != log.end());
  CHECK(std::find(log.begin(), log.end(), "b activate 1,0 -1>0") != log.end());

  // Clear: every tile deactivated, b before a on the tiles both hold.
  log.clear();
  world.clear(3);
  const auto b_last = std::find(log.begin(), log.end(), "b deactivate 1,0 0>-1");
  const auto a_last = std::find(log.begin(), log.end(), "a deactivate 1,0 0>-1");
  REQUIRE(b_last != log.end());
  REQUIRE(a_last != log.end());
  CHECK(b_last < a_last);
  CHECK(world.ring().active_count() == 0);
  CHECK(world.consumer_stats(0).deactivations == world.consumer_stats(0).activations);
}

TEST_CASE("world: a refused tile is counted and stays active for the others") {
  std::vector<std::string> log;
  Recorder a{"a", &log};
  Recorder b{"b", &log, /*refuse=*/true};
  World world(unlimited_params());
  world.add_consumer(a.row(0x7F));
  world.add_consumer(b.row(0x01));
  const UpdateStats& s = world.update(one(Vec3{0, 0, 0}), 0);
  CHECK(s.refused == s.per_ring[0]);
  CHECK(world.consumer_stats(1).refusals == s.per_ring[0]);
  CHECK(world.consumer_stats(0).refusals == 0);
  CHECK(world.ring().active_count() == s.active);
}

TEST_CASE("world declares a determinism stance") { CHECK(std::string(k_determinism) == "hashed"); }
