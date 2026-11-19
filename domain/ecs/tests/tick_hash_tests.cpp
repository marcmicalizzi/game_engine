// The state hash a tick and the divergence bisector (domain/ecs/tick_hash.h, domain/sim/hash_log.h;
// docs/subsystems/ecs.md, "The state hash a tick"; roadmap T53): a system made nondeterministic on
// purpose — it ranks entities by the address of a slot each holds, the bug of iterating a container
// keyed by pointer — is found at its tick, its phase and its table, and then at its entities and
// their field; a run replayed from its recorded input reports nothing, and one input changed is
// found at its tick.
#include <core/memory/memory.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/scheduled_tick.h>
#include <domain/ecs/sim_world.h>
#include <domain/ecs/tick_hash.h>
#include <domain/sim/hash_log.h>
#include <domain/sim/scheduler.h>
#include <foundation/input/input.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>
#include <schemas/ecs_demo.h>
#include <schemas/ecs_demo_ecs.h>
#include <string>
#include <test_temp_dir.h>

using namespace engine;
using namespace engine::ecs;

namespace {

constexpr u32 k_entities = 33;
constexpr u64 k_seed = 7;
// The tick the ranking system first acts: before it, the two runs agree everywhere.
constexpr u64 k_ranks_from = 137;

Id128 entity_id(u32 i) { return Id128{k_seed, 1000u + i}; }

// The world of the cases: entities with a standing and a label, a deterministic drift in physics,
// and the ranking system in `systems`.
struct World {
  SimWorld sim;
  sim::SimScheduler scheduler;
  // The slots the ranking reads: entity i holds `slots[order[i]]`, and the system ranks entities by
  // the slot's address. With `order` the identity the rank is the id order; reversed, it is not —
  // which is what a pointer-keyed container does to a run when the allocator hands out memory in
  // another order.
  Vector<u32> slots = Vector<u32>(k_entities);
  Vector<u32> order;
  Vector<flecs::entity_t> entities;
  bool ranks = true;
  // The player's input (the replay case): pushes at a tick add to entity 0's wealth.
  const input::InputLog* inputs = nullptr;
  input::ActionMap map;
  input::ActionId push = input::k_invalid_action;
  std::unique_ptr<input::InputState> state;
  u32 cursor = 0;
  std::unique_ptr<ScheduledTick> tick;
  std::unique_ptr<TickHash> hash;

  explicit World(bool reversed) {
    demo::register_ecs_demo_components(sim.world());
    for (u32 i = 0; i < k_entities; ++i) {
      order.push_back(reversed ? k_entities - 1u - i : i);
      flecs::entity e = create_entity(sim.world(), entity_id(i));
      e.set<demo::Standing>({Vec3{static_cast<f32>(i), 0.0f, 0.0f}, 100, 1.0f, i % 3u, 0u});
      e.set<demo::Label>({"stall " + std::to_string(i), static_cast<u8>(i)});
      entities.push_back(e.id());
    }
    push = map.add_action("push", input::ActionKind::Button);
    map.bind(push, input::Binding{input::Source::Key, 32u});
    state = std::make_unique<input::InputState>(map);

    sim::SystemDesc feed;
    feed.name = "test.input";
    feed.phase = sim::TickPhase::Input;
    feed.context = this;
    feed.begin_tick = &World::feed_input;
    scheduler.add_system(feed);

    sim::SystemDesc rank;
    rank.name = "test.rank_by_address";
    rank.phase = sim::TickPhase::Systems;
    rank.context = this;
    rank.begin_tick = &World::rank;
    scheduler.add_system(rank);

    sim.world()
        .system<demo::Standing>("test_drift")
        .kind(sim.phase(TickPhase::Physics))
        .each([](demo::Standing& s) { s.position.y += 0.25f; });
    tick = std::make_unique<ScheduledTick>(sim, scheduler);
  }

  void hash_into(sim::HashLog& log, u64 reserve = 0) {
    TickHashConfig config;
    config.reserve_ticks = reserve;
    hash = std::make_unique<TickHash>(sim, scheduler, log, config);
  }

  void run(u64 ticks) {
    for (u64 i = 0; i < ticks; ++i)
      tick->step();
  }

  static void feed_input(sim::SystemContext& context) {
    auto* self = static_cast<World*>(context.context);
    if (self->inputs == nullptr) return;
    self->state->begin_tick(context.tick);
    const std::span<const input::RawEvent> events = self->inputs->events();
    while (self->cursor < events.size() && events[self->cursor].tick.value <= context.tick.value)
      self->state->feed(events[self->cursor++]);
    self->state->end_tick();
    if (self->state->pressed(self->push)) {
      flecs::entity e(self->sim.world().c_ptr(), self->entities[0]);
      e.try_get_mut<demo::Standing>()->wealth += 7;
    }
  }

