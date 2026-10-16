// muscle_sim capability (docs/subsystems/muscle_sim.md): `LineActuator` records through
// materialization and the validator's gate, the scheduler entry ticking them through the
// supervisor, the outputs and events, and the same bytes at one worker and at four.
#include "two_bone.h"

#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <core/schema/materialize.h>
#include <domain/doc/document.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/materialize.h>
#include <domain/ecs/os_api.h>
#include <domain/ecs/scheduled_tick.h>
#include <domain/ecs/sim_world.h>
#include <domain/ecs/systems.h>
#include <domain/sim/materialize.h>
#include <systems/muscle/supervisor.h>
#include <systems/muscle_sim/muscle_sim.h>

#include <doctest/doctest.h>

#include <bit>
#include <cstdio>
#include <schemas/muscle_ecs.h>
#include <schemas/world_ecs.h>
#include <string>

using namespace engine;
using namespace engine::muscle_sim;

namespace {

constexpr const char* k_qualified =
    "9665d9f3c53dfae5861a5318536d6625b973eaed432fd67c533d4627cd5c1ed6";

const Id128 k_arm = Id128{0x6d73, 1};        // a character that bends its elbow
const Id128 k_stretched = Id128{0x6d73, 2};  // a character whose bone is lengthened at tick 20
const Id128 k_flexor = Id128{0x6d73, 11};
const Id128 k_extensor = Id128{0x6d73, 12};  // routed through a via point
const Id128 k_forged = Id128{0x6d73, 13};    // an identity outside the qualified domain
const Id128 k_torn = Id128{0x6d73, 21};

JsonValue attachment(const char* bone, Vec3 offset) {
  JsonValue a = JsonValue::object();
  a.set("bone", JsonValue(bone));
  JsonValue o = JsonValue::array();
  o.push_back(JsonValue(static_cast<double>(offset.x)));
  o.push_back(JsonValue(static_cast<double>(offset.y)));
  o.push_back(JsonValue(static_cast<double>(offset.z)));
  a.set("offset", std::move(o));
  return a;
}

void add_actuator(doc::Document& d, const Id128& id, const Id128& character, const char* sha,
                  double excitation, bool via) {
  JsonValue props = JsonValue::object();
  props.set("parameter_sha256", JsonValue(sha));
  props.set("origin", attachment("upper", test::k_origin));
  props.set("insertion", attachment("lower", test::k_insertion));
  if (via) {
    JsonValue v = JsonValue::array();
    v.push_back(attachment("upper", test::k_via));
    props.set("via", std::move(v));
  }
  props.set("excitation", JsonValue(excitation));
  REQUIRE(d.apply(doc::cmd_create(id, "engine.muscle.LineActuator", character, std::move(props)),
                  nullptr, nullptr));
}

void build_document(doc::Document& d) {
  d.add_layer("base", doc::LayerRole::Base);
  for (const Id128& character : {k_arm, k_stretched})
    REQUIRE(d.apply(doc::cmd_create(character, "engine.world.Node", Id128{}, JsonValue::object()),
                    nullptr, nullptr));
  add_actuator(d, k_flexor, k_arm, k_qualified, 0.3, false);
  add_actuator(d, k_extensor, k_arm, k_qualified, 0.6, true);
  add_actuator(d, k_forged, k_arm,
               "00000000000000000000000000000000000000000000000000000000000000ff", 0.3, false);
  add_actuator(d, k_torn, k_stretched, k_qualified, 0.3, false);
}

// One world: the document materialized through the entity store's hook and this capability's,
// the characters given their motions, and `ticks` ticks of the engine's scheduler at `workers`.
struct World {
  test::TwoBoneSource poses;
  jobs::JobSystem jobs{[] {
    jobs::JobSystemConfig c;
    c.performance_workers = 4;
    c.efficiency_workers = 1;
    return c;
  }()};
  ecs::JobOsApi adapter{jobs};
  ecs::SimWorld sim;
  MuscleSimSystem muscles{poses.source(), MuscleSimConfig{16, 8}};
  sim::SimScheduler scheduler;
  ecs::RecordMaterializer records{sim.world()};
  sim::MaterializeReport report;

