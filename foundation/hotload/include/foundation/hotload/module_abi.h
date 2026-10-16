#pragma once

// The boundary between the engine and a hot-reloadable game module (docs/subsystems/hotload.md).
//
// A game module is a shared library built by `engine_game_module()` (cmake/EngineGameModule.cmake)
// that links **no engine library**. This header is everything the two sides share, and it is
// written so that a module can include it with nothing but `core/base/types.h` beside it:
//
//   - the module exports one C function, `engine_hotload_describe`, which fills a `ModuleDesc`:
//     the ABI it was built against, its exported functions (name, signature, pointer) and the
//     schema type of its state;
//   - every call the module makes into the engine goes through the `HostApi` table of function
//     pointers it is handed — allocation, logging, the host's clock, and read-only views of schema
//     objects by descriptor — so the module never holds a second copy of the engine's log
//     categories, tunables or allocator registries;
//   - nothing crosses the boundary but C types, the host's own plain structs by value, and the
//     module's `schema::TypeInfo` by pointer (constant data plus the module's own construct and
//     destroy functions), which the host reads only while the module is loaded.
//
// Exceptions do not cross it either: the module is compiled like the engine (no exceptions on
// GCC and Clang, none used on MSVC), and every failure is a status code plus a line the module
// hands to `HostApi::fail`.

#include <core/base/types.h>

#if defined(_WIN32)
#define ENGINE_HOTLOAD_EXPORT __declspec(dllexport)
#else
#define ENGINE_HOTLOAD_EXPORT __attribute__((visibility("default")))
#endif

namespace engine::schema {
struct TypeInfo;
}

namespace engine::hotload {

// Bumped whenever anything in this header changes shape or meaning. A module built against
// another number is refused before any of its code but `describe` runs.
//
// **2** (2026-10-16): `HostApi` gained the game runner's five services at its end (`game_action`,
// `hud_text`, `hud_meter`, `end_run`, `nearest_placement`; docs/subsystems/apps.md, "The game
// contract"). Nothing before them moved, but a module built against 1 would hand the host a
// `ModuleDesc` it reads the same and call a table it believes is shorter, so it is refused all the
// same: one number, one layout.
inline constexpr u32 k_abi_version = 2;

// The one symbol the host looks up.
inline constexpr const char* k_entry_point = "engine_hotload_describe";

// ---- views: what a module reads, and how --------------------------------------------------------

// A host object the module may read: a slot and the version it had when the host bound it. A view
// whose object was unbound or rebound reads as an error, never as memory (the same rule as the
// Luau binding's views, docs/subsystems/scripting.md "Lifetime").
struct View {
  u32 slot = 0;
  u32 version = 0;
};

// A field path resolved once against a host type ("health", "attributes.strength"), by index into
// the host's table. Resolve in `on_load`, read by handle on every call.
struct FieldRef {
  u32 index = 0xFFFFFFFFu;
};

enum class ValueKind : u32 {
  Nil = 0,  // an empty optional
  Bool,
  Int,     // every signed integer, widened to i64
  UInt,    // every unsigned integer, widened to u64
  Float,   // f32 and f64, widened to f64
  String,  // borrowed: valid until the call that read it returns
  Enum,    // `i` is the value, `str` the enumerator's name (empty when it has none)
  Vector,  // vec2/vec3/vec4/quat in `v` (f32), worldpos/dvec3 in `d` (f64); `count` components
  Id128,   // `id`
  Array,   // an array or fixed array: `count` elements, read with HostApi::read_element
};

struct Value {
  ValueKind kind = ValueKind::Nil;
  u32 count = 0;
  union {
    bool b;
    i64 i;
    u64 u;
    f64 f;
    f32 v[4];
    f64 d[3];
    u64 id[2];
  };
  struct {
    const char* data;
    u64 length;
  } str = {nullptr, 0};

  Value() noexcept : d{0.0, 0.0, 0.0} {}
};

// What a HostApi function returns. Zero is success.
enum class Result : i32 {
  Ok = 0,
  NotFound = 1,     // a type, a field path or an enumerator name that does not exist
  StaleView = 2,    // the view's object was unbound or rebound
  WrongType = 3,    // a view of one type read with a field resolved against another
  Unsupported = 4,  // a field kind views do not read (json, bytes, maps)
  OutOfRange = 5,   // an element index past the end
  InvalidArgument = 6,
};

enum class LogLevel : u32 { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };

// The engine as a module sees it. Every function takes the table's `host` first. The table and
// everything it points at outlive every call into the module; the module may keep the pointer.
struct HostApi {
  u32 abi_version;
  u32 size;  // sizeof(HostApi) as the host built it
  void* host;

