// Values across the boundary: schema values into the VM (a field read), JSON into the VM (an
// argument), and whatever a script returns back out as JSON.
//
// Into the VM, the mapping is the one the type definitions declare (type_definitions.h), so the
// analyser and the run time agree about every type:
//   bool -> boolean; integers and floats -> number (a 64-bit integer only when a double holds it
//   exactly: past 2^53 the read is an error, never a silently different number); string and bytes
//   -> string; id128 -> 32 lowercase hex characters; vec2 and vec3 -> vector (vec2 with z = 0);
//   vec4 and quat -> a read-only {x, y, z, w} table; json -> read-only tables; enum -> the
//   enumerator's name (an unknown value -> its number, as schema JSON does); struct -> a nested
//   view; optional -> nil or the value; arrays and maps -> read-only tables, built on each read.
//
// Arrays and maps are copied into a table rather than viewed through another proxy so that
// everything a script can do with a read-only sequence — `#`, `ipairs`, `table.find`,
// `table.concat` — works, and the declared type `{ T }` is simply true. The cost is O(n) per read;
// hoist a collection read out of a loop.

#include "context_impl.h"

#include <core/base/assert.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <core/math/math.h>
#include <core/schema/json_reflect.h>

#include <cmath>
#include <cstring>
#include <string>

