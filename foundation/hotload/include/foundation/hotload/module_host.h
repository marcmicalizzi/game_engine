#pragma once

// The host side of a hot-reloadable C++ game module (docs/subsystems/hotload.md; experiment E7's
// C++ arm, docs/experiments/e7-luau-spike.md "The C++ hot-reload arm").
//
// A `ModuleHost` owns one loaded game module: the library (loaded from a per-load copy, because
// Windows keeps a loaded DLL's file locked and glibc hands back the same handle for the same
// path), its export table, its state object and the views it reads host objects through. It
// watches the library file the way `scripting::ScriptContext` watches scripts — `poll_changes()`
// stats it when the host asks — and on a change it loads the new build beside the old one,
// migrates the state from the old build's schema descriptor to the new one's, lets the new build
// resolve its field paths, and only then swaps the export table and unloads the old build. Any
// failure on the way keeps the old build running and says why in `last_error()`.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/schema/type_info.h>
#include <foundation/hotload/module_abi.h>

#include <memory>
#include <string>
#include <string_view>

namespace engine::hotload {

// Capability registration constants (ADR-0027, docs/plan/02-architecture.md §2.8).
//
// Determinism: "hashed" — a module's functions are native code over the views and the state, so
// they are as deterministic as the C++ in them; the host adds nothing machine-dependent (no wall
// clock through the API, the clock is the host's fixed step). Floating point follows the build
// (contraction off, ADR-0035) because a module is compiled with the engine's flags.
inline constexpr const char* k_determinism = "hashed";
// Reloading is a development-time service a host drives between ticks; it has no tick phase.
inline constexpr const char* k_phase = "none";

enum class Status : u8 {
  Ok,
  NotLoaded,          // no module is loaded
  CopyFailed,         // the per-load copy could not be written
  LoadFailed,         // the OS loader refused the library
  NoEntryPoint,       // no engine_hotload_describe
  DescribeFailed,     // describe returned non-zero
  AbiMismatch,        // built against another module_abi.h
  BadDescriptor,      // a state type or export table the host cannot use
  StateIncompatible,  // the new state type cannot take the old state
  LoadHookFailed,     // on_load returned non-zero, or a field path did not resolve
  MissingFunction,    // the new build lacks a function the host holds
  SignatureMismatch,  // a function exported under another signature
  NotFound,           // find(): no such export
  InvalidArgument,
};

const char* status_name(Status status) noexcept;

// An exported function as the host holds it: survives reloads (re-resolved by name).
struct FunctionId {
  u32 index = 0xFFFFFFFFu;
  bool valid() const noexcept { return index != 0xFFFFFFFFu; }
};

// A game runner's answers to `HostApi`'s game services (ABI 2; module_abi.h, and
// docs/subsystems/apps.md "The game contract"): the host forwards a module's call to these, with
// `user` first. A null function — every one, for a host that runs no game — answers `Unsupported`
// (`action`, `nearest_placement`) or does nothing. `action` returning anything but `Ok` is a name
// the load is refused for, with `why`, listed beside any field path that did not resolve.
struct GameServices {
  void* user = nullptr;
  Result (*action)(void* user, std::string_view name, u32* out_bit, std::string* why) = nullptr;
  void (*hud_text)(void* user, u32 line, std::string_view text) = nullptr;
  void (*hud_meter)(void* user, std::string_view name, f64 value, f64 max) = nullptr;
  void (*end_run)(void* user, std::string_view json) = nullptr;
  Result (*nearest_placement)(void* user, std::string_view kind, f64 radius, f64* out_position,
                              f64* out_distance) = nullptr;
};

struct HostConfig {
  // Where per-load copies of the library go. Empty: "<library>.hotload" beside the library.
  std::string shadow_directory;
  // The host's clock, as the module reads it (HostApi::tick, fixed_step).
  u64 tick = 0;
  f64 fixed_step = 1.0 / 60.0;
  // The game runner's services, when this host runs a game (engine-view's `--game`).
  GameServices game;
};

// What the last successful load or reload did to the state.
struct ReloadReport {
  u32 generation = 0;
  Vector<std::string> added;      // fields the new build has and the old did not: defaulted
  Vector<std::string> removed;    // fields the old build had and the new does not: dropped
  Vector<std::string> converted;  // fields whose kind changed compatibly (u16 -> u32, f32 -> f64)
  u64 state_bytes = 0;            // the serialized state handed across
  // Where the reload's time went: the per-load copy, the OS loader with describe and the checks,
  // the migration and on_load, and unloading the old build (its destructor, the OS, the copy's
  // deletion).
  i64 copy_ns = 0;
  i64 load_ns = 0;
  i64 migrate_ns = 0;
  i64 unload_ns = 0;
};

struct HostStats {
  u64 refused_reads = 0;  // reads refused: a stale view, a wrong type, a bad handle
  u32 loads = 0;          // successful loads and reloads
  u32 failed_loads = 0;
};

class ModuleHost {
 public:
  explicit ModuleHost(HostConfig config = {});
  ~ModuleHost();
  ENGINE_NON_COPYABLE(ModuleHost);

