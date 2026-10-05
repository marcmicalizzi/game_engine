// The entity store's hook (domain/ecs/materialize.h) under the driver (domain/sim/materialize.h):
// a document becomes entities keyed by its records' ids, the parent becomes `ChildOf`, later passes
// follow the document, and what systems change goes back.
#include <core/json/json.h>
#include <domain/doc/document.h>
#include <domain/ecs/components.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/materialize.h>
#include <domain/ecs/scheduled_tick.h>
#include <domain/ecs/sim_world.h>
#include <domain/ecs/systems.h>
#include <domain/sim/materialize.h>
#include <domain/sim/scheduler.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <schemas/ecs_demo.h>
#include <schemas/ecs_demo_ecs.h>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::ecs;

namespace {

const char* const k_stall = "engine.ecs.demo.Stall";
const char* const k_market = "engine.ecs.demo.Market";
const char* const k_note = "engine.ecs.demo.DemoNote";

Id128 rid(u64 n) { return Id128::from_parts(0xEC5, n); }

JsonValue vec3(f64 x, f64 y, f64 z) {
  JsonValue v = JsonValue::array();
  v.push_back(JsonValue(x));
  v.push_back(JsonValue(y));
  v.push_back(JsonValue(z));
  return v;
}

void create(doc::Document& d, u64 n, const char* type, u64 parent, JsonValue initial) {
  REQUIRE(d.apply(
      doc::cmd_create(rid(n), type, parent != 0 ? rid(parent) : Id128{}, std::move(initial)),
      nullptr, nullptr));
}

JsonValue stall(f64 x, const char* sign) {
  JsonValue p = JsonValue::object();
  p.set("position", vec3(x, 0, 0));
  p.set("sign", JsonValue(sign));
  return p;
}

//   M 30 Market          S2 20 Stall (root)
//   └ S1 10 Stall        N 5 DemoNote (document-only)
doc::Document market() {
  doc::Document d;
  d.add_layer("base", doc::LayerRole::Base);
  JsonValue m = JsonValue::object();
  m.set("position", vec3(100, 0, 0));
  create(d, 30, k_market, 0, std::move(m));
  create(d, 10, k_stall, 30, stall(101, "fish"));
  create(d, 20, k_stall, 0, stall(5, "bread"));
  JsonValue note = JsonValue::object();
  note.set("text", JsonValue("the market opens at dawn"));
  create(d, 5, k_note, 0, std::move(note));
  return d;
}

// A world with the demo components, the scheduler, the entity store's hook first in the table, and
// the driver bound to it — what a host assembles.
struct Rig {
  SimWorld world;
  sim::SimScheduler scheduler;
  RecordMaterializer records;
  sim::Materializer driver;

  Rig() : records(world.world()), driver(scheduler, config()) {
    demo::register_ecs_demo_components(world.world());
    scheduler.add_hooks(records.hooks());
    driver.set_target(records.target());
  }
  static sim::MaterializeConfig config() {
    sim::MaterializeConfig c;
    c.tier = 0;
    return c;
  }
  flecs::entity entity(u64 n) { return entity_for(world.world(), rid(n)); }
};

// FNV-1a over the materialized world in record-id order: each entity's id, its Standing bytes, its
// label and its parent's id. What two runs must agree on, and what a test pins.
u64 world_hash(Rig& rig, const std::vector<u64>& ids) {
  u64 hash = 1469598103934665603ull;
  const auto mix = [&hash](const void* data, usize size) {
    const auto* bytes = static_cast<const u8*>(data);
    for (usize i = 0; i < size; ++i) {
      hash ^= bytes[i];
      hash *= 1099511628211ull;
    }
  };
  for (const u64 n : ids) {
    const flecs::entity e = rig.entity(n);
    REQUIRE(e.is_valid());
    const Id128 id = rid(n);
    mix(&id, sizeof(id));
    const demo::Standing& s = e.get<demo::Standing>();
    mix(&s.position, sizeof(s.position));
    mix(&s.wealth, sizeof(s.wealth));
    mix(&s.importance, sizeof(s.importance));
    if (const demo::Label* label = e.try_get<demo::Label>())
      mix(label->text.data(), label->text.size());
    const flecs::entity target = e.target(flecs::ChildOf);
    const Id128 parent = target.is_valid() ? id_of(target) : Id128{};
    mix(&parent, sizeof(parent));
  }
  return hash;
}

struct DocumentSink {
  doc::Document* document = nullptr;
  u32 commits = 0;
  static bool commit(void* context, const sim::WriteBackBatch& batch) {
    auto* self = static_cast<DocumentSink*>(context);
    doc::Transaction t = self->document->begin(batch.attribution);
    for (const doc::Command& c : batch.commands) {
      if (!t.apply(c)) return false;
    }
    ++self->commits;
    return t.commit();
  }
};

}  // namespace

