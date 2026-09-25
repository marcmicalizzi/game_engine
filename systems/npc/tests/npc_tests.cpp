// npc capability (docs/subsystems/npc.md): residents through the materialization driver and the
// entity store's hook into a flecs world the engine's scheduler ticks. The routine's transitions on
// the wheel, the fast-forward executed and summarized, the summarizer's five conditions
// (docs/subsystems/sim.md), the same resident however and whenever it is materialized, promotion
// and demotion by the observer set, and residents that move between tiles.
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <core/schema/materialize.h>
#include <domain/doc/document.h>
#include <domain/doc/partition.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/materialize.h>
#include <domain/ecs/scheduled_tick.h>
#include <domain/ecs/sim_world.h>
#include <domain/sim/materialize.h>
#include <systems/npc/generator.h>
#include <systems/npc/npc.h>
#include <systems/npc/routine.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <schemas/npc.h>
#include <schemas/world.h>

using namespace engine;
using namespace engine::npc;

namespace {

// One hertz: a tick is a game second, so a test walks game hours in thousands of ticks. The routine
// is in minutes and the wheel in microseconds; nothing here depends on the step.
constexpr u32 k_hz = 1;
constexpr u64 k_seed = 42;
// Monday 06:00, when the day workers are getting up.
constexpr i64 k_morning = 6 * 60 * k_us_per_minute;

GeneratorParams small_world(u32 residents, i64 time_us) {
  GeneratorParams p;
  p.seed = k_seed;
  p.residents = residents;
  p.min_x = -96.0;
  p.min_z = -64.0;
  p.max_x = 160.0;
  p.max_z = 96.0;
  p.time_us = time_us;
  return p;
}

struct Rig {
  ecs::SimWorld sim;
  sim::SimScheduler scheduler;
  ecs::RecordMaterializer records;
  NpcSystem npc;
  sim::Materializer driver;
  std::unique_ptr<ecs::ScheduledTick> tick;

  static ecs::SimWorldConfig world_config() {
    ecs::SimWorldConfig c;
    c.hz = k_hz;
    return c;
  }
  static sim::SimSchedulerConfig scheduler_config(i64 epoch) {
    sim::SimSchedulerConfig c;
    c.hz = k_hz;
    c.epoch = GameTime{epoch};
    return c;
  }
  static sim::MaterializeConfig driver_config() {
    sim::MaterializeConfig c;
    c.tier = 2;
    return c;
  }
  static NpcConfig npc_config(u32 lod_every) {
    NpcConfig c;
    c.world_seed = k_seed;
    c.lod_every = lod_every;
    return c;
  }

  explicit Rig(i64 epoch, u32 lod_every = 1, sim::EventSink next = {})
      : sim(world_config()),
        scheduler(scheduler_config(epoch)),
        records(sim.world()),
        npc(npc_config(lod_every)),
        driver(scheduler, driver_config()) {
    npc.install(sim, scheduler, next);
    scheduler.add_hooks(records.hooks());
    scheduler.add_hooks(npc.hooks());
    driver.set_target(records.target());
    tick = std::make_unique<ecs::ScheduledTick>(sim, scheduler);
  }
  ~Rig() {
    driver.dematerialize_all();
    tick.reset();
  }

  void run(u32 ticks) {
    for (u32 i = 0; i < ticks; ++i)
      tick->step();
  }

  const NpcState* state(const Id128& id) {
    const flecs::entity e = ecs::entity_for(sim.world(), id);
    return e.is_valid() ? e.try_get<NpcState>() : nullptr;
  }
  Vec3 transform(const Id128& id) {
    const flecs::entity e = ecs::entity_for(sim.world(), id);
    const world::Transform* t = e.is_valid() ? e.try_get<world::Transform>() : nullptr;
    return t != nullptr ? t->position : Vec3{};
  }
};

doc::Document make_document(const GeneratorParams& params) {
  doc::Document d;
  d.add_layer("base", doc::LayerRole::Base);
  const u32 layer = d.add_layer(generate_layer(params));
  d.set_edit_layer(layer);
  d.rebuild_index();
  return d;
}

// Everything the routine decided about a resident, as bytes: its point, its components' routine
// fields and the due time of its timer.
struct Snapshot {
  RoutinePoint point;
  ResidentState state = ResidentState::Sleeping;
  i64 next_event = 0;
  Vec3 anchor;
  bool held = false;
  bool timer_live = false;
};

Snapshot snapshot(Rig& rig, const Id128& id) {
  Snapshot s;
  ResidentView view;
  s.held = rig.npc.find(id, view);
  if (!s.held) return s;
  s.point = view.point;
  s.timer_live = view.timer_live;
  if (const NpcState* state = rig.state(id)) {
    s.state = state->state;
    s.next_event = state->next_event;
    s.anchor = state->anchor;
  }
  return s;
}

bool same(const Snapshot& a, const Snapshot& b) {
  return a.held == b.held && std::memcmp(&a.point, &b.point, sizeof(RoutinePoint)) == 0 &&
         a.state == b.state && a.next_event == b.next_event && a.anchor == b.anchor &&
         a.timer_live == b.timer_live;
}

// The closed form, as the resident's components should hold it at `t`.
void check_closed_form(Rig& rig, const Id128& id, Routine routine, i64 t) {
  const RoutinePoint expected = routine_at(draw_variation(routine, k_seed, id), t);
  ResidentView view;
  REQUIRE(rig.npc.find(id, view));
  CHECK(std::memcmp(&view.point, &expected, sizeof(RoutinePoint)) == 0);
  const NpcState* state = rig.state(id);
  REQUIRE(state != nullptr);
  CHECK(state->state == expected.state);
  CHECK(state->next_event == expected.end_us);
  CHECK(state->next_event > t);
  CHECK(view.timer_live);
}

Routine routine_of(Rig& rig, const Id128& id) {
  const flecs::entity e = ecs::entity_for(rig.sim.world(), id);
  const NpcRoutine* r = e.try_get<NpcRoutine>();
  return r != nullptr ? r->routine : Routine::Idle;
}

// A write-back sink that applies the batch to a document, as engine-host's commits to a session.
struct DocumentSink {
  doc::Document* document = nullptr;
  u32 commits = 0;
  static bool commit(void* context, const sim::WriteBackBatch& batch) {
    auto* self = static_cast<DocumentSink*>(context);
    doc::Transaction transaction = self->document->begin(batch.attribution);
    for (const doc::Command& command : batch.commands) {
      if (!transaction.apply(command)) return false;
    }
    ++self->commits;
    return transaction.commit();
  }
};

}  // namespace

