// Micro-benchmarks for the npc capability (docs/plan/11-performance-principles.md §11.8,
// docs/experiments/e38-scheduled-npcs.md): what a resident costs where 05 §5.6's table puts a cost
// — a tick with 10^3..10^5 held at LOD2 (transitions only), the same with an observer in their
// midst (the LOD1 band moving its trips and the tier pass every sixth tick), one transition off the
// wheel, the summary of a game week, the first materialization, and the generator. And three costs
// that are not this capability's but that 10^5 residents put in front of it, so E38 can be re-taken
// with one command: the write-back's scan of every watched entity, one tile's pass over the
// document, and a 10^5-resident document's load.
#include <domain/doc/document.h>
#include <domain/doc/document_store.h>
#include <domain/ecs/materialize.h>
#include <domain/ecs/scheduled_tick.h>
#include <domain/ecs/sim_world.h>
#include <domain/sim/materialize.h>
#include <foundation/bench/bench.h>
#include <foundation/io/vfs.h>
#include <systems/npc/generator.h>
#include <systems/npc/npc.h>
#include <systems/npc/routine.h>

#include <test_temp_dir.h>

#include <memory>
#include <string>

using namespace engine;
using namespace engine::npc;

namespace {

constexpr i64 k_morning = 7 * 60 * k_us_per_minute;

// 10^5 residents over a kilometre square: about one a 10 m², clustered at their places.
GeneratorParams bench_world(u32 residents) {
  GeneratorParams p;
  p.seed = 1;
  p.residents = residents;
  const f64 half = residents >= 100'000 ? 512.0 : residents >= 10'000 ? 160.0 : 52.0;
  p.min_x = -half;
  p.min_z = -half;
  p.max_x = half;
  p.max_z = half;
  p.time_us = k_morning;
  return p;
}

// The row's population, or a thousand in the CTest smoke run, which proves a row runs and is not a
// measurement: a debug build takes a minute and a half over the full populations.
u32 population(const bench::State& state) {
  const u32 n = static_cast<u32>(state.arg());
  return bench::smoke_mode() && n > 1000 ? 1000u : n;
}

struct World {
  ecs::SimWorld sim;
  sim::SimScheduler scheduler;
  ecs::RecordMaterializer records;
  NpcSystem npc;
  sim::Materializer driver;
  std::unique_ptr<ecs::ScheduledTick> tick;
  doc::Document document;

  static sim::SimSchedulerConfig scheduler_config() {
    sim::SimSchedulerConfig c;
    c.epoch = GameTime{k_morning};
    return c;
  }
  static NpcConfig npc_config() {
    NpcConfig c = config_from_tunables();
    c.world_seed = 1;
    return c;
  }
  explicit World(u32 residents)
      : scheduler(scheduler_config()), records(sim.world()), npc(npc_config()), driver(scheduler) {
    npc.install(sim, scheduler);
    scheduler.add_hooks(records.hooks());
    scheduler.add_hooks(npc.hooks());
    driver.set_target(records.target());
    tick = std::make_unique<ecs::ScheduledTick>(sim, scheduler);
    document.add_layer("base", doc::LayerRole::Base);
    document.set_edit_layer(document.add_layer(generate_layer(bench_world(residents))));
    document.rebuild_index();
    npc.places().refresh(document);
  }
  ~World() {
    driver.dematerialize_all();
    tick.reset();
  }
};

}  // namespace

// One fixed step with every resident held at LOD2: the wheel pumped, whatever transitions fall in
// the step, the two systems' early outs. Items are residents, so the rate column is ns a resident a
// tick.
ENGINE_BENCH_ARGS(npc_tick, "npc.tick", 1000, 10000, 100000) {
  const u32 residents = population(state);
  World world(residents);
  world.driver.materialize(world.document);
  while (state.keep_running()) {
    world.tick->step();
    bench::keep(world.npc.stats().transitions);
  }
  state.set_items(residents);
}

// The same with one observer at the centre: the LOD0-1 band's trips move every tick and the tier
// pass runs every sixth (npc.lod.every_ticks) over every held resident.
ENGINE_BENCH_ARGS(npc_tick_observed, "npc.tick.observed", 1000, 10000, 100000) {
  const u32 residents = population(state);
  World world(residents);
  sim::ObserverSet observers;
  observers.add(Vec3{}, 1.0f);
  world.npc.set_observers(&observers);
  world.driver.materialize(world.document);
  // Let the rate limits settle the tiers before timing.
  for (u32 i = 0; i < 6 * 400; ++i)
    world.tick->step();
  while (state.keep_running()) {
    world.tick->step();
    bench::keep(world.npc.stats().moved);
  }
  u32 counts[4] = {};
  world.npc.tier_counts(counts);
  bench::keep(counts[1]);
  state.set_items(residents);
}