  // The engine heap, under the hotload tag. Never malloc in a module and free in the host, or the
  // reverse: on Windows a DLL may carry its own C runtime and its own heap.
  void* (*allocate)(void* host, u64 bytes, u64 align);
  void (*deallocate)(void* host, void* p, u64 bytes, u64 align);
  // A record in the `hotload` category with the module's name as a field. No fatal level: a module
  // does not get to stop the engine.
  void (*log)(void* host, LogLevel level, const char* message, u64 length);
  // The host's fixed-step clock, never the wall clock (what `engine.tick()` is to a Luau script).
  u64 (*tick)(void* host);
  f64 (*fixed_step)(void* host);

  // Resolves a dotted path through nested structs of a registered schema type. A failure is
  // remembered: when `on_load` returns, every path that did not resolve is listed in the
  // refusal, so a module with three misspelt names hears about all three at once.
  Result (*resolve)(void* host, const char* type, u64 type_length, const char* path,
                    u64 path_length, FieldRef* out);
  // An enumerator's value by name, for an enum-typed field; remembered like `resolve`.
  Result (*enum_value)(void* host, FieldRef field, const char* name, u64 length, i64* out);
  // Reads one field of a viewed object. On any failure `out` is Nil.
  Result (*read)(void* host, View view, FieldRef field, Value* out);
  // Element `index` of an array field (scalar elements only).
  Result (*read_element)(void* host, View view, FieldRef field, u64 index, Value* out);

  // The line a failing `describe` or `on_load` leaves for the host to report. Called from any
  // other export, it is that export's failure: a game runner ends its session with the line
  // (docs/subsystems/apps.md, "The game contract"), and a host that runs no game ignores it.
  void (*fail)(void* host, const char* message, u64 length);

  // ---- a game runner's services (ABI 2; docs/subsystems/apps.md, "The game contract") ----
  // engine-view's `--game` answers them; a host that runs no game answers `Unsupported` and
  // ignores the rest. Everything is by value or into the caller's memory: nothing the host
  // returns needs freeing, and nothing a module passes is kept past the call.

