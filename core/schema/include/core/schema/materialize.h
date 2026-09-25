#pragma once

// Type mapping as data: which components a document record type becomes in the runtime world, and
// how its properties map onto their fields (docs/plan/03-data-model.md §3.4, ADR-0028 seam 1).
//
// A `.schema` file declares it beside the types it joins:
//
//     materialize Mover @tiers(0, 1, 2) {
//       Transform.position = position @writeback
//       Velocity.angular = spin          // deg/s on the record, rad/s on the component
//       parent = ChildOf
//     }
//
// and schemac compiles it into a constant-initialized `MaterializeInfo` registered here at
// start-up, exactly as it does a `TypeInfo`. The runtime reads the table and runs no per-type code:
// the driver (`domain/sim/materialize.h`) walks a document's records, finds each one's row by its
// type's qualified name, and hands the row and the record's property values to the materialization
// hooks; the entity store's hook (`domain/ecs/materialize.h`) writes the fields.
//
// **Why the table lives in core and not in the entity store.** The protocol, the driver and the
// document validators have to see which record types materialize, and none of them links an ECS.
// A `MaterializeInfo` points at `TypeInfo`s and names fields by string; nothing in it is flecs'.
//
// **A record type without a row stays document-only, by design.** Canon facts, quest stages and
// provenance records are authored data that the runtime world never needs as entities; the driver
// counts them and says why it left them out, rather than treating the absence as an error.

#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/schema/json_reflect.h>
#include <core/schema/type_info.h>

#include <span>
#include <string_view>

namespace engine::schema {

// Row markings, from the IDL.
struct MaterializeFlag {
  // The two sides declare different `@unit`s, so the value is converted on the way in:
  // component = record × scale + offset, and on the way back the inverse.
  static constexpr u16 convert = 1;
  // `@writeback`: the runtime world may write this component field back to the record property
  // (docs/subsystems/sim.md, "Write-back"). Only a field that is its own bytes can carry it,
  // because "changed" is a byte comparison against what was last materialized or written.
  static constexpr u16 writeback = 2;
};

// What the document's parent becomes in the runtime world. `ChildOf` is the entity store's
// hierarchy relationship (flecs' `ChildOf`, which docs/plan/03-data-model.md §3.4 and ADR-0028
// name); `None` leaves the entity a root whatever the document says, for a type whose hierarchy is
// only an authoring convenience.
enum class MaterializeParent : u8 { None = 0, ChildOf = 1 };

const char* materialize_parent_name(MaterializeParent parent) noexcept;

// One row of a mapping: one component field, from one record property.
struct MaterializeField {
  const TypeInfo* component;  // the component this row writes, a `@kind(component)` struct
  const char* field;          // the component field's name
  const char* property;       // the record property's name
  f64 scale;  // component = record × scale + offset when `convert`; 1 and 0 otherwise
  f64 offset;
  u16 flags;  // MaterializeFlag bits
};

// One record type's mapping.
struct MaterializeInfo {
  const TypeInfo* record;  // the document record type, a `@kind(record)` struct
  // Every component the type materializes into, in the order the declaration first names them. A
  // component the declaration names with no field (`Marker` on a line of its own) is here and has
  // no row: the entity gets it with its schema defaults.
  std::span<const TypeInfo* const> components;
  // Grouped by component, in `components` order, and in declaration order within a component, so a
  // writer that visits them in order touches each component once.
  std::span<const MaterializeField> fields;
  MaterializeParent parent;
  // LOD tiers the type materializes at, one bit each; 0x0F when the declaration says none.
  u8 tiers;
  const char* doc;
};

// Every registered mapping, by record type. Populated during static initialization by the
// `MaterializeRegistrar`s schemac emits, like `Registry`; safe to query from `main()` onward.
class MaterializeRegistry {
 public:
  static MaterializeRegistry& global();

  // Two mappings for one record type is a build error that schemac catches within one schema
  // library; across two libraries it is caught here, at start-up, for the same reason `Registry`
  // refuses two types under one name.
  void add(const MaterializeInfo& info);
  const MaterializeInfo* find(std::string_view record_type) const noexcept;
  // In registration order: a library's mappings in declaration order, libraries in link order.
  std::span<const MaterializeInfo* const> all() const noexcept {
    return {mappings_.data(), mappings_.size()};
  }

 private:
  Vector<const MaterializeInfo*> mappings_;
  HashMap<std::string_view, const MaterializeInfo*> by_record_;
};

struct MaterializeRegistrar {
  MaterializeRegistrar(const MaterializeInfo* const* mappings, usize count);
};

// ---- the two directions of one row -------------------------------------------------------------
//
// Shared by every writer so that a conversion means one thing wherever it is applied: the entity
// store's hook, the tests that check a conversion without an ECS, and write-back.

// Reads the record property's JSON value into the component field at `component + field.offset`.
// A direct copy is `from_json` over the field's type; a converted row reads the number (or the two
// to four numbers of a vector) as f64, applies `scale` and `offset` in f64 and stores the field's
// own width. False, with a pathed diagnostic in `ctx`, when the value does not fit the field.
bool read_mapped(const MaterializeField& row, const FieldInfo& field, void* component,
                 const JsonValue& value, ReadContext& ctx);

// The inverse, for write-back: the component field's value as the record property's JSON. A
// converted row divides back out in f64, so a round trip is exact to the field's own precision and
// not beyond it — which is why write-back decides "changed" on the component's bytes, never on
// JSON.
bool write_mapped(const MaterializeField& row, const FieldInfo& field, const void* component,
                  JsonValue& out);

// Whether a field can carry `@writeback`: its every byte is its value (a scalar, an enum, an id, a
// vector, a quaternion, or a fixed array of those), so comparing bytes is comparing values.
bool is_byte_comparable(const TypeRef& type) noexcept;

// The schema type id a store event or projection names its type by (`store::EventRecord::type`,
// docs/subsystems/store.md): the 32-bit FNV-1a of the qualified name. A function of the name alone,
// so every build and every machine agrees on it without a registry of numbers; `session.events`
// prints it in decimal.
u32 stable_type_id(std::string_view qualified_name) noexcept;

}  // namespace engine::schema