  explicit World(u32 workers) {
    world::register_world_components(sim.world());
    muscles.install(sim);
    scheduler.add_hooks(records.hooks());
    scheduler.add_hooks(muscles.hooks());
    doc::Document d;
    build_document(d);
    sim::MaterializeConfig config;
    config.tier = 0;
    sim::Materializer driver(scheduler, config);
    driver.set_target(records.target());
    report = driver.materialize(d);
    poses.characters.push_back(ecs::entity_for(sim.world(), k_arm).id());
    poses.motions.push_back(test::Motion{});
    test::Motion stretch;
    stretch.amplitude = 0.0;
    stretch.extension = 0.055f;
    stretch.extend_at = 20;
    poses.characters.push_back(ecs::entity_for(sim.world(), k_stretched).id());
    poses.motions.push_back(stretch);
    if (workers > 1) REQUIRE(ecs::set_workers(sim.world(), workers));
  }
  ~World() { ecs::stop_workers(sim.world()); }

  const muscle::LineOutput& output(const Id128& id) {
    return ecs::entity_for(sim.world(), id).get<muscle::LineOutput>();
  }
};

u64 hash_world(World& w) {
  u64 h = k_hash_seed;
  for (const Id128& id : {k_flexor, k_extensor, k_torn}) {
    const muscle::LineOutput& o = w.output(id);
    h = hash_combine(h, std::bit_cast<u64>(o.tendon_force));
    h = hash_combine(h, std::bit_cast<u64>(o.path_length));
    h = hash_combine(h, static_cast<u64>(o.tier));
    muscle::LineState s{};
    REQUIRE(w.muscles.save(id, s));
    h = hash_combine(h, std::bit_cast<u64>(s.activation));
    h = hash_combine(h, std::bit_cast<u64>(s.fiber_length));
    h = hash_combine(h, std::bit_cast<u64>(s.committed_forces.tendon_force));
    h = hash_combine(h, s.last_commit_tick);
  }
  return h;
}

}  // namespace

TEST_CASE(
    "muscle_sim: the scheduler entry is one Physics system and the hooks gate on the "
    "validator") {
  World w(1);
  const sim::SystemDesc& desc = w.muscles.descriptor();
  CHECK(std::string(desc.name) == "muscle_sim.tick");
  CHECK(desc.phase == sim::TickPhase::Physics);
  CHECK(desc.determinism == sim::Determinism::Hashed);
  CHECK(desc.reads.test(ecs::component_index<muscle::LineActuatorSpec>(w.sim.world())));
  CHECK(desc.reads.test(ecs::component_index<muscle::LineExcitation>(w.sim.world())));
  CHECK(desc.writes.test(ecs::component_index<muscle::LineOutput>(w.sim.world())));
  CHECK(ecs::systems(w.sim.world()).count_in(sim::TickPhase::Physics) == 1);

  // Four records materialize as entities (the entity store's hook makes them); three are
  // actuators, and the forged identity is a document-level validation failure that never ticks.
  const schema::MaterializeInfo* mapping =
      schema::MaterializeRegistry::global().find("engine.muscle.LineActuator");
  REQUIRE(mapping != nullptr);
  CHECK(mapping->parent == schema::MaterializeParent::ChildOf);
  CHECK(w.report.created == 6);
  CHECK(w.muscles.stats().actuators == 3);
  REQUIRE(w.muscles.refusals().size() == 1);
  const RecordRefusal& refused = w.muscles.refusals()[0];
  CHECK(refused.record == k_forged);
  CHECK(refused.result.refusal == muscle::Refusal::DomainUnqualified);
  const flecs::entity forged = ecs::entity_for(w.sim.world(), k_forged);
  REQUIRE(forged.is_valid());
  CHECK(forged.has<muscle::LineActuatorSpec>());
  CHECK_FALSE(forged.has<muscle::LineOutput>());
  CHECK(w.muscles.slot_of(k_forged) == MuscleSimSystem::k_no_slot);
  CHECK(w.muscles.slot_of(k_flexor) != MuscleSimSystem::k_no_slot);
  const muscle::LineActuatorSpec& spec =
      ecs::entity_for(w.sim.world(), k_extensor).get<muscle::LineActuatorSpec>();
  CHECK(spec.via.size() == 1);
  CHECK(spec.insertion.bone == "lower");
  CHECK(ecs::entity_for(w.sim.world(), k_extensor).get<muscle::LineExcitation>().value == 0.6);
}