  // Declares a game action the module reads (`interact`, `use`, `game_1`..`game_4`): `out_bit` is
  // its bit in `GameTick`'s `actions_pressed`, `actions_held` and `actions_released`. In `on_load`
  // only; an unknown name is remembered like a field path that does not resolve, so the load is
  // refused naming it.
  Result (*game_action)(void* host, const char* name, u64 length, u32* out_bit);
  // This tick's HUD: text line `line` (0 at the top), and a horizontal meter `value` of `max`
  // under a name. The host clears both at the start of every tick, so a module says each tick
  // what the HUD shows; the last tick before a frame is what the frame draws.
  void (*hud_text)(void* host, u32 line, const char* text, u64 length);
  void (*hud_meter)(void* host, const char* name, u64 length, f64 value, f64 max);
  // Ends the run after this tick's frame, with `json` (an object, as text) as the run's result in
  // the summary and the report. Text that is not a JSON object ends the run with an error.
  void (*end_run)(void* host, const char* json, u64 length);
  // The nearest placement of a kind (a scatter's or the ruins', by name) within `radius` metres
  // of the walker's feet: its position (world, three f64) and its distance. `NotFound` when the
  // session streams no world, or nothing of that kind is that near.
  Result (*nearest_placement)(void* host, const char* kind, u64 length, f64 radius,
                              f64* out_position, f64* out_distance);
};

// What every exported function receives first. Owned by the host, at the same address for the
// module's whole life across reloads; `state` points at the module's state object (null for a
// module without one) and changes when a reload migrates it.
struct ModuleContext {
  const HostApi* api;
  void* state;
};

// ---- exported functions: a closed set of signatures ---------------------------------------------

using VoidFn = void (*)(ModuleContext*);
using NumberFn = f64 (*)(ModuleContext*, f64);
using ViewNumberFn = f64 (*)(ModuleContext*, View);
using TickFn = void (*)(ModuleContext*, View self, f32 dt);
using ConditionFn = bool (*)(ModuleContext*, View a, View b);

// A signature's number. A function type that is not in the list does not compile into an export
// table, and the host refuses to hand out a function under a signature it was not exported with,
// so a module and a host that disagree about a call are stopped before the call.
template <class F>
inline constexpr u32 k_signature = 0;
template <>
inline constexpr u32 k_signature<VoidFn> = 1;
template <>
inline constexpr u32 k_signature<NumberFn> = 2;
template <>
inline constexpr u32 k_signature<ViewNumberFn> = 3;
template <>
inline constexpr u32 k_signature<TickFn> = 4;
template <>
inline constexpr u32 k_signature<ConditionFn> = 5;

struct Export {
  const char* name;
  u32 signature;
  void (*function)();
};

template <class F>
Export export_function(const char* name, F function) noexcept {
  static_assert(k_signature<F> != 0,
                "engine hotload: this function's type is not one of the export signatures in "
                "module_abi.h (VoidFn, NumberFn, ViewNumberFn, TickFn, ConditionFn)");
  return Export{name, k_signature<F>, reinterpret_cast<void (*)()>(function)};
}

// ---- the description --------------------------------------------------------------------------

struct ModuleDesc {
  u32 abi_version;     // k_abi_version as the module saw it
  u32 desc_size;       // sizeof(ModuleDesc) as the module saw it
  u32 type_info_size;  // sizeof(schema::TypeInfo) the state type was built against (0: no state)
  u32 export_count;
  const char* name;
  const schema::TypeInfo* state_type;  // null: the module keeps no state
  const Export* exports;
  // Called on the new build before it replaces the old one, with the new state already
  // migrated: resolve field paths here. Non-zero, or any path that did not resolve, refuses
  // the load and keeps the old module running.
  i32 (*on_load)(ModuleContext*);
  // Called on the old build once its replacement has loaded, before its state is destroyed.
  void (*on_unload)(ModuleContext*);
};

using DescribeFn = i32 (*)(const HostApi* api, ModuleDesc* out);

// ---- module-side conveniences (header-only, so a module needs nothing else) ---------------------

inline void fail(const ModuleContext* context, const char* message) noexcept {
  u64 n = 0;
  while (message[n] != '\0')
    ++n;
  context->api->fail(context->api->host, message, n);
}

inline void fail(const HostApi* api, const char* message) noexcept {
  u64 n = 0;
  while (message[n] != '\0')
    ++n;
  api->fail(api->host, message, n);
}

inline Result resolve(const ModuleContext* context, const char* type, const char* path,
                      FieldRef& out) noexcept {
  u64 t = 0;
  while (type[t] != '\0')
    ++t;
  u64 p = 0;
  while (path[p] != '\0')
    ++p;
  return context->api->resolve(context->api->host, type, t, path, p, &out);
}

inline Result enum_value(const ModuleContext* context, FieldRef field, const char* name,
                         i64& out) noexcept {
  u64 n = 0;
  while (name[n] != '\0')
    ++n;
  return context->api->enum_value(context->api->host, field, name, n, &out);
}

inline Result read(const ModuleContext* context, View view, FieldRef field, Value& out) noexcept {
  return context->api->read(context->api->host, view, field, &out);
}

inline Result read_element(const ModuleContext* context, View view, FieldRef field, u64 index,
                           Value& out) noexcept {
  return context->api->read_element(context->api->host, view, field, index, &out);
}

inline void log(const ModuleContext* context, LogLevel level, const char* message) noexcept {
  u64 n = 0;
  while (message[n] != '\0')
    ++n;
  context->api->log(context->api->host, level, message, n);
}

// ---- the game runner's services (ABI 2) ----

inline u64 text_length(const char* text) noexcept {
  u64 n = 0;
  while (text[n] != '\0')
    ++n;
  return n;
}

inline Result game_action(const ModuleContext* context, const char* name, u32& bit) noexcept {
  return context->api->game_action(context->api->host, name, text_length(name), &bit);
}

// Whether action `bit` (from `game_action`) is set in one of `GameTick`'s three masks.
inline bool action_set(const Value& mask, u32 bit) noexcept {
  return bit < 64 && ((mask.u >> bit) & 1u) != 0;
}

inline void hud_text(const ModuleContext* context, u32 line, const char* text) noexcept {
  context->api->hud_text(context->api->host, line, text, text_length(text));
}

inline void hud_meter(const ModuleContext* context, const char* name, f64 value, f64 max) noexcept {
  context->api->hud_meter(context->api->host, name, text_length(name), value, max);
}

inline void end_run(const ModuleContext* context, const char* json) noexcept {
  context->api->end_run(context->api->host, json, text_length(json));
}

inline Result nearest_placement(const ModuleContext* context, const char* kind, f64 radius,
                                f64 (&position)[3], f64& distance) noexcept {
  return context->api->nearest_placement(context->api->host, kind, text_length(kind), radius,
                                         position, &distance);
}

template <class State>
State& state(ModuleContext* context) noexcept {
  return *static_cast<State*>(context->state);
}

}  // namespace engine::hotload

// Opens the module's entry point:
//
//   ENGINE_HOTLOAD_DESCRIBE(api, out) {
//     out->name = "sandbox_npc";
//     ...
//     return 0;
//   }
#define ENGINE_HOTLOAD_DESCRIBE(api, out)                                 \
  extern "C" ENGINE_HOTLOAD_EXPORT ::engine::i32 engine_hotload_describe( \
      const ::engine::hotload::HostApi* api, ::engine::hotload::ModuleDesc* out)
