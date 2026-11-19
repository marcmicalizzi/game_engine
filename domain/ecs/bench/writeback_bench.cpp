// The write-back flush (roadmap S11; docs/subsystems/ecs.md, "Write-back by change detection"):
// what `sim::Materializer::flush_writeback` costs over the entity store's hook at 10^3, 10^4 and
// 10^5 watched stalls when nothing changed since the last flush, when 1% did and when all did —
// with the tracked scan (`WritebackScan::Changed`, the default) and with the scan of every entity
// it replaced (`WritebackScan::Every`), in two shapes: every stall a root (one table, the shape of
// a world of residents) and one market per sixteen stalls (a table per market, the shape of a
// hierarchy).
//
// Only the flush is timed: the writes before it are a system's, made untimed through `modified` as
// a tracked writer makes them. The sink counts and keeps nothing — committing the batch to a
// document and logging its events is the host's — and once per row, on stderr, the bench says what
// one flush hands it (the fields, which are the document's commands, and the records, which are the
// store's events) and how many engine allocations it made.
#include <core/ids/id128.h>
#include <core/json/json_value.h>
#include <core/memory/memory.h>
#include <domain/doc/document.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/materialize.h>
#include <domain/ecs/sim_world.h>
#include <domain/sim/materialize.h>
#include <domain/sim/scheduler.h>
#include <foundation/bench/bench.h>

#include <cstdio>
#include <memory>
#include <schemas/ecs_demo.h>
#include <schemas/ecs_demo_ecs.h>

using namespace engine;

namespace {

constexpr u32 k_stalls_per_market = 16;

enum class Shape : u8 { Flat, Markets };
enum class Change : u8 { None, OnePercent, All };

Id128 record_id(u32 n) { return Id128::from_parts(0xB0C5, n + 1u); }

u32 size_of(const bench::State& state) {
  return bench::smoke_mode() ? 1000u : static_cast<u32>(state.arg());
}

JsonValue position(f64 x, f64 z) {
  JsonValue v = JsonValue::array();
  v.push_back(JsonValue(x));
  v.push_back(JsonValue(0.0));
  v.push_back(JsonValue(z));
  return v;
}

// `stalls` stalls; with markets, a market before every sixteen, each stall its child.
std::unique_ptr<doc::Document> stall_document(u32 stalls, Shape shape) {
  auto d = std::make_unique<doc::Document>();
  d->add_layer("base", doc::LayerRole::Base);
  u32 n = 0;
  Id128 market;
  for (u32 s = 0; s < stalls; ++s) {
    if (shape == Shape::Markets && s % k_stalls_per_market == 0) {
      JsonValue props = JsonValue::object();
      props.set("position", position(static_cast<f64>(s % 1000), static_cast<f64>(s / 1000)));
      market = record_id(n++);
      (void)d->apply(doc::cmd_create(market, "engine.ecs.demo.Market", Id128{}, props), nullptr,
                     nullptr);
    }
    JsonValue props = JsonValue::object();
    props.set("position", position(static_cast<f64>(s % 1000), static_cast<f64>(s / 1000)));
    props.set("wealth", JsonValue(static_cast<i64>(s)));
    props.set("sign", JsonValue("stall"));
    (void)d->apply(doc::cmd_create(record_id(n++), "engine.ecs.demo.Stall",
                                   shape == Shape::Markets ? market : Id128{}, props),
                   nullptr, nullptr);
  }
  return d;
}

struct Counter {
  u64 fields = 0;
  u64 records = 0;
  static bool commit(void* context, const sim::WriteBackBatch& batch) {
    auto* self = static_cast<Counter*>(context);
    self->fields += batch.changes.size();
    for (u32 i = 0; i < batch.changes.size(); ++i) {
      if (i == 0 || !(batch.changes[i].record == batch.changes[i - 1].record)) ++self->records;
    }
    return true;
  }
};

struct World {
  ecs::SimWorld sim;
  sim::SimScheduler scheduler;
  ecs::RecordMaterializer records;
  sim::Materializer driver;
  Counter counter;
  Vector<flecs::entity_t> stalls;