  // Loads a library and starts watching its file. A host holds one module; loading another
  // unloads the first.
  Status load(std::string_view library_path);
  // Loads the watched file again whether or not it changed: copy, load, describe, check,
  // migrate, on_load, swap, unload the old build. On failure the old build keeps running.
  Status reload();
  // Stats the watched file and reloads it when its size or modification time moved. Returns 1
  // when a new build was swapped in, 0 otherwise (a failed reload is in last_error(), and is not
  // retried until the file changes again).
  u32 poll_changes();
  // Calls on_unload, destroys the state, unloads the library and deletes its per-load copy.
  void unload();

  bool loaded() const noexcept { return module_ != nullptr; }
  u32 generation() const noexcept { return generation_; }
  const std::string& module_name() const noexcept;
  const std::string& library_path() const noexcept { return library_path_; }
  // The per-load copy the current build was loaded from (empty when nothing is loaded).
  const std::string& loaded_copy_path() const noexcept;
  const std::string& last_error() const noexcept { return last_error_; }
  // The line a module handed `HostApi::fail` from an export since the last call (empty: none),
  // cleared by the call. A load's `describe` and `on_load` failures are not here: they refuse the
  // load and are in `last_error()`. A game runner reads it after each tick (apps.md).
  std::string take_failure();
  const ReloadReport& last_report() const noexcept { return report_; }
  HostStats stats() const noexcept { return stats_; }

  // ---- the export table ----
  Status find_function(std::string_view name, u32 signature, FunctionId& out);
  template <class F>
  Status find(std::string_view name, FunctionId& out) {
    return find_function(name, k_signature<F>, out);
  }
  // The function's current pointer. Read it for each call (or after each reload): a reload
  // replaces the table. The id must have come from find<F>() with the same F.
  template <class F>
  F function(FunctionId id) const noexcept {
    return reinterpret_cast<F>(functions_[id.index].pointer);
  }
  ModuleContext* context() noexcept { return &context_; }

  // ---- host objects the module reads ----
  View bind(const void* object, const schema::TypeInfo& type);
  template <class T>
  View bind(const T& object) {
    return bind(&object, schema::type_of<T>());
  }
  // Points the view's slot at another object of the same type; views of the old one go stale.
  void rebind(View& view, const void* object);
  void unbind(View view);

  // ---- the state ----
  const schema::TypeInfo* state_type() const noexcept;
  const void* state() const noexcept { return context_.state; }
  // The state as its descriptor writes it (what a reload hands across).
  bool state_json(JsonValue& out) const;

  void set_time(u64 tick, f64 fixed_step) noexcept {
    config_.tick = tick;
    config_.fixed_step = fixed_step;
  }

  // The host's view reads, exposed for the tests and the bench (they are what HostApi::read is).
  Result read(View view, FieldRef field, Value& out) noexcept;
  Result resolve(std::string_view type, std::string_view path, FieldRef& out);

  struct Module;  // one loaded build (src/module_host.cpp)

  // One resolved field path: where it is in the root object and what it holds.
  struct Field {
    const schema::TypeInfo* root = nullptr;
    const schema::TypeRef* type = nullptr;
    u32 offset = 0;
    schema::Kind kind = schema::Kind::Bool;
    std::string type_name;
    std::string path;
  };
  // One bound host object.
  struct Slot {
    const void* object = nullptr;
    const schema::TypeInfo* type = nullptr;
    u32 version = 1;
    bool live = false;
  };

 private:
  Status load_build(Module& build, const std::string& copy_path);
  Status adopt(std::unique_ptr<Module> build);
  void destroy_build(std::unique_ptr<Module>& build);
  void fail(Status status, std::string message);

  friend struct HostCallbacks;

  HostConfig config_;
  HostApi api_{};
  ModuleContext context_{};
  std::unique_ptr<Module> module_;
  std::string library_path_;
  i64 watched_modified_ms_ = 0;
  u64 watched_size_ = 0;
  u32 generation_ = 0;
  std::string last_error_;
  ReloadReport report_;
  HostStats stats_;

  struct FunctionSlot {
    std::string name;
    u32 signature = 0;
    void (*pointer)() = nullptr;
  };
  Vector<FunctionSlot> functions_;
  Vector<Slot> slots_;
  Vector<u32> free_slots_;
  Vector<Field> fields_;
  // While a build's describe or on_load runs: the line it failed with and the paths it could not
  // resolve, reported together when it returns.
  std::string pending_failure_;
  Vector<std::string> pending_unresolved_;
  i64 pending_copy_ns_ = 0;
  i64 pending_load_ns_ = 0;
};

// Moves a state object from one build's descriptor to another's: the old one serialized as schema
// JSON, the new one read from it with unknown fields ignored, so a field added takes its default,
// a field removed is dropped, and a field retyped compatibly converts. A field whose kind changed
// incompatibly refuses the migration and `error` names it. `to` must be constructed already.
// Public because the bench measures it per state (`hotload.migrate/N`).
bool migrate_state(const schema::TypeInfo& from_type, const void* from,
                   const schema::TypeInfo& to_type, void* to, ReloadReport& report,
                   std::string& error);

// The kinds a module's state may hold: fixed-layout values only (scalars, vectors, ids, enums,
// fixed arrays and nested structs of those). Owning kinds (string, bytes, json, arrays, maps,
// optionals) are refused, naming the field: their memory would belong to the module's copy of
// the C++ runtime and outlive it.
bool check_state_type(const schema::TypeInfo& type, std::string& error);

}  // namespace engine::hotload