namespace engine::scripting {

namespace {

// 2^53: every integer of magnitude up to this is a double exactly.
constexpr u64 k_exact_double = u64{1} << 53;

i64 load_signed(schema::Kind kind, const void* p) noexcept {
  switch (kind) {
    case schema::Kind::I8: return *static_cast<const i8*>(p);
    case schema::Kind::I16: return *static_cast<const i16*>(p);
    case schema::Kind::I32: return *static_cast<const i32*>(p);
    case schema::Kind::I64: return *static_cast<const i64*>(p);
    case schema::Kind::U8: return *static_cast<const u8*>(p);
    case schema::Kind::U16: return *static_cast<const u16*>(p);
    case schema::Kind::U32: return *static_cast<const u32*>(p);
    case schema::Kind::U64: return static_cast<i64>(*static_cast<const u64*>(p));
    default: return 0;
  }
}

void push_components(lua_State* L, const f32* f, int count) {
  if (count <= 3) {
    lua_pushvector(L, f[0], f[1], count == 3 ? f[2] : 0.0f);
    return;
  }
  lua_createtable(L, 0, 4);
  lua_pushnumber(L, static_cast<f64>(f[0]));
  lua_setfield(L, -2, "x");
  lua_pushnumber(L, static_cast<f64>(f[1]));
  lua_setfield(L, -2, "y");
  lua_pushnumber(L, static_cast<f64>(f[2]));
  lua_setfield(L, -2, "z");
  lua_pushnumber(L, static_cast<f64>(f[3]));
  lua_setfield(L, -2, "w");
  lua_setreadonly(L, -1, true);
}

void push_enum(lua_State* L, const schema::TypeInfo& info, const void* p) {
  const i64 value = load_signed(info.enum_underlying, p);
  if (const schema::EnumValueInfo* ev = info.find_value(value)) {
    lua_pushstring(L, ev->name);
  } else {
    lua_pushnumber(L, static_cast<f64>(value));
  }
}

void push_id(lua_State* L, const Id128& id) {
  char hex[Id128::k_hex_length + 1];
  id.to_hex(hex);
  lua_pushlstring(L, hex, Id128::k_hex_length);
}

// A map key, as a script indexes it: strings and ids as strings, enums by name, integers as
// numbers.
void push_key(lua_State* L, Impl& impl, const schema::TypeRef& type, const void* p) {
  push_value(L, impl, type, p, 0, 0);
}

}  // namespace

void push_value(lua_State* L, Impl& impl, const schema::TypeRef& type, const void* p, u32 slot,
                u32 version) {
  using schema::Kind;
  lua_rawcheckstack(L, 3);
  switch (type.kind) {
    case Kind::Bool: lua_pushboolean(L, *static_cast<const bool*>(p) ? 1 : 0); return;
    case Kind::U8:
    case Kind::U16:
    case Kind::U32:
    case Kind::I8:
    case Kind::I16:
    case Kind::I32: lua_pushnumber(L, static_cast<f64>(load_signed(type.kind, p))); return;
    case Kind::I64: {
      const i64 v = *static_cast<const i64*>(p);
      const u64 magnitude = v < 0 ? u64{0} - static_cast<u64>(v) : static_cast<u64>(v);
      if (magnitude > k_exact_double) {
        luaL_error(L, "an i64 value (%lld) is beyond what a Luau number holds exactly (2^53)",
                   static_cast<long long>(v));
      }
      lua_pushnumber(L, static_cast<f64>(v));
      return;
    }
    case Kind::U64: {
      const u64 v = *static_cast<const u64*>(p);
      if (v > k_exact_double) {
        luaL_error(L, "a u64 value (%llu) is beyond what a Luau number holds exactly (2^53)",
                   static_cast<unsigned long long>(v));
      }
      lua_pushnumber(L, static_cast<f64>(v));
      return;
    }
    case Kind::F32: lua_pushnumber(L, static_cast<f64>(*static_cast<const f32*>(p))); return;
    case Kind::F64: lua_pushnumber(L, *static_cast<const f64*>(p)); return;
    case Kind::String: {
      const auto* s = static_cast<const std::string*>(p);
      lua_pushlstring(L, s->data(), s->size());
      return;
    }
    case Kind::Bytes: {
      const auto* b = static_cast<const Vector<u8>*>(p);
      lua_pushlstring(L, reinterpret_cast<const char*>(b->data()), b->size());
      return;
    }
    case Kind::Id128: push_id(L, *static_cast<const Id128*>(p)); return;
    case Kind::Vec2: push_components(L, static_cast<const f32*>(p), 2); return;
    case Kind::Vec3: push_components(L, static_cast<const f32*>(p), 3); return;
    case Kind::Vec4:
    case Kind::Quat: push_components(L, static_cast<const f32*>(p), 4); return;
    case Kind::Json: push_json(L, *static_cast<const JsonValue*>(p)); return;
    case Kind::Enum: push_enum(L, *type.type, p); return;
    case Kind::Struct: {
      const u32 type_index = type_binding(impl, *type.type);
      auto* view =
          static_cast<View*>(lua_newuserdatataggedwithmetatable(L, sizeof(View), k_view_tag));
      view->data = p;
      view->slot = slot;
      view->stamp = version;
      view->type_index = type_index;
      view->reserved = 0;
      return;
    }
    case Kind::Optional: {
      if (!type.optional_ops->has_value(p)) {
        lua_pushnil(L);
        return;
      }
      push_value(L, impl, *type.element, type.optional_ops->get(p), slot, version);
      return;
    }
    case Kind::Array:
    case Kind::FixedArray: {
      const usize n = type.array_ops->size(p);
      lua_createtable(L, static_cast<int>(n), 0);
      for (usize i = 0; i < n; ++i) {
        push_value(L, impl, *type.element, type.array_ops->at(p, i), slot, version);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
      }
      lua_setreadonly(L, -1, true);
      return;
    }
    case Kind::Map: {
      const usize n = type.map_ops->size(p);
      lua_createtable(L, 0, static_cast<int>(n));
      for (usize i = 0; i < n; ++i) {
        push_key(L, impl, *type.key, type.map_ops->key_at(p, i));
        push_value(L, impl, *type.element, type.map_ops->value_at(p, i), slot, version);
        lua_rawset(L, -3);
      }
      lua_setreadonly(L, -1, true);
      return;
    }
  }
  lua_pushnil(L);
}

void push_json(lua_State* L, const JsonValue& value) {
  lua_rawcheckstack(L, 3);
  switch (value.kind()) {
    case JsonValue::Kind::Null: lua_pushnil(L); return;
    case JsonValue::Kind::Bool: lua_pushboolean(L, value.as_bool() ? 1 : 0); return;
    case JsonValue::Kind::Int: lua_pushnumber(L, static_cast<f64>(value.as_int())); return;
    case JsonValue::Kind::Uint: lua_pushnumber(L, static_cast<f64>(value.as_uint())); return;
    case JsonValue::Kind::Float: lua_pushnumber(L, value.as_float()); return;
    case JsonValue::Kind::String: {
      const std::string_view s = value.as_string();
      lua_pushlstring(L, s.data(), s.size());
      return;
    }
    case JsonValue::Kind::Array: {
      const JsonValue::Array& array = value.as_array();
      lua_createtable(L, static_cast<int>(array.size()), 0);
      for (u32 i = 0; i < array.size(); ++i) {
        push_json(L, array[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
      }
      lua_setreadonly(L, -1, true);
      return;
    }
    case JsonValue::Kind::Object: {
      const JsonValue::Object& object = value.as_object();
      lua_createtable(L, 0, static_cast<int>(object.size()));
      for (u32 i = 0; i < object.size(); ++i) {
        const std::string& key = object.key_at(i);
        lua_pushlstring(L, key.data(), key.size());
        push_json(L, object.value_at(i));
        lua_rawset(L, -3);
      }
      lua_setreadonly(L, -1, true);
      return;
    }
  }
  lua_pushnil(L);
}

// ---- out of the VM -----------------------------------------------------------------------------

namespace {

constexpr int k_max_depth = 64;

JsonValue number_json(f64 n) {
  // An integral value in the exact range becomes an integer, so `return 3` round-trips as 3 and
  // not 3.0; anything else stays a double.
  if (std::isfinite(n) && std::floor(n) == n && std::fabs(n) <= static_cast<f64>(k_exact_double))
    return JsonValue(static_cast<i64>(n));
  return JsonValue(n);
}

bool convert(lua_State* L, Impl& impl, int index, JsonValue& out, std::string& error, int depth);

bool convert_table(lua_State* L, Impl& impl, int index, JsonValue& out, std::string& error,
                   int depth) {
  if (depth >= k_max_depth) {
    error = "a returned table nests deeper than 64 levels (or refers to itself)";
    return false;
  }
  if (!lua_checkstack(L, 3)) {
    error = "out of stack converting a returned table";
    return false;
  }
  index = lua_absindex(L, index);
  // Count the keys and classify them in one pass: a table is a sequence when its keys are exactly
  // 1..n, an object when they are all strings; anything else has no JSON form.
  const int length = lua_objlen(L, index);
  int keys = 0;
  bool all_strings = true;
  bool all_in_sequence = true;
  lua_pushnil(L);
  while (lua_next(L, index) != 0) {
    ++keys;
    const int key_type = lua_type(L, -2);
    if (key_type != LUA_TSTRING) all_strings = false;
    if (key_type == LUA_TNUMBER) {
      const f64 k = lua_tonumber(L, -2);
      if (!(k >= 1.0 && k <= static_cast<f64>(length) && std::floor(k) == k))
        all_in_sequence = false;
    } else {
      all_in_sequence = false;
    }
    lua_pop(L, 1);
  }

  if (keys == 0) {
    // Ambiguous in Lua; an empty list is the common case for data a script returns ("no
    // actions"), and the schema reader accepts [] for any array.
    out = JsonValue::array();
    return true;
  }
  if (all_in_sequence && keys == length) {
    JsonValue::Array array;
    array.reserve(static_cast<u32>(length));
    for (int i = 1; i <= length; ++i) {
      lua_rawgeti(L, index, i);
      JsonValue element;
      const bool ok = convert(L, impl, -1, element, error, depth + 1);
      lua_pop(L, 1);
      if (!ok) return false;
      array.push_back(std::move(element));
    }
    out = JsonValue(std::move(array));
    return true;
  }
  if (!all_strings) {
    error =
        "a returned table mixes key types or has non-string keys; data is a sequence (keys "
        "1..n) or a record (string keys)";
    return false;
  }
  JsonValue::Object object;
  lua_pushnil(L);
  while (lua_next(L, index) != 0) {
    size_t len = 0;
    const char* key = lua_tolstring(L, -2, &len);  // a string already: no conversion, no allocation
    JsonValue member;
    if (!convert(L, impl, -1, member, error, depth + 1)) {
      lua_pop(L, 2);
      return false;
    }
    object.insert_or_assign(std::string(key, len), std::move(member));
    lua_pop(L, 1);
  }
  out = JsonValue(std::move(object));
  return true;
}

bool convert(lua_State* L, Impl& impl, int index, JsonValue& out, std::string& error, int depth) {
  switch (lua_type(L, index)) {
    case LUA_TNIL:
    case LUA_TNONE: out = JsonValue(); return true;
    case LUA_TBOOLEAN: out = JsonValue(lua_toboolean(L, index) != 0); return true;
    case LUA_TNUMBER: {
      const f64 n = lua_tonumber(L, index);
      if (!std::isfinite(n)) {
        error = "a returned number is not finite (nan or inf has no JSON form)";
        return false;
      }
      out = number_json(n);
      return true;
    }
    case LUA_TSTRING: {
      size_t len = 0;
      const char* s = lua_tolstring(L, index, &len);
      out = JsonValue(std::string_view(s, len));
      return true;
    }
    case LUA_TVECTOR: {
      const f32* v = lua_tovector(L, index);
      JsonValue::Array array;
      for (int i = 0; i < 3; ++i)
        array.push_back(number_json(static_cast<f64>(v[i])));
      out = JsonValue(std::move(array));
      return true;
    }
    case LUA_TTABLE: return convert_table(L, impl, index, out, error, depth);
    case LUA_TUSERDATA: {
      const auto* view = static_cast<const View*>(lua_touserdatatagged(L, index, k_view_tag));
      const void* data = nullptr;
      const TypeBinding* binding = nullptr;
      if (view == nullptr) {
        error = "a returned userdata is not a schema object";
        return false;
      }
      if (!resolve_view(impl, *view, data, binding)) {
        error = "a returned object view is no longer valid";
        return false;
      }
      if (!schema::to_json(*binding->type, data, out)) {
        error = "a returned schema object did not convert to JSON";
        return false;
      }
      return true;
    }
    default: break;
  }
  error = std::string("a ") + lua_typename(L, lua_type(L, index)) +
          " has no JSON form (data is nil, booleans, numbers, strings, vectors, tables and "
          "schema objects)";
  return false;
}

}  // namespace

bool to_json(lua_State* L, Impl& impl, int index, JsonValue& out, std::string& error) {
  return convert(L, impl, index, out, error, 0);
}

}  // namespace engine::scripting