TEST_CASE("npc: residents materialize where their routine has them, and nothing needs writing") {
  const GeneratorParams params = small_world(200, k_morning);
  doc::Document d = make_document(params);
  Rig rig(k_morning);
  CHECK(rig.npc.places().refresh(d) == resolved(params).homes + resolved(params).workplaces +
                                           resolved(params).services + resolved(params).leisure);
  const sim::MaterializeReport report = rig.driver.materialize(d);
  CHECK(rig.npc.residents() == 200);
  CHECK(report.skipped == 0);
  for (u32 i = 0; i < 200; ++i) {
    const Id128 id = resident_id(k_seed, i);
    check_closed_form(rig, id, routine_of(rig, id), k_morning);
  }
  // The generator wrote the closed form at the same instant, so the world agrees with its document.
  DocumentSink sink{&d};
  rig.driver.set_writeback_sink(sim::WriteBackSink{&sink, &DocumentSink::commit});
  CHECK(rig.driver.flush_writeback(rig.scheduler.tick(), rig.scheduler.game_time()) == 0);
  CHECK(rig.npc.stats().materialized == 200);
  // A place is a transform and what it is for; it is not a resident.
  const flecs::entity place = ecs::entity_for(rig.sim.world(), place_id(k_seed, 0));
  REQUIRE(place.is_valid());
  CHECK(place.try_get<NpcPlace>() != nullptr);
}

TEST_CASE("npc: the wheel moves every resident on at its transitions, and the document follows") {
  const GeneratorParams params = small_world(120, k_morning);
  doc::Document d = make_document(params);
  Rig rig(k_morning);
  rig.npc.places().refresh(d);
  DocumentSink sink{&d};
  rig.driver.set_writeback_sink(sim::WriteBackSink{&sink, &DocumentSink::commit});
  rig.driver.set_writeback_every(60);
  rig.driver.install_writeback();
  rig.driver.materialize(d);
  // Three game hours: the day workers get up, travel and start work.
  rig.run(3 * 3600);
  const i64 now = rig.scheduler.game_time().us;
  CHECK(now == k_morning + 3 * 3600 * i64{1'000'000});
  CHECK(rig.npc.stats().transitions > 120);
  for (u32 i = 0; i < 120; ++i) {
    const Id128 id = resident_id(k_seed, i);
    check_closed_form(rig, id, routine_of(rig, id), now);
  }
  // What the wheel decided is in the document at the last Persist: state, next event, and the
  // anchor as the record's position.
  CHECK(sink.commits > 0);
  rig.driver.flush_writeback(rig.scheduler.tick(), rig.scheduler.game_time());
  for (u32 i = 0; i < 120; ++i) {
    const Id128 id = resident_id(k_seed, i);
    const NpcState* state = rig.state(id);
    REQUIRE(state != nullptr);
    i64 next = 0;
    REQUIRE(d.property(id, "next_event") != nullptr);
    CHECK(d.property(id, "next_event")->get_i64(next));
    CHECK(next == state->next_event);
    f64 x = 0.0;
    CHECK((*d.property(id, "position"))[0].get_f64(x));
    CHECK(static_cast<f32>(x) == state->anchor.x);
  }
}

TEST_CASE("npc: a resident is the same however, whenever and in whatever order it materializes") {
  const GeneratorParams params = small_world(150, k_morning);
  const u32 n = 150;
  // (a) whole, at the morning; (b) tile by tile in reverse tile order; (c) twice over;
  // (d) from the same stale document at noon, against (e) the morning run on to noon.
  doc::Document d = make_document(params);
  Rig a(k_morning);
  a.npc.places().refresh(d);
  a.driver.materialize(d);

  Rig b(k_morning);
  b.npc.places().refresh(d);
  Vector<doc::TileCoord> tiles;
  const doc::Layer& layer = d.layer(static_cast<u32>(d.find_layer("residents")));
  for (u32 i = 0; i < layer.records().size(); ++i) {
    doc::TileCoord t;
    if (!doc::tile_of(layer.records().value_at(i), layer.partition(), t)) continue;
    if (std::find(tiles.begin(), tiles.end(), t) == tiles.end()) tiles.push_back(t);
  }
  std::sort(tiles.begin(), tiles.end());
  CHECK(tiles.size() > 4);
  for (usize i = tiles.size(); i-- > 0;)
    b.driver.materialize(d, sim::MaterializeScope::of_tile(tiles[static_cast<u32>(i)]));
  CHECK(b.npc.residents() == n);

  Rig c(k_morning);
  c.npc.places().refresh(d);
  c.driver.materialize(d);
  c.driver.dematerialize_all();
  CHECK(c.npc.residents() == 0);
  c.driver.materialize(d);

  for (u32 i = 0; i < n; ++i) {
    const Id128 id = resident_id(k_seed, i);
    CAPTURE(i);
    CHECK(same(snapshot(a, id), snapshot(b, id)));
    CHECK(same(snapshot(a, id), snapshot(c, id)));
  }

  constexpr u32 k_ticks = 6 * 3600;
  Rig later(k_morning + i64{k_ticks} * 1'000'000);
  later.npc.places().refresh(d);
  later.driver.materialize(d);  // the document still says 06:00
  a.run(k_ticks);
  CHECK(a.scheduler.game_time() == later.scheduler.game_time());
  for (u32 i = 0; i < n; ++i) {
    const Id128 id = resident_id(k_seed, i);
    CAPTURE(i);
    CHECK(same(snapshot(a, id), snapshot(later, id)));
  }
}

