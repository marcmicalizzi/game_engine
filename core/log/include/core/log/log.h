#pragma once

// Structured logging (docs/plan/02-architecture.md §2.3, docs/plan/09-testing-profiling.md §9.3).
//
// A record is a static message plus typed fields, not a formatted string:
//
//     ENGINE_LOG_INFO(log_streaming, "tile loaded", log::field("tile", tile_id),
//                     log::field("ms", elapsed_ms));
//
// Text sinks render `tile loaded tile=... ms=3.21`; the JSON-lines sink emits one object per
// record with the fields as typed JSON values, so agents and tools query logs by key instead
// of parsing prose. Field arguments are evaluated only when the record passes the level
// filters, and records below ENGINE_LOG_MIN_LEVEL are compiled out entirely.
//
// Categories are module-scoped objects defined once (ENGINE_LOG_CATEGORY_DEFINE) and
// registered at static initialization, so levels can be set by name whether or not the
// category exists yet: log::apply_level_spec("info,jobs=debug,render.*=trace").
//
// Sinks receive records from every thread concurrently and must be internally thread-safe.
// With no sink registered, records go to stderr as text.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>

#include <array>
#include <atomic>
#include <concepts>
#include <cstdio>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

// Compile-time floor: records below this level cost nothing at run time. 0 trace, 1 debug,
// 2 info, 3 warn, 4 error, 5 fatal. Set from CMake with -DENGINE_LOG_MIN_LEVEL=<n>.
#ifndef ENGINE_LOG_MIN_LEVEL
#if ENGINE_DEBUG
#define ENGINE_LOG_MIN_LEVEL 0
#else
#define ENGINE_LOG_MIN_LEVEL 1
#endif
#endif
static_assert(ENGINE_LOG_MIN_LEVEL >= 0 && ENGINE_LOG_MIN_LEVEL <= 5,
              "ENGINE_LOG_MIN_LEVEL must be 0 (trace) .. 5 (fatal)");

namespace engine::log {

enum class Level : u8 { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Fatal = 5, Off = 6 };

// "trace", "debug", "info", "warn", "error", "fatal", "off".
const char* level_name(Level level) noexcept;
// Case-insensitive; also accepts "warning" and "err". Returns false for anything else.
bool parse_level(std::string_view text, Level& out) noexcept;

// A typed key/value attached to one record. Built on the caller's stack; strings are not
// copied, so a Field is valid only for the duration of the emit call that carries it. Sinks
// that retain records copy what they need (see RingSink).
struct Field {
  enum class Kind : u8 { Bool, Int, Uint, Float, String, Pointer, Hex128 };

  struct Str {
    const char* data;
    usize size;
  };
  struct Hex128 {
    u64 hi;
    u64 lo;
  };
  union Value {
    bool b;
    i64 i;
    u64 u;
    f64 f;
    Str s;
    const void* p;
    Hex128 h;
  };

  std::string_view key;
  Value value;
  Kind kind;

  std::string_view string() const noexcept { return {value.s.data, value.s.size}; }
};

inline Field field(std::string_view key, bool v) noexcept {
  Field f{key, {}, Field::Kind::Bool};
  f.value.b = v;
  return f;
}

template <class T>
  requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
Field field(std::string_view key, T v) noexcept {
  Field f{key, {}, std::is_signed_v<T> ? Field::Kind::Int : Field::Kind::Uint};
  if constexpr (std::is_signed_v<T>) {
    f.value.i = static_cast<i64>(v);
  } else {
    f.value.u = static_cast<u64>(v);
  }
  return f;
}

template <class T>
  requires std::is_floating_point_v<T>
Field field(std::string_view key, T v) noexcept {
  Field f{key, {}, Field::Kind::Float};
  f.value.f = static_cast<f64>(v);
  return f;
}

inline Field field(std::string_view key, std::string_view v) noexcept {
  Field f{key, {}, Field::Kind::String};
  f.value.s = {v.data(), v.size()};
  return f;
}
inline Field field(std::string_view key, const char* v) noexcept {
  return field(key, std::string_view{v != nullptr ? v : ""});
}
inline Field field(std::string_view key, const std::string& v) noexcept {
  return field(key, std::string_view{v});
}

// 128-bit identifiers (Id128 and anything shaped like it) render as 32 hex characters. The
// duck-typed constraint keeps this module independent of core/ids.
template <class T>
  requires requires(const T& v) {
    { v.hi } -> std::convertible_to<u64>;
    { v.lo } -> std::convertible_to<u64>;
    { T::k_hex_length } -> std::convertible_to<usize>;
  }
Field field(std::string_view key, const T& id) noexcept {
  Field f{key, {}, Field::Kind::Hex128};
  f.value.h = {static_cast<u64>(id.hi), static_cast<u64>(id.lo)};
  return f;
}

inline Field field_pointer(std::string_view key, const void* p) noexcept {
  Field f{key, {}, Field::Kind::Pointer};
  f.value.p = p;
  return f;
}
inline Field field_hex128(std::string_view key, u64 hi, u64 lo) noexcept {
  Field f{key, {}, Field::Kind::Hex128};
  f.value.h = {hi, lo};
  return f;
}

// Effective global floor applied on top of every category's own level.
Level global_min_level() noexcept;
void set_global_min_level(Level level) noexcept;

// A named source of records. Define one per module (or per subsystem within a module) with
// ENGINE_LOG_CATEGORY_DEFINE at namespace scope; the constructor registers it. Names are dotted
// lowercase ("jobs", "render.rt") and must be string literals or otherwise outlive the
// category. Categories unregister on destruction so tests may create temporary ones.
class Category {
 public:
  explicit Category(const char* name) noexcept;
  ~Category();
  ENGINE_NON_COPYABLE(Category);

