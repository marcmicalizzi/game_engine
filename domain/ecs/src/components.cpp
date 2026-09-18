#include "undeferred.h"

#include <core/log/log.h>
#include <domain/ecs/components.h>

namespace engine::ecs {

ENGINE_LOG_CATEGORY_DECLARE(log_ecs);

namespace {

// Which schema kinds have a flecs meta equivalent, and what a member of that kind looks like.
//
// The rule is deliberately narrow: a member is added only when flecs can describe the *exact
// bytes at that offset*. Everything else — `string`, `bytes`, a nested struct, an array, a map,
// an optional, a `json` payload — is left out, and flecs then treats the component as a partial
// type: the fields it knows are inspectable in the explorer and through flecs' own serializers,
// and the rest is opaque to it. That is the honest answer, because the authority on those fields
// is `core/schema`, which can render all of them; duplicating half of its walker in flecs' meta
// language would give the engine two serializers that have to agree.
//
// `count` is the element count of a fixed array (1 for a scalar). Returns false when the kind
// does not map.
bool meta_type_of(const schema::TypeRef& type, flecs::entity_t& out_type, i32& out_count) {
  out_count = 1;
  switch (type.kind) {
    case schema::Kind::Bool: out_type = flecs::Bool; return true;
    case schema::Kind::U8: out_type = flecs::U8; return true;
    case schema::Kind::U16: out_type = flecs::U16; return true;
    case schema::Kind::U32: out_type = flecs::U32; return true;
    case schema::Kind::U64: out_type = flecs::U64; return true;
    case schema::Kind::I8: out_type = flecs::I8; return true;
    case schema::Kind::I16: out_type = flecs::I16; return true;
    case schema::Kind::I32: out_type = flecs::I32; return true;
    case schema::Kind::I64: out_type = flecs::I64; return true;
    case schema::Kind::F32: out_type = flecs::F32; return true;
    case schema::Kind::F64: out_type = flecs::F64; return true;
    // engine::Id128 is two u64 and engine::Vec3 three f32, laid out exactly like that; the size
    // tables in core/ids and core/math are what make describing them as arrays safe.
    case schema::Kind::Id128:
      out_type = flecs::U64;
      out_count = 2;
      return true;
    case schema::Kind::Vec2:
      out_type = flecs::F32;
      out_count = 2;
      return true;
    case schema::Kind::Vec3:
      out_type = flecs::F32;
      out_count = 3;
      return true;
    case schema::Kind::Vec4:
    case schema::Kind::Quat:
      out_type = flecs::F32;
      out_count = 4;
      return true;
    // An enum's bytes are its underlying integer. flecs can hold a real enum type, but building
    // one means declaring every constant as an entity, and a schema enum's names already live in
    // the TypeInfo where the JSON path reads them. The integer is what the memory is.
    case schema::Kind::Enum: {
      if (type.type == nullptr) return false;
      schema::TypeRef underlying;
      underlying.kind = type.type->enum_underlying;
      i32 ignored = 1;
      return meta_type_of(underlying, out_type, ignored);
    }
    // A fixed array of a mappable scalar is a member with a count; a fixed array of anything
    // else is not.
    case schema::Kind::FixedArray: {
      if (type.element == nullptr || type.fixed_count == 0) return false;
      i32 inner = 1;
      if (!meta_type_of(*type.element, out_type, inner)) return false;
      if (inner != 1) return false;  // no fixed array of Vec3 yet; flecs members are one deep
      out_count = static_cast<i32>(type.fixed_count);
      return true;
    }
    case schema::Kind::String:
    case schema::Kind::Bytes:
    case schema::Kind::Json:
    case schema::Kind::Struct:
    case schema::Kind::Optional:
    case schema::Kind::Array:
    case schema::Kind::Map: return false;
  }
  return false;
}

// Is every byte of this type its own value — no pointer, no allocation, no length? That is what
// makes a component copyable as bytes.
bool is_flat(const schema::TypeRef& type);

bool is_flat(const schema::TypeInfo& info) {
  for (const schema::FieldInfo& field : info.fields) {
    if (!is_flat(field.type)) return false;
  }
  return true;
}

bool is_flat(const schema::TypeRef& type) {
  switch (type.kind) {
    case schema::Kind::Bool:
    case schema::Kind::U8:
    case schema::Kind::U16:
    case schema::Kind::U32:
    case schema::Kind::U64:
    case schema::Kind::I8:
    case schema::Kind::I16:
    case schema::Kind::I32:
    case schema::Kind::I64:
    case schema::Kind::F32:
    case schema::Kind::F64:
    case schema::Kind::Id128:
    case schema::Kind::Vec2:
    case schema::Kind::Vec3:
    case schema::Kind::Vec4:
    case schema::Kind::Quat:
    case schema::Kind::Enum: return true;
    case schema::Kind::FixedArray: return type.element != nullptr && is_flat(*type.element);
    case schema::Kind::Struct: return type.type != nullptr && is_flat(*type.type);
    case schema::Kind::String:
    case schema::Kind::Bytes:
    case schema::Kind::Json:
    case schema::Kind::Optional:
    case schema::Kind::Array:
    case schema::Kind::Map: return false;
  }
  return false;
}

}  // namespace

// --- ComponentRegistry -----------------------------------------------------------------------

u32 ComponentRegistry::add(const schema::TypeInfo* info, flecs::entity_t id, u32 size, u32 align,
                           bool transient) {
  if (const u32* existing = by_id_.find_value(id)) return types_[*existing].index;

  ComponentType type;
  type.info = info;
  type.id = id;
  type.size = size;
  type.align = align;
  type.transient = transient;
  type.flat = info != nullptr && is_flat(*info);

  const u32 row = static_cast<u32>(types_.size());
  if (row < sim::k_max_components) {
    type.index = row;
  } else {
    ++overflowed_;
    ENGINE_LOG_WARN(log_ecs, "component past the schedule's mask width",
                    log::field("component", info != nullptr ? info->qualified_name : "private"),
                    log::field("limit", sim::k_max_components));
  }

  types_.push_back(type);
  by_id_.insert_or_assign(id, row);
  if (info != nullptr) by_name_.insert_or_assign(std::string_view{info->qualified_name}, row);
  return type.index;
}

const ComponentType* ComponentRegistry::find(std::string_view qualified_name) const noexcept {
  const u32* row = by_name_.find_value(qualified_name);
  return row != nullptr ? &types_[*row] : nullptr;
}

const ComponentType* ComponentRegistry::find(flecs::entity_t id) const noexcept {
  const u32* row = by_id_.find_value(id);
  return row != nullptr ? &types_[*row] : nullptr;
}

u32 ComponentRegistry::index_of(flecs::entity_t id) const noexcept {
  const ComponentType* type = find(id);
  return type != nullptr ? type->index : k_invalid_component_index;
}

// --- registration ----------------------------------------------------------------------------

ComponentRegistry& components(flecs::world& world) {
  const detail::Undeferred undeferred(world.c_ptr());
  return world.ensure<ComponentRegistry>();
}

const ComponentRegistry* components_if_present(const flecs::world& world) noexcept {
  return world.try_get<ComponentRegistry>();
}

namespace detail {

std::string flecs_path(const char* qualified_name) {
  std::string path;
  if (qualified_name == nullptr) return path;
  for (const char* p = qualified_name; *p != '\0'; ++p) {
    if (*p == '.') {
      path += "::";
    } else {
      path.push_back(*p);
    }
  }
  return path;
}

void finish_component(flecs::world& world, const schema::TypeInfo* info, flecs::entity component) {
  const bool transient =
      info == nullptr || (info->flags & schema::TypeFlag::transient) == schema::TypeFlag::transient;

  // Members before anything else uses the component: flecs refuses to redescribe a type that is
  // already in use, and a registration that runs at world setup never is.
  if (info != nullptr) {
    for (const schema::FieldInfo& field : info->fields) {
      flecs::entity_t member_type = 0;
      i32 count = 1;
      if (!meta_type_of(field.type, member_type, count)) continue;
      ecs_member_t member = {};
      member.name = field.name;
      member.type = member_type;
      member.count = count;
      member.offset = static_cast<i32>(field.offset);
      member.use_offset = true;
      ecs_struct_add_member(world.c_ptr(), component, &member);
    }
  }

  if (transient) component.add<Transient>();

  // The size flecs actually stored, not `TypeInfo::size`: an empty schema struct registers as a
  // tag of size 0 and every writer has to see that rather than a C++ `sizeof` of 1.
  const ecs_type_info_t* stored = ecs_get_type_info(world.c_ptr(), component);
  const u32 size = stored != nullptr ? static_cast<u32>(stored->size) : 0u;
  const u32 align = stored != nullptr ? static_cast<u32>(stored->alignment) : 0u;
  components(world).add(info, component, size, align, transient);
}

}  // namespace detail

u32 component_index(flecs::world& world, std::string_view qualified_name) noexcept {
  const ComponentType* type = components(world).find(qualified_name);
  return type != nullptr ? type->index : k_invalid_component_index;
}

u32 component_index(flecs::world& world, flecs::entity_t id) noexcept {
  return components(world).index_of(id);
}

flecs::entity lookup_component(const flecs::world& world, const char* qualified_name) noexcept {
  // The dotted IDL spelling, resolved as a path with "." as the separator: the registration put
  // the component at exactly that path, so the schema's name for a type and flecs' name for it
  // are the same string. Not found is entity 0 bound to this world, so `is_valid()` is false and
  // a caller that forgets to check gets flecs' own diagnostic rather than a null-world assert.
  const flecs::entity_t id =
      qualified_name != nullptr
          ? ecs_lookup_path_w_sep(world.c_ptr(), 0, qualified_name, ".", ".", false)
          : 0;
  return flecs::entity(world.c_ptr(), id);
}

}  // namespace engine::ecs
