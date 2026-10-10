#pragma once

// ScriptContext: one sandboxed Luau VM and the scripts loaded into it
// (docs/subsystems/scripting.md; docs/plan/08-toolchain.md §8.2; experiment E7,
// docs/experiments/e7-luau-spike.md).
//
// The shape is the plan's "content logic": a script is a file of named functions — `on_tick(self,
// dt)`, `evaluate(condition)` — and the host calls one with arguments, under a budget, and gets
// data back. Arguments that are schema objects arrive as read-only views generated from the schema
// descriptors, so `npc.health` or `quest.state` reads the C++ object in place with the IDL's types
// and enum names, and assigning to one is an error. What a function returns is converted to a
// JsonValue: a quest condition returns `true`, an NPC routine returns `{ action = "flee" }`, and
// the host turns that into document commands. Scripts never mutate document state in v0; mutation
// stays declarative, one schema-typed command at a time (docs/plan/03-data-model.md §3.3).
//
//     scripting::ScriptContext context;                       // limits from ScriptLimits{}
//     scripting::ScriptId quest;
//     if (context.load("quests/q17.luau", source, quest) != scripting::Status::Ok)
//       report(context.last_error());                         // chunk, line, message
//     scripting::FunctionId evaluate;
//     context.find_function(quest, "evaluate", evaluate);
//     const scripting::Arg args[] = {scripting::Arg::object(scripting::object_ref(player))};
//     JsonValue done;
//     if (context.call(evaluate, args, &done) == scripting::Status::Ok && done == JsonValue(true))
//       ...;
//
// What the sandbox guarantees, and where each guarantee is enforced:
//   - No io, os, debug, require, loadstring, getfenv/setfenv, collectgarbage, gcinfo, print or
//     math.random: the libraries are never opened or the names are removed before the global
//     table is frozen (luaL_sandbox), so a script cannot reach the file system, the wall clock, or
//     a random seed it was not given. `engine.random(seed, index)` is the deterministic stand-in.
//   - A memory limit on the VM heap, enforced in the VM's allocator: at the limit an allocation
//     fails, the script gets Luau's out-of-memory error, the call returns MemoryExceeded, and a
//     full collection runs before the next call.
//   - A step budget per call, enforced in the VM's interrupt: Luau calls it at every loop back
//     edge, call and return, so any unbounded computation passes through it once per iteration.
//     Counting those is deterministic — the same script over the same inputs uses the same number
//     of steps on every machine — which a time budget is not. `time_budget_ns` is an optional
//     backstop that makes a call's outcome machine-dependent, and is off by default.
//   - Errors come back as a Status with the chunk, line and message in last_error(). Nothing is
//     thrown across the boundary: Luau is built with LUA_USE_LONGJMP (cmake/EngineScripting.cmake).
//
// A context is single-threaded: one thread at a time, and never re-entered from a call it is
// making. Many scripts share one context; each script has its own global table (a sandboxed
// thread's), frozen once its top level has run, so a script's globals are its entry points and
// assigning a new global at run time is an error.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/schema/type_info.h>

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace engine {
struct Id128;
}

namespace engine::scripting {

enum class Status : u8 {
  Ok,
  CompileError,     // the source did not compile; a reload kept the previous version
  RuntimeError,     // the script raised an error (last_error() has chunk, line, message)
  BudgetExceeded,   // the call used its step budget (or the optional time budget)
  MemoryExceeded,   // the VM heap reached ScriptLimits::memory_bytes
  NotFound,         // no such script, function, binding, or file
  InvalidArgument,  // a stale handle, a re-entrant call, or a value with no JSON form
  IoError,          // a script file could not be read
};
const char* status_name(Status status) noexcept;

// What the last failing operation said. `chunk` is the script's name as loaded (its path, for a
// file), `line` is 1-based and 0 when there is none — a memory error has no position, because
// Luau raises it without running an error handler.
struct ScriptError {
  Status status = Status::Ok;
  std::string chunk;
  u32 line = 0;
  std::string message;  // without the "chunk:line:" prefix Luau puts on it
};

struct ScriptLimits {
  // The VM heap, bytes. Enforced while a script runs; host-side setup (loading, arguments) is
  // bounded by the host's own data and is not refused, so a failed allocation can never happen
  // outside a protected call.
  u64 memory_bytes = u64{64} << 20;
  // Safepoints per call (loop iterations, calls, returns); 0 is unlimited. A trivial function
  // uses one or two; a quest condition tens to hundreds; the default is a runaway guard.
  u64 step_budget = 1'000'000;
  // Wall time per call, nanoseconds; 0 is off. Checked every 1024 steps.
  u64 time_budget_ns = 0;