TEST_CASE("ecs: a document becomes entities keyed by its records' ids, parents as ChildOf") {
  doc::Document d = market();
  Rig rig;
  const sim::MaterializeReport report = rig.driver.materialize(d);
  CHECK(report.created == 3);
  CHECK(report.skipped == 1);  // the note: document-only by design
  CHECK(report.live == 3);
  CHECK(identity_map(rig.world.world()).size() == 3);
  CHECK(rig.records.stats().created == 3);

  const flecs::entity s1 = rig.entity(10);
  const flecs::entity m = rig.entity(30);
  REQUIRE(s1.is_valid());
  REQUIRE(m.is_valid());
  CHECK(id_of(s1) == rid(10));
  // The document's parent is the entity's ChildOf; a root stays a root.
  CHECK(s1.target(flecs::ChildOf) == m);
  CHECK_FALSE(rig.entity(20).target(flecs::ChildOf).is_valid());

  const demo::Standing& standing = s1.get<demo::Standing>();
  CHECK(standing.position == Vec3{101.0f, 0.0f, 0.0f});
  CHECK(standing.wealth == 100);                        // the record type's default
  CHECK(standing.importance == doctest::Approx(1.0f));  // 100 percent, as a ratio
  CHECK(s1.get<demo::Label>().text == "fish");
  // A market maps only its position: it has Standing and no Label.
  CHECK(m.has<demo::Standing>());
  CHECK_FALSE(m.has<demo::Label>());
  CHECK(rig.records.watched() == 2);  // the two stalls carry write-back rows
}

TEST_CASE("ecs: later passes update, dematerialize without cascading, and relink") {
  doc::Document d = market();
  Rig rig;
  rig.driver.materialize(d);
  const u64 calls = rig.driver.stats().hook_calls;

  // Unchanged: nothing is handed to the hook, and the hook writes nothing.
  const u64 writes = rig.records.stats().component_writes;
  rig.driver.materialize(d);
  CHECK(rig.driver.stats().hook_calls == calls);
  CHECK(rig.records.stats().component_writes == writes);

  // A changed property reaches its field; the fields no mapping names are left alone.
  rig.entity(10).get_mut<demo::Standing>().faction = 7;
  REQUIRE(d.apply(doc::cmd_set(rid(10), "sign", JsonValue("closed")), nullptr, nullptr));
  rig.driver.materialize(d);
  CHECK(rig.entity(10).get<demo::Label>().text == "closed");
  CHECK(rig.entity(10).get<demo::Standing>().faction == 7);

  // The market goes: its stall keeps its entity, moved to the root, not deleted with it.
  REQUIRE(d.apply(doc::cmd_delete(rid(30)), nullptr, nullptr));
  sim::MaterializeReport gone = rig.driver.materialize(d);
  CHECK(gone.dematerialized == 1);
  CHECK(gone.orphans == 1);
  CHECK_FALSE(rig.entity(30).is_valid());
  REQUIRE(rig.entity(10).is_valid());
  CHECK_FALSE(rig.entity(10).target(flecs::ChildOf).is_valid());
  CHECK(rig.records.stats().unlinked_children == 1);

  // And comes back: the stall is linked again.
  JsonValue m = JsonValue::object();
  m.set("position", vec3(100, 0, 0));
  REQUIRE(d.apply(doc::cmd_create(rid(30), k_market, Id128{}, std::move(m)), nullptr, nullptr));
  sim::MaterializeReport back = rig.driver.materialize(d);
  CHECK(back.created == 1);
  CHECK(back.relinked == 1);
  CHECK(back.orphans == 0);
  CHECK(rig.entity(10).target(flecs::ChildOf) == rig.entity(30));
}

