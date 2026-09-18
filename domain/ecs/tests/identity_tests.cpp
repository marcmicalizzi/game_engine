// Seam 3 (ADR-0028): persistent identity is `Id128`, and the map from it to a flecs entity is
// maintained by the component's own observers, so it is right however the entity went away.
#include <core/ids/id128.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/sim_world.h>

#include <doctest/doctest.h>

using namespace engine;
using namespace engine::ecs;

TEST_CASE("ecs: an Id128 names an entity in both directions") {
  SimWorld sim;
  flecs::world& world = sim.world();

  IdGenerator ids(0x5eed);
  const Id128 first = ids.next();
  const Id128 second = ids.next();

  const flecs::entity a = create_entity(world, first);
  const flecs::entity b = create_entity(world, second);
  REQUIRE(a.is_valid());
  REQUIRE(b != a);

  CHECK(entity_for(world, first) == a);
  CHECK(entity_for(world, second) == b);
  // The reverse direction is the component, not a second map: an entity carries its own name.
  CHECK(id_of(a) == first);
  CHECK(id_of(b) == second);
  CHECK(identity_map(world).size() == 2);

  // Creating the same id twice is the same entity, which is what a replayed event log needs.
  CHECK(create_entity(world, first) == a);
  CHECK(identity_map(world).size() == 2);

  // An entity with no Identity has no persistent name and is not in the map.
  const flecs::entity anonymous = world.entity();
  CHECK(id_of(anonymous).is_null());
  CHECK(identity_map(world).size() == 2);
}

TEST_CASE("ecs: the map follows the component, however the entity goes away") {
  SimWorld sim;
  flecs::world& world = sim.world();
  IdGenerator ids(7);

  const Id128 destroyed = ids.next();
  const Id128 stripped = ids.next();
  const Id128 raw = ids.next();

  create_entity(world, destroyed);
  const flecs::entity b = create_entity(world, stripped);
  // A raw flecs set joins the map too: that is what makes this a seam and not a convention the
  // reviews have to enforce.
  world.entity().set<Identity>(Identity{raw});
  CHECK(identity_map(world).size() == 3);

  // Destroyed through flecs, by something that never heard of the map.
  entity_for(world, destroyed).destruct();
  CHECK(identity_map(world).find(destroyed) == 0);
  CHECK_FALSE(entity_for(world, destroyed).is_valid());

  // Or the component simply removed.
  b.remove<Identity>();
  CHECK(identity_map(world).find(stripped) == 0);
  CHECK(identity_map(world).size() == 1);
  CHECK(identity_map(world).find(raw) != 0);
}

TEST_CASE("ecs: deferred creation and destruction reach the map at the merge") {
  SimWorld sim;
  flecs::world& world = sim.world();
  IdGenerator ids(11);
  const Id128 id = ids.next();

  world.defer_begin();
  world.entity().set<Identity>(Identity{id});
  // The command has not merged yet, so the observer has not run.
  CHECK(identity_map(world).find(id) == 0);
  world.defer_end();
  CHECK(identity_map(world).find(id) != 0);

  world.defer_begin();
  entity_for(world, id).destruct();
  world.defer_end();
  CHECK(identity_map(world).find(id) == 0);
}

TEST_CASE("ecs: two entities claiming one id are counted rather than silently merged") {
  SimWorld sim;
  flecs::world& world = sim.world();
  const Id128 id = Id128::from_seed(3, 1);

  const flecs::entity first = create_entity(world, id);
  const flecs::entity second = world.entity();
  second.set<Identity>(Identity{id});

  // A duplicate identity is a content or protocol bug, not an engine one, so it is reported and
  // the world stays usable: the last writer holds the row.
  CHECK(identity_map(world).collisions() == 1);
  CHECK(identity_map(world).find(id) == second.id());

  // And the loser leaving does not evict the winner's row.
  first.destruct();
  CHECK(identity_map(world).find(id) == second.id());
}

TEST_CASE("ecs: the materialization bridge is the one place a runtime id is a number") {
  // `sim::MaterializationHooks` names a live entity with an opaque `sim::EntityHandle` and a
  // persistent record with an `Id128` (ADR-0028 seam 3). These three functions are where a flecs
  // entity id becomes one and stops being one, and nothing outside this module may look inside.
  SimWorld sim;
  flecs::world& world = sim.world();
  const Id128 id = Id128::from_seed(7, 1);

  const flecs::entity entity = create_entity(world, id);
  const sim::EntityHandle handle = handle_of(entity);
  CHECK_FALSE(handle.is_null());
  CHECK(handle == handle_for(world, id));
  CHECK(entity_of(world, handle) == entity);

  // A handle kept past its tick names an entity that is gone. It comes back as entity 0 — the same
  // "not found" `entity_for` returns — rather than asserting, which is what `is_alive()` would do
  // on entity 0 and what made the first version of this crash.
  entity.destruct();
  CHECK(entity_of(world, handle).id() == 0);
  CHECK(entity_of(world, sim::EntityHandle{}).id() == 0);
  CHECK(handle_for(world, id).is_null());
  CHECK(handle_for(world, Id128::from_seed(7, 2)).is_null());
}
