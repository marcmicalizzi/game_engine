#include <core/base/assert.h>
#include <core/schema/materialize.h>

#include <cstddef>
#include <cstring>

namespace engine::schema {

const char* materialize_parent_name(MaterializeParent parent) noexcept {
  switch (parent) {
    case MaterializeParent::None: return "none";
    case MaterializeParent::ChildOf: return "ChildOf";
  }
  return "?";
}

// --- the registry ------------------------------------------------------------------------------

MaterializeRegistry& MaterializeRegistry::global() {
  static MaterializeRegistry registry;
  return registry;
}

void MaterializeRegistry::add(const MaterializeInfo& info) {
  ENGINE_VERIFY(info.record != nullptr, "schema: a mapping names its record type");
  const std::string_view name(info.record->qualified_name);
  const MaterializeInfo* const* existing = by_record_.find_value(name);
  if (existing != nullptr) {
    // The same object registered twice is the same mapping (a library linked into two images of
    // one process registers its table once per image); a *second* mapping for one record type
    // would make materialization depend on link order, which is exactly what a registry exists to
    // rule out.
    ENGINE_VERIFY(*existing == &info, "schema: two mappings registered for one record type");
    return;
  }
  mappings_.push_back(&info);
  by_record_.insert(name, &info);
}

const MaterializeInfo* MaterializeRegistry::find(std::string_view record_type) const noexcept {
  const MaterializeInfo* const* p = by_record_.find_value(record_type);
  return p != nullptr ? *p : nullptr;
}

MaterializeRegistrar::MaterializeRegistrar(const MaterializeInfo* const* mappings, usize count) {
  MaterializeRegistry& registry = MaterializeRegistry::global();
  for (usize i = 0; i < count; ++i)
    registry.add(*mappings[i]);
}

// --- conversion ------------------------------------------------------------------------------

namespace {

// The number of floats a convertible kind holds, and whether they are f64. Only float-based kinds
// convert: scaling an integer would round and scaling a quaternion is not a unit change, and
// schemac refuses both before a table is ever emitted, so anything else here is a table that did
// not come from schemac.
u32 float_lanes(Kind kind, bool& wide) noexcept {
  wide = false;
  switch (kind) {
    case Kind::F32: return 1;
    case Kind::F64: wide = true; return 1;
    case Kind::Vec2: return 2;
    case Kind::Vec3: return 3;
    case Kind::Vec4: return 4;
    default: return 0;
  }
}

}  // namespace

bool read_mapped(const MaterializeField& row, const FieldInfo& field, void* component,
                 const JsonValue& value, ReadContext& ctx) {
  auto* at = static_cast<std::byte*>(component) + field.offset;
  if ((row.flags & MaterializeFlag::convert) == 0) return from_json(field.type, at, value, ctx);

  bool wide = false;
  const u32 lanes = float_lanes(field.type.kind, wide);
  if (lanes == 0) {
    ctx.error("a converted mapping row needs a float or vector field");
    return false;
  }
  // Scalar or array, read whole before anything is written: a value that does not fit leaves the
  // field as it was rather than half converted.
  f64 in[4] = {};
  if (lanes == 1) {
    if (!value.get_f64(in[0])) {
      ctx.error("expected a number");
      return false;
    }
  } else {
    if (!value.is_array() || value.size() != lanes) {
      ctx.error("expected an array of the type's component count");
      return false;
    }
    for (u32 i = 0; i < lanes; ++i) {
      if (!value[i].get_f64(in[i])) {
        ctx.error("expected numeric components");
        return false;
      }
    }
  }
  // In f64, then narrowed once. Floating-point contraction is off tree-wide (ADR-0035), so the
  // multiply and the add are two roundings on every compiler and the same bytes everywhere.
  for (u32 i = 0; i < lanes; ++i) {
    const f64 converted = in[i] * row.scale + row.offset;
    if (wide) {
      std::memcpy(at + i * sizeof(f64), &converted, sizeof(f64));
    } else {
      const f32 narrow = static_cast<f32>(converted);
      std::memcpy(at + i * sizeof(f32), &narrow, sizeof(f32));
    }
  }
  return true;
}

bool write_mapped(const MaterializeField& row, const FieldInfo& field, const void* component,
                  JsonValue& out) {
  const auto* at = static_cast<const std::byte*>(component) + field.offset;
  if ((row.flags & MaterializeFlag::convert) == 0) return to_json(field.type, at, out);

  bool wide = false;
  const u32 lanes = float_lanes(field.type.kind, wide);
  if (lanes == 0 || row.scale == 0.0) return false;
  f64 values[4] = {};
  for (u32 i = 0; i < lanes; ++i) {
    f64 stored = 0.0;
    if (wide) {
      std::memcpy(&stored, at + i * sizeof(f64), sizeof(f64));
    } else {
      f32 narrow = 0.0f;
      std::memcpy(&narrow, at + i * sizeof(f32), sizeof(f32));
      stored = static_cast<f64>(narrow);
    }
    values[i] = (stored - row.offset) / row.scale;
  }
  if (lanes == 1) {
    out = JsonValue(values[0]);
    return true;
  }
  out = JsonValue::array();
  for (u32 i = 0; i < lanes; ++i)
    out.push_back(JsonValue(values[i]));
  return true;
}

bool is_byte_comparable(const TypeRef& type) noexcept {
  switch (type.kind) {
    case Kind::Bool:
    case Kind::U8:
    case Kind::U16:
    case Kind::U32:
    case Kind::U64:
    case Kind::I8:
    case Kind::I16:
    case Kind::I32:
    case Kind::I64:
    case Kind::F32:
    case Kind::F64:
    case Kind::Id128:
    case Kind::Vec2:
    case Kind::Vec3:
    case Kind::Vec4:
    case Kind::Quat:
    case Kind::Enum: return true;
    case Kind::FixedArray: return type.element != nullptr && is_byte_comparable(*type.element);
    default: return false;
  }
}

u32 stable_type_id(std::string_view qualified_name) noexcept {
  // FNV-1a, 32 bits. An identity for a name, not a digest: it only has to be the same on every
  // build and every machine, which a function of the bytes alone is.
  u32 hash = 2166136261u;
  for (const char c : qualified_name) {
    hash ^= static_cast<u32>(static_cast<u8>(c));
    hash *= 16777619u;
  }
  return hash;
}

}  // namespace engine::schema
