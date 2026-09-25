// npc capability (docs/subsystems/npc.md): residents through the materialization driver and the
// entity store's hook into a flecs world the engine's scheduler ticks. The routine's transitions on
// the wheel, the fast-forward executed and summarized, the summarizer's five conditions
// (docs/subsystems/sim.md), the same resident however and whenever it is materialized, promotion
// and demotion by the observer set, and residents that move between tiles.
#include <core/json/json.h>
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