  // The bug, on purpose: the order the entities are visited in is their slots' addresses.
  static void rank(sim::SystemContext& context) {
    auto* self = static_cast<World*>(context.context);
    if (!self->ranks || context.tick.value < k_ranks_from) return;
    Vector<std::pair<const u32*, u32>> by_address;
    for (u32 i = 0; i < k_entities; ++i)
      by_address.push_back({&self->slots[self->order[i]], i});
    std::sort(by_address.begin(), by_address.end(),
              [](const auto& a, const auto& b) { return std::less<const u32*>()(a.first, b.first); });
    i64 rank = 0;
    for (const auto& [slot, i] : by_address) {
      flecs::entity e(self->sim.world().c_ptr(), self->entities[i]);
      e.try_get_mut<demo::Standing>()->wealth += rank++;
    }
  }
};

}  // namespace

TEST_CASE("tick hash: off is nothing, and on it holds nothing per entity") {
  World world(false);
  const u16 systems = world.scheduler.system_count();
  world.run(3);
  CHECK(world.scheduler.system_count() == systems);  // no hash, no system

  sim::HashLog log;
  world.hash_into(log, 64);
  CHECK(world.scheduler.system_count() == systems + sim::k_phase_count);
  world.run(2);  // the first hashed phase fixes the tables and reserves the rows
  const TickHashMemory memory = world.hash->memory();
  CHECK(memory.per_entity == 0);
  CHECK(memory.tables == world.hash->tables().size());
  CHECK(memory.per_tick == sim::k_phase_count * (sizeof(sim::HashLogRow) + 8u * memory.tables));
  // Every schema component the demo registers with a size, by name: Beacon, Label, the transient
  // Perception and Standing — empty tables too, so the header is the registration's.
  REQUIRE(world.hash->tables().size() == 4);
  CHECK(world.hash->tables()[0] == "engine.ecs.demo.Beacon");
  CHECK(world.hash->tables()[3] == "engine.ecs.demo.Standing");
  CHECK(world.hash->stats().rows == 2u * sim::k_phase_count);
  CHECK(world.hash->stats().entities == 2u * sim::k_phase_count * 2u * k_entities);
  // A steady tick appends into the reserved log and allocates nothing of the engine's.
  const u64 before = mem::allocation_counter();
  world.run(20);
  CHECK(mem::allocation_counter() == before);
  CHECK(log.rows().size() == 22u * sim::k_phase_count);
}

TEST_CASE("tick hash: the same world twice hashes the same, row for row and byte for byte") {
  sim::HashLog a;
  sim::HashLog b;
  World one(false);
  World two(false);
  one.hash_into(a);
  two.hash_into(b);
  one.run(200);
  two.run(200);
  const sim::Divergence d = sim::first_divergence(a, b);
  CHECK_FALSE(d.found);
  CHECK_FALSE(d.rows_differ);
  CHECK(d.rows_compared == 200u * sim::k_phase_count);
  std::string sa;
  std::string sb;
  a.serialize(sa);
  b.serialize(sb);
  CHECK(sa == sb);

  // The file form reads back to the same log, and refuses one cut short.
  test::TempDir dir;
  const std::string path = io::join_path(dir.path(), "a.hashlog");
  REQUIRE(a.save(path));
  sim::HashLog read;
  std::string error;
  REQUIRE_MESSAGE(read.load(path, &error), error);
  std::string sr;
  read.serialize(sr);
  CHECK(sr == sa);
  CHECK_FALSE(read.deserialize(std::string_view(sa).substr(0, sa.size() - 3), &error));
  CHECK(read.rows().empty());
}

TEST_CASE("bisector: iteration in address order is found at its tick, phase, table and field") {
  sim::HashLog a;
  sim::HashLog b;
  {
    World one(false);
    World two(true);  // the slots in the other order: what another allocator would have done
    one.hash_into(a);
    two.hash_into(b);
    one.run(200);
    two.run(200);
  }
  const sim::Divergence d = sim::first_divergence(a, b);
  REQUIRE(d.found);
  CHECK(d.tick == k_ranks_from);
  CHECK(d.phase == sim::TickPhase::Systems);
  REQUIRE(d.tables.size() == 1);
  CHECK(d.tables[0].table == "engine.ecs.demo.Standing");
  // Every row before it agreed: the earlier phases of the tick, and every earlier tick.
  CHECK(d.rows_compared == (k_ranks_from - 1u) * sim::k_phase_count + 4u);

  // The second step: each side run again to that point, the table captured, the captures diffed.
  sim::Capture ca;
  sim::Capture cb;
  {
    World one(false);
    World two(true);
    sim::HashLog scratch_a;
    sim::HashLog scratch_b;
    one.hash_into(scratch_a);
    two.hash_into(scratch_b);
    REQUIRE(one.hash->capture_at(d.tick, d.phase, d.tables[0].table, &ca));
    REQUIRE(two.hash->capture_at(d.tick, d.phase, d.tables[0].table, &cb));
    one.run(d.tick);
    two.run(d.tick);
    CHECK(one.hash->stats().captures == 1);
  }
  CHECK(ca.entities.size() == k_entities);
  u64 differing = 0;
  const sim::CaptureDiff diff = sim::diff_captures(ca, cb, 64, &differing);
  CHECK(diff.compared == k_entities);
  // Ranks 0..32 against 32..0: every entity but the middle one.
  CHECK(differing == k_entities - 1u);
  REQUIRE_FALSE(diff.entities.empty());
  for (const sim::EntityDifference& e : diff.entities) {
    CHECK_FALSE(e.only_in_a);
    REQUIRE(e.fields.size() == 1);
    CHECK(e.fields[0].path == "wealth");
  }
  // The middle entity, rank 16 on both sides, is not listed.
  char hex[Id128::k_hex_length + 1];
  entity_id(k_entities / 2u).to_hex(hex);
  for (const sim::EntityDifference& e : diff.entities)
    CHECK(e.id != std::string(hex));
  // A capture survives its file form.
  sim::Capture back;
  REQUIRE(sim::capture_from_json(sim::capture_to_json(ca), back));
  CHECK(back.entities == ca.entities);
  CHECK(back.table == ca.table);
}

