#include <core/base/assert.h>
#include <core/ids/id128.h>
#include <core/json/json_format.h>
#include <core/json/json_reader.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <core/schema/json_reflect.h>
#include <core/schema/json_text.h>

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
      return !is_unsigned_source
                 ? (v >= std::numeric_limits<i32>::min() && v <= std::numeric_limits<i32>::max())
                 : uv <= static_cast<u64>(std::numeric_limits<i32>::max());
    case Kind::I64:
      return !is_unsigned_source || uv <= static_cast<u64>(std::numeric_limits<i64>::max());
    default: return false;
  }
}

constexpr char k_hex[] = "0123456789abcdef";

usize component_count(Kind kind) noexcept {
  switch (kind) {
    case Kind::Vec2: return 2;
    case Kind::Vec3: return 3;
    case Kind::Vec4:
    case Kind::Quat: return 4;
    default: return 0;
  }
}

// Vec2/3/4 and Quat are plain f32 component arrays in memory.
bool write_components(Kind kind, const void* p, JsonValue& out) {
  const usize n = component_count(kind);
  const auto* f = static_cast<const f32*>(p);
  out = JsonValue::array();
  for (usize i = 0; i < n; ++i)
    out.push_back(JsonValue(static_cast<f64>(f[i])));
  return true;
}

// A double as a float, or false when it is past the float's range (T187): the cast of a value
// past FLT_MAX is undefined behaviour, and in practice an infinity that every field downstream —
// a position, a scale, a rate — took as a number. The parser refuses an out-of-range double, so
// `d` is finite here; within the range the cast rounds to nearest as it always did.
bool f32_of(f64 d, f32& out) noexcept {
  constexpr f64 k_max = static_cast<f64>(std::numeric_limits<f32>::max());
  if (!(d >= -k_max && d <= k_max)) return false;
  out = static_cast<f32>(d);
  return true;
}

constexpr const char* k_f32_range = "a number past a 32-bit float's range (about 3.4e38)";

// A short array of numbers as either reader found it (D40): whether the value was an array, how
// many elements it had, and the first four as numbers where they were numbers. Both readers check
// a vector through the one function below, so they refuse it with the same words.
struct NumberList {
  bool is_array = false;
  usize count = 0;
  bool numeric[4] = {};
  f64 value[4] = {};
};

NumberList number_list(const JsonValue& in) {
  NumberList list;
  list.is_array = in.is_array();
  if (!list.is_array) return list;
  list.count = in.size();
  for (usize i = 0; i < list.count && i < 4; ++i)
    list.numeric[i] = in[i].get_f64(list.value[i]);
  return list;
}

bool read_components(Kind kind, void* p, const NumberList& in, ReadContext& ctx) {
  const usize n = component_count(kind);
  if (!in.is_array || in.count != n) {
    ctx.error("expected an array of the type's component count");
    return false;
  }
  auto* f = static_cast<f32*>(p);
  for (usize i = 0; i < n; ++i) {
    if (!in.numeric[i]) {
      ctx.error("expected numeric components");
      return false;
    }
    if (!f32_of(in.value[i], f[i])) {
      ctx.error(k_f32_range);
      return false;
    }
  }
  return true;
}

// WorldPos and DVec3 are three f64 in memory (core/math/world.h), and three JSON numbers on the
// wire. A JsonValue holds the f64 itself and the writer prints the shortest text that reads back to
// it (`std::to_chars`, core/json), so the round trip is exact to the bit and the text is a function
// of the double alone, the same from every toolchain.
bool write_f64_vector(const void* p, JsonValue& out) {
  f64 v[3];
  std::memcpy(v, p, sizeof(v));
  out = JsonValue::array();
  for (const f64 c : v)
    out.push_back(JsonValue(c));
  return true;
}