  // The limits the `scripting.*` tunables say (docs/subsystems/tunables.md).
  static ScriptLimits from_tunables() noexcept;
};

inline constexpr u32 k_invalid_index = ~u32{0};

struct ScriptId {
  u32 index = k_invalid_index;
  constexpr bool valid() const noexcept { return index != k_invalid_index; }
  constexpr bool operator==(const ScriptId&) const noexcept = default;
};

// A named function of one script, resolved once. It survives a reload: the name is resolved
// again against the new chunk, and a function the new chunk no longer defines calls as NotFound.
struct FunctionId {
  u32 index = k_invalid_index;
  constexpr bool valid() const noexcept { return index != k_invalid_index; }
  constexpr bool operator==(const FunctionId&) const noexcept = default;
};

// A schema object the host keeps visible to scripts across calls (an NPC's record, a quest).
struct BindingId {
  u32 slot = k_invalid_index;
  u32 serial = 0;
  constexpr bool valid() const noexcept { return slot != k_invalid_index; }
  constexpr bool operator==(const BindingId&) const noexcept = default;
};

// A schema-typed object in host memory: a struct TypeInfo and a pointer to an instance of it.
struct ObjectRef {
  const schema::TypeInfo* type = nullptr;
  const void* object = nullptr;
};

template <class T>
ObjectRef object_ref(const T& value) noexcept {
  return ObjectRef{&schema::type_of<T>(), &value};
}

// One argument to a script function. Trivially copyable and never allocating: strings and JSON
// are borrowed for the duration of the call.
class Arg {
 public:
  enum class Kind : u8 { Nil, Boolean, Number, String, Object, Binding, Json };

  static Arg nil() noexcept { return Arg{}; }
  static Arg boolean(bool value) noexcept;
  static Arg number(f64 value) noexcept;
  static Arg string(std::string_view value) noexcept;
  // Visible to the script for this call only; a script that keeps the view and reads it on a
  // later call gets an error, never the memory.
  static Arg object(ObjectRef value) noexcept;
  static Arg binding(BindingId value) noexcept;
  // Converted to a read-only table (objects to tables with string keys, arrays to sequences).
  static Arg json(const JsonValue& value) noexcept;

  Kind kind() const noexcept { return kind_; }
  bool as_boolean() const noexcept { return boolean_; }
  f64 as_number() const noexcept { return number_; }
  std::string_view as_string() const noexcept { return {string_.data, string_.size}; }
  ObjectRef as_object() const noexcept { return object_; }
  BindingId as_binding() const noexcept { return binding_; }
  const JsonValue& as_json() const noexcept { return *json_; }

 private:
  struct Str {
    const char* data;
    usize size;
  };
  Kind kind_ = Kind::Nil;
  union {
    bool boolean_;
    f64 number_ = 0.0;
    Str string_;
    ObjectRef object_;
    BindingId binding_;
    const JsonValue* json_;
  };
};

// How `engine.object(id)` finds an object: the host maps an id to a schema object it owns. The
// object is visible for the rest of the call, like an Arg::object.
struct ObjectResolver {
  void* user = nullptr;
  bool (*resolve)(void* user, const Id128& id, ObjectRef& out) = nullptr;
};

// ---- programs: a client's script ----------------------------------------------------------------
//
// The other shape a script can take (docs/subsystems/scripting.md, "Programs: the console"). Not
// content logic a host calls for data, but a program a host runs once, top to bottom, which drives
// the host through the methods the host offers it: engine-host's console, where a coding agent's
// script calls the engine protocol (docs/subsystems/apps.md, "The console"). A program changes
// nothing itself either; it asks the host, one method call at a time, and the host decides — for
// the console, the protocol's dispatcher with its roles, attribution and journal.

// One method a program may call, as `engine.call(name, params)` and as a function named after it:
// `engine.doc.apply(params)` for "doc.apply". A name in the `engine` namespace is a function of
// `engine` itself ("engine.ping" is `engine.ping()`, not `engine.engine.ping()`), and a name that
// would replace `engine.call`, `engine.json` or another method's namespace gets no function of
// its own and is reached through `engine.call`.
struct HostMethod {
  std::string_view name;
  // The struct its params table is read as, through the descriptors: what turns Luau's one table
  // type into JSON's two (`{}` is `[]` for an array field, `{}` for a struct or a map) and keeps a
  // float field a float. Null reads the table untyped.
  const schema::TypeInfo* params = nullptr;
};

// Why a method call failed. The program sees it as a Luau error whose value is the table
// `{method, code, message, data}`, so `pcall` returns it to the script; uncaught, it ends the run.
struct HostError {
  i64 code = 0;
  std::string message;
  JsonValue data;
};

struct ProgramHost {
  void* user = nullptr;
  std::span<const HostMethod> methods;
  // One method call: false with `error` filled raises it in the program. Required.
  bool (*call)(void* user, std::string_view method, const JsonValue& params, JsonValue& result,
               HostError& error) = nullptr;
  // What `print(...)` writes: its arguments through tostring, tab-separated, with a newline. Null
  // drops it.
  void (*print)(void* user, std::string_view text) = nullptr;
};

struct ContextStats {
  // What the VM holds from the engine heap: Luau hands out small objects from 16 and 32 KiB pages
  // it allocates per size class, so this is its footprint, unused page space included.
  u64 heap_bytes = 0;
  u64 heap_peak_bytes = 0;
  // The bytes of the objects the VM counts as live (Luau's own total), without page slack.
  u64 live_bytes = 0;
  u64 allocated_bytes = 0;  // everything the VM ever allocated, for allocation rates
  u64 calls = 0;
  u64 last_call_steps = 0;
  u32 scripts = 0;
  u32 bindings = 0;
};

class ScriptContext {
 public:
  explicit ScriptContext(const ScriptLimits& limits = ScriptLimits{});
  ~ScriptContext();
  ENGINE_NON_COPYABLE(ScriptContext);

