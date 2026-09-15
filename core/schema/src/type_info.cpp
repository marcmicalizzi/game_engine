#include <core/schema/type_info.h>

#include <core/base/assert.h>

#include <cstring>

namespace engine::schema {

const char* kind_name(Kind kind) noexcept {
  switch (kind) {
    case Kind::Bool: return "bool";
    case Kind::U8: return "u8";
    case Kind::U16: return "u16";
    case Kind::U32: return "u32";
    case Kind::U64: return "u64";
    case Kind::I8: return "i8";
    case Kind::I16: return "i16";
    case Kind::I32: return "i32";
    case Kind::I64: return "i64";
    case Kind::F32: return "f32";
    case Kind::F64: return "f64";
    case Kind::String: return "string";
    case Kind::Bytes: return "bytes";
    case Kind::Id128: return "id128";
    case Kind::Vec2: return "vec2";
    case Kind::Vec3: return "vec3";
    case Kind::Vec4: return "vec4";
    case Kind::Quat: return "quat";
    case Kind::Json: return "json";
    case Kind::Enum: return "enum";
    case Kind::Struct: return "struct";
    case Kind::Optional: return "optional";
    case Kind::Array: return "array";
    case Kind::FixedArray: return "fixed array";
    case Kind::Map: return "map";
  }
  return "?";
}

const FieldInfo* TypeInfo::find_field(std::string_view field_name) const noexcept {
  for (const FieldInfo& f : fields) {
    if (field_name == f.name) return &f;
  }
  return nullptr;
}

const EnumValueInfo* TypeInfo::find_value(std::string_view value_name) const noexcept {
  for (const EnumValueInfo& v : values) {
    if (value_name == v.name) return &v;
  }
  return nullptr;
}

const EnumValueInfo* TypeInfo::find_value(i64 value) const noexcept {
  for (const EnumValueInfo& v : values) {
    if (v.value == value) return &v;
  }
  return nullptr;
}

Registry& Registry::global() {
  static Registry registry;
  return registry;
}

void Registry::add(const TypeInfo& info) {
  const std::string_view name(info.qualified_name);
  const TypeInfo* const* existing = by_name_.find_value(name);
  if (existing != nullptr) {
    ENGINE_VERIFY(*existing == &info, "schema: two different types registered under the same qualified name");
    return;
  }
  types_.push_back(&info);
  by_name_.insert(name, &info);
}

const TypeInfo* Registry::find(std::string_view qualified_name) const noexcept {
  const TypeInfo* const* p = by_name_.find_value(qualified_name);
  return p != nullptr ? *p : nullptr;
}

Registrar::Registrar(const TypeInfo* const* types, usize count) {
  Registry& registry = Registry::global();
  for (usize i = 0; i < count; ++i) registry.add(*types[i]);
}

}  // namespace engine::schema
