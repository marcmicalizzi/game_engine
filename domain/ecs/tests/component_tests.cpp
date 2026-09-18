// Seam 1 (ADR-0028): a component is declared once, in a `.schema` file, and the registration
// schemac generates is what the entity store knows about it.
//
// The components under test are compiled from `tests/ecs_demo.schema` by schemac like any other
// schema, because "components come from the IDL" is not a claim a test with hand-written structs
// could make.
#include <core/schema/json_reflect.h>
#include <core/schema/type_info.h>
#include <domain/ecs/components.h>
#include <domain/ecs/sim_world.h>

#include <doctest/doctest.h>

#include <cstddef>
#include <schemas/ecs_demo.h>
#include <schemas/ecs_demo_ecs.h>
#include <string>

using namespace engine;
using namespace engine::ecs;
namespace demo = engine::ecs::demo;

namespace {

const ecs_member_t* member_of(flecs::world& world, flecs::entity component, const char* name) {
  return ecs_struct_get_member(world.c_ptr(), component, name);
}

}  // namespace

TEST_CASE("ecs: schema components register under their qualified names") {
  SimWorld sim;
  flecs::world& world = sim.world();
  demo::register_ecs_demo_components(world);

  // The schema's dotted name is the world's name for the type: one spelling, wherever it is
  // read from. The flecs path is the same name with the namespace as flecs scopes, so the
  // explorer shows engine -> ecs -> demo -> Standing.
  const flecs::entity standing = lookup_component(world, "engine.ecs.demo.Standing");
  const flecs::entity perception = lookup_component(world, "engine.ecs.demo.Perception");
  REQUIRE(standing.is_valid());
  REQUIRE(perception.is_valid());
  CHECK(standing == world.component<demo::Standing>());
  CHECK(std::string(standing.path().c_str()) == "::engine::ecs::demo::Standing");

  // A record in the same schema file is not a component and was not registered.
  CHECK_FALSE(lookup_component(world, "engine.ecs.demo.DemoNote").is_valid());

  // Registration is idempotent: a second call finds what the first left.
  const u32 before = components(world).size();
  demo::register_ecs_demo_components(world);
  CHECK(components(world).size() == before);
}

TEST_CASE("ecs: the schema's transient marking reaches the entity store") {
  SimWorld sim;
  flecs::world& world = sim.world();
  demo::register_ecs_demo_components(world);

  // The tag is on the component entity, so "what would a save file contain" is a question that
  // can be answered by walking the world's components.
  CHECK_FALSE(lookup_component(world, "engine.ecs.demo.Standing").has<Transient>());
  CHECK(lookup_component(world, "engine.ecs.demo.Perception").has<Transient>());

  const ComponentRegistry& registry = components(world);
  const ComponentType* standing = registry.find("engine.ecs.demo.Standing");
  const ComponentType* perception = registry.find("engine.ecs.demo.Perception");
  REQUIRE(standing != nullptr);
  REQUIRE(perception != nullptr);
  CHECK_FALSE(standing->transient);
  CHECK(perception->transient);

  // And it came from the schema descriptor, not from anything flecs knows: the store and the
  // protocol read the same bit without linking an ECS.
  CHECK((schema::type_of<demo::Perception>().flags & schema::TypeFlag::transient) != 0);
  CHECK((schema::type_of<demo::Standing>().flags & schema::TypeFlag::transient) == 0);
  // The field-level marking is the narrower thing and still works inside a component.
  const schema::FieldInfo* scratch = schema::type_of<demo::Standing>().find_field("scratch");
  REQUIRE(scratch != nullptr);
  CHECK((scratch->flags & schema::FieldFlag::transient) != 0);
}

