#include <core/schema/json_reflect.h>

#include <core/base/assert.h>
#include <core/ids/id128.h>

#include <cmath>
#include <cstring>
#include <limits>

namespace engine::schema {

// --- ReadContext ------------------------------------------------------------------------------

void ReadContext::error(std::string_view message) {
  diagnostics.push_back(Diagnostic{path(), std::string(message)});
}

std::string ReadContext::path() const {
  std::string out;
  for (u32 i = 0; i < segments_.size(); ++i) {
    const std::string& s = segments_[i];
    if (!s.empty() && s[0] == '[') {
      out += s;
    } else {
      if (!out.empty()) out.push_back('.');
      out += s;
    }
  }
  return out;
}

void ReadContext::push_field(const char* name) { segments_.push_back(std::string(name)); }

void ReadContext::push_index(usize index) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "[%llu]", static_cast<unsigned long long>(index));
  segments_.push_back(std::string(buf));
}

void ReadContext::pop() {
  if (!segments_.empty()) segments_.pop_back();
}

namespace {

// --- scalar helpers ---------------------------------------------------------------------------

template <class T>
T load(const void* p) noexcept {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

template <class T>
void store(void* p, T v) noexcept {
  std::memcpy(p, &v, sizeof(T));
}

i64 load_integer(Kind kind, const void* p) noexcept {
  switch (kind) {
    case Kind::U8: return load<u8>(p);
    case Kind::U16: return load<u16>(p);
    case Kind::U32: return load<u32>(p);
    case Kind::U64: return static_cast<i64>(load<u64>(p));
    case Kind::I8: return load<i8>(p);
    case Kind::I16: return load<i16>(p);
    case Kind::I32: return load<i32>(p);
    case Kind::I64: return load<i64>(p);
    default: return 0;
  }
}

void store_integer(Kind kind, void* p, i64 v) noexcept {
  switch (kind) {
    case Kind::U8: store<u8>(p, static_cast<u8>(v)); break;
    case Kind::U16: store<u16>(p, static_cast<u16>(v)); break;
    case Kind::U32: store<u32>(p, static_cast<u32>(v)); break;
    case Kind::U64: store<u64>(p, static_cast<u64>(v)); break;
    case Kind::I8: store<i8>(p, static_cast<i8>(v)); break;
    case Kind::I16: store<i16>(p, static_cast<i16>(v)); break;
    case Kind::I32: store<i32>(p, static_cast<i32>(v)); break;
    case Kind::I64: store<i64>(p, v); break;
    default: break;
  }
}

bool integer_fits(Kind kind, i64 v, u64 uv, bool is_unsigned_source) noexcept {
  switch (kind) {
    case Kind::U8: return is_unsigned_source ? uv <= 0xFF : (v >= 0 && v <= 0xFF);
    case Kind::U16: return is_unsigned_source ? uv <= 0xFFFF : (v >= 0 && v <= 0xFFFF);
    case Kind::U32: return is_unsigned_source ? uv <= 0xFFFFFFFFull : (v >= 0 && v <= 0xFFFFFFFFll);
    case Kind::U64: return is_unsigned_source || v >= 0;
    case Kind::I8: return !is_unsigned_source ? (v >= -128 && v <= 127) : uv <= 127;
    case Kind::I16: return !is_unsigned_source ? (v >= -32768 && v <= 32767) : uv <= 32767;
    case Kind::I32:
      return !is_unsigned_source ? (v >= std::numeric_limits<i32>::min() && v <= std::numeric_limits<i32>::max())
                                 : uv <= static_cast<u64>(std::numeric_limits<i32>::max());
    case Kind::I64: return !is_unsigned_source || uv <= static_cast<u64>(std::numeric_limits<i64>::max());
    default: return false;
  }
}

constexpr char k_hex[] = "0123456789abcdef";

int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// --- writing ----------------------------------------------------------------------------------

bool write_integer(Kind kind, const void* p, JsonValue& out) {
  if (kind == Kind::U64) {
    out = JsonValue(load<u64>(p));
  } else if (is_unsigned(kind)) {
    out = JsonValue(static_cast<u64>(load_integer(kind, p)));
  } else {
    out = JsonValue(load_integer(kind, p));
  }
  return true;
}

bool write_enum(const TypeInfo& info, const void* p, JsonValue& out) {
  const i64 v = load_integer(info.enum_underlying, p);
  if (const EnumValueInfo* ev = info.find_value(v)) {
    out = JsonValue(std::string_view(ev->name));
  } else {
    out = JsonValue(v);  // unknown enumerator: keep the raw value rather than losing it
  }
  return true;
}

bool write_struct(const TypeInfo& info, const void* obj, JsonValue& out) {
  out = JsonValue::object();
  bool ok = true;
  const auto* base = static_cast<const std::byte*>(obj);
  for (const FieldInfo& f : info.fields) {
    if ((f.flags & FieldFlag::transient) != 0) continue;
    JsonValue v;
    ok = to_json(f.type, base + f.offset, v) && ok;
    out.set(f.name, std::move(v));
  }
  return ok;
}

// --- reading ----------------------------------------------------------------------------------

bool read_integer(Kind kind, void* p, const JsonValue& in, ReadContext& ctx) {
  i64 v = 0;
  u64 uv = 0;
  bool is_unsigned_source = false;
  if (in.is_uint()) {
    uv = in.as_uint();
    is_unsigned_source = true;
    v = static_cast<i64>(uv);
  } else if (in.is_int()) {
    v = in.as_int();
    uv = static_cast<u64>(v);
  } else if (in.is_float()) {
    if (!in.get_i64(v)) {
      if (in.get_u64(uv)) {
        is_unsigned_source = true;
      } else {
        ctx.error("expected an integer");
        return false;
      }
    } else {
      uv = static_cast<u64>(v);
    }
  } else {
    ctx.error("expected an integer");
    return false;
  }
  if (!integer_fits(kind, v, uv, is_unsigned_source)) {
    ctx.error("integer out of range for the field type");
    return false;
  }
  store_integer(kind, p, is_unsigned_source && kind == Kind::U64 ? static_cast<i64>(uv) : v);
  return true;
}

bool read_enum(const TypeInfo& info, void* p, const JsonValue& in, ReadContext& ctx) {
  if (in.is_string()) {
    const EnumValueInfo* ev = info.find_value(in.as_string());
    if (ev == nullptr) {
      ctx.error("unknown enumerator name");
      return false;
    }
    store_integer(info.enum_underlying, p, ev->value);
    return true;
  }
  i64 v;
  if (!in.get_i64(v)) {
    ctx.error("expected an enumerator name or integer");
    return false;
  }
  if (!integer_fits(info.enum_underlying, v, static_cast<u64>(v), false)) {
    ctx.error("enum value out of range for its underlying type");
    return false;
  }
  store_integer(info.enum_underlying, p, v);
  return true;
}

bool read_struct(const TypeInfo& info, void* obj, const JsonValue& in, ReadContext& ctx) {
  if (!in.is_object()) {
    ctx.error("expected an object");
    return false;
  }
  bool ok = true;
  auto* base = static_cast<std::byte*>(obj);
  for (auto [key, value] : in.as_object()) {
    const FieldInfo* f = info.find_field(key);
    if (f == nullptr) {
      if (!ctx.options.ignore_unknown_fields) {
        ctx.push_field(key.c_str());
        ctx.error("unknown field");
        ctx.pop();
        ok = false;
      }
      continue;
    }
    if ((f->flags & FieldFlag::transient) != 0) continue;
    ctx.push_field(f->name);
    ok = from_json(f->type, base + f->offset, value, ctx) && ok;
    ctx.pop();
  }
  return ok;
}

}  // namespace

// --- TypeRef walkers --------------------------------------------------------------------------

bool to_json(const TypeRef& type, const void* object, JsonValue& out) {
  switch (type.kind) {
    case Kind::Bool: out = JsonValue(load<bool>(object)); return true;
    case Kind::U8:
    case Kind::U16:
    case Kind::U32:
    case Kind::U64:
    case Kind::I8:
    case Kind::I16:
    case Kind::I32:
    case Kind::I64: return write_integer(type.kind, object, out);
    case Kind::F32: out = JsonValue(static_cast<f64>(load<f32>(object))); return true;
    case Kind::F64: out = JsonValue(load<f64>(object)); return true;
    case Kind::String: out = JsonValue(std::string_view(*static_cast<const std::string*>(object))); return true;
    case Kind::Bytes: {
      const auto& bytes = *static_cast<const Vector<u8>*>(object);
      std::string hex;
      hex.reserve(bytes.size() * 2);
      for (u8 b : bytes) {
        hex.push_back(k_hex[b >> 4]);
        hex.push_back(k_hex[b & 0xF]);
      }
      out = JsonValue(std::string_view(hex));
      return true;
    }
    case Kind::Id128: {
      char hex[33];
      static_cast<const Id128*>(object)->to_hex(hex);
      out = JsonValue(std::string_view(hex, 32));
      return true;
    }
    case Kind::Enum: return write_enum(*type.type, object, out);
    case Kind::Struct: return write_struct(*type.type, object, out);
    case Kind::Optional: {
      if (!type.optional_ops->has_value(object)) {
        out = JsonValue();
        return true;
      }
      return to_json(*type.element, type.optional_ops->get(object), out);
    }
    case Kind::Array:
    case Kind::FixedArray: {
      const usize n = type.array_ops->size(object);
      out = JsonValue::array();
      bool ok = true;
      for (usize i = 0; i < n; ++i) {
        JsonValue v;
        ok = to_json(*type.element, type.array_ops->at(object, i), v) && ok;
        out.push_back(std::move(v));
      }
      return ok;
    }
    case Kind::Map: {
      const usize n = type.map_ops->size(object);
      bool ok = true;
      if (type.key->kind == Kind::String) {
        out = JsonValue::object();
        for (usize i = 0; i < n; ++i) {
          JsonValue v;
          ok = to_json(*type.element, type.map_ops->value_at(object, i), v) && ok;
          out.set(*static_cast<const std::string*>(type.map_ops->key_at(object, i)), std::move(v));
        }
      } else {
        out = JsonValue::array();
        for (usize i = 0; i < n; ++i) {
          JsonValue pair = JsonValue::array();
          JsonValue k, v;
          ok = to_json(*type.key, type.map_ops->key_at(object, i), k) && ok;
          ok = to_json(*type.element, type.map_ops->value_at(object, i), v) && ok;
          pair.push_back(std::move(k));
          pair.push_back(std::move(v));
          out.push_back(std::move(pair));
        }
      }
      return ok;
    }
  }
  return false;
}

bool from_json(const TypeRef& type, void* object, const JsonValue& in, ReadContext& ctx) {
  switch (type.kind) {
    case Kind::Bool: {
      bool b;
      if (!in.get_bool(b)) {
        ctx.error("expected a boolean");
        return false;
      }
      store<bool>(object, b);
      return true;
    }
    case Kind::U8:
    case Kind::U16:
    case Kind::U32:
    case Kind::U64:
    case Kind::I8:
    case Kind::I16:
    case Kind::I32:
    case Kind::I64: return read_integer(type.kind, object, in, ctx);
    case Kind::F32:
    case Kind::F64: {
      f64 d;
      if (!in.get_f64(d)) {
        ctx.error("expected a number");
        return false;
      }
      if (type.kind == Kind::F32) {
        store<f32>(object, static_cast<f32>(d));
      } else {
        store<f64>(object, d);
      }
      return true;
    }
    case Kind::String: {
      std::string_view s;
      if (!in.get_string(s)) {
        ctx.error("expected a string");
        return false;
      }
      static_cast<std::string*>(object)->assign(s.data(), s.size());
      return true;
    }
    case Kind::Bytes: {
      std::string_view s;
      if (!in.get_string(s) || (s.size() % 2) != 0) {
        ctx.error("expected a hex string with an even number of digits");
        return false;
      }
      auto& bytes = *static_cast<Vector<u8>*>(object);
      bytes.clear();
      bytes.reserve(static_cast<u32>(s.size() / 2));
      for (usize i = 0; i < s.size(); i += 2) {
        const int hi = hex_value(s[i]);
        const int lo = hex_value(s[i + 1]);
        if (hi < 0 || lo < 0) {
          ctx.error("invalid hex digit in bytes");
          return false;
        }
        bytes.push_back(static_cast<u8>((hi << 4) | lo));
      }
      return true;
    }
    case Kind::Id128: {
      std::string_view s;
      Id128 id;
      if (!in.get_string(s) || !Id128::from_hex(s, id)) {
        ctx.error("expected a 32-character hex id");
        return false;
      }
      *static_cast<Id128*>(object) = id;
      return true;
    }
    case Kind::Enum: return read_enum(*type.type, object, in, ctx);
    case Kind::Struct: return read_struct(*type.type, object, in, ctx);
    case Kind::Optional: {
      if (in.is_null()) {
        type.optional_ops->reset(object);
        return true;
      }
      void* slot = type.optional_ops->emplace(object);
      return from_json(*type.element, slot, in, ctx);
    }
    case Kind::Array: {
      if (!in.is_array()) {
        ctx.error("expected an array");
        return false;
      }
      type.array_ops->clear(object);
      bool ok = true;
      const JsonValue::Array& items = in.as_array();
      for (u32 i = 0; i < items.size(); ++i) {
        void* slot = type.array_ops->push_back(object);
        ctx.push_index(i);
        ok = from_json(*type.element, slot, items[i], ctx) && ok;
        ctx.pop();
      }
      return ok;
    }
    case Kind::FixedArray: {
      if (!in.is_array() || in.size() != type.fixed_count) {
        ctx.error("expected an array of the declared fixed length");
        return false;
      }
      bool ok = true;
      const JsonValue::Array& items = in.as_array();
      for (u32 i = 0; i < items.size(); ++i) {
        ctx.push_index(i);
        ok = from_json(*type.element, type.array_ops->at_mut(object, i), items[i], ctx) && ok;
        ctx.pop();
      }
      return ok;
    }
    case Kind::Map: {
      bool ok = true;
      type.map_ops->clear(object);
      if (type.key->kind == Kind::String) {
        if (!in.is_object()) {
          ctx.error("expected an object");
          return false;
        }
        for (auto [key, value] : in.as_object()) {
          void* slot = type.map_ops->insert(object, &key);
          ctx.push_field(key.c_str());
          ok = from_json(*type.element, slot, value, ctx) && ok;
          ctx.pop();
        }
        return ok;
      }
      if (!in.is_array()) {
        ctx.error("expected an array of [key, value] pairs");
        return false;
      }
      const JsonValue::Array& pairs = in.as_array();
      // Keys are decoded into a temporary of the key type; a small stack buffer covers every
      // scalar key kind the IDL permits (integers, ids, enums).
      alignas(16) std::byte key_storage[32];
      ENGINE_VERIFY(type.key->size <= sizeof(key_storage), "schema: map key type too large");
      for (u32 i = 0; i < pairs.size(); ++i) {
        ctx.push_index(i);
        if (!pairs[i].is_array() || pairs[i].size() != 2) {
          ctx.error("expected a [key, value] pair");
          ctx.pop();
          ok = false;
          continue;
        }
        std::memset(key_storage, 0, sizeof(key_storage));
        if (!from_json(*type.key, key_storage, pairs[i][0], ctx)) {
          ctx.pop();
          ok = false;
          continue;
        }
        void* slot = type.map_ops->insert(object, key_storage);
        ok = from_json(*type.element, slot, pairs[i][1], ctx) && ok;
        ctx.pop();
      }
      return ok;
    }
  }
  return false;
}

// --- TypeInfo entry points --------------------------------------------------------------------

bool to_json(const TypeInfo& type, const void* object, JsonValue& out) {
  TypeRef ref;
  ref.kind = type.kind;
  ref.type = &type;
  ref.size = type.size;
  ref.align = type.align;
  return to_json(ref, object, out);
}

bool from_json(const TypeInfo& type, void* object, const JsonValue& in, ReadContext& ctx) {
  TypeRef ref;
  ref.kind = type.kind;
  ref.type = &type;
  ref.size = type.size;
  ref.align = type.align;
  return from_json(ref, object, in, ctx);
}

// --- migrations -------------------------------------------------------------------------------

MigrationRegistry& MigrationRegistry::global() {
  static MigrationRegistry registry;
  return registry;
}

void MigrationRegistry::add(const Migration& migration) { migrations_.push_back(migration); }

bool MigrationRegistry::migrate(const TypeInfo& type, JsonValue& object, u16 from_version, ReadContext& ctx) const {
  const std::string_view name(type.qualified_name);
  for (u16 v = from_version; v < type.version; ++v) {
    const Migration* step = nullptr;
    for (const Migration& m : migrations_) {
      if (m.from_version == v && name == m.type_qualified_name) {
        step = &m;
        break;
      }
    }
    if (step == nullptr) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "no migration registered from version %u to %u", v, v + 1);
      ctx.error(buf);
      return false;
    }
    if (!step->apply(object, ctx)) return false;
  }
  return true;
}

bool from_json_versioned(const TypeInfo& type, void* object, JsonValue in, u16 stored_version, ReadContext& ctx) {
  if (stored_version > type.version) {
    ctx.error("stored schema version is newer than this build understands");
    return false;
  }
  if (stored_version < type.version && !MigrationRegistry::global().migrate(type, in, stored_version, ctx)) {
    return false;
  }
  return from_json(type, object, in, ctx);
}

}  // namespace engine::schema