  const ScriptLimits& limits() const noexcept;

  // ---- scripts ---------------------------------------------------------------------------

  // Compiles `source` and runs its top level, which defines the script's functions. `name` is
  // the chunk name errors carry. On failure nothing is loaded and `out` is left invalid.
  Status load(std::string_view name, std::string_view source, ScriptId& out);
  // load() from a file, named by its path, and watched by poll_changes().
  Status load_file(std::string_view path, ScriptId& out);
  // Replaces a script's chunk. The new source is compiled and its top level run first, and only
  // when both succeed does it replace the old one: a compile or run error keeps the last good
  // chunk and returns the error.
  Status reload(ScriptId script, std::string_view source);
  // Hot reload, the way gfx::ShaderLibrary::poll_changes() does it for shaders: every file
  // script whose file changed since it was loaded is read and reload()ed. Returns how many were
  // replaced (and appends them to `reloaded`). A save that fails to compile keeps the previous
  // chunk, is logged once, and is in last_error(); the next save is tried again.
  u32 poll_changes(Vector<ScriptId>* reloaded = nullptr);
  // Increments on every successful load or reload of `script`; 0 for an unknown id.
  u32 generation(ScriptId script) const noexcept;

  // ---- calls -----------------------------------------------------------------------------

  // Resolves a global function of `script` by name. NotFound when the chunk defines none.
  Status find_function(ScriptId script, std::string_view name, FunctionId& out);
  // Calls `function` under the step and memory budgets. The first result, when `result` is not
  // null, is converted to JSON: nil to null, booleans, numbers (integral values as integers),
  // strings, tables (a sequence 1..n to an array, string keys to an object, an empty table to an
  // empty array), vectors to [x, y, z], schema object views to their schema JSON.
  Status call(FunctionId function, std::span<const Arg> args = {}, JsonValue* result = nullptr);
  // find_function() and call() in one step, for cold paths.
  Status call(ScriptId script, std::string_view function, std::span<const Arg> args = {},
              JsonValue* result = nullptr);

  // ---- programs --------------------------------------------------------------------------

  // Runs `source` once as a program: compiled as chunk `name`, its top level called with `args`
  // as `...`, under ONE step budget and the memory limit for the whole run (a program is one
  // call). Its globals are its own and writable, and read through to the sandbox's libraries; in
  // place of the content-logic API it sees `print` and an `engine` table of `call`, `json(value
  // [, pretty])` — a value's JSON text — and one function per host method. A method's result is
  // an ordinary table the program may change and send back. Nothing of the program stays loaded.
  // Errors are reported as call() reports them: a host error the program did not catch as
  // "<method>: error <code>: <message>" plus its data as JSON, at the line that made the call.
  Status run_program(std::string_view name, std::string_view source, const ProgramHost& host,
                     std::span<const std::string_view> args = {});

  // ---- document state --------------------------------------------------------------------

  // Makes a schema object visible across calls. The object must outlive the binding and must not
  // change shape (a vector reallocating) while bound; rebind() after it may have.
  BindingId bind(ObjectRef object);
  // Points a binding at another object. Views the script took of the old object's nested
  // structs are invalidated; the binding's own view follows the new object.
  Status rebind(BindingId binding, ObjectRef object);
  void unbind(BindingId binding) noexcept;
  void set_resolver(ObjectResolver resolver) noexcept;
  // What `engine.tick()` and `engine.fixed_step()` return. The host's fixed-step clock, never the
  // wall clock.
  void set_time(u64 tick, f64 fixed_step) noexcept;

  // ---- diagnostics -----------------------------------------------------------------------

  const ScriptError& last_error() const noexcept;
  ContextStats stats() const noexcept;
  // A full collection. Not for the frame loop.
  void collect_garbage() noexcept;
  // While set, the duration of every incremental GC step is appended to `pauses_ns`. For
  // measurement; costs two clock reads per step.
  void record_gc_pauses(Vector<u64>* pauses_ns) noexcept;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace engine::scripting