TEST_CASE("ecs: materialization is the same entities in the same order every run") {
  std::vector<flecs::entity_t> handles[2];
  u64 hashes[2] = {};
  u64 orders[2] = {};
  for (u32 run = 0; run < 2; ++run) {
    doc::Document d = market();
    Rig rig;
    rig.driver.materialize(d);
    orders[run] = rig.driver.last_order_hash();
    hashes[run] = world_hash(rig, {10, 20, 30});
    // Even flecs' own ids agree, because the order of creation does.
    for (const u64 n : {10u, 20u, 30u})
      handles[run].push_back(rig.entity(n).id());
  }
  CHECK(orders[0] == orders[1]);
  CHECK(hashes[0] == hashes[1]);
  CHECK(handles[0] == handles[1]);
  // Pinned: the world a document makes is a function of the document.
  CHECK(hashes[0] == 6156574484176281038ull);
}

TEST_CASE("ecs: what a system changes goes back to the document at Persist, attributed to system") {
  doc::Document d = market();
  Rig rig;
  DocumentSink sink;
  sink.document = &d;
  rig.driver.set_writeback_sink(sim::WriteBackSink{&sink, &DocumentSink::commit});
  rig.driver.install_writeback();

  // A system that moves every stall a metre a tick, pays it, and repaints its sign. Position and
  // wealth are write-back rows; the sign is not, so it stays in the world.
  flecs::world& w = rig.world.world();
  sim::SystemDesc desc;
  desc.name = "test.drift";
  desc.phase = TickPhase::Systems;
  desc.writes = mask_of<demo::Standing, demo::Label>(w);
  register_system(rig.world, desc, [](flecs::world& world, flecs::entity phase) {
    return world.system<demo::Standing, demo::Label>("test.drift")
        .kind(phase)
        .each([](demo::Standing& s, demo::Label& l) {
          s.position.x += 1.0f;
          s.wealth += 5;
          l.text = "moved";
        });
  });
  rig.driver.materialize(d);
  const u32 journal = d.journal().size();

  ScheduledTick tick(rig.world, rig.scheduler);
  for (int i = 0; i < 3; ++i)
    tick.step();

  // One transaction a tick, each attributed to the system.
  CHECK(sink.commits == 3);
  REQUIRE(d.journal().size() == journal + 3);
  const doc::Patch& last = d.journal().back();
  CHECK(last.attribution.actor == "system");
  CHECK(last.attribution.task == "sim.write_back");
  CHECK(last.forward.size() == 4);  // two stalls, two writable fields each
  CHECK((*d.property(rid(10), "position"))[0].as_float() == doctest::Approx(104.0));
  CHECK(d.property(rid(20), "wealth")->as_int() == 115);
  // Not a write-back row: the document still says what it said.
  CHECK(d.property(rid(10), "sign")->as_string() == "fish");
  // The market has no system moving it and no write-back rows.
  CHECK((*d.property(rid(30), "position"))[0].as_float() == doctest::Approx(100.0));

  // The world wrote it, so the world is in sync with it: the next pass calls no hook.
  const u64 calls = rig.driver.stats().hook_calls;
  sim::MaterializeReport after = rig.driver.materialize(d);
  CHECK(after.unchanged == 2);
  CHECK(rig.driver.stats().hook_calls == calls);
  CHECK(rig.records.stats().writeback_changes == 12);
}