TEST_CASE("npc: a game week executed under a large budget and summarized under a small one agree") {
  const GeneratorParams params = small_world(100, k_morning);
  doc::Document d = make_document(params);
  const GameTime week{k_morning + 7 * k_day_us};

  Rig executed(k_morning);
  executed.npc.places().refresh(d);
  executed.driver.materialize(d);
  const sim::FastForwardResult run = executed.npc.fast_forward(week, 10'000'000);
  CHECK_FALSE(run.over_budget);
  CHECK(run.summarized == 0);
  CHECK(run.delivered > 100 * 7 * 5);
  CHECK(executed.npc.stats().transitions == run.delivered);

  Rig summarized(k_morning);
  summarized.npc.places().refresh(d);
  summarized.driver.materialize(d);
  const sim::FastForwardResult skip = summarized.npc.fast_forward(week, 16);
  CHECK(skip.over_budget);
  CHECK(skip.summarized == 1);
  // Condition 4: nothing of the residents is delivered from inside the gap.
  CHECK(skip.delivered == 0);
  CHECK(summarized.npc.stats().transitions == 0);
  CHECK(summarized.npc.stats().summarized == 100);

  for (u32 i = 0; i < 100; ++i) {
    const Id128 id = resident_id(k_seed, i);
    CAPTURE(i);
    CHECK(same(snapshot(executed, id), snapshot(summarized, id)));
    check_closed_form(summarized, id, routine_of(summarized, id), week.us);
  }
}

TEST_CASE("npc: the summarizer meets sim.md's five conditions") {
  const GeneratorParams params = small_world(80, k_morning);
  doc::Document d = make_document(params);
  const GameTime a{k_morning};
  const GameTime b{k_morning + 2 * k_day_us + 17 * k_us_per_minute};
  const GameTime c{k_morning + 9 * k_day_us + 301 * k_us_per_minute};

  // 3. Idempotence over a partition: (a, b] then (b, c] is (a, c].
  Rig split(k_morning);
  split.npc.places().refresh(d);
  split.driver.materialize(d);
  split.npc.summarize(a, b);
  split.npc.summarize(b, c);
  Rig whole(k_morning);
  whole.npc.places().refresh(d);
  whole.driver.materialize(d);
  whole.npc.summarize(a, c);
  for (u32 i = 0; i < 80; ++i) {
    const Id128 id = resident_id(k_seed, i);
    CAPTURE(i);
    CHECK(same(snapshot(split, id), snapshot(whole, id)));
    // 1. Equivalence, exact: the closed form at c, which is what executing gets to (above).
    check_closed_form(whole, id, routine_of(whole, id), c.us);
  }

  // 2. Determinism from stated inputs: a rig built the other way round — materialized tile by
  // tile, with a different world before it on the wheel — summarizes to the same bytes.
  Rig other(k_morning, 1);
  other.npc.places().refresh(d);
  sim::TimerPayload noise;
  noise.kind = 99;
  for (u32 i = 0; i < 50; ++i)
    other.scheduler.wheel().schedule(GameTime{k_morning + i * 7919 * i64{1'000'000}}, noise);
  const doc::Layer& layer = d.layer(static_cast<u32>(d.find_layer("residents")));
  for (u32 i = layer.records().size(); i-- > 0;) {
    doc::TileCoord t;
    if (doc::tile_of(layer.records().value_at(i), layer.partition(), t))
      other.driver.materialize(d, sim::MaterializeScope::of_tile(t));
  }
  other.npc.summarize(a, c);
  for (u32 i = 0; i < 80; ++i) {
    const Id128 id = resident_id(k_seed, i);
    CHECK(same(snapshot(other, id), snapshot(whole, id)));
  }

  // 5. Bounded cost: a summary visits every held resident once, whatever the gap — a day or a
  // decade — and never a skipped transition.
  Rig day(k_morning);
  day.npc.places().refresh(d);
  day.driver.materialize(d);
  CHECK(day.npc.summarize(a, GameTime{a.us + k_day_us}) == 80);
  Rig decade(k_morning);
  decade.npc.places().refresh(d);
  decade.driver.materialize(d);
  CHECK(decade.npc.summarize(a, GameTime{a.us + 3650 * k_day_us}) == 80);
  CHECK(day.npc.stats().summary_visits == decade.npc.stats().summary_visits);
  CHECK(decade.npc.stats().summarized <= 80);

  // 4. No events out: every resident's timer is past `to` after a summary, and the wheel has
  // nothing of theirs to deliver inside the gap.
  CHECK(decade.scheduler.wheel().count_due(GameTime{a.us + 3650 * k_day_us}, 1'000'000) == 0);
}

TEST_CASE("npc: the fast-forward routes other timers on and never summarizes them away") {
  u32 foreign = 0;
  auto counter = [&foreign](const sim::TimerEvent& event) {
    if (event.payload.kind == 7) ++foreign;
  };
  const GeneratorParams params = small_world(40, k_morning);
  doc::Document d = make_document(params);
  Rig rig(k_morning, 1, sim::make_sink(counter));
  rig.npc.places().refresh(d);
  rig.driver.materialize(d);
  sim::TimerPayload payload;
  payload.kind = 7;
  rig.scheduler.wheel().schedule(GameTime{k_morning + 3600 * i64{1'000'000}}, payload);
  const sim::FastForwardResult r = rig.npc.fast_forward(GameTime{k_morning + 3 * k_day_us}, 4);
  CHECK(r.over_budget);
  CHECK(r.delivered == 1);  // the one-shot that is not a resident's: it has no coarse form
  CHECK(foreign == 1);
}

