// Micro-benchmarks for the world capability (docs/subsystems/world.md, "Performance notes";
// docs/plan/11-performance-principles.md §11.8): the ring's update, which runs once a frame or a
// tick, and one tile's round trip through the store, which runs once per tile that goes and comes.
// The ruins consumer's activation is measured where it runs, in E35's flythrough.
#include <domain/sim/tiers.h>
#include <foundation/bench/bench.h>
#include <systems/world/tile_ring.h>
#include <systems/world/tile_store.h>
#include <systems/world/world.h>

#include <cmath>

using namespace engine;

namespace {

WorldPos path_at(u32 t) {
  // 1.5 m a frame along a gentle curve: E35's flythrough moves about that fast.
  const f32 s = static_cast<f32>(t);
  return WorldPos{static_cast<f64>(s * 1.5f - 1500.0f), 10.0,
                  static_cast<f64>(200.0f * std::sin(s * 0.001f))};
}

}  // namespace

// One update of the default rings (1.5 / 8 / 24 tiles of 32 m) over an observer moving along a
// path, with the default budget; the argument is how many observers, each offset from the first.
ENGINE_BENCH_ARGS(world_ring_update, "world.ring.update", 1, 2, 4) {
  const u32 count = static_cast<u32>(state.arg());
  world::RingParams params;
  world::TileRing ring(params);
  Vector<world::TileEvent> events;
  sim::ObserverSet observers;
  // Warm: the ring filled round the path's start, so each update is the steady state.
  for (u32 o = 0; o < count; ++o)
    observers.add(path_at(0) + DVec3{static_cast<f64>(o) * 700.0, 0.0, 0.0}, 1.0f);
  ring.update(observers, events, true);
  u32 t = 0;
  u64 total_events = 0;
  while (state.keep_running()) {
    observers.clear();
    ++t;
    for (u32 o = 0; o < count; ++o)
      observers.add(path_at(t) + DVec3{static_cast<f64>(o) * 700.0, 0.0, 0.0}, 1.0f);
    events.clear();
    total_events += ring.update(observers, events);
    bench::keep(events.data());
  }
  bench::keep(total_events);
  state.set_items(1);
}

// The first fill of the default rings from nothing, unlimited: about 1,800 tiles activated at once.
ENGINE_BENCH(world_ring_fill, "world.ring.fill") {
  world::RingParams params;
  Vector<world::TileEvent> events;
  sim::ObserverSet observers;
  observers.add(WorldPos{10.0, 0.0, 10.0}, 1.0f);
  u32 activated = 0;
  while (state.keep_running()) {
    world::TileRing ring(params);
    events.clear();
    activated = ring.update(observers, events, true);
    bench::keep(events.data());
  }
  state.set_items(activated);
}

// A tile of 64 records written (projections and snapshot) and read back, in an in-memory store.
ENGINE_BENCH(world_store_tile, "world.store.tile") {
  world::WorldStore store;
  if (store.open_memory() != store::Status::Ok) return;
  Vector<world::TileRow> rows(64);
  for (u32 i = 0; i < rows.size(); ++i) {
    rows[i].record = Id128::from_parts(1, i + 1);
    rows[i].type = "engine.world.Node";
    rows[i].position = WorldPos{static_cast<f64>(i), 0.0, 3.0};
  }
  Vector<world::TileRow> back;
  bool has_snapshot = false;
  store::SnapshotInfo info;
  u64 tick = 0;
  while (state.keep_running()) {
    ++tick;
    store.write_tile(world::TileCoord{3, -4},
                     std::span<const world::TileRow>(rows.data(), rows.size()), tick,
                     static_cast<i64>(tick) * 16'667);
    store.read_tile(world::TileCoord{3, -4}, back, has_snapshot, info);
    bench::keep(back.data());
  }
  state.set_items(rows.size());
}