  explicit World(ecs::WritebackScan scan)
      : records(sim.world(), config_of(scan)), driver(scheduler, sim::MaterializeConfig{}) {
    ecs::demo::register_ecs_demo_components(sim.world());
    scheduler.add_hooks(records.hooks());
    driver.set_target(records.target());
    driver.set_writeback_sink(sim::WriteBackSink{&counter, &Counter::commit});
  }
  static ecs::RecordMaterializerConfig config_of(ecs::WritebackScan scan) {
    ecs::RecordMaterializerConfig c;
    c.scan = scan;
    return c;
  }
};

void run(bench::State& state, ecs::WritebackScan scan, Shape shape, Change change,
         const char* name) {
  const u32 n = size_of(state);
  const std::unique_ptr<doc::Document> document = stall_document(n, shape);
  World world(scan);
  (void)world.driver.materialize(*document);
  flecs::world& w = world.sim.world();
  world.sim.world().each(
      [&world](flecs::entity e, const ecs::demo::Label&) { world.stalls.push_back(e.id()); });
  // The first flush takes the creation's tables; what is measured is a flush after a system's work.
  (void)world.driver.flush_writeback(SimTick{1}, GameTime{0});
  const u32 step = change == Change::OnePercent ? 100u : 1u;
  u64 tick = 2;
  bool reported = false;
  while (state.keep_running()) {
    state.pause_timing();
    if (change != Change::None) {
      for (u32 i = 0; i < world.stalls.size(); i += step) {
        const flecs::entity e(w, world.stalls[i]);
        e.get_mut<ecs::demo::Standing>().wealth += 1;
        e.modified<ecs::demo::Standing>();
      }
    }
    const u64 fields = world.counter.fields;
    const u64 records = world.counter.records;
    const u64 allocations = mem::allocation_counter();
    state.resume_timing();
    const u32 written = world.driver.flush_writeback(SimTick{tick++}, GameTime{0});
    state.pause_timing();
    bench::keep(written);
    if (!reported) {
      reported = true;
      std::fprintf(stderr,
                   "%s/%u: %llu fields (document commands), %llu records (store events), %llu "
                   "engine allocations, %llu bytes held\n",
                   name, n, static_cast<unsigned long long>(world.counter.fields - fields),
                   static_cast<unsigned long long>(world.counter.records - records),
                   static_cast<unsigned long long>(mem::allocation_counter() - allocations),
                   static_cast<unsigned long long>(world.records.writeback_bytes()));
    }
    state.resume_timing();
  }
  state.set_items(n);
}

}  // namespace

#define ENGINE_WRITEBACK_BENCH(ident, name, scan, shape, change)              \
  ENGINE_BENCH_ARGS(ident, name, 1000, 10000, 100000) {                       \
    run(state, ecs::WritebackScan::scan, Shape::shape, Change::change, name); \
  }

ENGINE_WRITEBACK_BENCH(wb_flat_none, "ecs.writeback.flat.nochange", Changed, Flat, None)
ENGINE_WRITEBACK_BENCH(wb_flat_one, "ecs.writeback.flat.one_percent", Changed, Flat, OnePercent)
ENGINE_WRITEBACK_BENCH(wb_flat_all, "ecs.writeback.flat.all", Changed, Flat, All)
ENGINE_WRITEBACK_BENCH(wb_markets_none, "ecs.writeback.markets.nochange", Changed, Markets, None)
ENGINE_WRITEBACK_BENCH(wb_markets_one, "ecs.writeback.markets.one_percent", Changed, Markets,
                       OnePercent)
ENGINE_WRITEBACK_BENCH(wb_markets_all, "ecs.writeback.markets.all", Changed, Markets, All)
ENGINE_WRITEBACK_BENCH(wb_every_flat_none, "ecs.writeback.every.flat.nochange", Every, Flat, None)
ENGINE_WRITEBACK_BENCH(wb_every_flat_one, "ecs.writeback.every.flat.one_percent", Every, Flat,
                       OnePercent)
ENGINE_WRITEBACK_BENCH(wb_every_flat_all, "ecs.writeback.every.flat.all", Every, Flat, All)
ENGINE_WRITEBACK_BENCH(wb_every_markets_none, "ecs.writeback.every.markets.nochange", Every,
                       Markets, None)
ENGINE_WRITEBACK_BENCH(wb_every_markets_one, "ecs.writeback.every.markets.one_percent", Every,
                       Markets, OnePercent)
ENGINE_WRITEBACK_BENCH(wb_every_markets_all, "ecs.writeback.every.markets.all", Every, Markets, All)