TEST_CASE("ecs: member reflection covers the fields flecs meta can describe") {
  SimWorld sim;
  flecs::world& world = sim.world();
  demo::register_ecs_demo_components(world);
  const flecs::entity standing = lookup_component(world, "engine.ecs.demo.Standing");

  // Offsets come from the schema descriptor, so flecs points at exactly the bytes the C++ type
  // has and no layout assumption is made anywhere.
  const ecs_member_t* wealth = member_of(world, standing, "wealth");
  REQUIRE(wealth != nullptr);
  CHECK(wealth->type == flecs::I64);
  CHECK(static_cast<usize>(wealth->offset) == offsetof(demo::Standing, wealth));

  const ecs_member_t* importance = member_of(world, standing, "importance");
  REQUIRE(importance != nullptr);
  CHECK(importance->type == flecs::F32);

  // A Vec3 is three floats at one offset, which is what makes it describable at all.
  const ecs_member_t* position = member_of(world, standing, "position");
  REQUIRE(position != nullptr);
  CHECK(position->type == flecs::F32);
  CHECK(position->count == 3);
  CHECK(static_cast<usize>(position->offset) == offsetof(demo::Standing, position));

  // An Id128 is two u64.
  const flecs::entity perception = lookup_component(world, "engine.ecs.demo.Perception");
  const ecs_member_t* nearest = member_of(world, perception, "nearest");
  REQUIRE(nearest != nullptr);
  CHECK(nearest->type == flecs::U64);
  CHECK(nearest->count == 2);

  // A std::string is not describable to flecs and is left out rather than described wrongly.
  // core/schema remains the authority on it, which is where the JSON path reads it.
  const flecs::entity label = lookup_component(world, "engine.ecs.demo.Label");
  CHECK(member_of(world, label, "text") == nullptr);
  CHECK(member_of(world, label, "rank") != nullptr);
}

TEST_CASE("ecs: a component knows whether it is its own bytes") {
  SimWorld sim;
  flecs::world& world = sim.world();
  demo::register_ecs_demo_components(world);
  const ComponentRegistry& registry = components(world);

  // Flat: every field, recursively, is a scalar or a fixed array of scalars, so the component is
  // exactly its bytes and `WorldCommands::set_bytes` may copy one in.
  REQUIRE(registry.find("engine.ecs.demo.Standing") != nullptr);
  CHECK(registry.find("engine.ecs.demo.Standing")->flat);
  CHECK(registry.find("engine.ecs.demo.Perception")->flat);
  // Not flat: the string owns a heap allocation, and its bytes are a pointer.
  CHECK_FALSE(registry.find("engine.ecs.demo.Label")->flat);

  CHECK(registry.find("engine.ecs.demo.Standing")->size == sizeof(demo::Standing));
}

TEST_CASE("ecs: components have an index space the scheduler's masks address") {
  SimWorld sim;
  flecs::world& world = sim.world();
  demo::register_ecs_demo_components(world);

  const u32 standing = component_index<demo::Standing>(world);
  const u32 label = component_index<demo::Label>(world);
  REQUIRE(standing != k_invalid_component_index);
  REQUIRE(label != k_invalid_component_index);
  CHECK(standing != label);
  CHECK(components(world).overflowed() == 0);

  // Indices are what `sim::ComponentMask` holds, so a system's declared sets and the world's
  // components are the same vocabulary.
  const sim::ComponentMask mask = mask_of<demo::Standing, demo::Label>(world);
  CHECK(mask.test(standing));
  CHECK(mask.test(label));
  CHECK_FALSE(mask.test(component_index<demo::Perception>(world)));
  CHECK(mask_of<>(world).any() == false);
}

TEST_CASE("ecs: a module-private component is registered as transient and unnameable") {
  struct ScratchCache {
    u32 generation = 0;
  };
  SimWorld sim;
  flecs::world& world = sim.world();

  const flecs::entity cache = register_private_component<ScratchCache>(world, "demo_scratch");
  CHECK(cache.has<Transient>());
  // It has no schema descriptor, so nothing outside the module can name it: `WorldCommands`
  // looks types up by qualified name and there is no name to find.
  const ComponentType* type = components(world).find(cache.id());
  REQUIRE(type != nullptr);
  CHECK(type->info == nullptr);
  CHECK(type->transient);
  CHECK(components(world).find("demo_scratch") == nullptr);
}

TEST_CASE("ecs: a registered component still round-trips through core/schema's JSON") {
  // The point of seam 1: one type, two readers. flecs iterates it, core/schema serializes it,
  // and neither had to be told about the other.
  SimWorld sim;
  demo::register_ecs_demo_components(sim.world());

  demo::Standing written;
  written.position = Vec3{1.0f, 2.0f, 3.0f};
  written.wealth = -42;
  written.importance = 0.5f;
  written.faction = 7;
  written.scratch = 99;

  const JsonValue json = schema::to_json(written);
  demo::Standing read;
  schema::ReadContext context;
  REQUIRE(schema::from_json(read, json, context));
  CHECK(context.ok());
  CHECK(read.position == written.position);
  CHECK(read.wealth == written.wealth);
  CHECK(read.faction == written.faction);
  // Transient fields are not serialized, so `scratch` comes back at its default.
  CHECK(read.scratch == 0);
}