TEST_CASE("npc: the observer set promotes and demotes residents, and a tier changes nothing true") {
  const GeneratorParams params = small_world(200, 8 * 60 * k_us_per_minute + 5 * k_us_per_minute);
  doc::Document d = make_document(params);
  const i64 t0 = params.time_us;
  Rig observed(t0, 1);
  Rig unobserved(t0, 1);
  for (Rig* rig : {&observed, &unobserved}) {
    rig->npc.places().refresh(d);
    rig->driver.materialize(d);
  }
  // Someone on their way somewhere, and an observer standing at where they are going.
  Id128 traveller;
  for (u32 i = 0; i < 200 && traveller.is_null(); ++i) {
    ResidentView view;
    REQUIRE(observed.npc.find(resident_id(k_seed, i), view));
    if (view.point.state == ResidentState::Travelling && view.point.end_us - t0 > 5 * 60'000'000)
      traveller = view.id;
  }
  REQUIRE_FALSE(traveller.is_null());
  ResidentView start;
  REQUIRE(observed.npc.find(traveller, start));
  // Materialized with observers present: at LOD2 (05 §5.5 step 4), drawn at its destination.
  sim::ObserverSet observers;
  observers.add(start.anchor, 1.0f);
  observed.npc.set_observers(&observers);
  observed.driver.materialize(d);  // unchanged: nothing is re-materialized
  CHECK(start.tier == 2);
  CHECK(start.drawn == start.anchor);

  observed.run(2);
  unobserved.run(2);
  ResidentView near;
  REQUIRE(observed.npc.find(traveller, near));
  CHECK(near.tier == 0);
  // Near, a trip is drawn on its segment by elapsed fraction; the anchor is unchanged.
  CHECK(near.anchor == start.anchor);
  CHECK_FALSE(near.drawn == near.anchor);
  CHECK(observed.transform(traveller) == near.drawn);
  u32 counts[4] = {};
  observed.npc.tier_counts(counts);
  CHECK(counts[0] + counts[1] > 0);
  CHECK(counts[2] > 0);
  CHECK(counts[3] == 0);
  CHECK(observed.npc.stats().promotions > 0);
  // A still observer and settled tiers: the next passes change (almost) nothing — a trip that
  // crosses a band still may — so the hooks are not handed the same changes again.
  observed.run(20);  // past the rate limits' 64 a pass
  unobserved.run(20);
  const u64 promoted = observed.npc.stats().promotions;
  const u64 demoted = observed.npc.stats().demotions;
  observed.run(10);
  unobserved.run(10);
  CHECK(observed.npc.stats().promotions + observed.npc.stats().demotions - promoted - demoted < 10);

  // The observer goes: everything demotes, the trip is drawn at its destination again.
  observers.clear();
  observers.add(Vec3{10'000.0f, 0.0f, 10'000.0f}, 1.0f);
  observed.run(30);
  unobserved.run(30);
  ResidentView far;
  REQUIRE(observed.npc.find(traveller, far));
  CHECK(far.tier == 2);
  CHECK(observed.npc.stats().demotions > 0);
  if (far.point.state == ResidentState::Travelling) CHECK(far.drawn == far.anchor);

  // What is true — the point, the state, the next event, the anchor — is the same observed or not.
  for (u32 i = 0; i < 200; ++i) {
    const Id128 id = resident_id(k_seed, i);
    CHECK(same(snapshot(observed, id), snapshot(unobserved, id)));
  }
}

TEST_CASE("npc: a resident that walks into another tile is reported, refiled or let go") {
  // Tile by tile, with a write-back that commits to the document: a resident whose anchor moved to
  // a place in another tile is a record that moved (docs/subsystems/world.md, "Records that move").
  const GeneratorParams params = small_world(150, k_morning);
  doc::Document d = make_document(params);
  Rig rig(k_morning);
  rig.npc.places().refresh(d);
  DocumentSink sink{&d};
  rig.driver.set_writeback_sink(sim::WriteBackSink{&sink, &DocumentSink::commit});
  rig.driver.set_writeback_every(60);
  rig.driver.install_writeback();
  const doc::Layer& layer = d.layer(static_cast<u32>(d.find_layer("residents")));
  Vector<doc::TileCoord> tiles;
  for (u32 i = 0; i < layer.records().size(); ++i) {
    doc::TileCoord t;
    if (doc::tile_of(layer.records().value_at(i), layer.partition(), t) &&
        std::find(tiles.begin(), tiles.end(), t) == tiles.end())
      tiles.push_back(t);
  }
  for (const doc::TileCoord& t : tiles)
    rig.driver.materialize(d, sim::MaterializeScope::of_tile(t));
  REQUIRE(rig.npc.residents() == 150);

  rig.run(4 * 3600);  // to 10:00: the workers are at work, most of them in another tile
  rig.driver.flush_writeback(rig.scheduler.tick(), rig.scheduler.game_time());
  Vector<sim::TileMove> moved;
  rig.driver.take_moved(moved);
  CHECK(moved.size() > 10);
  // Half are refiled (their new tile is live), half let go (it is not).
  u32 kept = 0;
  Vector<Id128> gone;
  for (usize i = 0; i < moved.size(); ++i) {
    const sim::TileMove& m = moved[static_cast<u32>(i)];
    CHECK(m.to_tiled);
    if (i % 2 == 0) {
      CHECK(rig.driver.refile(m.id, true, m.to));
      ++kept;
    } else {
      gone.push_back(m.id);
    }
  }
  CHECK(rig.driver.dematerialize(std::span<const Id128>(gone.data(), gone.size())) == gone.size());
  CHECK(rig.npc.residents() == 150 - gone.size());
  // A refiled resident is held under the tile its document now puts it in.
  const sim::TileMove& refiled = moved[0];
  Vector<Id128> held;
  rig.driver.held(sim::MaterializeScope::of_tile(refiled.to), held);
  CHECK(std::find(held.begin(), held.end(), refiled.id) != held.end());

  // A resident let go comes back when its tile does, where its routine has it by then.
  rig.run(1800);
  const sim::TileMove& back = moved[1];
  rig.driver.materialize(d, sim::MaterializeScope::of_tile(back.to));
  ResidentView view;
  REQUIRE(rig.npc.find(back.id, view));
  check_closed_form(rig, back.id, routine_of(rig, back.id), rig.scheduler.game_time().us);
  (void)kept;
}

TEST_CASE("npc: a record that is not a resident is left to the other hooks") {
  doc::Document d;
  d.add_layer("base", doc::LayerRole::Base);
  JsonValue props = JsonValue::object();
  REQUIRE(d.apply(doc::cmd_create(Id128::from_parts(1, 2), "engine.world.Node", Id128{}, props),
                  nullptr, nullptr));
  Rig rig(0);
  const sim::MaterializeReport report = rig.driver.materialize(d);
  CHECK(report.created == 1);
  CHECK(rig.npc.residents() == 0);
  CHECK(rig.npc.stats().materialized == 0);
}

