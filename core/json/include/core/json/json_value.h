#pragma once

// JsonValue: a compact JSON document model.
//
// 24 bytes per value: a kind byte and a 16-byte payload (scalar, or a heap-only container).
// Strings are stored as byte vectors (not null-terminated) and exposed as string_view;
// objects are FlatMaps keyed by std::string, so keys are always sorted and the canonical
// writer needs no extra pass. Integers keep their exact 64-bit value with a signed/unsigned
// kind so identifiers and hashes round-trip; anything with a fraction or exponent is a double.

#include <core/base/assert.h>
#include <core/base/types.h>
#include <core/containers/flat_map.h>
#include <core/containers/vector.h>

#include <string>
#include <string_view>
#include <utility>

namespace engine {

class JsonValue {
 public:
  enum class Kind : u8 { Null, Bool, Int, Uint, Float, String, Array, Object };

  using Array = Vector<JsonValue>;
  using Object = FlatMap<std::string, JsonValue>;

  // --- construction -----------------------------------------------------------------------

  JsonValue() noexcept : kind_(Kind::Null), i_(0) {}
  JsonValue(bool v) noexcept : kind_(Kind::Bool), b_(v) {}
  JsonValue(i32 v) noexcept : kind_(Kind::Int), i_(v) {}
  JsonValue(i64 v) noexcept : kind_(Kind::Int), i_(v) {}
  JsonValue(u32 v) noexcept : kind_(Kind::Uint), u_(v) {}
  JsonValue(u64 v) noexcept : kind_(Kind::Uint), u_(v) {}
  JsonValue(f32 v) noexcept : kind_(Kind::Float), f_(static_cast<f64>(v)) {}
  JsonValue(f64 v) noexcept : kind_(Kind::Float), f_(v) {}
  JsonValue(std::string_view s);
  JsonValue(const char* s) : JsonValue(std::string_view(s)) {}
  JsonValue(const std::string& s) : JsonValue(std::string_view(s)) {}
  JsonValue(Array&& a) noexcept;
  JsonValue(Object&& o) noexcept;

  static JsonValue null() noexcept { return JsonValue(); }
  static JsonValue array() noexcept { return JsonValue(Array{}); }
  static JsonValue object() noexcept { return JsonValue(Object{}); }

  JsonValue(const JsonValue& other);
  JsonValue(JsonValue&& other) noexcept;
  JsonValue& operator=(const JsonValue& other);
  JsonValue& operator=(JsonValue&& other) noexcept;
  ~JsonValue() { destroy(); }

  // --- kind -------------------------------------------------------------------------------

  Kind kind() const noexcept { return kind_; }
  bool is_null() const noexcept { return kind_ == Kind::Null; }
  bool is_bool() const noexcept { return kind_ == Kind::Bool; }
  bool is_int() const noexcept { return kind_ == Kind::Int; }
  bool is_uint() const noexcept { return kind_ == Kind::Uint; }
  bool is_float() const noexcept { return kind_ == Kind::Float; }
  bool is_number() const noexcept { return kind_ == Kind::Int || kind_ == Kind::Uint || kind_ == Kind::Float; }
  bool is_string() const noexcept { return kind_ == Kind::String; }
  bool is_array() const noexcept { return kind_ == Kind::Array; }
  bool is_object() const noexcept { return kind_ == Kind::Object; }

  // --- scalar access ----------------------------------------------------------------------
  // as_* assert the kind. get_* convert between numeric kinds and return false on mismatch.

  bool as_bool() const noexcept {
    ENGINE_ASSERT(kind_ == Kind::Bool, "JsonValue: not a bool");
    return b_;
  }
  i64 as_int() const noexcept {
    ENGINE_ASSERT(kind_ == Kind::Int, "JsonValue: not an int");
    return i_;
  }
  u64 as_uint() const noexcept {
    ENGINE_ASSERT(kind_ == Kind::Uint, "JsonValue: not a uint");
    return u_;
  }
  f64 as_float() const noexcept {
    ENGINE_ASSERT(kind_ == Kind::Float, "JsonValue: not a float");
    return f_;
  }
  std::string_view as_string() const noexcept {
    ENGINE_ASSERT(kind_ == Kind::String, "JsonValue: not a string");
    return std::string_view(str_.data(), str_.size());
  }

  bool get_bool(bool& out) const noexcept;
  bool get_i64(i64& out) const noexcept;   // Int, or Uint that fits, or Float with integral value
  bool get_u64(u64& out) const noexcept;   // Uint, or non-negative Int, or Float with integral value
  bool get_f64(f64& out) const noexcept;   // any number
  bool get_string(std::string_view& out) const noexcept;

  // --- container access -------------------------------------------------------------------

  Array& as_array() noexcept {
    ENGINE_ASSERT(kind_ == Kind::Array, "JsonValue: not an array");
    return arr_;
  }
  const Array& as_array() const noexcept {
    ENGINE_ASSERT(kind_ == Kind::Array, "JsonValue: not an array");
    return arr_;
  }
  Object& as_object() noexcept {
    ENGINE_ASSERT(kind_ == Kind::Object, "JsonValue: not an object");
    return obj_;
  }
  const Object& as_object() const noexcept {
    ENGINE_ASSERT(kind_ == Kind::Object, "JsonValue: not an object");
    return obj_;
  }

  // Number of elements or members; 0 for scalars.
  usize size() const noexcept;

  // Object member lookup; nullptr when absent or not an object.
  const JsonValue* find(std::string_view key) const noexcept;
  JsonValue* find(std::string_view key) noexcept;
  bool contains(std::string_view key) const noexcept { return find(key) != nullptr; }
  // Object member access, inserting null when absent. Asserts the value is an object.
  JsonValue& operator[](std::string_view key);
  // Array element access; asserts kind and range.
  JsonValue& operator[](usize index) noexcept;
  const JsonValue& operator[](usize index) const noexcept;
  // Appends to an array; asserts kind.
  JsonValue& push_back(JsonValue value);
  // Sets an object member, replacing any existing one.
  JsonValue& set(std::string_view key, JsonValue value);

  friend bool operator==(const JsonValue& a, const JsonValue& b) noexcept;

 private:
  void destroy() noexcept;
  void copy_from(const JsonValue& other);
  void move_from(JsonValue&& other) noexcept;

  Kind kind_;
  union {
    bool b_;
    i64 i_;
    u64 u_;
    f64 f_;
    Vector<char> str_;
    Array arr_;
    Object obj_;
  };
};

}  // namespace engine