// Read whole and checked before anything is stored, so a refused value leaves the field as it
// was. A world position from outside — a file, the wire — must be one a cell can name
// (`world_cell_valid`, ADR-0053): past ±1.37e11 m the GPU's form of it does not exist, and the
// conversion to it does not check, so the check is here, at the door, with the field's path.
bool read_f64_vector(Kind kind, void* p, const NumberList& in, ReadContext& ctx) {
  if (!in.is_array || in.count != 3) {
    ctx.error(kind == Kind::WorldPos ? "expected a world position, an array of 3 numbers"
                                     : "expected a displacement, an array of 3 numbers");
    return false;
  }
  f64 v[3];
  for (usize i = 0; i < 3; ++i) {
    if (!in.numeric[i]) {
      ctx.error("expected numeric components");
      return false;
    }
    v[i] = in.value[i];
    if (!std::isfinite(v[i])) {
      ctx.error("a component is not finite");
      return false;
    }
  }
  if (kind == Kind::WorldPos && !world_cell_valid(WorldPos{v[0], v[1], v[2]})) {
    ctx.error(
        "world position out of range: every axis must lie in [-137438953408, 137438953408) m, "
        "where a 64 m cell's i32 index ends (ADR-0053)");
    return false;
  }
  std::memcpy(p, v, sizeof(v));
  return true;
}

int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// A string field's kinds read from the string itself, by both readers (D40).
constexpr const char* k_bytes_expected = "expected a hex string with an even number of digits";
constexpr const char* k_id_expected = "expected a 32-character hex id";

