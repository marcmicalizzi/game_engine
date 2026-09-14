#include <core/json/json_value.h>

#include <cmath>
#include <cstring>
#include <memory>

namespace engine {

JsonValue::JsonValue(std::string_view s) : kind_(Kind::String), str_() {
  str_.reserve(static_cast<u32>(s.size()));
  str_.append(std::span<const char>(s.data(), s.size()));
}

JsonValue::JsonValue(Array&& a) noexcept : kind_(Kind::Array), arr_(std::move(a)) {}
JsonValue::JsonValue(Object&& o) noexcept : kind_(Kind::Object), obj_(std::move(o)) {}

JsonValue::JsonValue(const JsonValue& other) : kind_(Kind::Null), i_(0) { copy_from(other); }
JsonValue::JsonValue(JsonValue&& other) noexcept : kind_(Kind::Null), i_(0) { move_from(std::move(other)); }

JsonValue& JsonValue::operator=(const JsonValue& other) {
  if (this != &other) {
    destroy();
    copy_from(other);
  }
  return *this;
}

JsonValue& JsonValue::operator=(JsonValue&& other) noexcept {
  if (this != &other) {
    destroy();
    move_from(std::move(other));
  }
  return *this;
}

void JsonValue::destroy() noexcept {
  switch (kind_) {
    case Kind::String: std::destroy_at(&str_); break;
    case Kind::Array: std::destroy_at(&arr_); break;
    case Kind::Object: std::destroy_at(&obj_); break;
    default: break;
  }
  kind_ = Kind::Null;
  i_ = 0;
}

void JsonValue::copy_from(const JsonValue& other) {
  kind_ = other.kind_;
  switch (kind_) {
    case Kind::Null: i_ = 0; break;
    case Kind::Bool: b_ = other.b_; break;
    case Kind::Int: i_ = other.i_; break;
    case Kind::Uint: u_ = other.u_; break;
    case Kind::Float: f_ = other.f_; break;
    case Kind::String: std::construct_at(&str_, other.str_); break;
    case Kind::Array: std::construct_at(&arr_, other.arr_); break;
    case Kind::Object: std::construct_at(&obj_, other.obj_); break;
  }
}

void JsonValue::move_from(JsonValue&& other) noexcept {
  kind_ = other.kind_;
  switch (kind_) {
    case Kind::Null: i_ = 0; break;
    case Kind::Bool: b_ = other.b_; break;
    case Kind::Int: i_ = other.i_; break;
    case Kind::Uint: u_ = other.u_; break;
    case Kind::Float: f_ = other.f_; break;
    case Kind::String: std::construct_at(&str_, std::move(other.str_)); break;
    case Kind::Array: std::construct_at(&arr_, std::move(other.arr_)); break;
    case Kind::Object: std::construct_at(&obj_, std::move(other.obj_)); break;
  }
  other.destroy();
}

// --- scalar conversion --------------------------------------------------------------------------

bool JsonValue::get_bool(bool& out) const noexcept {
  if (kind_ != Kind::Bool) return false;
  out = b_;
  return true;
}

bool JsonValue::get_i64(i64& out) const noexcept {
  switch (kind_) {
    case Kind::Int: out = i_; return true;
    case Kind::Uint:
      if (u_ > static_cast<u64>(INT64_MAX)) return false;
      out = static_cast<i64>(u_);
      return true;
    case Kind::Float:
      if (!std::isfinite(f_) || std::floor(f_) != f_) return false;
      if (f_ < -9223372036854775808.0 || f_ >= 9223372036854775808.0) return false;
      out = static_cast<i64>(f_);
      return true;
    default: return false;
  }
}

bool JsonValue::get_u64(u64& out) const noexcept {
  switch (kind_) {
    case Kind::Uint: out = u_; return true;
    case Kind::Int:
      if (i_ < 0) return false;
      out = static_cast<u64>(i_);
      return true;
    case Kind::Float:
      if (!std::isfinite(f_) || std::floor(f_) != f_ || f_ < 0.0 || f_ >= 18446744073709551616.0) return false;
      out = static_cast<u64>(f_);
      return true;
    default: return false;
  }
}

bool JsonValue::get_f64(f64& out) const noexcept {
  switch (kind_) {
    case Kind::Int: out = static_cast<f64>(i_); return true;
    case Kind::Uint: out = static_cast<f64>(u_); return true;
    case Kind::Float: out = f_; return true;
    default: return false;
  }
}

bool JsonValue::get_string(std::string_view& out) const noexcept {
  if (kind_ != Kind::String) return false;
  out = std::string_view(str_.data(), str_.size());
  return true;
}

// --- containers ---------------------------------------------------------------------------------

usize JsonValue::size() const noexcept {
  switch (kind_) {
    case Kind::Array: return arr_.size();
    case Kind::Object: return obj_.size();
    default: return 0;
  }
}

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
  if (kind_ != Kind::Object) return nullptr;
  return obj_.find_value(key);
}

JsonValue* JsonValue::find(std::string_view key) noexcept {
  if (kind_ != Kind::Object) return nullptr;
  return obj_.find_value(key);
}

JsonValue& JsonValue::operator[](std::string_view key) {
  ENGINE_ASSERT(kind_ == Kind::Object, "JsonValue: not an object");
  return obj_[key];
}

JsonValue& JsonValue::operator[](usize index) noexcept {
  ENGINE_ASSERT(kind_ == Kind::Array, "JsonValue: not an array");
  return arr_[static_cast<u32>(index)];
}

const JsonValue& JsonValue::operator[](usize index) const noexcept {
  ENGINE_ASSERT(kind_ == Kind::Array, "JsonValue: not an array");
  return arr_[static_cast<u32>(index)];
}

JsonValue& JsonValue::push_back(JsonValue value) {
  ENGINE_ASSERT(kind_ == Kind::Array, "JsonValue: not an array");
  return arr_.push_back(std::move(value));
}

JsonValue& JsonValue::set(std::string_view key, JsonValue value) {
  ENGINE_ASSERT(kind_ == Kind::Object, "JsonValue: not an object");
  return obj_.insert_or_assign(key, std::move(value)).first->second;
}

bool operator==(const JsonValue& a, const JsonValue& b) noexcept {
  if (a.kind_ != b.kind_) {
    // Numbers of different kinds compare by value.
    if (a.is_number() && b.is_number()) {
      f64 x, y;
      i64 ia, ib;
      if (a.get_i64(ia) && b.get_i64(ib)) return ia == ib;
      u64 ua, ub;
      if (a.get_u64(ua) && b.get_u64(ub)) return ua == ub;
      return a.get_f64(x) && b.get_f64(y) && x == y;
    }
    return false;
  }
  switch (a.kind_) {
    case JsonValue::Kind::Null: return true;
    case JsonValue::Kind::Bool: return a.b_ == b.b_;
    case JsonValue::Kind::Int: return a.i_ == b.i_;
    case JsonValue::Kind::Uint: return a.u_ == b.u_;
    case JsonValue::Kind::Float: return a.f_ == b.f_;
    case JsonValue::Kind::String: return a.as_string() == b.as_string();
    case JsonValue::Kind::Array: return a.arr_ == b.arr_;
    case JsonValue::Kind::Object: return a.obj_ == b.obj_;
  }
  return false;
}

}  // namespace engine
