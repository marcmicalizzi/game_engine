// The materialization driver (domain/sim/materialize.h; docs/subsystems/sim.md, "The driver"):
// the walk, its order, what is skipped and why, incremental passes, tiles, write-back, and the
// hooks' order. The world here is a fake that keeps plain structs, because the driver's contract is
// what it hands the hooks; the entity store's hook is tested against flecs in domain/ecs.
#include <core/json/json.h>
#include <core/schema/materialize.h>
#include <domain/doc/document.h>
#include <domain/doc/partition.h>
#include <domain/sim/materialize.h>
#include <domain/sim/scheduler.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <map>
#include <schemas/sim_world.h>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::sim;
namespace t = engine::sim::test;

namespace {

const char* const k_crate = "engine.sim.test.Crate";
const char* const k_shelf = "engine.sim.test.Shelf";
const char* const k_fact = "engine.sim.test.Fact";

Id128 id_of(u64 n) { return Id128::from_parts(0x5151, n); }

JsonValue vec3(f64 x, f64 y, f64 z) {
  JsonValue v = JsonValue::array();
  v.push_back(JsonValue(x));
  v.push_back(JsonValue(y));
  v.push_back(JsonValue(z));
  return v;
}

JsonValue props(std::initializer_list<std::pair<const char*, JsonValue>> list) {
  JsonValue out = JsonValue::object();
  for (const auto& [key, value] : list)
    out.set(key, value);
  return out;
}

// A world that keeps what the hooks were handed. One entity per record, keyed by the record's id
// exactly as the entity store keys its entities, and the handles are its own numbers.
struct FakeWorld {
  struct Entity {
    u64 handle = 0;
    Id128 parent;
    u8 tier = 0;
    u32 writes = 0;
    t::Body body;
    t::Label label;
  };
  std::map<Id128, Entity> entities;
  std::map<u64, Id128> by_handle;
  u64 next_handle = 1;
  bool has_label = true;
  std::vector<WriteBackChange> pending;
  std::vector<std::string>* log = nullptr;

  static EntityHandle materialize(void* context, const EntityRecord& record, u8 tier) {
    auto* self = static_cast<FakeWorld*>(context);
    if (record.source == nullptr) return {};
    const RecordSource& source = *record.source;
    Entity& e = self->entities[record.entity];
    if (e.handle == 0) {
      e.handle = self->next_handle++;
      self->by_handle[e.handle] = record.entity;
    }
    e.parent = source.parent;
    e.tier = tier;
    ++e.writes;
    for (u32 i = 0; i < source.mapping->fields.size(); ++i) {
      const schema::MaterializeField& row = source.mapping->fields[i];
      void* component = row.component == &schema::type_of<t::Body>() ? static_cast<void*>(&e.body)
                                                                     : static_cast<void*>(&e.label);
      schema::ReadContext ctx;
      REQUIRE(schema::read_mapped(row, *row.component->find_field(row.field), component,
                                  *source.values[i], ctx));
    }
    return EntityHandle{e.handle};
  }
  static void dematerialize(void* context, EntityHandle handle) {
    auto* self = static_cast<FakeWorld*>(context);
    if (self->log != nullptr) self->log->push_back("world");
    const auto it = self->by_handle.find(handle.value);
    if (it == self->by_handle.end()) return;
    self->entities.erase(it->second);
    self->by_handle.erase(it);
  }
  static bool has_component(void* context, const schema::TypeInfo& component) {
    auto* self = static_cast<FakeWorld*>(context);
    return &component != &schema::type_of<t::Label>() || self->has_label;
  }
  static EntityHandle resolve(void* context, const Id128& id) {
    auto* self = static_cast<FakeWorld*>(context);
    const auto it = self->entities.find(id);
    return it != self->entities.end() ? EntityHandle{it->second.handle} : EntityHandle{};
  }
  static void collect(void* context, Vector<WriteBackChange>& out) {
    auto* self = static_cast<FakeWorld*>(context);
    for (WriteBackChange& change : self->pending)
      out.push_back(std::move(change));
    self->pending.clear();
  }