TEST_CASE(
    "muscle_sim: sixty ticks through the scheduler produce forces, a degradation in the "
    "component, delivered events, and the same bytes at one worker and four") {
  u64 hashes[2] = {};
  const u32 counts[2] = {1, 4};
  for (u32 run = 0; run < 2; ++run) {
    CAPTURE(counts[run]);
    World w(counts[run]);
    ecs::ScheduledTick tick(w.sim, w.scheduler);
    u32 degraded_at = 0;
    u32 changed_at = 0;
    u32 failures = 0;
    for (u32 t = 1; t <= 60; ++t) {
      tick.step();
      for (const muscle::ValidationEvent& e : w.muscles.events()) {
        CHECK(e.scheduler_tick == t);
        CHECK(e.actuator_id == k_torn);  // the healthy two emit nothing
        if (e.kind == muscle::ValidationKind::TierDegraded) degraded_at = t;
        if (e.kind == muscle::ValidationKind::TierChanged) changed_at = t;
        if (e.kind == muscle::ValidationKind::DomainFailure ||
            e.kind == muscle::ValidationKind::RootFailure)
          ++failures;
      }
      if (t == 25) CHECK(w.output(k_torn).forces_stale);
    }
    CHECK(w.sim.tick().value == 60);
    // The two healthy actuators produce force from their routed, moving paths.
    for (const Id128& id : {k_flexor, k_extensor}) {
      const muscle::LineOutput& o = w.output(id);
      CHECK(o.force_output);
      CHECK(o.tendon_force > 1.0);
      CHECK_FALSE(o.forces_stale);
      CHECK(o.tier == muscle::LineTier::Compliant);
      CHECK(o.path_length > 0.13);
      CHECK(o.path_length < 0.16);
    }
    // The stretched one: its path left the law's domain at tick 20, eight frozen ticks degraded
    // it, and the projection at the next tick's start made it rigid, which the component says.
    const muscle::LineOutput& torn = w.output(k_torn);
    CHECK(failures == 8);
    CHECK(degraded_at == 27);
    CHECK(changed_at == 28);
    CHECK(torn.tier == muscle::LineTier::Rigid);
    CHECK(torn.force_output);
    CHECK(torn.tendon_force > 0.0);
    CHECK(torn.path_length > 0.19);
    CHECK(w.muscles.stats().rigid == 1);
    CHECK(w.muscles.stats().ticked == 3);
    hashes[run] = hash_world(w);
    std::printf("muscle_sim: 60 ticks at %u worker(s): forces %.4f %.4f %.4f N, hash 0x%016llx\n",
                counts[run], w.output(k_flexor).tendon_force, w.output(k_extensor).tendon_force,
                torn.tendon_force, static_cast<unsigned long long>(hashes[run]));
  }
  CHECK(hashes[0] == hashes[1]);
}

TEST_CASE("muscle_sim: a saved actuator restored into another world ticks the same bytes") {
  World a(1);
  World b(1);
  ecs::ScheduledTick ta(a.sim, a.scheduler);
  ecs::ScheduledTick tb(b.sim, b.scheduler);
  for (u32 t = 1; t <= 10; ++t)
    ta.step();
  // The second world reaches tick 10 with nothing ticked that matters: its actuators are
  // overwritten from the first world's records and path histories.
  for (u32 t = 1; t <= 10; ++t)
    tb.step();
  for (const Id128& id : {k_flexor, k_extensor, k_torn}) {
    muscle::LineState s{};
    PathHistory h;
    REQUIRE(a.muscles.save(id, s, &h));
    REQUIRE(b.muscles.restore(id, s, &h));
  }
  for (u32 t = 11; t <= 40; ++t) {
    ta.step();
    tb.step();
    REQUIRE(hash_world(a) == hash_world(b));
  }
}
