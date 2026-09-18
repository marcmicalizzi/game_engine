#pragma once

// Systems register through the engine's descriptor (ADR-0028 seam 2).
//
// `register_system(sim, desc, make)` does two things in one call. `desc` is `sim::SystemDesc` —
// the phase, the read and write component sets, the LOD tiers, the determinism stance — declared
// to the *engine*; `make` builds the flecs system, with flecs' own builder, flecs' terms and
// flecs' `each`/`run`. Neither half is wrapped and neither is optional.
//
// **Why the declaration is not derived from the query.** It could be, for the terms; it could
// not be for phase, tiers or determinism, and it must not be for what a system touches outside
// its terms — a singleton it reads, a pool it writes through an index, a relationship it adds.
// Those are exactly the accesses a scheduler has to know about and exactly the ones a query does
// not mention. So the declaration is written, and what the query *does* say is checked against
// it in debug builds, which catches the common half of the mistake for free.
//
// **What the check asserts, and what it deliberately does not.** A term the system has and the
// descriptor does not is an error: the scheduler would run that system in parallel with one that
// writes the component, and nothing downstream could tell. A declaration the query does not use
// is *not* an error, because that is how a system tells the schedule about the accesses above.
// Over-declaring costs parallelism and is visible in `schedule_hash()`; under-declaring costs
// correctness and is invisible, so only one of the two is worth an assert.
//
// **Resources are not query terms.** `sim::SystemDesc::reads_resources`/`writes_resources` name
// data that is not in the ECS at all — a pose pool, a GPU upload buffer, the storage [03 §3.4]
// tells a hot system to own — so the query has nothing to say about them and the check does not
// look. That is the point of them: they are how a system declares the accesses its terms *cannot*
// mention, which the paragraph above says is the whole reason the declaration is written by hand.
// The two masks are distinct types, so a resource id cannot land in a component mask by accident;
// what the check does add is that every declared resource id is one the registry handed out, which
// catches a mask built from a stale constant instead of from `sim::resource_mask()`.
//
// **What v1 does not check.** Relationship terms — `(Likes, *)`, `(ChildOf, parent)` — have no
// place in `sim::ComponentMask`, which addresses plain components by index, so a pair term is
// skipped. That is the same gap ADR-0028 leaves open around spatial relationships, and closing
// it means deciding what a relationship's bit means, which is the scheduler's design and not
// this seam's.
//
// **Why the executor is still flecs' pipeline.** Because the point of the seam is that it does
// not have to be. `desc` goes into a table this module owns, the flecs system is created with
// `.kind(phase)` and flecs' pipeline runs it today; replacing the executor means reading that
// table in a different order, and not one line of any system's data access.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/ecs/components.h>
#include <domain/ecs/sim_world.h>
#include <domain/sim/scheduler.h>

#include <flecs.h>
#include <span>

namespace engine::ecs {

// One registered system, as the engine sees it. `desc` is copied: `SystemDesc` holds a `name`
// pointer the caller owns (a string literal, as ADR-0027's static table intends) and nothing
// else that outlives the call.
struct RegisteredSystem {
  sim::SystemDesc desc;
  flecs::entity_t system = 0;
  u16 index = 0;  // position in this world's registration order, which is flecs' run order too
};

// The systems of one world, in registration order. A flecs singleton, like the component table.
//
// This is the table the engine's own scheduler will read when it takes the tick over. Today it
// is the honest record of what every system declared, which is what makes the declarations worth
// writing before anything consumes them.
class SystemRegistry {
 public:
  u16 add(const sim::SystemDesc& desc, flecs::entity_t system);
  std::span<const RegisteredSystem> all() const noexcept {
    return {systems_.data(), systems_.size()};
  }
  u16 size() const noexcept { return static_cast<u16>(systems_.size()); }
  const RegisteredSystem* find(flecs::entity_t system) const noexcept;
  // Systems declared for one phase, in registration order.
  u16 count_in(TickPhase phase) const noexcept;

 private:
  Vector<RegisteredSystem> systems_;
};

SystemRegistry& systems(flecs::world& world);
const SystemRegistry* systems_if_present(const flecs::world& world) noexcept;

namespace detail {

// The non-template half of a registration: the record, and the debug check of the declared sets
// against the built query's terms. Separate so the check exists once in the binary.
u16 finish_system(flecs::world& world, const sim::SystemDesc& desc, flecs::entity_t system);

}  // namespace detail

// Registers a system: the descriptor with the engine, the body with flecs.
//
// `make(world, phase)` is handed the world and the phase entity `desc.phase` names, and returns
// the built flecs system. It is a callable rather than a pre-built system so that `.kind(phase)`
// cannot be forgotten and cannot disagree with `desc.phase`:
//
//     ecs::register_system(sim, k_move_desc, [](flecs::world& w, flecs::entity phase) {
//       return w.system<Position, const Velocity>("move").kind(phase).multi_threaded()
//               .each([](Position& p, const Velocity& v) { p.x += v.x; p.y += v.y; });
//     });
//
// Returns the flecs system, so the caller can keep driving it as flecs' own object.
template <class Make>
flecs::system register_system(SimWorld& sim, const sim::SystemDesc& desc, Make&& make) {
  flecs::world& world = sim.world();
  const flecs::system system = make(world, sim.phase(desc.phase));
  detail::finish_system(world, desc, system);
  return system;
}

// The same, for a world that is not a `SimWorld` — a test, or a tool that built its own phases.
template <class Make>
flecs::system register_system(flecs::world& world, flecs::entity phase, const sim::SystemDesc& desc,
                              Make&& make) {
  const flecs::system system = make(world, phase);
  detail::finish_system(world, desc, system);
  return system;
}

// What a system's query says it touches, as the debug check reads it. Public because a test that
// asserts the check works has to be able to ask the same question the check asks.
struct QueryAccess {
  sim::ComponentMask reads;
  sim::ComponentMask writes;
  u32 unregistered_terms = 0;  // terms on components this world never registered
  u32 pair_terms = 0;          // relationship terms, which masks cannot express yet
};

QueryAccess query_access(flecs::world& world, flecs::entity_t system) noexcept;

}  // namespace engine::ecs