// ADR-0053: a `worldpos` row fills a `WorldPos` field with the record's double, exactly, at 420 km,
// 10,000 km and 1e8 m; a km row converts in f64 (the values are chosen so that ×1000 is exact);
// write-back writes nothing while nothing moved, and a 1/1024 m move comes back as the double it
// is.
TEST_CASE("ecs far: world positions materialize and write back exactly far from the origin") {
  constexpr f64 k_step = 1.0 / 1024.0;
  struct Site {
    WorldPos at;
    WorldPos survey_km;
  };
  const Site sites[] = {
      {{419072.0, 1.5, -419072.0 - k_step}, {419.0703125, 0.0, -0.25}},
      {{10000000.0 - k_step, -3.0, 10000000.0}, {10000.0, 0.001953125, -10000.0}},
      {{100000000.25, 0.0, -100000000.0}, {100000.25, 0.0, 1e8}},
      {{419070.2, -10000000.4, 100000000.25}, {0.5, 0.5, 0.5}},
  };
  doc::Document d;
  d.add_layer("base", doc::LayerRole::Base);
  for (u64 i = 0; i < 4; ++i) {
    JsonValue p = JsonValue::object();
    p.set("at", vec3(sites[i].at.x, sites[i].at.y, sites[i].at.z));
    p.set("survey", vec3(sites[i].survey_km.x, sites[i].survey_km.y, sites[i].survey_km.z));
    create(d, 100 + i, "engine.ecs.demo.Post", 0, std::move(p));
  }
  Rig rig;
  DocumentSink sink;
  sink.document = &d;
  rig.driver.set_writeback_sink(sim::WriteBackSink{&sink, &DocumentSink::commit});
  rig.driver.install_writeback();
  rig.driver.materialize(d);
  for (u64 i = 0; i < 4; ++i) {
    CAPTURE(i);
    // A copy, not a reference through the temporary entity: GCC's -Wdangling-reference cannot see
    // that the component outlives the handle, and warnings are errors.
    const demo::Beacon b = rig.entity(100 + i).get<demo::Beacon>();
    CHECK(b.at == sites[i].at);
    CHECK(b.survey == WorldPos{sites[i].survey_km.x * 1000.0, sites[i].survey_km.y * 1000.0,
                               sites[i].survey_km.z * 1000.0});
  }

  // Ticks with nothing moving: no write-back at all, so the document keeps the doubles it had.
  const u32 journal = d.journal().size();
  ScheduledTick tick(rig.world, rig.scheduler);
  tick.step();
  tick.step();
  CHECK(sink.commits == 0);
  CHECK(d.journal().size() == journal);

  // Moved by 1/1024 m along x: the document receives the new position to the bit, and the survey,
  // moved by 1 m, comes back in km as (m - 0) / 1000 in f64.
  for (u64 i = 0; i < 4; ++i) {
    demo::Beacon& b = rig.entity(100 + i).get_mut<demo::Beacon>();
    b.at += DVec3{k_step, 0.0, 0.0};
    b.survey += DVec3{0.0, 0.0, 1.0};
    rig.entity(100 + i).modified<demo::Beacon>();
  }
  tick.step();
  CHECK(sink.commits == 1);
  for (u64 i = 0; i < 4; ++i) {
    CAPTURE(i);
    const JsonValue& at = *d.property(rid(100 + i), "at");
    CHECK(at[0].as_float() == sites[i].at.x + k_step);
    CHECK(at[1].as_float() == sites[i].at.y);
    CHECK(at[2].as_float() == sites[i].at.z);
    const JsonValue& survey = *d.property(rid(100 + i), "survey");
    CHECK(survey[2].as_float() == (sites[i].survey_km.z * 1000.0 + 1.0) / 1000.0);
  }
}
