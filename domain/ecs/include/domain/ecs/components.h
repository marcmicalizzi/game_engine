#pragma once

// Components come from the schema IDL (ADR-0028 seam 1).
//
// A component is declared once, in a `.schema` file, as a struct carrying `@kind(component)`.
// `schemac` emits its C++ type as it always has, and, next to it, `<schemas/<stem>_ecs.h>`, a
// header whose one function registers every one of that file's components with a `flecs::world`.
// This file is the other half: the registration itself, the per-world table it fills, and the
// index space that turns a component into a bit of `sim::ComponentMask`.
//
// **Why the type system is the schema's and not flecs'.** A component the protocol can read, the
// store can persist, a migration can rewrite and an agent can name has to be *one* type, not a
// C++ struct with a flecs registration beside a schema declaration that drifts from it. Going
// through the IDL makes the qualified name, the field layout, the JSON rendering, the version
// and the `@transient` marking one description with one owner. Nothing here wraps flecs — a
// system still iterates `Position` as a plain C++ struct through a plain flecs query; what is
// centralized is only *how the type became known*, which is the part that has to agree with four
// other subsystems.
//
// **The one exception.** A module may hand-register a flecs component for state that is private
// to it, never persisted and never visible to the protocol — a per-world cache, a scratch tag.
// `register_private_component()` says so at the call site and marks the type `Transient`, so the
// distinction is visible in the world rather than only in a comment.
//
// **The index space.** `sim::ComponentMask` addresses components by a small integer, because a
// schedule is computed from mask intersections and 256 bits is four `and`s. Nothing assigned
// those integers until now; `ComponentRegistry` does, in registration order, per world. A
// component registered past `sim::k_max_components` gets `k_invalid_component_index` and a
// warning rather than silently aliasing another component's bit (ADR-0017: a limit that is
// stated and checked is not a hidden one).

#include <core/base/assert.h>
#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <core/json/json_value.h>
#include <core/schema/type_info.h>
#include <domain/sim/scheduler.h>

#include <flecs.h>
#include <span>
#include <string>
#include <string_view>

namespace engine::ecs {

// A component whose schema struct carries `@transient`, or which was hand-registered as
// module-private state. It exists in the runtime world and is never written to the persistent
// store (docs/plan/03-data-model.md §3.4). The tag is on the *component entity*, not on the
// entities that carry the component, so a walk of the world's components can answer "what would
// a save file contain" without consulting anything outside flecs.
struct Transient {};

inline constexpr u32 k_invalid_component_index = 0xFFFF'FFFFu;

// One registered component, as the engine sees it.
struct ComponentType {
  const schema::TypeInfo* info = nullptr;  // null for a hand-registered private component
  flecs::entity_t id = 0;                  // the flecs component id in this world
  u32 index = k_invalid_component_index;   // bit index in sim::ComponentMask
  u32 size = 0;                            // 0 for a tag
  u32 align = 0;
  bool transient = false;
  // Every field, recursively, is a scalar or a fixed array of scalars: no string, bytes, array,
  // map, optional or json anywhere. Such a component is exactly its bytes, which is what lets
  // `WorldCommands::set_bytes` copy one in without a serializer and what a future binary
  // protocol encoder will key on.
  bool flat = false;
};

// The components of one world, in registration order. Lives as a flecs singleton, so it is
// created with the first component and destroyed with the world.
//
// Hold the reference `components()` returns only for the call that obtained it: it points into
// flecs' storage for the singleton and a world mutation may move it.
class ComponentRegistry {
 public:
  // Records a component. Returns its index, or `k_invalid_component_index` past
  // `sim::k_max_components`. Registering the same component id twice returns the first row.
  u32 add(const schema::TypeInfo* info, flecs::entity_t id, u32 size, u32 align, bool transient);

  const ComponentType* find(std::string_view qualified_name) const noexcept;
  const ComponentType* find(flecs::entity_t id) const noexcept;
  u32 index_of(flecs::entity_t id) const noexcept;

  std::span<const ComponentType> all() const noexcept { return {types_.data(), types_.size()}; }
  u32 size() const noexcept { return static_cast<u32>(types_.size()); }
  // How many components did not fit the mask. Non-zero means the schedule cannot see them.
  u32 overflowed() const noexcept { return overflowed_; }

