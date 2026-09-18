// Seam 4 (ADR-0028): mutation from outside a system is queued by `Id128` and by schema type
// name, validated where it is queued, and applied at a phase boundary.
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/ecs/components.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/sim_world.h>
#include <domain/ecs/world_commands.h>

#include <doctest/doctest.h>

#include <schemas/ecs_demo.h>
#include <schemas/ecs_demo_ecs.h>
#include <string>

using namespace engine;
using namespace engine::ecs;
namespace demo = engine::ecs::demo;

namespace {

JsonValue parse(const char* text) {
  JsonValue value;
  const JsonParseResult result = parse_json(text, value);
  REQUIRE_MESSAGE(result.ok, result.message);
  return value;
}

}  // namespace

TEST_CASE("ecs: a batch creates, sets from JSON, removes and destroys by Id128") {
  SimWorld sim;
  flecs::world& world = sim.world();
  demo::register_ecs_demo_components(world);
  WorldCommands commands(world);

  IdGenerator ids(0xc0de);
  const Id128 alpha = ids.next();
  const Id128 beta = ids.next();

  commands.create(alpha);
  commands.create(beta);
  CHECK(commands.set_json(alpha, "engine.ecs.demo.Standing",
                          parse(R"({"position":[1,2,3],"wealth":42,"importance":0.5,
                                    "faction":7})")));
  CHECK(
      commands.set_json(beta, "engine.ecs.demo.Label", parse(R"({"text":"ironbound","rank":3})")));
  CHECK(commands.pending() == 4);
  // Nothing has happened yet: the queue is a queue.
  CHECK_FALSE(entity_for(world, alpha).is_valid());

  const CommandStats stats = commands.apply();
  CHECK(stats.created == 2);
  CHECK(stats.set == 2);
  CHECK(stats.failed == 0);
  CHECK(commands.pending() == 0);

  const flecs::entity entity = entity_for(world, alpha);
  REQUIRE(entity.is_valid());
  const demo::Standing* standing = entity.try_get<demo::Standing>();
  REQUIRE(standing != nullptr);
  CHECK(standing->position == Vec3{1.0f, 2.0f, 3.0f});
  CHECK(standing->wealth == 42);
  CHECK(standing->faction == 7);

  // A component with a heap-owning field arrives through the same path.
  const demo::Label* label = entity_for(world, beta).try_get<demo::Label>();
  REQUIRE(label != nullptr);
  CHECK(label->text == "ironbound");
  CHECK(label->rank == 3);

  // And out again: what flecs holds is what core/schema serializes, which is seam 1's claim
  // checked from the far end.
  const JsonValue rendered = schema::to_json(*standing);
  demo::Standing round_tripped;
  schema::ReadContext context;
  REQUIRE(schema::from_json(round_tripped, rendered, context));
  CHECK(round_tripped.wealth == 42);
  CHECK(round_tripped.position == standing->position);

  commands.remove(alpha, "engine.ecs.demo.Standing");
  commands.destroy(beta);
  const CommandStats second = commands.apply();
  CHECK(second.removed == 1);
  CHECK(second.destroyed == 1);
  CHECK(entity_for(world, alpha).try_get<demo::Standing>() == nullptr);
  CHECK_FALSE(entity_for(world, beta).is_valid());
  CHECK(identity_map(world).size() == 1);
}

TEST_CASE("ecs: a bad payload fails where it was queued, not where it is applied") {
  SimWorld sim;
  flecs::world& world = sim.world();
  demo::register_ecs_demo_components(world);
  WorldCommands commands(world);
  const Id128 id = Id128::from_seed(1, 1);
  commands.create(id);

  // A type this world does not have.
  CHECK_FALSE(commands.set_json(id, "engine.ecs.demo.Nonesuch", parse("{}")));
  // A record that is not a component was never registered, so it cannot be set either.
  CHECK_FALSE(commands.set_json(id, "engine.ecs.demo.DemoNote", parse(R"({"text":"x"})")));

  // JSON that does not fit the type, with the field path core/schema produces.
  Vector<schema::Diagnostic> diagnostics;
  CHECK_FALSE(commands.set_json(id, "engine.ecs.demo.Standing",
                                parse(R"({"wealth":"not a number"})"), &diagnostics));
  REQUIRE(diagnostics.size() >= 1);
  CHECK(diagnostics[0].path == "wealth");

  // None of the three reached the queue, so apply() cannot fail on a parse.
  CHECK(commands.pending() == 1);
  const CommandStats stats = commands.apply();
  CHECK(stats.created == 1);
  CHECK(stats.failed == 0);
}

TEST_CASE("ecs: bytes are accepted only for a component that is its own bytes") {
  SimWorld sim;
  flecs::world& world = sim.world();
  demo::register_ecs_demo_components(world);
  WorldCommands commands(world);
  const Id128 id = Id128::from_seed(2, 1);
  commands.create(id);
  commands.apply();

  demo::Standing source;
  source.position = Vec3{4.0f, 5.0f, 6.0f};
  source.wealth = -9;
  source.scratch = 123;
  const auto* raw = reinterpret_cast<const u8*>(&source);

  CHECK(commands.set_bytes(id, "engine.ecs.demo.Standing", {raw, sizeof(source)}));
  // The wrong size is refused rather than read past.
  CHECK_FALSE(commands.set_bytes(id, "engine.ecs.demo.Standing", {raw, sizeof(source) - 1}));
  // A component holding a std::string is a pointer in another process's heap, whatever the
  // caller believes; refusing is the only correct answer.
  demo::Label label;
  CHECK_FALSE(commands.set_bytes(id, "engine.ecs.demo.Label",
                                 {reinterpret_cast<const u8*>(&label), sizeof(label)}));

  CHECK(commands.apply().set == 1);
  const demo::Standing* stored = entity_for(world, id).try_get<demo::Standing>();
  REQUIRE(stored != nullptr);
  CHECK(stored->position == source.position);
  CHECK(stored->wealth == -9);
  // Bytes are bytes: a transient *field* is not stripped by a byte copy, which is one more
  // reason the JSON path is the one the protocol uses.
  CHECK(stored->scratch == 123);
}

TEST_CASE("ecs: a command for an entity that is gone is counted, not fatal") {
  SimWorld sim;
  flecs::world& world = sim.world();
  demo::register_ecs_demo_components(world);
  WorldCommands commands(world);
  const Id128 missing = Id128::from_seed(4, 1);

  // Queuing succeeds — the type is real — and application reports what it could not do. There is
  // no rollback: the event log is what makes an edit undoable (ADR-0003), not this queue.
  CHECK(commands.set_json(missing, "engine.ecs.demo.Standing", parse("{}")));
  commands.destroy(missing);
  commands.remove(missing, "engine.ecs.demo.Standing");
  const CommandStats stats = commands.apply();
  CHECK(stats.failed == 3);
  CHECK(stats.set == 0);

  // A queue that is never applied still frees what it parsed.
  CHECK(commands.set_json(missing, "engine.ecs.demo.Label", parse(R"({"text":"leak me"})")));
  commands.clear();
  CHECK(commands.pending() == 0);
}

TEST_CASE("ecs: installed commands land on a phase boundary") {
  SimWorld sim;
  flecs::world& world = sim.world();
  demo::register_ecs_demo_components(world);
  WorldCommands commands(world);
  commands.install(sim, TickPhase::EventsIn);

  const Id128 id = Id128::from_seed(5, 1);
  commands.create(id);
  commands.set_json(id, "engine.ecs.demo.Standing", parse(R"({"wealth":5})"));

  // A system in a later phase of the same tick sees the whole batch, and never half of it.
  u32 seen = 0;
  i64 wealth = 0;
  world.system<const demo::Standing>("reader")
      .kind(sim.phase(TickPhase::Systems))
      .each([&seen, &wealth](const demo::Standing& standing) {
        ++seen;
        wealth = standing.wealth;
      });

  CHECK(commands.pending() == 2);
  sim.step();
  CHECK(commands.pending() == 0);
  CHECK(seen == 1);
  CHECK(wealth == 5);

  // The next tick applies an empty queue and changes nothing.
  sim.step();
  CHECK(seen == 2);
}
