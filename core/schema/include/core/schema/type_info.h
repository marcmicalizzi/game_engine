#pragma once

// Type descriptors for schema-declared types (ADR-0007, docs/plan/02-architecture.md §2.5).
//
// schemac emits, for every struct and enum in a .schema file, a constant-initialized TypeInfo
// plus FieldInfo/TypeRef tables and registers them in the global Registry at startup. Generic
// code (JSON, diffing, the protocol, editor property panels) walks these descriptors instead of
// needing per-type code. Containers and optionals are reached through small type-erased
// operation tables (see ops.h) so the walker never instantiates templates itself.

#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>

#include <span>
#include <string_view>

namespace engine::schema {

enum class Kind : u8 {
  Bool,
  U8,
  U16,
  U32,
  U64,
  I8,
  I16,
  I32,
  I64,
  F32,
  F64,
  String,  // std::string
  Bytes,   // Vector<u8>, serialized as hex
  Id128,   // engine::Id128, serialized as 32 hex characters
  Vec2,    // engine::Vec2, serialized as [x, y]
  Vec3,    // engine::Vec3, serialized as [x, y, z]
  Vec4,    // engine::Vec4, serialized as [x, y, z, w]
  Quat,    // engine::Quat, serialized as [x, y, z, w]
  Json,    // engine::JsonValue, serialized as itself (schema-free payloads)
  Enum,    // enum class with a schema-declared underlying integer
  Struct,
  Optional,    // std::optional<T>
  Array,       // Vector<T>
  FixedArray,  // std::array<T, N>
  Map,         // FlatMap<K, V>
  // Appended, never inserted: a kind's number is part of what a reader older than it was built
  // with, so the list only grows at its end (schemas/README.md, "Evolution rules").
  //
  // A position in the world and a displacement in f64 (ADR-0053, core/math/world.h): three f64,
  // 24 bytes, serialized as [x, y, z] in the shortest text that reads back to the same double. Two
  // kinds rather than one because the C++ types are two — a point and a displacement do not add —
  // and the IDL keeps what the types keep. A `WorldPos` read from outside must be finite and inside
  // `world_cell_valid`; a `DVec3` must be finite.
  WorldPos,  // engine::WorldPos
  DVec3,     // engine::DVec3
};

const char* kind_name(Kind kind) noexcept;
constexpr bool is_integer(Kind k) noexcept { return k >= Kind::U8 && k <= Kind::I64; }
constexpr bool is_unsigned(Kind k) noexcept { return k >= Kind::U8 && k <= Kind::U64; }
constexpr bool is_float(Kind k) noexcept { return k == Kind::F32 || k == Kind::F64; }
// The kinds that are three f64 in memory.
constexpr bool is_f64_vector(Kind k) noexcept { return k == Kind::WorldPos || k == Kind::DVec3; }

struct TypeInfo;

// Type-erased operations on std::optional<T>.
struct OptionalOps {
  bool (*has_value)(const void* opt);
  const void* (*get)(const void* opt);
  void* (*emplace)(void* opt);  // default-constructs the value and returns it
  void (*reset)(void* opt);
};

// Type-erased operations on Vector<T> and std::array<T, N>. For fixed arrays push_back and
// clear are null and size returns N.
struct ArrayOps {
  usize (*size)(const void* arr);
  const void* (*at)(const void* arr, usize index);
  void* (*at_mut)(void* arr, usize index);
  void* (*push_back)(void* arr);  // default-constructs a new element and returns it
  void (*clear)(void* arr);
};

// Type-erased operations on FlatMap<K, V>, indexed in key order.
struct MapOps {
  usize (*size)(const void* map);
  const void* (*key_at)(const void* map, usize index);
  const void* (*value_at)(const void* map, usize index);
  void* (*insert)(void* map, const void* key);  // finds or default-constructs the value
  void (*clear)(void* map);
};

// Describes one C++ type as used by a field: a scalar, a named struct or enum, or a container
// of another TypeRef.
struct TypeRef {
  Kind kind = Kind::Bool;
  u32 fixed_count = 0;               // FixedArray
  usize size = 0;                    // sizeof the C++ representation
  usize align = 0;                   // alignof the C++ representation
  const TypeInfo* type = nullptr;    // Enum, Struct
  const TypeRef* element = nullptr;  // Optional, Array, FixedArray, Map value
  const TypeRef* key = nullptr;      // Map
  const OptionalOps* optional_ops = nullptr;
  const ArrayOps* array_ops = nullptr;
  const MapOps* map_ops = nullptr;
};

struct FieldFlag {
  static constexpr u16 transient = 1;   // never serialized
  static constexpr u16 deprecated = 2;  // still read and written; flagged for removal
};

// Whole-type markings, from the struct's own attributes.
//
// `transient` is the type-level counterpart of `FieldFlag::transient`: the type exists only in
// the runtime world and is never written to the persistent store (docs/plan/03-data-model.md
// §3.4). It lives here, on the descriptor, rather than only on the entity store's side, because
// persistence, the protocol and migrations have to see it and none of them link an ECS
// ([ADR-0028](../../../../docs/adr/0028-ecs-and-persistent-store.md) seam 1). What kind of type
// it is — a component, a record, an event — stays in `tag`, which is `@kind`'s free-form
// argument; a marking only becomes a flag when generic code has to branch on it.
struct TypeFlag {
  static constexpr u16 transient = 1;
};

struct FieldInfo {
  const char* name;
  TypeRef type;
  u32 offset;
  u16 since_version;  // schema version that introduced the field
  u16 flags;          // FieldFlag bits
  const char* doc;
};

struct EnumValueInfo {
  const char* name;
  i64 value;
  const char* doc;
};

// Type-erased whole-object operations for a struct.
struct StructOps {
  void (*construct)(void* obj);
  void (*destroy)(void* obj);
  void (*copy_assign)(void* dst, const void* src);
  bool (*equals)(const void* a, const void* b);
};

struct TypeInfo {
  const char* qualified_name;  // "engine.content.AssetProvenance", dotted as in the IDL
  const char* name;            // "AssetProvenance"
  const char* ns;              // "engine.content"
  Kind kind;                   // Struct or Enum
  u32 size;
  u32 align;
  u16 version;      // @version, structs only; 1 when unspecified
  u16 flags;        // TypeFlag bits
  const char* tag;  // @kind attribute ("component", "event", ...) or ""
  const char* doc;
  std::span<const FieldInfo> fields;      // Struct
  std::span<const EnumValueInfo> values;  // Enum
  Kind enum_underlying;                   // Enum
  const StructOps* ops;                   // Struct

  const FieldInfo* find_field(std::string_view field_name) const noexcept;
  const EnumValueInfo* find_value(std::string_view value_name) const noexcept;
  const EnumValueInfo* find_value(i64 value) const noexcept;
};

// Specialized by generated code for every schema type.
template <class T>
const TypeInfo& type_of();

// All registered types, by qualified name. Populated during static initialization by the
// Registrar objects schemac emits; safe to query from main() onward.
class Registry {
 public:
  static Registry& global();

  void add(const TypeInfo& info);
  const TypeInfo* find(std::string_view qualified_name) const noexcept;
  std::span<const TypeInfo* const> all() const noexcept { return {types_.data(), types_.size()}; }

 private:
  Vector<const TypeInfo*> types_;
  HashMap<std::string_view, const TypeInfo*> by_name_;
};

struct Registrar {
  Registrar(const TypeInfo* const* types, usize count);
};

}  // namespace engine::schema