bool read_bytes(std::string_view s, void* object, ReadContext& ctx) {
  if ((s.size() % 2) != 0) {
    ctx.error(k_bytes_expected);
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

bool read_id(std::string_view s, void* object, ReadContext& ctx) {
  Id128 id;
  if (!Id128::from_hex(s, id)) {
    ctx.error(k_id_expected);
    return false;
  }
  *static_cast<Id128*>(object) = id;
  return true;
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

bool read_enum_name(const TypeInfo& info, void* p, std::string_view name, ReadContext& ctx) {
  const EnumValueInfo* ev = info.find_value(name);
  if (ev == nullptr) {
    ctx.error("unknown enumerator name");
    return false;
  }
  store_integer(info.enum_underlying, p, ev->value);
  return true;
}

bool read_enum(const TypeInfo& info, void* p, const JsonValue& in, ReadContext& ctx) {
  if (in.is_string()) return read_enum_name(info, p, in.as_string(), ctx);
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

// The name, other than `key`, under which `object` also carries `field` — its own or another former
// one — or nullptr.
const char* other_name_present(const TypeInfo& info, const FieldInfo& field,
                               const JsonValue& object, std::string_view key) {
  if (key != field.name && object.find(field.name) != nullptr) return field.name;
  const auto index = static_cast<u32>(&field - info.fields.data());
  for (const FieldAlias& a : info.field_aliases) {
    if (a.field == index && key != a.former_name && object.find(a.former_name) != nullptr)
      return a.former_name;
  }
  return nullptr;
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
    if (f == nullptr && !info.field_aliases.empty()) {
      // A former name (`@alias`) reads as the field. An object carrying the field under two of its
      // names is refused by both rather than read as whichever came last.
      f = info.find_field_alias(key);
      if (f != nullptr) {
        if (const char* other = other_name_present(info, *f, in, key)) {
          ctx.push_field(key.c_str());
          ctx.error(std::string("carries both '") + key + "' and '" + other + "', two names of '" +
                    f->name + "'");
          ctx.pop();
          ok = false;
          continue;
        }
      }
    }
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

// One [key, value] pair of a map whose keys are not strings, the pair's index `i` on the path.
bool read_map_pair(const TypeRef& type, void* object, const JsonValue& pair, u32 i,
                   ReadContext& ctx) {
  // Keys are decoded into a temporary of the key type; a small stack buffer covers every scalar
  // key kind the IDL permits (integers, ids, enums).
  alignas(16) std::byte key_storage[32];
  ENGINE_VERIFY(type.key->size <= sizeof(key_storage), "schema: map key type too large");
  ctx.push_index(i);
  if (!pair.is_array() || pair.size() != 2) {
    ctx.error("expected a [key, value] pair");
    ctx.pop();
    return false;
  }
  std::memset(key_storage, 0, sizeof(key_storage));
  if (!from_json(*type.key, key_storage, pair[0], ctx)) {
    ctx.pop();
    return false;
  }
  void* slot = type.map_ops->insert(object, key_storage);
  const bool ok = from_json(*type.element, slot, pair[1], ctx);
  ctx.pop();
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
    case Kind::String:
      out = JsonValue(std::string_view(*static_cast<const std::string*>(object)));
      return true;
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
    case Kind::Vec2:
    case Kind::Vec3:
    case Kind::Vec4:
    case Kind::Quat: return write_components(type.kind, object, out);
    case Kind::WorldPos:
    case Kind::DVec3: return write_f64_vector(object, out);
    case Kind::Json: out = *static_cast<const JsonValue*>(object); return true;
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
        f32 f = 0.0f;
        if (!f32_of(d, f)) {
          ctx.error(k_f32_range);
          return false;
        }
        store<f32>(object, f);
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
      if (!in.get_string(s)) {
        ctx.error(k_bytes_expected);
        return false;
      }
      return read_bytes(s, object, ctx);
    }
    case Kind::Id128: {
      std::string_view s;
      if (!in.get_string(s)) {
        ctx.error(k_id_expected);
        return false;
      }
      return read_id(s, object, ctx);
    }
    case Kind::Vec2:
    case Kind::Vec3:
    case Kind::Vec4:
    case Kind::Quat: return read_components(type.kind, object, number_list(in), ctx);
    case Kind::WorldPos:
    case Kind::DVec3: return read_f64_vector(type.kind, object, number_list(in), ctx);
    case Kind::Json: *static_cast<JsonValue*>(object) = in; return true;
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
      for (u32 i = 0; i < pairs.size(); ++i)
        ok = read_map_pair(type, object, pairs[i], i, ctx) && ok;
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

// --- former names -----------------------------------------------------------------------------

namespace {

bool rename_value(const TypeRef& type, JsonValue& value, ReadContext& ctx, u32& renamed) {
  switch (type.kind) {
    case Kind::Struct:
      if (!value.is_object()) return true;  // the reader says what is wrong with it
      return rename_former_fields(*type.type, value.as_object(), ctx, renamed);
    case Kind::Optional: return value.is_null() || rename_value(*type.element, value, ctx, renamed);
    case Kind::Array:
    case Kind::FixedArray: {
      if (!value.is_array()) return true;
      bool ok = true;
      for (usize i = 0; i < value.size(); ++i) {
        ctx.push_index(i);
        ok = rename_value(*type.element, value[i], ctx, renamed) && ok;
        ctx.pop();
      }
      return ok;
    }
    case Kind::Map: {
      bool ok = true;
      if (value.is_object()) {
        JsonValue::Object& members = value.as_object();
        for (u32 i = 0; i < members.size(); ++i) {
          ctx.push_field(members.key_at(i).c_str());
          ok = rename_value(*type.element, members.value_at(i), ctx, renamed) && ok;
          ctx.pop();
        }
      } else if (value.is_array()) {  // a map with non-string keys: [[key, value], ...]
        for (usize i = 0; i < value.size(); ++i) {
          JsonValue& pair = value[i];
          if (!pair.is_array() || pair.size() != 2) continue;
          ctx.push_index(i);
          ok = rename_value(*type.element, pair[1], ctx, renamed) && ok;
          ctx.pop();
        }
      }
      return ok;
    }
    default: return true;  // scalars and `json` hold no field of a schema type
  }
}

}  // namespace

bool rename_former_fields(const TypeInfo& type, JsonValue::Object& members, ReadContext& ctx,
                          u32& renamed) {
  if (type.kind != Kind::Struct) return true;
  bool ok = true;
  if (!type.field_aliases.empty()) {
    // Found first and renamed after: a rename moves an entry of the sorted map.
    Vector<std::string> former;
    for (u32 i = 0; i < members.size(); ++i) {
      const std::string& key = members.key_at(i);
      if (type.find_field(key) == nullptr && type.find_field_alias(key) != nullptr)
        former.push_back(key);
    }
    for (const std::string& key : former) {
      const FieldInfo& field = *type.find_field_alias(key);
      if (members.contains(std::string_view(field.name))) {
        // Two names of one field, the current one and a former one, or two former ones (the
        // first already renamed): refused by name, never read as whichever was last.
        ctx.push_field(key.c_str());
        ctx.error(std::string("carries both '") + key + "' and '" + field.name +
                  "', two names of one field");
        ctx.pop();
        ok = false;
        continue;
      }
      JsonValue value = std::move(*members.find_value(key));
      members.erase(key);
      members.insert_or_assign(std::string(field.name), std::move(value));
      ++renamed;
    }
  }
  for (u32 i = 0; i < members.size(); ++i) {
    const FieldInfo* field = type.find_field(members.key_at(i));
    if (field == nullptr) continue;
    ctx.push_field(field->name);
    ok = rename_value(field->type, members.value_at(i), ctx, renamed) && ok;
    ctx.pop();
  }
  return ok;
}

bool rename_former_fields(const TypeRef& type, JsonValue& value, ReadContext& ctx, u32& renamed) {
  return rename_value(type, value, ctx, renamed);
}

// --- migrations -------------------------------------------------------------------------------

MigrationRegistry& MigrationRegistry::global() {
  static MigrationRegistry registry;
  return registry;
}

void MigrationRegistry::add(const Migration& migration) { migrations_.push_back(migration); }

bool additive_step(const TypeInfo& type, u16 to_version) noexcept {
  if (type.kind != Kind::Struct || to_version < 2 || to_version > type.version) return false;
  // Only a version the schema declares additive (`@additive`; review B4, batch 68): a version
  // that adds a field and also changes an existing one's meaning under its name must be refused
  // until a step says what to do, and the tables cannot tell the two apart.
  if (to_version > 63 || (type.additive_versions & (u64{1} << to_version)) == 0) return false;
  for (const FieldInfo& field : type.fields) {
    if (field.since_version == to_version) return true;
  }
  return false;
}

bool MigrationRegistry::migrate(const TypeInfo& type, JsonValue& object, u16 from_version,
                                ReadContext& ctx) const {
  const std::string_view name(type.qualified_name);
  for (u16 v = from_version; v < type.version; ++v) {
    const Migration* step = nullptr;
    for (const Migration& m : migrations_) {
      if (m.from_version == v && name == m.type_qualified_name) {
        step = &m;
        break;
      }
    }
    // **The additive step** (schemas/README.md, "Evolution rules"; plan 03 §3.8): a version that
    // only added fields, each `@since` it with a default, needs nothing done to the JSON — a field
    // the object lacks takes its default when it is read — so the step is derived from the tables
    // schemac generated rather than written by hand. A version that adds no field (a field's type
    // changed, a meaning changed) is not additive and still needs a registered step, and one
    // registered by hand wins over the derived one, which is how a version that adds a field and
    // also renames or removes one says what to do with the old one. A removal or rename left to
    // the derived step is still refused, by the reader, as an unknown field.
    if (step == nullptr && additive_step(type, static_cast<u16>(v + 1u))) continue;
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

bool upcast(const TypeInfo& type, JsonValue& object, u16 stored_version, ReadContext& ctx) {
  if (stored_version > type.version) {
    ctx.error("stored schema version is newer than this build understands");
    return false;
  }
  if (stored_version == 0) {
    ctx.error("stored schema version 0 is not a version");
    return false;
  }
  return stored_version == type.version ||
         MigrationRegistry::global().migrate(type, object, stored_version, ctx);
}

bool from_json_versioned(const TypeInfo& type, void* object, JsonValue in, u16 stored_version,
                         ReadContext& ctx) {
  if (stored_version > type.version) {
    ctx.error("stored schema version is newer than this build understands");
    return false;
  }
  if (stored_version < type.version &&
      !MigrationRegistry::global().migrate(type, in, stored_version, ctx)) {
    return false;
  }
  return from_json(type, object, in, ctx);
}

// --- JSON text without a tree (D40; json_text.h) --------------------------------------------------

namespace {

using Event = JsonReader::Event;

bool is_scalar(Event e) noexcept { return e != Event::Array && e != Event::Object; }

TypeRef ref_of(const TypeInfo& type) {
  TypeRef ref;
  ref.kind = type.kind;
  ref.type = &type;
  ref.size = type.size;
  ref.align = type.align;
  return ref;
}

// The schema walk of `from_json`, over a reader instead of a tree. Every value whose shape the
// field does not take is handed to `from_json` as a tree of that value alone, so the refusal is the
// tree path's; an object whose keys are not strictly increasing, or that names a field by a former
// name, stops the walk (`irregular`), and the caller reads the text again by the tree path.
class TextWalker {
 public:
  TextWalker(JsonReader& reader, ReadContext& ctx, const TextReadOptions& options)
      : r_(reader), ctx_(ctx), options_(options) {
    keys_.reserve(options.parse.max_depth + 2u);  // never moves while a key is referred to
  }

  bool irregular() const noexcept { return irregular_; }
  bool stopped() const noexcept { return irregular_ || r_.failed(); }

  bool value(const TypeRef& type, void* object, Event e, bool root);

 private:
  bool by_tree(const TypeRef& type, void* object, Event e) {
    JsonValue v;
    if (!r_.rest(e, v)) return false;
    return from_json(type, object, v, ctx_);
  }
  bool numbers(Kind kind, void* object);
  bool structure(const TypeInfo& info, void* object, bool root);
  bool string_map(const TypeRef& type, void* object);
  bool pair_map(const TypeRef& type, void* object);
  bool array(const TypeRef& type, void* object);
  // Records `key` as the last key of the object open at this depth: false when it does not follow
  // the one before it in the tree's (byte) order.
  bool in_order(bool first, std::string_view key);
  const std::string& last_key() const { return keys_[r_.depth()]; }
  const TreeField* tree_field(const char* name) const {
    for (const TreeField& t : options_.tree_fields)
      if (std::strcmp(t.name, name) == 0) return &t;
    return nullptr;
  }

  JsonReader& r_;
  ReadContext& ctx_;
  const TextReadOptions& options_;
  bool irregular_ = false;
  Vector<std::string> keys_;
};

bool TextWalker::in_order(bool first, std::string_view key) {
  const u32 depth = r_.depth();
  while (keys_.size() <= depth)
    keys_.push_back(std::string());
  std::string& last = keys_[depth];
  if (!first && !(std::string_view(last) < key)) return false;
  last.assign(key.data(), key.size());
  return true;
}

bool TextWalker::value(const TypeRef& type, void* object, Event e, bool root) {
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
      if (is_scalar(e) && e != Event::String) return from_json(type, object, r_.scalar(e), ctx_);
      return by_tree(type, object, e);
    case Kind::String: {
      if (e != Event::String) return by_tree(type, object, e);
      const std::string_view s = r_.string_value();
      static_cast<std::string*>(object)->assign(s.data(), s.size());
      return true;
    }
    case Kind::Bytes:
      if (e != Event::String) return by_tree(type, object, e);
      return read_bytes(r_.string_value(), object, ctx_);
    case Kind::Id128:
      if (e != Event::String) return by_tree(type, object, e);
      return read_id(r_.string_value(), object, ctx_);
    case Kind::Vec2:
    case Kind::Vec3:
    case Kind::Vec4:
    case Kind::Quat:
    case Kind::WorldPos:
    case Kind::DVec3:
      if (e != Event::Array) return by_tree(type, object, e);
      return numbers(type.kind, object);
    case Kind::Json: {
      JsonValue v;
      if (!r_.rest(e, v)) return false;
      *static_cast<JsonValue*>(object) = std::move(v);
      return true;
    }
    case Kind::Enum:
      if (e == Event::String) return read_enum_name(*type.type, object, r_.string_value(), ctx_);
      if (is_scalar(e)) return from_json(type, object, r_.scalar(e), ctx_);
      return by_tree(type, object, e);
    case Kind::Struct:
      if (e != Event::Object) return by_tree(type, object, e);
      return structure(*type.type, object, root);
    case Kind::Optional:
      if (e == Event::Null) {
        type.optional_ops->reset(object);
        return true;
      }
      return value(*type.element, type.optional_ops->emplace(object), e, false);
    case Kind::Array:
      if (e != Event::Array) return by_tree(type, object, e);
      return array(type, object);
    case Kind::FixedArray:  // its length is checked before any element is read: the tree's
      return by_tree(type, object, e);
    case Kind::Map:
      if (type.key->kind == Kind::String) {
        if (e != Event::Object) return by_tree(type, object, e);
        return string_map(type, object);
      }
      if (e != Event::Array) return by_tree(type, object, e);
      return pair_map(type, object);
  }
  return false;
}

bool TextWalker::numbers(Kind kind, void* object) {
  NumberList list;
  list.is_array = true;
  bool more = false;
  while (r_.next_element(more) && more) {
    Event e;
    if (!r_.next_value(e)) return false;
    if (list.count < 4) {
      bool& numeric = list.numeric[list.count];
      f64& v = list.value[list.count];
      numeric = true;
      switch (e) {
        case Event::Int: v = static_cast<f64>(r_.int_value()); break;
        case Event::Uint: v = static_cast<f64>(r_.uint_value()); break;
        case Event::Float: v = r_.float_value(); break;
        default: numeric = false; break;
      }
    }
    if (!r_.skip(e)) return false;
    ++list.count;
  }
  if (r_.failed()) return false;
  return is_f64_vector(kind) ? read_f64_vector(kind, object, list, ctx_)
                             : read_components(kind, object, list, ctx_);
}

bool TextWalker::structure(const TypeInfo& info, void* object, bool root) {
  auto* base = static_cast<std::byte*>(object);
  bool ok = true;
  bool more = false;
  bool first = true;
  std::string_view key;
  while (r_.next_member(more, key) && more) {
    if (!in_order(first, key)) {
      irregular_ = true;
      return false;
    }
    first = false;
    const FieldInfo* f = info.find_field(key);
    if (f == nullptr && !info.field_aliases.empty() && info.find_field_alias(key) != nullptr) {
      irregular_ = true;  // a former name: the tree path reads it, and refuses two names of one field
      return false;
    }
    if (f == nullptr) {
      if (!ctx_.options.ignore_unknown_fields) {
        ctx_.push_field(last_key().c_str());
        ctx_.error("unknown field");
        ctx_.pop();
        ok = false;
      }
      if (!r_.skip_value()) return false;
      continue;
    }
    if ((f->flags & FieldFlag::transient) != 0) {
      if (!r_.skip_value()) return false;
      continue;
    }
    if (root) {
      if (const TreeField* t = tree_field(f->name)) {
        JsonValue v;
        if (!r_.read_value(v)) return false;
        if (!t->prepare(v, ctx_, t->user)) {
          irregular_ = true;  // refused in the tree path's order, by the tree path
          return false;
        }
        ctx_.push_field(f->name);
        ok = from_json(f->type, base + f->offset, v, ctx_) && ok;
        ctx_.pop();
        continue;
      }
    }
    ctx_.push_field(f->name);
    Event e;
    const bool read = r_.next_value(e) && value(f->type, base + f->offset, e, false);
    ctx_.pop();
    if (stopped()) return false;
    ok = read && ok;
  }
  return !r_.failed() && ok;
}

bool TextWalker::string_map(const TypeRef& type, void* object) {
  type.map_ops->clear(object);
  bool ok = true;
  bool more = false;
  bool first = true;
  std::string_view key;
  while (r_.next_member(more, key) && more) {
    if (!in_order(first, key)) {
      irregular_ = true;
      return false;
    }
    first = false;
    const std::string& name = last_key();
    void* slot = type.map_ops->insert(object, &name);
    ctx_.push_field(name.c_str());
    Event e;
    const bool read = r_.next_value(e) && value(*type.element, slot, e, false);
    ctx_.pop();
    if (stopped()) return false;
    ok = read && ok;
  }
  return !r_.failed() && ok;
}

bool TextWalker::pair_map(const TypeRef& type, void* object) {
  type.map_ops->clear(object);
  bool ok = true;
  bool more = false;
  u32 i = 0;
  while (r_.next_element(more) && more) {
    JsonValue pair;
    if (!r_.read_value(pair)) return false;
    ok = read_map_pair(type, object, pair, i++, ctx_) && ok;
  }
  return !r_.failed() && ok;
}

bool TextWalker::array(const TypeRef& type, void* object) {
  type.array_ops->clear(object);
  bool ok = true;
  bool more = false;
  u32 i = 0;
  while (r_.next_element(more) && more) {
    void* slot = type.array_ops->push_back(object);
    ctx_.push_index(i++);
    Event e;
    const bool read = r_.next_value(e) && value(*type.element, slot, e, false);
    ctx_.pop();
    if (stopped()) return false;
    ok = read && ok;
  }
  return !r_.failed() && ok;
}

// --- the canonical text from the fields --------------------------------------------------------

struct StringOut {
  std::string& out;
  void append(const char* p, usize n) { out.append(p, n); }
  void put(char c) { out.push_back(c); }
  bool stopped() const noexcept { return false; }
};

// Compares what would be written against `text` as it is written, and stops at the first byte that
// differs: no string, no tree.
struct CompareOut {
  std::string_view text;
  usize pos = 0;
  bool same = true;
  void append(const char* p, usize n) {
    if (!same) return;
    if (text.size() - pos < n || std::memcmp(text.data() + pos, p, n) != 0) {
      same = false;
      return;
    }
    pos += n;
  }
  void put(char c) {
    if (!same) return;
    if (pos >= text.size() || text[pos] != c) {
      same = false;
      return;
    }
    ++pos;
  }
  bool stopped() const noexcept { return !same; }
};

constexpr u32 k_max_written_fields = 1024;

// `to_json` then `write_json`, in one walk: a struct's fields in the order its JSON object keeps
// them (by name), every non-transient one written, the rest spelt by json_format.h.
template <class Sink>
class TextWriter {
 public:
  TextWriter(Sink& out, bool pretty) : out_(out), pretty_(pretty) {}

  bool value(const TypeRef& type, const void* p, u32 depth) {
    using namespace json_format;
    switch (type.kind) {
      case Kind::Bool:
        if (load<bool>(p))
          out_.append("true", 4);
        else
          out_.append("false", 5);
        return true;
      case Kind::U64: integer(out_, load<u64>(p)); return true;
      case Kind::U8:
      case Kind::U16:
      case Kind::U32: integer(out_, static_cast<u64>(load_integer(type.kind, p))); return true;
      case Kind::I8:
      case Kind::I16:
      case Kind::I32:
      case Kind::I64: integer(out_, load_integer(type.kind, p)); return true;
      case Kind::F32: return number(out_, static_cast<f64>(load<f32>(p)));
      case Kind::F64: return number(out_, load<f64>(p));
      case Kind::String: string(out_, *static_cast<const std::string*>(p)); return true;
      case Kind::Bytes: {
        const auto& bytes = *static_cast<const Vector<u8>*>(p);
        out_.put('"');
        char chunk[128];
        usize n = 0;
        for (const u8 b : bytes) {
          chunk[n++] = k_hex[b >> 4];
          chunk[n++] = k_hex[b & 0xF];
          if (n == sizeof(chunk)) {
            out_.append(chunk, n);
            n = 0;
          }
        }
        if (n != 0) out_.append(chunk, n);
        out_.put('"');
        return true;
      }
      case Kind::Id128: {
        char hex[33];
        static_cast<const Id128*>(p)->to_hex(hex);
        string(out_, std::string_view(hex, 32));
        return true;
      }
      case Kind::Vec2:
      case Kind::Vec3:
      case Kind::Vec4:
      case Kind::Quat: {
        f64 v[4];
        const usize n = component_count(type.kind);
        for (usize i = 0; i < n; ++i)
          v[i] = static_cast<f64>(static_cast<const f32*>(p)[i]);
        return numbers(v, n, depth);
      }
      case Kind::WorldPos:
      case Kind::DVec3: {
        f64 v[3];
        std::memcpy(v, p, sizeof(v));
        return numbers(v, 3, depth);
      }
      case Kind::Json: return json_format::value(out_, *static_cast<const JsonValue*>(p), pretty_, depth);
      case Kind::Enum: {
        const i64 v = load_integer(type.type->enum_underlying, p);
        if (const EnumValueInfo* ev = type.type->find_value(v))
          string(out_, std::string_view(ev->name));
        else
          integer(out_, v);
        return true;
      }
      case Kind::Struct: return structure(*type.type, p, depth);
      case Kind::Optional:
        if (!type.optional_ops->has_value(p)) {
          out_.append("null", 4);
          return true;
        }
        return value(*type.element, type.optional_ops->get(p), depth);
      case Kind::Array:
      case Kind::FixedArray: {
        const usize n = type.array_ops->size(p);
        if (n == 0) {
          out_.append("[]", 2);
          return true;
        }
        bool ok = true;
        out_.put('[');
        for (usize i = 0; i < n && !out_.stopped(); ++i) {
          if (i > 0) out_.put(',');
          if (pretty_) indent(out_, depth + 1);
          ok = value(*type.element, type.array_ops->at(p, i), depth + 1) && ok;
        }
        close(']', depth);
        return ok;
      }
      case Kind::Map: {
        const usize n = type.map_ops->size(p);
        const bool keyed = type.key->kind == Kind::String;
        if (n == 0) {
          out_.append(keyed ? "{}" : "[]", 2);
          return true;
        }
        bool ok = true;
        out_.put(keyed ? '{' : '[');
        for (usize i = 0; i < n && !out_.stopped(); ++i) {
          if (i > 0) out_.put(',');
          if (pretty_) indent(out_, depth + 1);
          if (keyed) {
            string(out_, *static_cast<const std::string*>(type.map_ops->key_at(p, i)));
            separator();
            ok = value(*type.element, type.map_ops->value_at(p, i), depth + 1) && ok;
          } else {
            out_.put('[');
            if (pretty_) indent(out_, depth + 2);
            ok = value(*type.key, type.map_ops->key_at(p, i), depth + 2) && ok;
            out_.put(',');
            if (pretty_) indent(out_, depth + 2);
            ok = value(*type.element, type.map_ops->value_at(p, i), depth + 2) && ok;
            close(']', depth + 1);
          }
        }
        close(keyed ? '}' : ']', depth);
        return ok;
      }
    }
    return false;
  }

 private:
  void close(char c, u32 depth) {
    if (pretty_) json_format::indent(out_, depth);
    out_.put(c);
  }
  void separator() {
    if (pretty_)
      out_.append(": ", 2);
    else
      out_.put(':');
  }
  bool numbers(const f64* v, usize n, u32 depth) {
    bool ok = true;
    out_.put('[');
    for (usize i = 0; i < n; ++i) {
      if (i > 0) out_.put(',');
      if (pretty_) json_format::indent(out_, depth + 1);
      ok = json_format::number(out_, v[i]) && ok;
    }
    close(']', depth);
    return ok;
  }
  bool structure(const TypeInfo& info, const void* p, u32 depth) {
    u16 order[k_max_written_fields];
    u32 n = 0;
    ENGINE_VERIFY(info.fields.size() <= k_max_written_fields, "schema: too many fields to write");
    for (u32 i = 0; i < info.fields.size(); ++i) {
      if ((info.fields[i].flags & FieldFlag::transient) != 0) continue;
      // Insertion by name: the order a JSON object keeps its keys in.
      u32 at = n++;
      while (at > 0 && std::strcmp(info.fields[i].name, info.fields[order[at - 1]].name) < 0) {
        order[at] = order[at - 1];
        --at;
      }
      order[at] = static_cast<u16>(i);
    }
    if (n == 0) {
      out_.append("{}", 2);
      return true;
    }
    const auto* base = static_cast<const std::byte*>(p);
    bool ok = true;
    out_.put('{');
    for (u32 k = 0; k < n && !out_.stopped(); ++k) {
      const FieldInfo& f = info.fields[order[k]];
      if (k > 0) out_.put(',');
      if (pretty_) json_format::indent(out_, depth + 1);
      json_format::string(out_, std::string_view(f.name));
      separator();
      ok = value(f.type, base + f.offset, depth + 1) && ok;
    }
    close('}', depth);
    return ok;
  }

  Sink& out_;
  bool pretty_;
};

}  // namespace

bool read_json_text(const TypeInfo& type, void* object, std::string_view text, ReadContext& ctx,
                    JsonParseResult& parse, const TextReadOptions& options, bool* streamed) {
  ENGINE_VERIFY(type.kind == Kind::Struct && type.ops != nullptr,
                "read_json_text: the type must be a struct");
  const auto reset = [&] {
    type.ops->destroy(object);
    type.ops->construct(object);
  };
  const usize before = ctx.diagnostics.size();
  const auto restore = [&] {
    while (ctx.diagnostics.size() > before)
      ctx.diagnostics.pop_back();
  };
  if (streamed != nullptr) *streamed = false;
  reset();
  {
    JsonReader reader(text, options.parse);
    TextWalker walker(reader, ctx, options);
    Event e;
    bool ok = false;
    if (reader.next_value(e)) ok = walker.value(ref_of(type), object, e, true);
    if (!walker.irregular()) {
      (void)reader.finish();
      parse = reader.result();
      if (!parse.ok) {
        restore();
        reset();
        return false;
      }
      if (streamed != nullptr) *streamed = true;
      return ok;
    }
  }
  // The tree path, from the start and a fresh object.
  restore();
  reset();
  JsonValue root;
  parse = parse_json(text, root, options.parse);
  if (!parse.ok) return false;
  for (const TreeField& t : options.tree_fields) {
    JsonValue* member = root.is_object() ? root.find(t.name) : nullptr;
    if (member != nullptr && !t.prepare(*member, ctx, t.user)) return false;
  }
  return from_json(type, object, root, ctx);
}

bool write_json_text(const TypeInfo& type, const void* object, std::string& out,
                     const JsonWriteOptions& options) {
  StringOut sink{out};
  TextWriter<StringOut> writer(sink, options.pretty);
  return writer.value(ref_of(type), object, 0);
}

bool canonical_json_equals(const TypeInfo& type, const void* object, std::string_view text,
                           std::string_view suffix, const JsonWriteOptions& options) {
  CompareOut sink{text};
  TextWriter<CompareOut> writer(sink, options.pretty);
  (void)writer.value(ref_of(type), object, 0);
  return sink.same && text.substr(sink.pos) == suffix;
}

}  // namespace engine::schema
