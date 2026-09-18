// Seam 2 (ADR-0028): a system declares itself to the engine and its body to flecs, in one call,
// and a debug build checks the declaration against the query.
#include <domain/ecs/components.h>
#include <domain/ecs/sim_world.h>
#include <domain/ecs/systems.h>
#include <domain/sim/scheduler.h>

#include <doctest/doctest.h>

#include <schemas/ecs_demo.h>
#include <schemas/ecs_demo_ecs.h>
#include <string>

using namespace engine;
using namespace engine::ecs;
namespace demo = engine::ecs::demo;

TEST_CASE("ecs: the phase enum is the scheduler's own") {
  // Not "the same enumerators in the same order" — the same type. A capability that fills in
  // SystemDesc::phase and a body that attaches to a flecs phase entity now cannot disagree.
  static_assert(std::is_same_v<ecs::TickPhase, sim::TickPhase>);
  CHECK(static_cast<u32>(TickPhase::Count) == sim::k_phase_count);
  CHECK(std::string(sim::phase_name(TickPhase::Lod)) == "lod");
  CHECK(std::string(phase_entity_name(TickPhase::Lod)) == "sim_lod");
}

TEST_CASE("ecs: register_system records the declaration and builds the flecs system") {
  SimWorld sim;
  flecs::world& world = sim.world();
  demo::register_ecs_demo_components(world);

  for (int i = 0; i < 8; ++i)
    world.entity().set<demo::Standing>(demo::Standing{});

  u32 visited = 0;
  sim::SystemDesc desc;
  desc.name = "demo_accrue";
  desc.phase = TickPhase::Systems;
  desc.writes = mask_of<demo::Standing>(world);
  desc.determinism = sim::Determinism::Hashed;
  desc.tiers = 0x03;

  const flecs::system system =
      register_system(sim, desc, [&visited](flecs::world& w, flecs::entity phase) {
        return w.system<demo::Standing>("demo_accrue")
            .kind(phase)
            .each([&visited](demo::Standing& standing) {
              standing.wealth += 1;
              ++visited;
            });
      });

  // The flecs system is flecs': it landed in the phase the descriptor named and it runs.
  REQUIRE(system.is_valid());
  CHECK(system.has(flecs::DependsOn, sim.phase(TickPhase::Systems)));
  sim.step();
  CHECK(visited == 8);

  // The declaration is the engine's, and it is kept whole — this is the table the engine's own
  // scheduler will read when it takes the tick over.
  const SystemRegistry& registry = systems(world);
  REQUIRE(registry.size() == 1);
  const RegisteredSystem* row = registry.find(system.id());
  REQUIRE(row != nullptr);
  CHECK(std::string(row->desc.name) == "demo_accrue");
  CHECK(row->desc.phase == TickPhase::Systems);
  CHECK(row->desc.tiers == 0x03);
  CHECK(row->desc.determinism == sim::Determinism::Hashed);
  CHECK(row->desc.writes == desc.writes);
  CHECK(registry.count_in(TickPhase::Systems) == 1);
  CHECK(registry.count_in(TickPhase::Physics) == 0);
}

TEST_CASE("ecs: the query's terms are read back as reads and writes") {
  SimWorld sim;
  flecs::world& world = sim.world();
  demo::register_ecs_demo_components(world);

  sim::SystemDesc desc;
  desc.name = "demo_observe";
  desc.phase = TickPhase::Systems;
  desc.reads = mask_of<demo::Standing>(world);
  desc.writes = mask_of<demo::Perception>(world);

  const flecs::system system = register_system(sim, desc, [](flecs::world& w, flecs::entity phase) {
    return w.system<demo::Perception, const demo::Standing>("demo_observe")
        .kind(phase)
        .each([](demo::Perception&, const demo::Standing&) {});
  });

  const QueryAccess access = query_access(world, system.id());
  const u32 standing = component_index<demo::Standing>(world);
  const u32 perception = component_index<demo::Perception>(world);

  // `const T` is an `In` term; a non-const one is flecs' default, which means the system may
  // write, so the check reads it as a write. That conservative reading is the point: a
  // declaration that omits the write is exactly the bug worth finding.
  CHECK(access.reads.test(standing));
  CHECK_FALSE(access.writes.test(standing));
  CHECK(access.writes.test(perception));
  CHECK(access.reads.test(perception));
  CHECK(access.pair_terms == 0);
  CHECK(access.unregistered_terms == 0);
}

TEST_CASE("ecs: a relationship term is counted, not masked") {
  SimWorld sim;
  flecs::world& world = sim.world();
  demo::register_ecs_demo_components(world);

  struct MemberOf {};
  const flecs::entity faction = world.entity("ironbound");

  sim::SystemDesc desc;
  desc.name = "demo_faction";
  desc.phase = TickPhase::Systems;
  desc.reads = mask_of<demo::Standing>(world);

  const flecs::system system =
      register_system(sim, desc, [faction](flecs::world& w, flecs::entity phase) {
        return w.system<const demo::Standing>("demo_faction")
            .with<MemberOf>(faction)
            .kind(phase)
            .each([](const demo::Standing&) {});
      });

  // `sim::ComponentMask` addresses plain components by index and has no room for a pair, so the
  // check counts relationship terms and says nothing about them rather than guessing. Closing
  // that gap is the scheduler's design, not this seam's.
  const QueryAccess access = query_access(world, system.id());
  CHECK(access.reads.test(component_index<demo::Standing>(world)));
  CHECK(access.pair_terms >= 1);
}

TEST_CASE("ecs: over-declaring is allowed and under-declaring is not") {
  SimWorld sim;
  flecs::world& world = sim.world();
  demo::register_ecs_demo_components(world);

  // Declaring more than the query mentions is how a system tells the schedule about what it
  // touches *outside* its terms — a singleton, a pool it writes through an index, a component it
  // adds. It costs parallelism and is visible in the schedule hash; it is never an error.
  sim::SystemDesc desc;
  desc.name = "demo_over";
  desc.phase = TickPhase::Systems;
  desc.reads = mask_of<demo::Standing, demo::Label>(world);
  desc.writes = mask_of<demo::Perception>(world);

  const flecs::system system = register_system(sim, desc, [](flecs::world& w, flecs::entity phase) {
    return w.system<demo::Perception>("demo_over").kind(phase).each([](demo::Perception&) {});
  });
  REQUIRE(system.is_valid());

  const QueryAccess access = query_access(world, system.id());
  CHECK_FALSE(access.reads.test(component_index<demo::Standing>(world)));
  CHECK(access.writes.test(component_index<demo::Perception>(world)));

  // The other direction is what a debug build asserts on, and an assert terminates rather than
  // throws, so the check itself is what this asserts against: the query names a component the
  // declaration does not, and `query_access` is the function the assert reads.
  sim::SystemDesc under;
  under.name = "demo_under";
  under.phase = TickPhase::Systems;
  under.writes = mask_of<demo::Perception>(world);
  const QueryAccess would_be = [&] {
    const flecs::system probe = world.system<demo::Perception, demo::Standing>("demo_probe")
                                    .kind(sim.phase(TickPhase::Systems))
                                    .each([](demo::Perception&, demo::Standing&) {});
    return query_access(world, probe.id());
  }();
  bool undeclared_write = false;
  for (u32 word = 0; word < sim::k_component_mask_words; ++word)
    undeclared_write |= (would_be.writes.words[word] & ~under.writes.words[word]) != 0;
  CHECK(undeclared_write);
}