TEST_CASE("npc: a place that moves moves the residents at it, found through the document") {
  const GeneratorParams params = small_world(60, k_morning);
  doc::Document d = make_document(params);
  Rig rig(k_morning);
  const u32 places = rig.npc.refresh_places(d);
  CHECK(places > 0);
  rig.driver.materialize(d);
  // Somebody at home, and their home moved ten metres east by an edit.
  Id128 who;
  ResidentView before;
  for (u32 i = 0; i < 60 && who.is_null(); ++i) {
    REQUIRE(rig.npc.find(resident_id(k_seed, i), before));
    if (before.point.place == PlaceRole::Home) who = before.id;
  }
  REQUIRE_FALSE(who.is_null());
  const flecs::entity e = ecs::entity_for(rig.sim.world(), who);
  const Id128 home = e.try_get<NpcRoutine>()->home;
  JsonValue position = JsonValue::array();
  position.push_back(JsonValue(static_cast<f64>(before.anchor.x) + 10.0));
  position.push_back(JsonValue(0.0));
  position.push_back(JsonValue(static_cast<f64>(before.anchor.z)));
  REQUIRE(d.apply(doc::cmd_set(home, "position", std::move(position)), nullptr, nullptr));
  const u64 generation = rig.npc.places().generation();
  CHECK(rig.npc.refresh_places(d) == places);
  CHECK(rig.npc.places().generation() != generation);
  ResidentView after;
  REQUIRE(rig.npc.find(who, after));
  CHECK(after.anchor.x == before.anchor.x + 10.0f);
  CHECK(rig.state(who)->anchor.x == after.anchor.x);
  // Nothing changed since: the same generation, nothing looked up again.
  CHECK(rig.npc.refresh_places(d) == places);
}

TEST_CASE("npc: a resident's clock offset puts game time 0 at any time of its day") {
  // A world whose clock starts at 0 but whose residents' day is at 07:00 — what a headless run that
  // starts at tick 0 needs to see anyone travel in its first game minutes.
  GeneratorParams params = small_world(100, 0);
  params.clock_offset_us = 7 * 60 * k_us_per_minute;
  doc::Document d = make_document(params);
  Rig rig(0);
  rig.npc.refresh_places(d);
  DocumentSink sink{&d};
  rig.driver.set_writeback_sink(sim::WriteBackSink{&sink, &DocumentSink::commit});
  rig.driver.materialize(d);
  // The generator wrote the same closed form on the same clock: nothing to write back.
  CHECK(rig.driver.flush_writeback(rig.scheduler.tick(), rig.scheduler.game_time()) == 0);
  u32 awake = 0;
  for (u32 i = 0; i < 100; ++i) {
    const Id128 id = resident_id(k_seed, i);
    ResidentView view;
    REQUIRE(rig.npc.find(id, view));
    const RoutinePoint expected = routine_at_offset(draw_variation(routine_of(rig, id), k_seed, id),
                                                    0, params.clock_offset_us);
    CHECK(std::memcmp(&view.point, &expected, sizeof(RoutinePoint)) == 0);
    CHECK(view.point.end_us > 0);
    awake += view.point.state != ResidentState::Sleeping ? 1u : 0u;
  }
  CHECK(awake > 10);
  rig.run(1800);
  CHECK(rig.npc.stats().transitions > 0);
}

// ---- the schedule index (docs/subsystems/npc.md, "The schedule index") ------------------------

