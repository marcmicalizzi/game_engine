// Generic schema bindings: one metatable for every schema struct, driven by the descriptors
// schemac emits (docs/subsystems/scripting.md, "The generated bindings").
//
// Why generic rather than a binding generated per type: the descriptors already say everything a
// read needs — each field's name, kind and offset, each enum's names — and a per-type binding
// would be a second copy of that, generated into every schema module whether or not a script ever
// reads it, and compiled into the engine library of every game. The generic path costs a field
// lookup per read, which the string atoms below make a binary search over a handful of 4-byte
// entries; bench/scripting_bench.cpp measures what a read costs end to end.

#include "context_impl.h"

#include <core/base/assert.h>
#include <core/schema/type_info.h>

#include <algorithm>
#include <cstring>

namespace engine::scripting {

namespace {

// Everything a view's metamethods raise names the type, because "attempt to index" on a line
// with three property reads is not enough to fix a script by.
const char* type_name_of(const TypeBinding* binding) noexcept {
  return binding != nullptr && binding->type != nullptr ? binding->type->qualified_name : "object";
}

const schema::FieldInfo* find_field(const TypeBinding& binding, const char* key, usize len,
                                    int atom) noexcept {
  if (atom >= 0) {
    const FieldAtom* first = binding.by_atom.begin();
    const FieldAtom* last = binding.by_atom.end();
    const FieldAtom* it = std::lower_bound(
        first, last, atom, [](const FieldAtom& a, int value) { return a.atom < value; });
    if (it != last && it->atom == atom) return &binding.type->fields[it->field];
    if (binding.atoms_complete) return nullptr;
  }
  // No atom (a string built at run time before its name had one, or more field names than atoms):
  // compare names.
  const std::string_view name(key, len);
  return binding.type->find_field(name);
}

View* check_view(lua_State* L, int index) noexcept {
  return static_cast<View*>(lua_touserdatatagged(L, index, k_view_tag));
}

int view_index(lua_State* L) {
  Impl& impl = impl_of(L);
  const View* view = check_view(L, 1);
  const void* data = nullptr;
  const TypeBinding* binding = nullptr;
  if (view == nullptr || !resolve_view(impl, *view, data, binding)) {
    luaL_error(L,
               "this object view is no longer valid: the host unbound or replaced the object, "
               "or it was passed for one call only");
  }
  if (lua_type(L, 2) != LUA_TSTRING) {
    luaL_error(L, "%s: a property name must be a string, not %s", type_name_of(binding),
               luaL_typename(L, 2));
  }
  size_t len = 0;
  int atom = -1;
  const char* key = lua_tolstringatom(L, 2, &len, &atom);
  const schema::FieldInfo* field = find_field(*binding, key, len, atom);
  if (field == nullptr) luaL_error(L, "%s has no property '%s'", type_name_of(binding), key);

  // A nested view carries the slot's version; the root view's own stamp is its serial, so read
  // the version from the slot.
  const Slot& slot = impl.slots[view->slot];
  push_value(L, impl, field->type, static_cast<const u8*>(data) + field->offset, view->slot,
             slot.version);
  return 1;
}

int view_newindex(lua_State* L) {
  Impl& impl = impl_of(L);
  const View* view = check_view(L, 1);
  const void* data = nullptr;
  const TypeBinding* binding = nullptr;
  const bool valid = view != nullptr && resolve_view(impl, *view, data, binding);
  const char* key = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : "?";
  luaL_error(L,
             "%s is read-only: '%s' cannot be assigned (scripts return data; the host changes "
             "the document with commands)",
             valid ? type_name_of(binding) : "this object view", key);
}

int view_tostring(lua_State* L) {
  Impl& impl = impl_of(L);
  const View* view = check_view(L, 1);
  const void* data = nullptr;
  const TypeBinding* binding = nullptr;
  if (view == nullptr || !resolve_view(impl, *view, data, binding)) {
    lua_pushliteral(L, "<stale object view>");
  } else {
    lua_pushstring(L, binding->type->qualified_name);
  }
  return 1;
}

}  // namespace

void build_atoms(Impl& impl) {
  impl.atoms.clear();
  impl.atoms_exhausted = false;
  for (const schema::TypeInfo* type : schema::Registry::global().all()) {
    if (type->kind != schema::Kind::Struct) continue;
    for (const schema::FieldInfo& field : type->fields) {
      const std::string_view name(field.name);
      if (impl.atoms.contains(name)) continue;
      if (impl.atoms.size() >= 0x7fff) {
        impl.atoms_exhausted = true;  // the rest fall back to string compares, and still work
        return;
      }
      impl.atoms.insert(name, static_cast<i16>(impl.atoms.size()));
    }
  }
}

i16 atom_of(Impl& impl, const char* s, usize len) noexcept {
  const i16* atom = impl.atoms.find_value(std::string_view(s, len));
  return atom != nullptr ? *atom : i16{-1};
}

void register_views(lua_State* L) {
  lua_createtable(L, 0, 5);
  lua_pushcfunction(L, view_index, "__index");
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, view_newindex, "__newindex");
  lua_setfield(L, -2, "__newindex");
  lua_pushcfunction(L, view_tostring, "__tostring");
  lua_setfield(L, -2, "__tostring");
  lua_pushliteral(L, "SchemaObject");
  lua_setfield(L, -2, "__type");
  // getmetatable(view) returns this string rather than the table, so a script cannot reach the
  // metamethods; and the table is frozen in case something else hands it out.
  lua_pushliteral(L, "locked");
  lua_setfield(L, -2, "__metatable");
  lua_setreadonly(L, -1, true);
  lua_setuserdatametatable(L, k_view_tag);  // pops the table
}

