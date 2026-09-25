// The materialization driver over the entity store's hook (docs/subsystems/sim.md, "The driver";
// domain/ecs/materialize.h): a document of 10^4 and 10^5 records into a fresh world, the pass after
// it that finds nothing changed, and one that finds 1% changed.
//
// The document is E6's shape at materialization's scale: one `Market` per sixteen `Stall`s, each
// stall a child of its market, so a tenth of a second's worth of records is also a hierarchy and
// `ChildOf` fragments the archetypes the way a real document would (one table per parent's
// children). Every stall maps four fields into two components and writes two of them back.
#include <core/ids/id128.h>
#include <core/json/json_value.h>
#include <domain/doc/document.h>
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

Id128 record_id(u32 n) { return Id128::from_parts(0xBE7C, n + 1u); }

// The row's size, or a thousand records in a smoke run: CTest runs every bench once to prove it
// works, in a debug build, and a hundred thousand records there proves nothing more than a
// thousand.
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

// `records` records, a market first and then its sixteen stalls, repeated. Built once per size and
// kept for the process: building it is not what is measured.
const doc::Document& market_document(u32 records) {
  struct Cached {
    u32 records = 0;
    std::unique_ptr<doc::Document> document;
  };
  static Cached cache[4];
  for (Cached& c : cache) {
    if (c.records == records && c.document != nullptr) return *c.document;
  }
  Cached* slot = &cache[0];
  for (Cached& c : cache) {
    if (c.document == nullptr) {
      slot = &c;
      break;
    }
  }
  slot->records = records;
  slot->document = std::make_unique<doc::Document>();
  doc::Document& d = *slot->document;
  d.add_layer("base", doc::LayerRole::Base);
  u32 market = 0;
  for (u32 n = 0; n < records; ++n) {
    JsonValue props = JsonValue::object();
    props.set("position", position(static_cast<f64>(n % 1000), static_cast<f64>(n / 1000)));
    if (n % (k_stalls_per_market + 1u) == 0) {
      market = n;
      (void)d.apply(doc::cmd_create(record_id(n), "engine.ecs.demo.Market", Id128{}, props),
                    nullptr, nullptr);
      continue;
    }
    props.set("wealth", JsonValue(static_cast<i64>(n)));
    props.set("sign", JsonValue("stall"));
    (void)d.apply(doc::cmd_create(record_id(n), "engine.ecs.demo.Stall", record_id(market), props),
                  nullptr, nullptr);
  }
  return d;
}

// A world, its scheduler, the entity store's hook and the driver: what a host assembles.
struct World {
  ecs::SimWorld sim;
  sim::SimScheduler scheduler;
  ecs::RecordMaterializer records;
  sim::Materializer driver;

  World() : records(sim.world()), driver(scheduler, config()) {
    ecs::demo::register_ecs_demo_components(sim.world());
    scheduler.add_hooks(records.hooks());
    driver.set_target(records.target());
  }
  static sim::MaterializeConfig config() {
    sim::MaterializeConfig c;
    c.tier = 0;
    return c;
  }
};

}  // namespace

// A fresh world each iteration; only the pass is timed.
ENGINE_BENCH_ARGS(materialize_full, "ecs.materialize.full", 10000, 100000) {
  const u32 records = size_of(state);
  const doc::Document& document = market_document(records);
  bool reported = false;
  while (state.keep_running()) {
    state.pause_timing();
    auto world = std::make_unique<World>();
    state.resume_timing();
    const sim::MaterializeReport report = world->driver.materialize(document);
    state.pause_timing();
    bench::keep(report.live);
    if (!reported) {
      // Once per size, on stderr beside the table: what the hierarchy did to the archetype set.
      reported = true;
      ecs::TableWatch watch;
      watch.check(world->sim.world());
      std::fprintf(stderr, "ecs.materialize.full/%u: %u entities, %u tables\n", records,
                   report.live, watch.tables());
    }
    world.reset();
    state.resume_timing();
  }
  state.set_items(records);
}

// The pass after a full one when nothing changed: the change feed answers "nothing", so no record
// is visited and no hook is called. This is the number that says an unchanged record costs nothing.
ENGINE_BENCH_ARGS(materialize_nochange, "ecs.materialize.nochange", 10000, 100000) {
  const u32 records = size_of(state);
  const doc::Document& document = market_document(records);
  World world;
  (void)world.driver.materialize(document);
  while (state.keep_running()) {
    const sim::MaterializeReport report = world.driver.materialize(document);
    bench::keep(report.visited);
  }
  state.set_items(records);
}

// 1% of the stalls repainted between passes: the incremental pass visits and rewrites exactly
// those.
ENGINE_BENCH_ARGS(materialize_one_percent, "ecs.materialize.incremental_1pct", 10000, 100000) {
  const u32 records = size_of(state);
  // A private copy: this one is edited, and the shared documents stay as the other rows built them.
  doc::Document document;
  {
    const doc::Document& shared = market_document(records);
    for (u32 i = 0; i < shared.layer_count(); ++i) {
      doc::Layer layer(shared.layer(i).name(), shared.layer(i).role());
      for (auto [id, record] : shared.layer(i).records())
        layer.set(record);
      document.add_layer(std::move(layer));
    }
  }
  World world;
  (void)world.driver.materialize(document);
  const u32 changed = records / 100u;
  u32 round = 0;
  while (state.keep_running()) {
    state.pause_timing();
    ++round;
    for (u32 k = 0; k < changed; ++k) {
      u32 n = (k * 97u + round) % records;
      if (n % (k_stalls_per_market + 1u) == 0) ++n;  // a stall, not a market
      (void)document.apply(
          doc::cmd_set(record_id(n), "sign", JsonValue(round % 2u == 0 ? "open" : "shut")), nullptr,
          nullptr);
    }
    state.resume_timing();
    const sim::MaterializeReport report = world.driver.materialize(document);
    bench::keep(report.updated);
  }
  state.set_items(changed);
}