  const char* name() const noexcept { return name_; }
  Level min_level() const noexcept { return min_level_.load(std::memory_order_relaxed); }
  void set_min_level(Level level) noexcept { min_level_.store(level, std::memory_order_relaxed); }

  bool enabled(Level level) const noexcept {
    return level >= min_level_.load(std::memory_order_relaxed) && level >= global_min_level();
  }

  // Registry chain, most recently registered first.
  Category* next() const noexcept { return next_; }

 private:
  const char* name_;
  Category* next_ = nullptr;
  std::atomic<Level> min_level_{Level::Trace};
};

Category* first_category() noexcept;
Category* find_category(std::string_view name) noexcept;

// Level spec: comma-separated entries, each `level` (sets the global floor) or `name=level`
// where `name` may end in `*` to match a prefix ("render.*"). Entries apply to categories
// registered now and later; successive calls accumulate. Returns false and describes the first
// malformed entry in `error` (entries before it are applied). The accumulated spec is bounded;
// exceeding the bound is an error too.
bool apply_level_spec(std::string_view spec, std::string* error = nullptr);
// Forgets every spec entry, sets every category back to Trace and the global floor to its
// default (Debug in debug builds, Info otherwise).
void reset_levels() noexcept;

struct Record {
  i64 monotonic_ns;
  i64 wall_unix_us;
  const Category* category;
  std::string_view message;
  std::span<const Field> fields;
  const char* file;  // null when unknown
  u32 line;
  u32 thread_index;
  Level level;
};

// Renders `HH:MM:SS.mmm level category [tN] message key=value ...` (UTC, no trailing newline).
// Strings that contain spaces, quotes, '=' or control characters are JSON-quoted.
void format_text(const Record& record, std::string& out);
// Renders one JSON object (no trailing newline). Shape is documented in docs/subsystems/log.md.
void format_json_line(const Record& record, std::string& out);

class Sink {
 public:
  virtual ~Sink() = default;
  virtual void write(const Record& record) = 0;
  virtual void flush() {}

  Level min_level() const noexcept { return min_level_.load(std::memory_order_relaxed); }
  void set_min_level(Level level) noexcept { min_level_.store(level, std::memory_order_relaxed); }

 private:
  std::atomic<Level> min_level_{Level::Trace};
};

// Registered sinks are not owned and must outlive their registration. At most k_max_sinks.
inline constexpr usize k_max_sinks = 8;
void add_sink(Sink* sink);
void remove_sink(Sink* sink);
usize sink_count() noexcept;
void flush();

// Delivers a record to every sink whose level admits it. The macros perform the category and
// compile-time checks first; call this directly only for levels chosen at run time. Fatal
// records flush every sink.
void emit(const Category& category, Level level, const char* file, u32 line,
          std::string_view message, std::span<const Field> fields) noexcept;

// Flushes every sink, reports through the assertion path, and terminates.
[[noreturn]] void fatal_terminate(std::string_view message, const char* file, u32 line) noexcept;

// Routes assertion failures (core/base) through the log sinks as Fatal records in the
// "engine.assert" category before the process terminates. Idempotent.
void install_assert_hook() noexcept;

// Writes text or JSON lines to a C stream. Each record is one buffered write; Error and above
// flush. Optionally closes the stream on destruction.
class StreamSink final : public Sink {
 public:
  enum class Format : u8 { Text, JsonLines };
  explicit StreamSink(std::FILE* stream, Format format = Format::Text,
                      bool close_on_destroy = false) noexcept;
  ~StreamSink() override;
  ENGINE_NON_COPYABLE(StreamSink);

  void write(const Record& record) override;
  void flush() override;