u32 type_binding(Impl& impl, const schema::TypeInfo& type) {
  if (const u32* found = impl.type_index.find_value(&type)) return *found;
  ENGINE_ASSERT(type.kind == schema::Kind::Struct, "scripting: only structs are bound");
  TypeBinding binding;
  binding.type = &type;
  binding.atoms_complete = true;
  for (usize i = 0; i < type.fields.size(); ++i) {
    const i16 atom = atom_of(impl, type.fields[i].name, std::strlen(type.fields[i].name));
    if (atom < 0) {
      binding.atoms_complete = false;
      continue;
    }
    binding.by_atom.push_back(FieldAtom{atom, static_cast<u16>(i)});
  }
  std::sort(binding.by_atom.begin(), binding.by_atom.end(),
            [](const FieldAtom& a, const FieldAtom& b) { return a.atom < b.atom; });
  const u32 index = impl.types.size();
  impl.types.push_back(std::move(binding));
  impl.type_index.insert(&type, index);
  return index;
}

u32 acquire_slot(Impl& impl, ObjectRef object, bool temporary) {
  u32 index = 0;
  if (!impl.free_slots.empty()) {
    index = impl.free_slots.back();
    impl.free_slots.pop_back();
  } else {
    index = impl.slots.size();
    impl.slots.emplace_back();
  }
  Slot& slot = impl.slots[index];
  slot.type = object.type;
  slot.object = object.object;
  slot.type_index = type_binding(impl, *object.type);
  slot.live = true;
  slot.temporary = temporary;
  slot.view_ref = LUA_NOREF;
  if (temporary) impl.temporaries.push_back(index);
  return index;
}

void release_slot(Impl& impl, u32 index) noexcept {
  Slot& slot = impl.slots[index];
  if (!slot.live) return;
  if (slot.view_ref != LUA_NOREF) {
    lua_unref(impl.L, slot.view_ref);
    slot.view_ref = LUA_NOREF;
  }
  slot.live = false;
  slot.temporary = false;
  slot.object = nullptr;
  ++slot.serial;
  ++slot.version;
  impl.free_slots.push_back(index);
}

void release_temporaries(Impl& impl) noexcept {
  for (const u32 index : impl.temporaries)
    release_slot(impl, index);
  impl.temporaries.clear();
}

void push_root_view(lua_State* L, Impl& impl, u32 index) {
  const u32 serial = impl.slots[index].serial;
  auto* view = static_cast<View*>(lua_newuserdatataggedwithmetatable(L, sizeof(View), k_view_tag));
  view->data = nullptr;
  view->slot = index;
  view->stamp = serial;
  view->type_index = 0;
  view->reserved = 0;
}

bool resolve_view(const Impl& impl, const View& view, const void*& data,
                  const TypeBinding*& binding) noexcept {
  if (view.slot >= impl.slots.size()) return false;
  const Slot& slot = impl.slots[view.slot];
  if (!slot.live) return false;
  if (view.data == nullptr) {
    if (slot.serial != view.stamp) return false;
    data = slot.object;
    binding = &impl.types[slot.type_index];
    return true;
  }
  if (slot.version != view.stamp) return false;
  data = view.data;
  binding = &impl.types[view.type_index];
  return true;
}

// ---- ScriptContext: bindings -----------------------------------------------------------------

BindingId ScriptContext::bind(ObjectRef object) {
  Impl& impl = *impl_;
  if (object.type == nullptr || object.object == nullptr ||
      object.type->kind != schema::Kind::Struct) {
    impl.error = ScriptError{Status::InvalidArgument, {}, 0, "bind: not a schema struct"};
    return BindingId{};
  }
  const u32 index = acquire_slot(impl, object, false);
  push_root_view(impl.L, impl, index);
  impl.slots[index].view_ref = lua_ref(impl.L, -1);
  lua_pop(impl.L, 1);
  return BindingId{index, impl.slots[index].serial};
}

Status ScriptContext::rebind(BindingId binding, ObjectRef object) {
  Impl& impl = *impl_;
  if (!binding.valid() || binding.slot >= impl.slots.size() || !impl.slots[binding.slot].live ||
      impl.slots[binding.slot].serial != binding.serial || impl.slots[binding.slot].temporary) {
    impl.error = ScriptError{Status::NotFound, {}, 0, "rebind: no such binding"};
    return Status::NotFound;
  }
  if (object.type == nullptr || object.object == nullptr ||
      object.type->kind != schema::Kind::Struct) {
    impl.error = ScriptError{Status::InvalidArgument, {}, 0, "rebind: not a schema struct"};
    return Status::InvalidArgument;
  }
  const u32 type_index = type_binding(impl, *object.type);
  Slot& slot = impl.slots[binding.slot];
  slot.type = object.type;
  slot.object = object.object;
  slot.type_index = type_index;
  ++slot.version;  // nested views of the old object are stale; the root view follows the slot
  return Status::Ok;
}

void ScriptContext::unbind(BindingId binding) noexcept {
  Impl& impl = *impl_;
  if (!binding.valid() || binding.slot >= impl.slots.size()) return;
  const Slot& slot = impl.slots[binding.slot];
  if (!slot.live || slot.serial != binding.serial || slot.temporary) return;
  release_slot(impl, binding.slot);
}

}  // namespace engine::scripting