// One transition: a game day of 10^4 residents executed off the wheel under a budget that never
// trips, per event delivered — the closed form, the components written, the next timer armed.
ENGINE_BENCH_ARGS(npc_transition, "npc.transition", 10000) {
  const u32 residents = population(state);
  World world(residents);
  world.driver.materialize(world.document);
  u64 delivered = 0;
  i64 at = k_morning;
  while (state.keep_running()) {
    at += k_day_us;
    const sim::FastForwardResult r = world.npc.fast_forward(GameTime{at}, u64{1} << 40);
    delivered = r.delivered;
    bench::keep(delivered);
  }
  state.set_items(delivered);
}

// The summary of a game week over every held resident; items are residents.
ENGINE_BENCH_ARGS(npc_summarize, "npc.summarize", 1000, 10000, 100000) {
  const u32 residents = population(state);
  World world(residents);
  world.driver.materialize(world.document);
  i64 at = k_morning;
  while (state.keep_running()) {
    const i64 from = at;
    at += 7 * k_day_us;
    bench::keep(world.npc.summarize(GameTime{from}, GameTime{at}));
  }
  state.set_items(residents);
}

// The first materialization of a document of places and residents into an empty world, through the
// entity store's hook and this capability's; items are records.
ENGINE_BENCH_ARGS(npc_materialize, "npc.materialize", 1000, 10000) {
  const u32 residents = population(state);
  World world(residents);
  while (state.keep_running()) {
    world.driver.materialize(world.document);
    state.pause_timing();
    world.driver.dematerialize_all();
    state.resume_timing();
  }
  state.set_items(world.document.object_count());
}

// The generator on this thread; items are records.
ENGINE_BENCH_ARGS(npc_generate, "npc.generate", 1000, 10000, 100000) {
  const GeneratorParams p = bench_world(population(state));
  u32 records = 0;
  while (state.keep_running()) {
    const doc::Layer layer = generate_layer(p);
    records = layer.size();
    bench::keep(records);
  }
  state.set_items(records);
}

// The write-back flush with nothing changed, over every held resident's three watched fields: the
// scan sim.md and ecs.md warn about (E38, "The write-back scan"). Items are residents.
ENGINE_BENCH_ARGS(npc_writeback_scan, "npc.writeback_scan", 10000, 100000) {
  const u32 residents = population(state);
  World world(residents);
  world.driver.materialize(world.document);
  sim::WriteBackSink sink;
  sink.commit = [](void*, const sim::WriteBackBatch&) { return true; };
  world.driver.set_writeback_sink(sink);
  while (state.keep_running())
    bench::keep(world.driver.flush_writeback(world.scheduler.tick(), world.scheduler.game_time()));
  state.set_items(residents);
}

// One tile of the partitioned layer materialized and let go again, in a document of 10^4 or 10^5
// residents: the driver's tile scope classifies every live record to find the tile's (sim.md, "A
// tile pass is a pass over the document"), so this grows with the document, not the tile.
ENGINE_BENCH_ARGS(npc_tile_pass, "npc.tile_pass", 10000, 100000) {
  const u32 residents = population(state);
  World world(residents);
  const doc::TileCoord tile{0, 0};
  while (state.keep_running()) {
    bench::keep(
        world.driver.materialize(world.document, sim::MaterializeScope::of_tile(tile)).created);
    state.pause_timing();
    world.driver.dematerialize(sim::MaterializeScope::of_tile(tile));
    state.resume_timing();
  }
  state.set_items(1);
}

// A generated document of 10^4 or 10^5 residents and their places loaded from its directory: the
// document part of a save's load time (E38). Items are records.
ENGINE_BENCH_ARGS(npc_document_load, "npc.document_load", 10000, 100000) {
  const test::TempDir tmp("npc_bench_document_load");
  io::Vfs vfs;
  std::string error;
  const std::string dir = tmp.file("world");
  generate_document(vfs, dir, bench_world(population(state)), nullptr, error);
  u32 records = 0;
  while (state.keep_running()) {
    doc::Document loaded;
    doc::DocumentManifest manifest;
    bench::keep(doc::DocumentStore::load(vfs, dir, loaded, manifest, &error));
    records = loaded.object_count();
  }
  state.set_items(records);
}