TEST_CASE("replay: a recorded run played again from its record reports nothing, one input changed") {
  test::TempDir dir;
  // Record: the player pushes at four ticks; the run writes its input log, its hash log and the
  // record that names them.
  {
    World world(false);
    input::InputLog inputs;
    inputs.set_map(world.map);
    for (const u64 t : {5u, 40u, 90u, 150u}) {
      inputs.record(input::RawEvent{SimTick{t}, input::Source::Key, 32u, 1.0f, 0u});
      inputs.record(input::RawEvent{SimTick{t + 1u}, input::Source::Key, 32u, 0.0f, 0u});
    }
    world.inputs = &inputs;
    sim::HashLog log;
    world.hash_into(log);
    world.run(180);
    REQUIRE(inputs.save(io::join_path(dir.path(), "input.jsonl")) == io::Status::Ok);
    REQUIRE(log.save(io::join_path(dir.path(), "hash.hashlog")));
    sim::RunRecord record;
    record.seed = k_seed;
    record.hz = 60;
    record.ticks = 180;
    record.input_log = "input.jsonl";
    record.hash_log = "hash.hashlog";
    std::string error;
    REQUIRE_MESSAGE(sim::write_run_record(dir.path(), record, &error), error);
  }

  // Replay: everything from the record.
  const auto replay = [&](bool tamper, sim::HashLog& out) {
    sim::RunRecord record;
    std::string error;
    REQUIRE_MESSAGE(sim::read_run_record(dir.path(), record, &error), error);
    input::InputLog inputs;
    REQUIRE(inputs.load(sim::run_file(dir.path(), record.input_log), &error) == io::Status::Ok);
    if (tamper) {
      input::InputLog changed;
      changed.set_map_hash(inputs.map_hash());
      for (input::RawEvent e : inputs.events()) {
        if (e.tick.value == 40u || e.tick.value == 41u) e.tick.value += 3u;  // a push late
        changed.record(e);
      }
      inputs = changed;
    }
    World world(false);
    REQUIRE(inputs.check_map(*world.state));
    world.inputs = &inputs;
    world.hash_into(out);
    world.run(record.ticks);
  };
  sim::RunRecord record;
  REQUIRE(sim::read_run_record(dir.path(), record));
  sim::HashLog recorded;
  REQUIRE(recorded.load(sim::run_file(dir.path(), record.hash_log)));

  sim::HashLog again;
  replay(false, again);
  const sim::Divergence clean = sim::first_divergence(recorded, again);
  CHECK_FALSE(clean.found);
  CHECK(clean.rows_compared == 180u * sim::k_phase_count);

  sim::HashLog tampered;
  replay(true, tampered);
  const sim::Divergence d = sim::first_divergence(recorded, tampered);
  REQUIRE(d.found);
  CHECK(d.tick == 40u);
  CHECK(d.phase == sim::TickPhase::Input);
  REQUIRE(d.tables.size() == 1);
  CHECK(d.tables[0].table == "engine.ecs.demo.Standing");
}

TEST_CASE("run record: its files are plain names beside it, and nothing else") {
  test::TempDir dir;
  sim::RunRecord record;
  record.ticks = 10;
  for (const char* bad : {"../escape.jsonl", "sub/dir.jsonl", "C:\\x.jsonl", "/abs.jsonl", ".."}) {
    record.input_log = bad;
    CHECK_FALSE(sim::write_run_record(dir.path(), record));
    CHECK(sim::run_file(dir.path(), bad).empty());
  }
  // A record written by hand with a path in it is refused at read.
  REQUIRE(io::write_file(io::join_path(dir.path(), "run.json"),
                         R"({"format":"engine.sim.run.v1","seed":1,"hz":60,"start_tick":0,)"
                         R"("start_time_us":0,"ticks":5,"hash_log":"../../x.hashlog"})") ==
          io::Status::Ok);
  sim::RunRecord read;
  std::string error;
  CHECK_FALSE(sim::read_run_record(dir.path(), read, &error));
  CHECK(error.find("hash_log") != std::string::npos);
}