namespace {

constexpr f64 k_tile_size = 32.0;  // GeneratorParams' default grid
constexpr i64 k_hour = 60 * k_us_per_minute;

bool tile_of_vec(Vec3 at, doc::TileCoord& out) {
  JsonValue position = JsonValue::array();
  position.push_back(JsonValue(static_cast<f64>(at.x)));
  position.push_back(JsonValue(static_cast<f64>(at.y)));
  position.push_back(JsonValue(static_cast<f64>(at.z)));
  return doc::tile_of_position(position, k_tile_size, out);
}

// A resident as the test reads it from the document, independently of the capability: its routine
// and its four places' tiles (a role with no place has none).
struct Reading {
  Routine routine = Routine::Idle;
  Id128 places[4];
  doc::TileCoord tiles[4];
  bool tiled[4] = {};
};

Reading read_resident(const doc::Document& d, const PlaceIndex& places, const Id128& id) {
  Reading r;
  schema::ReadContext ctx;
  if (const JsonValue* v = d.property(id, "routine")) (void)schema::from_json(r.routine, *v, ctx);
  const char* const keys[4] = {"home", "job", "service", "leisure"};
  for (u32 k = 0; k < 4; ++k) {
    std::string_view hex;
    const JsonValue* v = d.property(id, keys[k]);
    if (v == nullptr || !v->get_string(hex) || !Id128::from_hex(hex, r.places[k])) continue;
    const u32 p = places.find(r.places[k]);
    if (p != k_no_place) r.tiled[k] = tile_of_vec(places.position(p), r.tiles[k]);
  }
  return r;
}

// Where the routine has a resident at `t`: the tile of its row's place. The closed form the
// capability uses, called here on the test's own reading of the document.
bool anchor_at(const Reading& r, const Id128& id, i64 t, doc::TileCoord& out) {
  const RoutinePoint p = routine_at(draw_variation(r.routine, k_seed, id), t);
  const u32 role = static_cast<u32>(p.place) & 3u;
  if (!r.tiled[role]) return false;
  out = r.tiles[role];
  return true;
}

// Every tile that holds a record of the layer, sorted: the tiles a world could activate.
Vector<doc::TileCoord> occupied(const doc::Document& d) {
  Vector<doc::TileCoord> tiles;
  for (const Id128& id : d.objects()) {
    doc::TileCoord t;
    if (d.object_tile(id, t) && std::find(tiles.begin(), tiles.end(), t) == tiles.end())
      tiles.push_back(t);
  }
  std::sort(tiles.begin(), tiles.end());
  return tiles;
}

// What engine-host's document consumer does between ticks (world.md, `DocumentTiles::settle`),
// against the driver's own live tiles: arrivals in, then the records a write-back moved refiled
// under a live tile or let go.
void settle(Rig& rig, const doc::Document& d) {
  rig.driver.materialize_arrivals(d);
  Vector<sim::TileMove> moves;
  rig.driver.take_moved(moves);
  Vector<Id128> gone;
  for (const sim::TileMove& m : moves) {
    if (m.to_tiled && rig.driver.tile_live(m.to)) {
      rig.driver.refile(m.id, true, m.to);
    } else {
      gone.push_back(m.id);
    }
  }
  rig.driver.dematerialize(std::span<const Id128>(gone.data(), gone.size()));
}

void run_settled(Rig& rig, const doc::Document& d, u32 ticks) {
  for (u32 i = 0; i < ticks; ++i) {
    rig.tick->step();
    settle(rig, d);
  }
}

// The residents a driver holds, sorted.
Vector<Id128> held_residents(Rig& rig, u32 n) {
  Vector<Id128> out;
  for (u32 i = 0; i < n; ++i) {
    if (rig.driver.holds(resident_id(k_seed, i))) out.push_back(resident_id(k_seed, i));
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

TEST_CASE("npc: the schedule index lists the residents whose routines visit a tile") {
  const u32 n = 150;
  doc::Document d = make_document(small_world(n, k_morning));
  Rig rig(k_morning);
  rig.npc.refresh_places(d);
  CHECK(rig.npc.scheduled() == n);
  CHECK(rig.npc.schedule_stats().builds == 1);

  // Every occupied tile's list against the test's own reading of every resident's four places.
  auto check_lists = [&]() {
    u32 listed = 0;
    for (const doc::TileCoord& t : occupied(d)) {
      Vector<Id128> want;
      for (const Id128& id : d.objects()) {
        if (d.type_of(id) != k_resident_type) continue;
        const Reading r = read_resident(d, rig.npc.places(), id);
        for (u32 k = 0; k < 4; ++k) {
          if (r.tiled[k] && r.tiles[k] == t) {
            want.push_back(id);
            break;
          }
        }
      }
      std::sort(want.begin(), want.end());
      Vector<Id128> have;
      rig.npc.visiting(t, have);
      CAPTURE(t.x);
      CAPTURE(t.y);
      CHECK(have == want);
      listed += have.size();
    }
    return listed;
  };
  CHECK(check_lists() > n);  // a resident's places are in more than one tile

  // A resident given another home: read again from the change feed, not rebuilt.
  const Id128 who = resident_id(k_seed, 3);
  const Id128 elsewhere = place_id(k_seed, 7);
  char hex[33];
  elsewhere.to_hex(hex);
  REQUIRE(d.apply(doc::cmd_set(who, "home", JsonValue(std::string(hex, 32))), nullptr, nullptr));
  // A resident removed from the document.
  REQUIRE(d.apply(doc::cmd_delete(resident_id(k_seed, 4)), nullptr, nullptr));
  rig.npc.refresh_places(d);
  CHECK(rig.npc.schedule_stats().builds == 1);
  CHECK(rig.npc.schedule_stats().updates >= 2);
  CHECK(rig.npc.scheduled() == n - 1);
  check_lists();

  // A place moved into another tile: the tiles of every resident change, so the index is built
  // again.
  const Id128 place = place_id(k_seed, 0);
  JsonValue far = JsonValue::array();
  far.push_back(JsonValue(155.25));
  far.push_back(JsonValue(0.0));
  far.push_back(JsonValue(90.25));
  REQUIRE(d.apply(doc::cmd_set(place, "position", std::move(far)), nullptr, nullptr));
  rig.npc.refresh_places(d);
  CHECK(rig.npc.schedule_stats().builds == 2);
  check_lists();
  CHECK(rig.npc.bytes_scheduled() >= u64{n - 1} * sizeof(ScheduledResident));
}

TEST_CASE(
    "npc: a resident whose routine brings it into a live tile comes in, wherever its record is") {
  // The boundary npc.md used to state: a day worker who walked to work in a tile the ring does not
  // simulate stayed there, a record, even when its routine had it home again in a live tile. With
  // the schedule index it is watched while it is away and comes home with its routine.
  const u32 n = 150;
  const GeneratorParams params = small_world(n, k_morning);

  // A day worker whose job is in another tile than its home, read from a scratch document.
  Id128 who;
  doc::TileCoord home;
  doc::TileCoord job;
  {
    doc::Document d = make_document(params);
    Rig probe(k_morning);
    probe.npc.refresh_places(d);
    for (u32 i = 0; i < n && who.is_null(); ++i) {
      const Id128 id = resident_id(k_seed, i);
      const Reading r = read_resident(d, probe.npc.places(), id);
      if (r.routine != Routine::DayWorker || !r.tiled[0] || !r.tiled[1]) continue;
      if (r.tiles[0] == r.tiles[1]) continue;
      // Nobody else's home in the job's tile matters; the job's tile is the one left out.
      who = id;
      home = r.tiles[0];
      job = r.tiles[1];
    }
  }
  REQUIRE_FALSE(who.is_null());

  // Two worlds on every occupied tile but the job's, one with the schedule index and one without.
  auto make = [&](bool with_index, doc::Document& d, DocumentSink& sink) {
    auto rig = std::make_unique<Rig>(k_morning);
    if (with_index) rig->driver.add_tile_source(rig->npc.tile_source());
    rig->npc.refresh_places(d);
    rig->driver.set_writeback_sink(sim::WriteBackSink{&sink, &DocumentSink::commit});
    rig->driver.set_writeback_every(60);
    rig->driver.install_writeback();
    for (const doc::TileCoord& t : occupied(d)) {
      if (!(t == job)) rig->driver.materialize(d, sim::MaterializeScope::of_tile(t));
    }
    return rig;
  };
  doc::Document with_doc = make_document(params);
  doc::Document without_doc = make_document(params);
  DocumentSink with_sink{&with_doc};
  DocumentSink without_sink{&without_doc};
  std::unique_ptr<Rig> with = make(true, with_doc, with_sink);
  std::unique_ptr<Rig> without = make(false, without_doc, without_sink);
  REQUIRE(with->driver.holds(who));  // 06:00: asleep at home, in a live tile
  REQUIRE(without->driver.holds(who));

  // To 11:00: at work, in the tile nobody simulates, and let go by both.
  run_settled(*with, with_doc, 5 * 3600);
  run_settled(*without, without_doc, 5 * 3600);
  CHECK_FALSE(with->driver.holds(who));
  CHECK_FALSE(without->driver.holds(who));
  CHECK(with->npc.watching() > 0);
  CHECK(with->npc.schedule_stats().watches > 0);

  // To 21:00: its routine has it home. Only the world with the index has it.
  run_settled(*with, with_doc, 10 * 3600);
  run_settled(*without, without_doc, 10 * 3600);
  CHECK(with->npc.schedule_stats().arrivals > 0);
  const i64 now = with->scheduler.game_time().us;
  const Reading r = read_resident(with_doc, with->npc.places(), who);
  doc::TileCoord at;
  REQUIRE(anchor_at(r, who, now, at));
  REQUIRE(at == home);
  CHECK(with->driver.holds(who));
  CHECK_FALSE(without->driver.holds(who));
  check_closed_form(*with, who, Routine::DayWorker, now);
  // And its record follows it home at the write-back, filed where the driver holds it.
  with->driver.flush_writeback(with->scheduler.tick(), with->scheduler.game_time());
  settle(*with, with_doc);
  doc::TileCoord filed;
  REQUIRE(with_doc.object_tile(who, filed));
  CHECK(filed == home);
  Vector<Id128> in_home;
  with->driver.held(sim::MaterializeScope::of_tile(home), in_home);
  CHECK(std::find(in_home.begin(), in_home.end(), who) != in_home.end());

  // Every resident the world with the index holds is one its routine has in a live tile, and every
  // resident its routine has in a live tile is held: the rule, over the whole population.
  for (u32 i = 0; i < n; ++i) {
    const Id128 id = resident_id(k_seed, i);
    const Reading ri = read_resident(with_doc, with->npc.places(), id);
    doc::TileCoord a;
    const bool in_live = anchor_at(ri, id, now, a) && with->driver.tile_live(a);
    CAPTURE(i);
    CHECK(with->driver.holds(id) == in_live);
  }
}

TEST_CASE("npc: what the tiles bring in does not depend on the order they came in") {
  // The document says 06:00 and the world is at noon, so most residents' routines have them away
  // from the tile their record is in. A set of tiles activated in five orders, then one of them let
  // go, then the write-back and a settle: every order must hold the same residents, filed under the
  // same tiles, in the same state.
  //
  // The rule (npc.md): after the passes, a resident is held when its record's tile or the tile its
  // routine has it in is live, and filed under the second when that is live, the first otherwise;
  // after the write-back, exactly the residents whose routines have them in a live tile are held.
  const u32 n = 150;
  const GeneratorParams params = small_world(n, k_morning);
  constexpr i64 k_noon = 12 * k_hour;
  doc::Document probe_doc = make_document(params);
  const Vector<doc::TileCoord> all = occupied(probe_doc);
  // Every other tile of the checkerboard, and one more, left out of it so that it goes later.
  Vector<doc::TileCoord> live;
  for (const doc::TileCoord& t : all) {
    if (((t.x + t.y) & 1) == 0) live.push_back(t);
  }
  REQUIRE(live.size() > 6);
  const doc::TileCoord leaving = live[live.size() / 2];

  struct Outcome {
    Vector<Id128> held;
    Vector<Vector<Id128>> filed;  // per live tile, in `live` order
    Vector<Snapshot> states;
    Vector<Id128> after_leave;
    Vector<Id128> after_settle;
    u32 watching = 0;
  };
  u64 rng = 0x0bde'4001'0000'0001ull;
  auto shuffled = [&](Vector<doc::TileCoord> tiles) {
    for (u32 i = tiles.size(); i > 1; --i) {
      rng = rng * 6364136223846793005ull + 1442695040888963407ull;
      const u32 j = static_cast<u32>((rng >> 33) % i);
      std::swap(tiles[i - 1], tiles[j]);
    }
    return tiles;
  };

  Vector<Outcome> outcomes;
  for (u32 order = 0; order < 5; ++order) {
    doc::Document d = make_document(params);
    DocumentSink sink{&d};
    Rig rig(k_noon);
    rig.driver.add_tile_source(rig.npc.tile_source());
    rig.npc.refresh_places(d);
    rig.driver.set_writeback_sink(sim::WriteBackSink{&sink, &DocumentSink::commit});
    const Vector<doc::TileCoord> sequence = order == 0 ? live : shuffled(live);
    for (const doc::TileCoord& t : sequence)
      rig.driver.materialize(d, sim::MaterializeScope::of_tile(t));

    Outcome o;
    o.held = held_residents(rig, n);
    for (const doc::TileCoord& t : live) {
      Vector<Id128> filed;
      rig.driver.held(sim::MaterializeScope::of_tile(t), filed);
      o.filed.push_back(std::move(filed));
    }
    for (const Id128& id : o.held)
      o.states.push_back(snapshot(rig, id));
    o.watching = rig.npc.watching();

    // The rule itself, against the test's own reading of the document and the closed form.
    if (order == 0) {
      auto is_live = [&](doc::TileCoord t) {
        return std::find(live.begin(), live.end(), t) != live.end();
      };
      for (u32 i = 0; i < n; ++i) {
        const Id128 id = resident_id(k_seed, i);
        const Reading r = read_resident(d, rig.npc.places(), id);
        doc::TileCoord a;
        doc::TileCoord rec;
        const bool a_live = anchor_at(r, id, k_noon, a) && is_live(a);
        const bool r_live = d.object_tile(id, rec) && is_live(rec);
        CAPTURE(i);
        CHECK(rig.driver.holds(id) == (a_live || r_live));
        if (!rig.driver.holds(id)) continue;
        const doc::TileCoord want = a_live ? a : rec;
        Vector<Id128> filed;
        rig.driver.held(sim::MaterializeScope::of_tile(want), filed);
        CHECK(std::find(filed.begin(), filed.end(), id) != filed.end());
      }
    }

    rig.driver.dematerialize(sim::MaterializeScope::of_tile(leaving));
    o.after_leave = held_residents(rig, n);
    rig.driver.flush_writeback(rig.scheduler.tick(), rig.scheduler.game_time());
    settle(rig, d);
    o.after_settle = held_residents(rig, n);
    if (order == 0) {
      for (u32 i = 0; i < n; ++i) {
        const Id128 id = resident_id(k_seed, i);
        const Reading r = read_resident(d, rig.npc.places(), id);
        doc::TileCoord a;
        const bool a_live = anchor_at(r, id, k_noon, a) && rig.driver.tile_live(a);
        CAPTURE(i);
        CHECK(rig.driver.holds(id) == a_live);
      }
    }
    outcomes.push_back(std::move(o));
  }

  const Outcome& first = outcomes[0];
  CHECK(first.held.size() > 20);
  CHECK(first.after_settle.size() < first.held.size());
  for (u32 k = 1; k < outcomes.size(); ++k) {
    const Outcome& o = outcomes[k];
    CAPTURE(k);
    CHECK(o.held == first.held);
    CHECK(o.watching == first.watching);
    REQUIRE(o.filed.size() == first.filed.size());
    for (u32 t = 0; t < o.filed.size(); ++t)
      CHECK(o.filed[t] == first.filed[t]);
    REQUIRE(o.states.size() == first.states.size());
    for (u32 s = 0; s < o.states.size(); ++s)
      CHECK(same(o.states[s], first.states[s]));
    CHECK(o.after_leave == first.after_leave);
    CHECK(o.after_settle == first.after_settle);
  }
}

TEST_CASE(
    "npc: a world brought in from its written-back document holds what the running one does") {
  // What a load does (world.md, "Save and load"): the same tiles activated from the document the
  // run left, at the run's time. The running world got there through twelve game hours of
  // transitions, watches, arrivals and let-goes; the loaded one in one pass per tile, in another
  // order.
  const u32 n = 150;
  const GeneratorParams params = small_world(n, k_morning);
  doc::Document d = make_document(params);
  const Vector<doc::TileCoord> all = occupied(d);
  Vector<doc::TileCoord> live;
  for (const doc::TileCoord& t : all) {
    if (t.x < 2) live.push_back(t);  // the west of the square; the east is where many work
  }
  REQUIRE(live.size() < all.size());

  DocumentSink sink{&d};
  Rig running(k_morning);
  running.driver.add_tile_source(running.npc.tile_source());
  running.npc.refresh_places(d);
  running.driver.set_writeback_sink(sim::WriteBackSink{&sink, &DocumentSink::commit});
  running.driver.set_writeback_every(60);
  running.driver.install_writeback();
  for (const doc::TileCoord& t : live)
    running.driver.materialize(d, sim::MaterializeScope::of_tile(t));
  run_settled(running, d, 12 * 3600);
  running.driver.flush_writeback(running.scheduler.tick(), running.scheduler.game_time());
  settle(running, d);
  CHECK(running.npc.schedule_stats().arrivals > 0);
  CHECK(running.npc.stats().dematerialized > 0);

  Rig loaded(running.scheduler.game_time().us);
  loaded.driver.add_tile_source(loaded.npc.tile_source());
  loaded.npc.refresh_places(d);
  for (u32 i = live.size(); i-- > 0;)
    loaded.driver.materialize(d, sim::MaterializeScope::of_tile(live[i]));
  REQUIRE(loaded.scheduler.game_time() == running.scheduler.game_time());
  const Vector<Id128> held = held_residents(running, n);
  CHECK(held.size() > 10);
  CHECK(held_residents(loaded, n) == held);
  CHECK(loaded.npc.watching() == running.npc.watching());
  for (const Id128& id : held) {
    CAPTURE(id.lo);
    CHECK(same(snapshot(running, id), snapshot(loaded, id)));
  }
  for (const doc::TileCoord& t : live) {
    Vector<Id128> a;
    Vector<Id128> b;
    running.driver.held(sim::MaterializeScope::of_tile(t), a);
    loaded.driver.held(sim::MaterializeScope::of_tile(t), b);
    CHECK(a == b);
  }
}

TEST_CASE("npc: a summary looks at every watch once and fires none of them") {
  // A watch is a one-shot on the wheel, which a summary delivers rather than coarsens; the
  // capability's summarizer takes its watches with the residents it holds, so a fast-forward over a
  // day summarized delivers none of them and leaves the same residents watched as the executed day.
  const u32 n = 150;
  const GeneratorParams params = small_world(n, k_morning);
  const Vector<doc::TileCoord> all = occupied(make_document(params));
  u32 watching[2] = {};
  for (u32 path = 0; path < 2; ++path) {
    doc::Document d = make_document(params);
    Rig rig(k_morning);
    rig.driver.add_tile_source(rig.npc.tile_source());
    rig.npc.refresh_places(d);
    for (const doc::TileCoord& t : all) {
      if (t.x < 2) rig.driver.materialize(d, sim::MaterializeScope::of_tile(t));
    }
    // Everyone away from the live tiles at 06:00 is watched; the residents at home are held.
    const u64 wakes = rig.npc.schedule_stats().wakes;
    const sim::FastForwardResult r =
        rig.npc.fast_forward(GameTime{k_morning + 24 * k_hour}, path == 0 ? (u64{1} << 40) : 16);
    if (path == 1) {
      CHECK(r.summarized > 0);
      CHECK(rig.npc.schedule_stats().wakes == wakes);  // no watch fired inside the gap
    } else {
      CHECK(rig.npc.schedule_stats().wakes > wakes);
    }
    // Either way every watch was looked at by the end of the day and armed again past it.
    watching[path] = rig.npc.watching();
    CHECK(watching[path] > 0);
  }
  CHECK(watching[0] == watching[1]);
}