  MaterializationHooks hooks() {
    MaterializationHooks row;
    row.name = "fake";
    row.context = this;
    row.materialize = &materialize;
    row.dematerialize = &dematerialize;
    return row;
  }
  MaterializeTarget target() {
    MaterializeTarget target;
    target.context = this;
    target.name = "fake";
    target.has_component = &has_component;
    target.resolve = &resolve;
    target.collect_writeback = &collect;
    return target;
  }
};

// A scheduler, a fake world with its hooks registered first, and the driver bound to both.
struct Rig {
  SimScheduler scheduler;
  FakeWorld world;
  Materializer driver;

  explicit Rig(u8 tier = 0) : driver(scheduler, config(tier)) {
    scheduler.add_hooks(world.hooks());
    driver.set_target(world.target());
  }
  static MaterializeConfig config(u8 tier) {
    MaterializeConfig c;
    c.tier = tier;
    return c;
  }
};

doc::Document base_document() {
  doc::Document d;
  d.add_layer("base", doc::LayerRole::Base);
  return d;
}

void create(doc::Document& d, u64 n, const char* type, u64 parent = 0, JsonValue initial = {}) {
  Vector<doc::Diagnostic> diagnostics;
  const bool ok = d.apply(
      doc::cmd_create(id_of(n), type, parent != 0 ? id_of(parent) : Id128{}, std::move(initial)),
      nullptr, &diagnostics);
  REQUIRE_MESSAGE(ok, (diagnostics.empty() ? std::string() : diagnostics[0].message));
}

// Ids chosen so that id order is not depth order: a child with a smaller id than its parent is
// what makes "parents before children" a rule the test can see being kept.
//
//   S 50 Shelf (root)     B 20 Crate (root)
//   └ A 10 Crate          F  1 Fact (root, document-only)
//     └ C 5 Crate
doc::Document yard() {
  doc::Document d = base_document();
  create(d, 50, k_shelf, 0, props({{"position", vec3(1, 0, 1)}}));
  create(d, 20, k_crate, 0, props({{"position", vec3(2, 0, 0)}, {"label", JsonValue("b")}}));
  create(d, 10, k_crate, 50, props({{"position", vec3(3, 0, 0)}}));
  create(d, 5, k_crate, 10, props({{"speed", JsonValue(72.0)}}));
  create(d, 1, k_fact, 0, props({{"claim", JsonValue("the yard is old")}}));
  return d;
}

const MaterializedType* type_row(const MaterializeReport& report, const char* type) {
  for (const MaterializedType& row : report.types) {
    if (row.type == type) return &row;
  }
  return nullptr;
}

// Commits a write-back the way a session would: one transaction, the batch's attribution.
struct DocumentSink {
  doc::Document* document = nullptr;
  u32 commits = 0;
  static bool commit(void* context, const WriteBackBatch& batch) {
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

TEST_CASE("sim: the driver materializes parents before children, then by id, the same every run") {
  Vector<Id128> orders[2];
  u64 hashes[2] = {};
  for (u32 run = 0; run < 2; ++run) {
    doc::Document d = yard();
    Rig rig;
    const MaterializeReport report = rig.driver.materialize(d);
    CHECK(report.full);
    CHECK(report.visited == 5);
    CHECK(report.created == 4);
    CHECK(report.skipped == 1);
    CHECK(report.live == 4);
    CHECK(report.orphans == 0);
    for (const Id128& id : rig.driver.last_order())
      orders[run].push_back(id);
    hashes[run] = rig.driver.last_order_hash();

    // Depth 0 in id order (B 20, S 50), then A 10, then C 5.
    REQUIRE(orders[run].size() == 4);
    CHECK(orders[run][0] == id_of(20));
    CHECK(orders[run][1] == id_of(50));
    CHECK(orders[run][2] == id_of(10));
    CHECK(orders[run][3] == id_of(5));

    // What the hooks were handed: the composed property, or the type's default converted from km/h.
    const FakeWorld::Entity& a = rig.world.entities.at(id_of(10));
    CHECK(a.parent == id_of(50));
    CHECK(a.body.position == Vec3{3.0f, 0.0f, 0.0f});
    CHECK(a.body.speed == doctest::Approx(10.0f));  // the default 36 km/h
    CHECK(a.label.text == "crate");                 // the default label
    CHECK(rig.world.entities.at(id_of(5)).body.speed == doctest::Approx(20.0f));
    CHECK(rig.world.entities.at(id_of(20)).label.text == "b");
    CHECK(rig.world.entities.at(id_of(20)).parent.is_null());

    const MaterializedType* crates = type_row(report, k_crate);
    REQUIRE(crates != nullptr);
    CHECK(crates->mapped);
    CHECK(crates->records == 3);
    CHECK(crates->materialized == 3);
    const MaterializedType* facts = type_row(report, k_fact);
    REQUIRE(facts != nullptr);
    CHECK_FALSE(facts->mapped);
    CHECK(facts->skipped == 1);
    CHECK(facts->reason == SkipReason::NoMapping);
  }
  CHECK(orders[0] == orders[1]);
  CHECK(hashes[0] == hashes[1]);
  // Pinned: the order is a function of the document alone, so its hash is a constant of this test.
  CHECK(hashes[0] == 14571387425004446014ull);
}

TEST_CASE("sim: later passes follow the change feed, and an unchanged record costs no hook call") {
  doc::Document d = yard();
  Rig rig;
  rig.driver.materialize(d);
  const u64 calls = rig.driver.stats().hook_calls;
  CHECK(calls == 4);

  // Nothing changed: nothing visited, no hook called.
  MaterializeReport again = rig.driver.materialize(d);
  CHECK_FALSE(again.full);
  CHECK(again.visited == 0);
  CHECK(rig.driver.stats().hook_calls == calls);
  CHECK(again.live == 4);

  // One property of one record: that record, and only it.
  REQUIRE(d.apply(doc::cmd_set(id_of(10), "position", vec3(9, 0, 0)), nullptr, nullptr));
  MaterializeReport changed = rig.driver.materialize(d);
  CHECK_FALSE(changed.full);
  CHECK(changed.visited == 1);
  CHECK(changed.updated == 1);
  CHECK(rig.driver.stats().hook_calls == calls + 1);
  CHECK(rig.world.entities.at(id_of(10)).body.position == Vec3{9.0f, 0.0f, 0.0f});
  CHECK(rig.world.entities.at(id_of(10)).writes == 2);
  CHECK(rig.world.entities.at(id_of(20)).writes == 1);

  // A deletion dematerializes; a creation creates; a document-only record is visited and skipped.
  REQUIRE(d.apply(doc::cmd_delete(id_of(20)), nullptr, nullptr));
  create(d, 30, k_crate);
  create(d, 2, k_fact);
  MaterializeReport third = rig.driver.materialize(d);
  CHECK(third.dematerialized == 1);
  CHECK(third.created == 1);
  CHECK(third.skipped == 1);
  CHECK(third.live == 4);
  CHECK(rig.world.entities.count(id_of(20)) == 0);
  CHECK(rig.world.entities.count(id_of(30)) == 1);
  CHECK(rig.driver.stats().hook_calls == calls + 2);
}

TEST_CASE("sim: a record the world cannot hold is skipped whole, and the report says why") {
  SUBCASE("a tier the mapping does not materialize at, and the orphan it leaves") {
    doc::Document d = yard();
    Rig rig(2);  // shelves materialize at LOD0 and LOD1 only
    const MaterializeReport report = rig.driver.materialize(d);
    const MaterializedType* shelves = type_row(report, k_shelf);
    REQUIRE(shelves != nullptr);
    CHECK(shelves->skipped == 1);
    CHECK(shelves->reason == SkipReason::Tier);
    CHECK(report.live == 3);
    // A's parent has no entity, so A is an orphan until it does.
    CHECK(report.orphans == 1);
    CHECK(rig.world.entities.at(id_of(10)).parent == id_of(50));  // what the hook was told
  }
  SUBCASE("a component this world does not have") {
    doc::Document d = yard();
    Rig rig;
    rig.world.has_label = false;
    const MaterializeReport report = rig.driver.materialize(d);
    const MaterializedType* crates = type_row(report, k_crate);
    REQUIRE(crates != nullptr);
    CHECK(crates->skipped == 3);
    CHECK(crates->reason == SkipReason::MissingComponent);
    CHECK(crates->detail == "engine.sim.test.Label");
    CHECK(report.live == 1);  // the shelf, which needs no label
    // Listed by id, in walk order: the fact (id 1, no mapping) first, then the crates.
    REQUIRE(report.skips.size() == 4);
    CHECK(report.skips[0].reason == SkipReason::NoMapping);
    CHECK(report.skips[1].id == id_of(5));
    CHECK(report.skips[1].reason == SkipReason::MissingComponent);
  }
  SUBCASE("a world with no entity store") {
    doc::Document d = yard();
    SimScheduler scheduler;
    Materializer driver(scheduler);
    const MaterializeReport report = driver.materialize(d);
    CHECK(report.live == 0);
    CHECK(report.skipped == 5);
    CHECK(type_row(report, k_crate)->reason == SkipReason::NoEntityStore);
  }
  SUBCASE("a type this build has never heard of") {
    doc::Document d = base_document();
    create(d, 7, "engine.nowhere.Thing");
    Rig rig;
    const MaterializeReport report = rig.driver.materialize(d);
    REQUIRE(report.types.size() == 1);
    CHECK(report.types[0].reason == SkipReason::UnknownType);
  }
}

TEST_CASE("sim: a tile is materialized on its own, and a parent in another tile relinks") {
  doc::Document d;
  d.add_layer("base", doc::LayerRole::Base);
  doc::LayerPartition partition;
  partition.tile_size = 10.0;
  d.set_layer_partition(0, partition);
  // A shelf in tile (0, 0) and its crate in tile (1, 0); a loose crate in tile (1, 0).
  create(d, 50, k_shelf, 0, props({{"position", vec3(5, 0, 5)}}));
  create(d, 10, k_crate, 50, props({{"position", vec3(15, 0, 5)}}));
  create(d, 11, k_crate, 0, props({{"position", vec3(12, 0, 1)}}));

  Rig rig;
  MaterializeReport east = rig.driver.materialize(d, MaterializeScope::of_tile({1, 0}));
  CHECK(east.visited == 2);
  CHECK(east.created == 2);
  CHECK(east.orphans == 1);  // the crate whose shelf is in the other tile
  CHECK_FALSE(rig.driver.holds(id_of(50)));

  MaterializeReport west = rig.driver.materialize(d, MaterializeScope::of_tile({0, 0}));
  CHECK(west.visited == 1);
  CHECK(west.created == 1);
  CHECK(west.relinked == 1);
  CHECK(west.orphans == 0);
  CHECK(rig.world.entities.at(id_of(10)).writes == 2);

  // A whole pass afterwards finds everything in sync.
  MaterializeReport whole = rig.driver.materialize(d);
  CHECK(whole.full);
  CHECK(whole.unchanged == 3);
  CHECK(whole.created + whole.updated == 0);

  // A crate moved to another tile leaves the tile it was filed under.
  REQUIRE(d.apply(doc::cmd_set(id_of(11), "position", vec3(-3, 0, 1)), nullptr, nullptr));
  MaterializeReport moved = rig.driver.materialize(d, MaterializeScope::of_tile({1, 0}));
  CHECK(moved.dematerialized == 1);
  CHECK_FALSE(rig.driver.holds(id_of(11)));
}

TEST_CASE("sim: a tile is dematerialized on its own, and its children elsewhere wait for it") {
  // The mirror of a tile pass, which a tile that goes inactive needs (docs/subsystems/world.md):
  // only what was filed under the tile goes, deepest first, and a child in another tile whose
  // parent went is an orphan until the parent's tile comes back.
  doc::Document d;
  d.add_layer("base", doc::LayerRole::Base);
  doc::LayerPartition partition;
  partition.tile_size = 10.0;
  d.set_layer_partition(0, partition);
  create(d, 50, k_shelf, 0, props({{"position", vec3(5, 0, 5)}}));    // tile (0, 0)
  create(d, 10, k_crate, 50, props({{"position", vec3(15, 0, 5)}}));  // tile (1, 0), child of 50
  create(d, 11, k_crate, 0, props({{"position", vec3(12, 0, 1)}}));   // tile (1, 0)
  create(d, 12, k_crate, 50, props({{"position", vec3(4, 0, 4)}}));   // tile (0, 0), child of 50

  Rig rig;
  rig.driver.materialize(d, MaterializeScope::of_tile({0, 0}));
  rig.driver.materialize(d, MaterializeScope::of_tile({1, 0}));
  REQUIRE(rig.driver.live() == 4);
  Vector<Id128> west;
  rig.driver.held(MaterializeScope::of_tile({0, 0}), west);
  REQUIRE(west.size() == 2);
  CHECK(west[0] == id_of(12));  // sorted
  CHECK(west[1] == id_of(50));

  std::vector<std::string> log;
  rig.world.log = &log;
  CHECK(rig.driver.dematerialize(MaterializeScope::of_tile({0, 0})) == 2);
  CHECK(log.size() == 2);
  CHECK_FALSE(rig.driver.holds(id_of(50)));
  CHECK_FALSE(rig.driver.holds(id_of(12)));
  CHECK(rig.driver.holds(id_of(10)));
  CHECK(rig.driver.holds(id_of(11)));
  CHECK(rig.world.entities.count(id_of(10)) == 1);

  // The tile comes back: the shelf and its crate are created again, the other tile's crate relinks.
  const MaterializeReport back = rig.driver.materialize(d, MaterializeScope::of_tile({0, 0}));
  CHECK(back.created == 2);
  CHECK(back.relinked == 1);
  CHECK(back.orphans == 0);
  // Dematerializing a tile nothing is filed under is nothing.
  CHECK(rig.driver.dematerialize(MaterializeScope::of_tile({7, 7})) == 0);
  CHECK(rig.driver.live() == 4);
}

TEST_CASE("sim: write-back commits one attributed transaction and the next pass leaves it alone") {
  doc::Document d = yard();
  Rig rig;
  DocumentSink sink;
  sink.document = &d;
  rig.driver.set_writeback_sink(WriteBackSink{&sink, &DocumentSink::commit});
  rig.driver.materialize(d);
  const u64 calls = rig.driver.stats().hook_calls;

  const schema::MaterializeInfo* crate = schema::MaterializeRegistry::global().find(k_crate);
  REQUIRE(crate != nullptr);
  WriteBackChange change;
  change.record = id_of(10);
  change.mapping = crate;
  change.row = 0;  // Body.position <- position
  change.value = vec3(4, 5, 6);
  rig.world.pending.push_back(change);

  CHECK(rig.driver.flush_writeback(SimTick{7}, GameTime{1000}) == 1);
  CHECK(sink.commits == 1);
  const doc::Patch& patch = d.journal().back();
  CHECK(patch.attribution.actor == "system");
  CHECK(patch.attribution.role == "system");
  CHECK(patch.attribution.task == "sim.write_back");
  CHECK(patch.attribution.rationale.find("tick 7") != std::string::npos);
  REQUIRE(patch.forward.size() == 1);
  CHECK(patch.forward[0].kind == doc::CommandKind::SetProperty);
  CHECK(*d.property(id_of(10), "position") == vec3(4, 5, 6));

  // The world wrote it, so the world is already in sync with it: no hook call.
  MaterializeReport after = rig.driver.materialize(d);
  CHECK(after.unchanged == 1);
  CHECK(after.updated == 0);
  CHECK(rig.driver.stats().hook_calls == calls);
  CHECK(rig.driver.stats().writeback_fields == 1);

  // An edit by someone else before the flush still reaches the world after it.
  REQUIRE(d.apply(doc::cmd_set(id_of(10), "label", JsonValue("mine")), nullptr, nullptr));
  change.value = vec3(7, 7, 7);
  rig.world.pending.push_back(change);
  CHECK(rig.driver.flush_writeback(SimTick{8}, GameTime{2000}) == 1);
  MaterializeReport edited = rig.driver.materialize(d);
  CHECK(edited.updated == 1);
  CHECK(rig.world.entities.at(id_of(10)).label.text == "mine");
}

// Records that move (sim.md): a write-back that puts a record in another tile of the document's is
// reported by `take_moved`, once, and the world decides — `refile` it under the tile it is in now,
// or let it go with `dematerialize(ids)`. The driver itself decides neither.
TEST_CASE("sim: a record the world moved into another tile is reported, and refiled or let go") {
  doc::Document d;
  d.add_layer("base", doc::LayerRole::Base);
  doc::LayerPartition partition;
  partition.tile_size = 10.0;
  d.set_layer_partition(0, partition);
  create(d, 10, k_crate, 0, props({{"position", vec3(5, 0, 5)}}));  // tile (0, 0)
  create(d, 11, k_crate, 0, props({{"position", vec3(6, 0, 5)}}));  // tile (0, 0)
  create(d, 12, k_crate, 0, props({{"position", vec3(7, 0, 5)}}));  // tile (0, 0)

  Rig rig;
  DocumentSink sink;
  sink.document = &d;
  rig.driver.set_writeback_sink(WriteBackSink{&sink, &DocumentSink::commit});
  rig.driver.materialize(d, MaterializeScope::of_tile({0, 0}));
  REQUIRE(rig.driver.live() == 3);
  Vector<TileMove> moves;
  rig.driver.take_moved(moves);
  CHECK(moves.empty());

  // The world moves two of them out of the tile, and one within it.
  const schema::MaterializeInfo* crate = schema::MaterializeRegistry::global().find(k_crate);
  REQUIRE(crate != nullptr);
  auto move_to = [&](u64 n, f64 x) {
    WriteBackChange change;
    change.record = id_of(n);
    change.mapping = crate;
    change.row = 0;
    change.value = vec3(x, 0, 5);
    rig.world.pending.push_back(change);
  };
  move_to(10, 14.0);  // into (1, 0)
  move_to(11, 8.0);   // still (0, 0)
  move_to(12, 23.0);  // into (2, 0)
  REQUIRE(rig.driver.flush_writeback(SimTick{3}, GameTime{50000}) == 3);
  rig.driver.take_moved(moves);
  REQUIRE(moves.size() == 2);
  CHECK(moves[0].id == id_of(10));
  CHECK(moves[0].from_tiled);
  CHECK(moves[0].from == doc::TileCoord{0, 0});
  CHECK(moves[0].to == doc::TileCoord{1, 0});
  CHECK(moves[1].id == id_of(12));
  CHECK(moves[1].to == doc::TileCoord{2, 0});
  // Reported once.
  Vector<TileMove> again;
  rig.driver.take_moved(again);
  CHECK(again.empty());

  // One follows its tile, the other is let go; the tile it was filed under no longer holds either.
  CHECK(rig.driver.refile(id_of(10), true, doc::TileCoord{1, 0}));
  const Id128 gone[] = {id_of(12), id_of(99)};
  CHECK(rig.driver.dematerialize(std::span<const Id128>(gone, 2)) == 1);
  CHECK_FALSE(rig.driver.holds(id_of(12)));
  CHECK(rig.world.entities.count(id_of(12)) == 0);
  Vector<Id128> held;
  rig.driver.held(MaterializeScope::of_tile({0, 0}), held);
  REQUIRE(held.size() == 1);
  CHECK(held[0] == id_of(11));
  rig.driver.held(MaterializeScope::of_tile({1, 0}), held);
  REQUIRE(held.size() == 1);
  CHECK(held[0] == id_of(10));
  // The tile it went to lets it go with itself, and a pass of it afterwards finds it in sync.
  CHECK(rig.driver.dematerialize(MaterializeScope::of_tile({1, 0})) == 1);
  CHECK_FALSE(rig.driver.refile(id_of(10), true, doc::TileCoord{1, 0}));
  const MaterializeReport back = rig.driver.materialize(d, MaterializeScope::of_tile({1, 0}));
  CHECK(back.created == 1);
  CHECK(rig.world.entities.at(id_of(10)).body.position == Vec3{14.0f, 0.0f, 5.0f});
}

TEST_CASE("sim: write-back is a system in the scheduler's table at Persist, on its cadence") {
  doc::Document d = yard();
  Rig rig;
  DocumentSink sink;
  sink.document = &d;
  rig.driver.set_writeback_sink(WriteBackSink{&sink, &DocumentSink::commit});
  rig.driver.set_writeback_every(2);
  rig.driver.install_writeback();
  rig.driver.materialize(d);
  REQUIRE(rig.scheduler.system_count() == 1);
  CHECK(rig.scheduler.system(0).phase == TickPhase::Persist);
  CHECK(std::string(rig.scheduler.system(0).name) == "sim.write_back");

  const schema::MaterializeInfo* crate = schema::MaterializeRegistry::global().find(k_crate);
  WriteBackChange change;
  change.record = id_of(20);
  change.mapping = crate;
  change.row = 0;
  change.value = vec3(1, 1, 1);
  rig.world.pending.push_back(change);
  rig.scheduler.step();  // tick 1: not on the cadence
  CHECK(sink.commits == 0);
  rig.scheduler.step();  // tick 2
  CHECK(sink.commits == 1);
  CHECK(*d.property(id_of(20), "position") == vec3(1, 1, 1));
}

TEST_CASE("sim: dematerialization walks the hooks backwards, deepest record first") {
  doc::Document d = yard();
  std::vector<std::string> log;
  Rig rig;
  rig.world.log = &log;
  // A second hook that attaches to what the first created, as a capability's does.
  struct Observer {
    std::vector<std::string>* log;
    static void dematerialize(void* context, EntityHandle) {
      static_cast<Observer*>(context)->log->push_back("observer");
    }
  } observer{&log};
  MaterializationHooks row;
  row.name = "observer";
  row.context = &observer;
  row.dematerialize = &Observer::dematerialize;
  rig.scheduler.add_hooks(row);

  rig.driver.materialize(d);
  CHECK(rig.driver.dematerialize_all() == 4);
  REQUIRE(log.size() == 8);
  for (u32 i = 0; i < 8; i += 2) {
    CHECK(log[i] == "observer");
    CHECK(log[i + 1] == "world");
  }
  CHECK(rig.world.entities.empty());
}

namespace {

// The path a tile pass took before the document kept a tile index, kept here as the oracle and
// nowhere at run time: every live record of the document, its defining record found by visiting its
// layers, classified by that record's tile under its layer's partition.
Vector<Id128> classified(const doc::Document& d, const MaterializeScope& scope) {
  Vector<Id128> out;
  for (const Id128& id : d.objects()) {
    const doc::ObjectRecord* defining = nullptr;
    u32 layer = 0;
    d.visit_records(id, [&](u32 i, const doc::ObjectRecord& r) {
      if (r.type.empty()) return;
      defining = &r;
      layer = i;
    });
    doc::TileCoord tile;
    bool tiled = false;
    if (defining != nullptr && d.layer(layer).partitioned())
      tiled = doc::tile_of(*defining, d.layer(layer).partition(), tile);
    const bool in =
        scope.kind == MaterializeScope::Kind::Whole ||
        (scope.kind == MaterializeScope::Kind::Tile ? tiled && tile == scope.tile : !tiled);
    if (in) out.push_back(id);
  }
  return out;
}

struct Equivalence {
  u32 passes = 0;
  u32 records = 0;
  std::string first;
};

// One pass of `scope` against the oracle: it looked at exactly the records the classification
// finds, every one of them the world can hold is held after it, and nothing filed under the scope
// is a record the classification would not have put there.
void check_pass(Rig& rig, const doc::Document& d, const MaterializeScope& scope, Equivalence& e) {
  const Vector<Id128> want = classified(d, scope);
  const MaterializeReport report = rig.driver.materialize(d, scope);
  ++e.passes;
  e.records += want.size();
  auto fail = [&](const std::string& what) {
    if (e.first.empty()) {
      e.first = "pass " + std::to_string(e.passes) + " of " +
                (scope.kind == MaterializeScope::Kind::Tile
                     ? "tile " + std::to_string(scope.tile.x) + "_" + std::to_string(scope.tile.y)
                     : std::string("the untiled records")) +
                ": " + what;
    }
  };
  if (report.visited != want.size())
    fail("visited " + std::to_string(report.visited) + ", the classification finds " +
         std::to_string(want.size()));
  for (const Id128& id : want) {
    const bool holdable = d.type_of(id) != k_fact;
    if (holdable && !rig.driver.holds(id)) fail("a record of the scope is not held");
  }
  Vector<Id128> held;
  rig.driver.held(scope, held);
  for (const Id128& id : held) {
    if (std::find(want.begin(), want.end(), id) == want.end())
      fail("the scope holds a record the classification puts elsewhere");
  }
}

}  // namespace

TEST_CASE("sim: a tile pass through the document's tile index takes what a classification takes") {
  // Four layers: two partitioned on one grid (one naming the property, one letting the type say),
  // an unpartitioned layer of overrides, and a partitioned edit layer. Crates and shelves defined
  // in each, some in parent chains across tiles, some with no position, facts that never
  // materialize; then three hundred seeded rounds of moves, overrides of the position in the
  // unpartitioned layer (which move nothing), tombstones, removed records, redefinitions in a
  // stronger layer, and new records — each followed by passes of random tiles and of the untiled
  // records, a let-go of a random tile, and now and then a whole pass, every tile or untiled pass
  // held to the oracle.
  doc::Document d;
  d.add_layer("base", doc::LayerRole::Base);
  d.add_layer("props", doc::LayerRole::Feature);
  d.add_layer("overrides", doc::LayerRole::Session);
  d.add_layer("edits", doc::LayerRole::Session);
  doc::LayerPartition by_type;
  by_type.tile_size = 10.0;
  doc::LayerPartition named;
  named.property = "position";
  named.tile_size = 10.0;
  d.set_layer_partition(0, by_type);
  d.set_layer_partition(1, named);
  d.set_layer_partition(3, by_type);

  u64 rng = 0x51de'7115'eed0'0001ull;
  auto next = [&](u32 bound) {
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<u32>((rng >> 33) % bound);
  };
  auto somewhere = [&]() {
    return vec3(static_cast<f64>(next(60)) - 30.0, 0.0, static_cast<f64>(next(60)) - 30.0);
  };
  constexpr u32 k_ids = 90;
  auto create_in = [&](u32 layer, u64 n) {
    d.set_edit_layer(layer);
    const u32 kind = next(10);
    const char* type = kind < 6 ? k_crate : kind < 9 ? k_shelf : k_fact;
    JsonValue initial = JsonValue::object();
    if (type != k_fact && next(5) != 0) initial.set("position", somewhere());
    const u64 parent = type == k_crate && next(3) == 0 ? 1 + next(k_ids) : 0;
    const Id128 p = parent != 0 && d.exists(id_of(parent)) ? id_of(parent) : Id128{};
    (void)d.apply(doc::cmd_create(id_of(n), type, p, std::move(initial)), nullptr, nullptr);
  };
  for (u64 n = 1; n <= 60; ++n)
    create_in(n % 3 == 0 ? 1 : (n % 5 == 0 ? 3 : 0), n);

  Rig rig;
  Equivalence e;
  for (u32 round = 0; round < 300 && e.first.empty(); ++round) {
    for (u32 k = 0; k < 4; ++k) {
      const u64 n = 1 + next(k_ids);
      const u32 what = next(10);
      if (what < 4) {
        // A move where the record is defined, or an override where it is not.
        d.set_edit_layer(next(4));
        (void)d.apply(doc::cmd_set(id_of(n), "position", somewhere()), nullptr, nullptr, false);
      } else if (what < 5) {
        d.set_edit_layer(next(4));
        (void)d.apply(doc::cmd_clear(id_of(n), "position"), nullptr, nullptr);
      } else if (what < 6) {
        d.set_edit_layer(next(4));
        (void)d.apply(doc::cmd_delete(id_of(n)), nullptr, nullptr);
      } else if (what < 7) {
        d.set_edit_layer(next(4));
        (void)d.apply(doc::cmd_remove_record(id_of(n)), nullptr, nullptr);
      } else {
        create_in(next(4), n);
      }
    }
    for (u32 k = 0; k < 3; ++k) {
      const doc::TileCoord tile{static_cast<i32>(next(6)) - 3, static_cast<i32>(next(6)) - 3};
      check_pass(rig, d, MaterializeScope::of_tile(tile), e);
    }
    check_pass(rig, d, MaterializeScope::untiled(), e);
    rig.driver.dematerialize(MaterializeScope::of_tile(
        doc::TileCoord{static_cast<i32>(next(6)) - 3, static_cast<i32>(next(6)) - 3}));
    if (round % 50 == 49) rig.driver.materialize(d);
    CHECK(d.validate_index());
  }
  INFO(e.first);
  CHECK(e.first.empty());
  CHECK(e.passes >= 1200);
  CHECK(e.records > 1000);  // the scopes the passes covered were not empty
}
