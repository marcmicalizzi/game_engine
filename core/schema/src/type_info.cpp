#include <core/base/assert.h>
#include <core/schema/type_info.h>

#include <cstring>
#include <string>
#include <utility>

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
    case Kind::WorldPos: return "worldpos";
    case Kind::DVec3: return "dvec3";
  }
  return "?";
}

const FieldInfo* TypeInfo::find_field(std::string_view field_name) const noexcept {
  for (const FieldInfo& f : fields) {
    if (field_name == f.name) return &f;
  }
  return nullptr;
}

const FieldInfo* TypeInfo::find_field_alias(std::string_view former_name) const noexcept {
  for (const FieldAlias& a : field_aliases) {
    if (former_name == a.former_name && a.field < fields.size()) return &fields[a.field];
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

std::string Registry::collision(const TypeInfo& info) const {
  const std::string_view name(info.qualified_name);
  const TypeInfo* const* existing = by_name_.find_value(name);
  if (existing != nullptr) {
    if (*existing == &info) return {};  // registered again by the same descriptor: nothing new
    return "schema: two different types are registered under the name '" + std::string(name) + "'";
  }
  // A name means one type to a reader, current or former (`resolve` answers the current name
  // first, so a former name that is another type's current one would read an old file's records
  // as that other type, with no diagnostic; roadmap D44). schemac refuses every collision within
  // the files it compiles together; this is the check across libraries, in either arrival order.
  const TypeInfo* const* renamed = by_former_name_.find_value(name);
  if (renamed != nullptr) {
    return "schema: '" + std::string(name) + "' is the type " + std::string(name) +
           " and a former name of " + std::string((*renamed)->qualified_name) +
           ": a stored record naming it would be read as either";
  }
  for (const char* former : info.aliases) {
    const std::string_view f(former);
    if (f == name) continue;  // a type's own current name read as a former one is the same type
    if (const TypeInfo* const* current = by_name_.find_value(f); current != nullptr) {
      return "schema: '" + std::string(f) + "' is the type " + std::string(f) +
             " and a former name of " + std::string(name) +
             ": a stored record naming it would be read as either";
    }
    // Two types claiming one former name: an old file's records would be whichever registered
    // first. Refused like the rest, never settled by order.
    if (const TypeInfo* const* other = by_former_name_.find_value(f); other != nullptr) {
      return "schema: '" + std::string(f) + "' is a former name of both " +
             std::string((*other)->qualified_name) + " and " + std::string(name) +
             ": a stored record naming it would be read as either";
    }
  }
  return {};
}

bool Registry::try_add(const TypeInfo& info, std::string* error) {
  std::string why = collision(info);
  if (!why.empty()) {
    if (error != nullptr) *error = std::move(why);
    return false;
  }
  const std::string_view name(info.qualified_name);
  if (by_name_.find_value(name) != nullptr) return true;  // the same descriptor again
  types_.push_back(&info);
  by_name_.insert(name, &info);
  for (const char* former : info.aliases) {
    const std::string_view f(former);
    if (f != name && by_former_name_.find_value(f) == nullptr) by_former_name_.insert(f, &info);
  }
  for (const FieldAlias& a : info.field_aliases) {
    const std::string_view f(a.former_name);
    if (former_field_names_.find_value(f) == nullptr) former_field_names_.insert(f, &info);
  }
  return true;
}

void Registry::add(const TypeInfo& info) {
  // Registration runs at static initialization, before anything could read an error: a collision
  // is a build that links two libraries disagreeing about a name, and it stops the process naming
  // both types and the name.
  std::string why;
  const bool added = try_add(info, &why);
  ENGINE_VERIFY(added, why.c_str());
}

const TypeInfo* Registry::find(std::string_view qualified_name) const noexcept {
  const TypeInfo* const* p = by_name_.find_value(qualified_name);
  return p != nullptr ? *p : nullptr;
}

const TypeInfo* Registry::find_former(std::string_view former_name) const noexcept {
  if (by_former_name_.empty()) return nullptr;
  const TypeInfo* const* p = by_former_name_.find_value(former_name);
  return p != nullptr ? *p : nullptr;
}

Registrar::Registrar(const TypeInfo* const* types, usize count) {
  Registry& registry = Registry::global();
  for (usize i = 0; i < count; ++i)
    registry.add(*types[i]);
}

}  // namespace engine::schema
