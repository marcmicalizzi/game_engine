#pragma once

// Tunables registry (ADR-0011; docs/plan/08-toolchain.md §8.7; docs/plan/11 §11.8).
//
// A tunable is a named run-time parameter with a default, a range or a list of choices, and
// documentation. It is defined once at namespace scope and registers itself during static
// initialization:
//
//     tunables::Int culling_batch_size{"culling.batch_size", 256, 16, 4096,
//                                      "Instances per culling job"};
//     ...
//     const i64 batch = culling_batch_size.get();   // hoisted out of the hot loop
//
// Reading is one relaxed atomic load: do it once outside inner loops, never per element.
// Setting validates against the range, then bumps the tunable's version and the registry
// generation so that state derived from a value can notice a change. Values arrive from
// config files (a JSON object of name -> value), command lines ("name=value,..."), the bench
// harness (sweeps), and later the engine protocol; the registry is the one place they meet.
//
// Hardware-selected parameters (batch sizes, prefetch distances, thread counts, workgroup
// sizes) are tunables. Data-selected dimensions (formats, modes, feature flags) are compile-
// time variants behind dispatch tables, never tunables. Core-layer modules do not read the
// registry; they take config structs that the application layer fills from tunables.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>

#include <atomic>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace engine::tunables {

enum class Kind : u8 { Bool, Int, Float, Enum };
const char* kind_name(Kind kind) noexcept;

// Type-erased view of one tunable, for registries, config loaders, and the protocol. Names are
// dotted lower case ("jobs.spin_iterations"), unique, and must outlive the tunable (string
// literals). Tunables unregister on destruction so tests may create temporary ones.
class Tunable {
 public:
  ENGINE_NON_COPYABLE(Tunable);

  const char* name() const noexcept { return name_; }
  const char* doc() const noexcept { return doc_; }
  Kind kind() const noexcept { return kind_; }
  // Incremented on every accepted change of value.
  u32 version() const noexcept { return version_.load(std::memory_order_relaxed); }
  // Registry chain, most recently registered first.
  Tunable* next() const noexcept { return next_; }

  virtual bool is_default() const noexcept = 0;
  virtual void reset() noexcept = 0;
  // Text forms for command lines and tables. Leading and trailing spaces are ignored.
  virtual bool set_from_text(std::string_view text, std::string* error = nullptr) = 0;
  virtual void append_value(std::string& out) const = 0;
  virtual void append_default(std::string& out) const = 0;
  // JSON forms for config files and the protocol.
  virtual bool set_from_json(const JsonValue& value, std::string* error = nullptr) = 0;
  virtual JsonValue value_json() const = 0;
  virtual JsonValue default_json() const = 0;
  // {"name","kind","value","default","doc","modified","version"} plus "min"/"max" or
  // "choices" depending on the kind.
  JsonValue describe() const;

 protected:
  Tunable(const char* name, const char* doc, Kind kind) noexcept;
  virtual ~Tunable();
  void changed() noexcept;
  virtual void describe_range(JsonValue& object) const = 0;

 private:
  // Laid out to 40 bytes with no tail padding, so derived classes have the same size under
  // the MSVC and Itanium ABIs (Itanium places derived members in base tail padding).
  const char* name_;
  const char* doc_;
  Tunable* next_ = nullptr;
  Kind kind_;
  std::atomic<u32> version_{0};
};

class Int final : public Tunable {
 public:
  Int(const char* name, i64 default_value, i64 min, i64 max, const char* doc) noexcept;
  ~Int() override = default;

  i64 get() const noexcept { return value_.load(std::memory_order_relaxed); }
  // False, and unchanged, when the value is outside [min, max].
  bool set(i64 value) noexcept;
  i64 default_value() const noexcept { return default_; }
  i64 min() const noexcept { return min_; }
  i64 max() const noexcept { return max_; }

  bool is_default() const noexcept override { return get() == default_; }
  void reset() noexcept override { (void)set(default_); }
  bool set_from_text(std::string_view text, std::string* error) override;
  void append_value(std::string& out) const override;
  void append_default(std::string& out) const override;
  bool set_from_json(const JsonValue& value, std::string* error) override;
  JsonValue value_json() const override { return JsonValue(get()); }
  JsonValue default_json() const override { return JsonValue(default_); }

 private:
  void describe_range(JsonValue& object) const override;
  std::atomic<i64> value_;
  i64 default_;
  i64 min_;
  i64 max_;
};

class Float final : public Tunable {
 public:
  Float(const char* name, f64 default_value, f64 min, f64 max, const char* doc) noexcept;
  ~Float() override = default;