 private:
  std::FILE* stream_;
  Format format_;
  bool close_on_destroy_;
};

// Keeps the most recent records in memory with copies of their strings, for tests and for the
// protocol's log query. Every record receives a sequence number (from 0, +1 per record) so a
// reader can ask for everything since the last one it saw.
class RingSink final : public Sink {
 public:
  struct Entry {
    u64 sequence;
    i64 monotonic_ns;
    i64 wall_unix_us;
    const Category* category;
    std::string_view message;
    std::span<const Field> fields;  // string values point into the sink's storage
    const char* file;
    u32 line;
    u32 thread_index;
    Level level;
  };

  explicit RingSink(u32 capacity);
  ENGINE_NON_COPYABLE(RingSink);

  void write(const Record& record) override;

  u32 capacity() const noexcept { return static_cast<u32>(slots_.size()); }
  usize size() const noexcept;
  u64 next_sequence() const noexcept;   // the sequence the next record will get
  u64 first_sequence() const noexcept;  // oldest retained (== next_sequence when empty)
  void clear() noexcept;

  // Visits retained records with sequence >= since, oldest first, holding the sink's lock.
  // Entry views are valid only inside the callback. Returns the number visited.
  template <class F>
  usize for_each(u64 since, F&& fn) const {
    std::lock_guard lock(mutex_);
    usize visited = 0;
    for (u32 i = 0; i < count_; ++i) {
      const Stored& s = slots_[(head_ + i) % slots_.size()];
      if (s.sequence < since) continue;
      fn(view_of(s));
      ++visited;
    }
    return visited;
  }

 private:
  struct Stored {
    u64 sequence = 0;
    i64 monotonic_ns = 0;
    i64 wall_unix_us = 0;
    const Category* category = nullptr;
    const char* file = nullptr;
    u32 line = 0;
    u32 thread_index = 0;
    u32 message_size = 0;
    Level level = Level::Trace;
    std::string blob;      // message, then keys and string values
    Vector<Field> fields;  // views into blob
  };

  static Entry view_of(const Stored& s) noexcept;

  mutable std::mutex mutex_;
  Vector<Stored> slots_;
  u32 head_ = 0;
  u32 count_ = 0;
  u64 next_sequence_ = 0;
};

namespace detail {

template <class... Fields>
std::array<Field, sizeof...(Fields)> fields_of(Fields&&... fields) noexcept {
  if constexpr (sizeof...(Fields) == 0) {
    return {};
  } else {
    return {{static_cast<Field>(fields)...}};
  }
}

}  // namespace detail

}  // namespace engine::log

// Category definition. Put the DEFINE in one .cpp at namespace scope; DECLARE in a header when
// other translation units of the module log to it.
#define ENGINE_LOG_CATEGORY_DECLARE(ident) extern ::engine::log::Category ident
#define ENGINE_LOG_CATEGORY_DEFINE(ident, name) \
  ::engine::log::Category ident {               \
    name                                        \
  }

// Emits at a compile-time level. `level` must be a constant expression; use log::emit for
// levels chosen at run time. Field arguments are evaluated only when the record is enabled.
#define ENGINE_LOG_AT(category, level, message, ...)                                             \
  do {                                                                                           \
    if constexpr (static_cast<int>(level) >= ENGINE_LOG_MIN_LEVEL) {                             \
      if ((category).enabled(level)) [[unlikely]] {                                              \
        ::engine::log::emit((category), (level), __FILE__, static_cast<::engine::u32>(__LINE__), \
                            (message), ::engine::log::detail::fields_of(__VA_ARGS__));           \
      }                                                                                          \
    }                                                                                            \
  } while (false)

#define ENGINE_LOG_TRACE(category, message, ...) \
  ENGINE_LOG_AT(category, ::engine::log::Level::Trace, message, __VA_ARGS__)
#define ENGINE_LOG_DEBUG(category, message, ...) \
  ENGINE_LOG_AT(category, ::engine::log::Level::Debug, message, __VA_ARGS__)
#define ENGINE_LOG_INFO(category, message, ...) \
  ENGINE_LOG_AT(category, ::engine::log::Level::Info, message, __VA_ARGS__)
#define ENGINE_LOG_WARN(category, message, ...) \
  ENGINE_LOG_AT(category, ::engine::log::Level::Warn, message, __VA_ARGS__)
#define ENGINE_LOG_ERROR(category, message, ...) \
  ENGINE_LOG_AT(category, ::engine::log::Level::Error, message, __VA_ARGS__)

// Fatal records ignore level filters, flush every sink, and terminate the process.
#define ENGINE_LOG_FATAL(category, message, ...)                                               \
  do {                                                                                         \
    ::engine::log::emit((category), ::engine::log::Level::Fatal, __FILE__,                     \
                        static_cast<::engine::u32>(__LINE__), (message),                       \
                        ::engine::log::detail::fields_of(__VA_ARGS__));                        \
    ::engine::log::fatal_terminate((message), __FILE__, static_cast<::engine::u32>(__LINE__)); \
  } while (false)