 private:
  Vector<ComponentType> types_;
  // Keys are the `TypeInfo::qualified_name` pointers, which have static storage duration.
  HashMap<std::string_view, u32> by_name_;
  HashMap<flecs::entity_t, u32> by_id_;
  u32 overflowed_ = 0;
};

// The world's component table, created on first use.
ComponentRegistry& components(flecs::world& world);
// The table if this world has one, without creating it. For an inspector that must not mutate.
const ComponentRegistry* components_if_present(const flecs::world& world) noexcept;

namespace detail {

// "engine.sim.Position" -> "engine::sim::Position", which is the path separator flecs' component
// registration uses, so a schema namespace becomes a flecs scope and `world.lookup("engine.sim.
// Position", ".", ".")` finds the component the schema named.
std::string flecs_path(const char* qualified_name);

// Everything about a registration that does not need the C++ type: the member reflection, the
// transient tag and the registry row. Kept out of the template so the mapping from schema kinds
// to flecs meta types exists once in the binary and is reviewed in one place.
void finish_component(flecs::world& world, const schema::TypeInfo* info, flecs::entity component);

}  // namespace detail

// Registers a schema-declared component with `world`. Idempotent: flecs returns the existing
// component for a second call and the registry keeps the first row.
//
// `T` must be a struct schemac generated; `schema::type_of<T>()` is what carries the qualified
// name, the fields and the `@transient` marking. Generated `<schemas/<stem>_ecs.h>` calls this
// once per component, which is the only intended caller.
template <class T>
flecs::entity register_schema_component(flecs::world& world) {
  const schema::TypeInfo& info = schema::type_of<T>();
  const std::string path = detail::flecs_path(info.qualified_name);
  const flecs::entity component = world.component<T>(path.c_str());
  detail::finish_component(world, &info, component);
  return component;
}

// Registers a component that is *not* a schema type: module-private state, never persisted and
// never visible to the protocol (seam 1's single exception). It is marked `Transient` and has no
// `TypeInfo`, so `WorldCommands` cannot name it and a persistence walk skips it.
template <class T>
flecs::entity register_private_component(flecs::world& world, const char* name) {
  const flecs::entity component = world.component<T>(name);
  detail::finish_component(world, nullptr, component);
  return component;
}

// The component's bit index in `sim::ComponentMask`, or `k_invalid_component_index` when the
// world does not have it. Cold: a system builds its masks once, at registration.
u32 component_index(flecs::world& world, std::string_view qualified_name) noexcept;
u32 component_index(flecs::world& world, flecs::entity_t id) noexcept;

template <class T>
u32 component_index(flecs::world& world) noexcept {
  return component_index(world, std::string_view{schema::type_of<T>().qualified_name});
}

// Builds a mask from schema types. A type the world does not know is an error the caller has to
// see, so it asserts in debug and contributes no bit in release.
template <class... Ts>
sim::ComponentMask mask_of(flecs::world& world) {
  sim::ComponentMask mask;
  if constexpr (sizeof...(Ts) > 0) {
    const u32 indices[] = {component_index<Ts>(world)...};
    for (const u32 index : indices) {
      ENGINE_ASSERT(index != k_invalid_component_index,
                    "ecs::mask_of: component is not registered with this world");
      if (index != k_invalid_component_index) mask.set(index);
    }
  } else {
    (void)world;
  }
  return mask;
}

// The component the schema named, or a null entity. `qualified_name` is dotted as the IDL
// writes it ("engine.sim.Position").
flecs::entity lookup_component(const flecs::world& world, const char* qualified_name) noexcept;

// The component `qualified_name` of the entity `id` names, rendered by core/schema's JSON: the read
// half of `WorldCommands::set_json`, by the same names, so a host outside `systems/` — a predicate
// over live entities, the protocol's future `world.query` — reads a component without a flecs type
// in sight. False when the entity, the component or its schema type is not there.
bool component_json(flecs::world& world, const Id128& id, std::string_view qualified_name,
                    JsonValue& out);

}  // namespace engine::ecs