  f64 get() const noexcept { return value_.load(std::memory_order_relaxed); }
  // False, and unchanged, for NaN or a value outside [min, max].
  bool set(f64 value) noexcept;
  f64 default_value() const noexcept { return default_; }
  f64 min() const noexcept { return min_; }
  f64 max() const noexcept { return max_; }

  bool is_default() const noexcept override { return get() == default_; }
  void reset() noexcept override { (void)set(default_); }
  bool set_from_text(std::string_view text, std::string* error) override;
  void append_value(std::string& out) const override;
  void append_default(std::string& out) const override;
  bool set_from_json(const JsonValue& value, std::string* error) override;
  JsonValue value_json() const override { return JsonValue(get()); }
  JsonValue default_json() const override { return JsonValue(default_); }

 private:
  void describe_range(JsonValue& object) const override;
  std::atomic<f64> value_;
  f64 default_;
  f64 min_;
  f64 max_;
};

class Bool final : public Tunable {
 public:
  Bool(const char* name, bool default_value, const char* doc) noexcept;
  ~Bool() override = default;

  bool get() const noexcept { return value_.load(std::memory_order_relaxed); }
  void set(bool value) noexcept;
  bool default_value() const noexcept { return default_; }

  bool is_default() const noexcept override { return get() == default_; }
  void reset() noexcept override { set(default_); }
  // Accepts true/false, on/off, yes/no, 1/0 (case-insensitive).
  bool set_from_text(std::string_view text, std::string* error) override;
  void append_value(std::string& out) const override;
  void append_default(std::string& out) const override;
  bool set_from_json(const JsonValue& value, std::string* error) override;
  JsonValue value_json() const override { return JsonValue(get()); }
  JsonValue default_json() const override { return JsonValue(default_); }

 private:
  void describe_range(JsonValue&) const override {}
  std::atomic<bool> value_;
  bool default_;
};

// Choice among named variants. Choice i names enumerator value i; the enumeration must be
// dense from zero. Text and JSON accept a choice name (case-insensitive); JSON also accepts
// the index.
class EnumBase : public Tunable {
 public:
  u32 index() const noexcept { return index_.load(std::memory_order_relaxed); }
  bool set_index(u32 index) noexcept;
  u32 default_index() const noexcept { return default_; }
  std::span<const char* const> choices() const noexcept { return choices_; }
  const char* choice_name(u32 index) const noexcept {
    return index < choices_.size() ? choices_[index] : "?";
  }

  bool is_default() const noexcept override { return index() == default_; }
  void reset() noexcept override { (void)set_index(default_); }
  bool set_from_text(std::string_view text, std::string* error) override;
  void append_value(std::string& out) const override;
  void append_default(std::string& out) const override;
  bool set_from_json(const JsonValue& value, std::string* error) override;
  JsonValue value_json() const override { return JsonValue(choice_name(index())); }
  JsonValue default_json() const override { return JsonValue(choice_name(default_)); }

 protected:
  EnumBase(const char* name, u32 default_index, std::span<const char* const> choices,
           const char* doc) noexcept;
  ~EnumBase() override = default;

 private:
  void describe_range(JsonValue& object) const override;
  bool find_choice(std::string_view text, u32& out) const noexcept;
  std::atomic<u32> index_;
  u32 default_;
  std::span<const char* const> choices_;
};

template <class E>
  requires std::is_enum_v<E>
class Enum final : public EnumBase {
 public:
  Enum(const char* name, E default_value, std::span<const char* const> choices,
       const char* doc) noexcept
      : EnumBase(name, static_cast<u32>(default_value), choices, doc) {}
  ~Enum() override = default;

  E get() const noexcept { return static_cast<E>(index()); }
  bool set(E value) noexcept { return set_index(static_cast<u32>(value)); }
  E default_value() const noexcept { return static_cast<E>(default_index()); }
};

// ---- registry --------------------------------------------------------------------------------

Tunable* first() noexcept;
Tunable* find(std::string_view name) noexcept;
usize count() noexcept;
// Incremented on every accepted change of any tunable.
u64 generation() noexcept;

// "name=value, other=value". Every valid entry is applied; the first problem (unknown name,
// malformed entry, rejected value) is described in `error` and the call returns false.
bool apply_overrides(std::string_view spec, std::string* error = nullptr);
void reset_all() noexcept;

// A JSON object of name -> value. Every valid entry is applied; problems are appended to
// `problems` (one line each) and the call returns false if there were any.
bool load_json(const JsonValue& object, Vector<std::string>* problems = nullptr);
JsonValue save_json(bool only_modified = true);
// Array of describe() for every tunable, in name order.
JsonValue describe_all();

// Whole-file convenience over load_json / save_json (canonical JSON). Cold path.
bool load_file(const char* path, Vector<std::string>* problems = nullptr);
bool save_file(const char* path, bool only_modified = true);

}  // namespace engine::tunables
