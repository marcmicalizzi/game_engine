// ScriptContext: the VM, the sandbox, the budgets, loading and hot reload, calls, and the engine
// API a script sees (docs/subsystems/scripting.md).

#include "context_impl.h"

#include <core/base/assert.h>
#include <core/hash/hash.h>
#include <core/ids/id128.h>
#include <core/log/log.h>
#include <core/time/time.h>
#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <luacode.h>
#include <string>

namespace engine::scripting {

ENGINE_LOG_CATEGORY_DEFINE(log_scripting, "scripting");

namespace {

// ---- tunables --------------------------------------------------------------------------------
//
// Read once, when a context is created (ScriptLimits::from_tunables); a context's limits do not
// change under a running script.

tunables::Int tunable_memory_limit_mb{
    "scripting.memory_limit_mb", 64, 1, 1 << 16,
    "Heap limit of one script VM, MiB; an allocation past it fails the "
    "call with MemoryExceeded"};
tunables::Int tunable_step_budget{
    "scripting.step_budget", 1'000'000, 0, i64{1} << 40,
    "Safepoints (loop iterations, calls, returns) one script call may use; "
    "0 is unlimited"};
tunables::Int tunable_time_budget_us{
    "scripting.time_budget_us", 0, 0, i64{1} << 32,
    "Wall-time backstop per script call, microseconds; 0 is off. Makes "
    "a call's outcome machine-dependent, so off unless debugging"};

mem::TagId scripting_tag() {
  static const mem::TagId tag = mem::register_tag("scripting");
  return tag;
}

// ---- the VM's allocator ------------------------------------------------------------------------
//
// Every byte the VM owns goes through the engine heap under the "scripting" tag, so the memory
// report attributes it and the global allocation counter sees it. The limit is enforced here and
// only while a script runs (depth > 0): at the limit a request fails, Luau raises its
// out-of-memory error inside the protected call, and the call returns MemoryExceeded. Host-side
// work between calls — loading, pushing arguments, converting results — is bounded by the host's
// own data and is never refused, because a failure there would have no protected call to land in.
//
// Luau hands out small objects from pages it allocates here, so these calls are page-sized and
// rare; "heap" is the VM's footprint including the unused part of its pages.

constexpr usize k_alignment = 16;

void* allocate(void* ud, void* ptr, size_t osize, size_t nsize) {
  Impl& impl = *static_cast<Impl*>(ud);
  if (nsize == 0) {
    if (ptr != nullptr) {
      mem::deallocate(ptr, osize, k_alignment);
      impl.heap -= osize;
    }
    return nullptr;
  }
  const u64 old_size = ptr != nullptr ? osize : 0;
  if (impl.depth > 0 && nsize > old_size &&
      impl.heap - old_size + nsize > impl.limits.memory_bytes) {
    if (impl.cause == Cause::None) impl.cause = Cause::Memory;
    return nullptr;
  }
  mem::TagScope scope(impl.tag);
  void* block = mem::try_allocate(nsize, k_alignment);
  if (block == nullptr) return nullptr;
  if (ptr != nullptr) {
    std::memcpy(block, ptr, std::min<usize>(osize, nsize));
    mem::deallocate(ptr, osize, k_alignment);
  }
  impl.heap = impl.heap - old_size + nsize;
  impl.heap_peak = std::max(impl.heap_peak, impl.heap);
  impl.allocated += nsize;
  return block;
}

// ---- the step budget ---------------------------------------------------------------------------
//
// Luau calls this at every loop back edge, call and return of interpreted code (and at the start
// and end of every incremental GC step, with gc >= 0). A script that has used its budget is
// stopped at its next safepoint with an ordinary Luau error; a script that catches it with pcall
// only reaches the next safepoint, which raises again, so the budget cannot be caught and
// ignored. The message is a string made once, so raising it allocates nothing.

void raise_budget(lua_State* L, Impl& impl, Cause cause, int message_ref) {
  if (impl.cause == Cause::None) impl.cause = cause;
  lua_rawcheckstack(L, 1);
  lua_getref(L, message_ref);
  lua_error(L);
}

void interrupt(lua_State* L, int gc) {
  Impl& impl = impl_of(L);
  if (gc >= 0) {
    // luaC_step brackets every incremental step with two calls, start then end.
    if (impl.gc_pauses != nullptr) {
      if (!impl.in_gc_step) {
        impl.gc_step_start = time::monotonic_ns();
        impl.in_gc_step = true;
      } else {
        impl.gc_pauses->push_back(static_cast<u64>(time::monotonic_ns() - impl.gc_step_start));
        impl.in_gc_step = false;
      }
    }
    return;
  }
  if (impl.depth == 0) return;
  ++impl.steps;
  if (impl.step_limit != 0 && impl.steps > impl.step_limit)
    raise_budget(L, impl, Cause::Budget, impl.budget_message_ref);
  if (impl.deadline_ns != 0 && (impl.steps & 1023u) == 0 && time::monotonic_ns() > impl.deadline_ns)
    raise_budget(L, impl, Cause::Time, impl.time_message_ref);
}

int16_t useratom(lua_State* L, const char* s, size_t len) { return atom_of(impl_of(L), s, len); }

// An unprotected error has nowhere to go. Every entry into the VM that can raise is either a
// protected call or host-side work the memory limit does not apply to, so this is a bug here or a
// real out-of-memory, and stopping is the honest answer.
void panic(lua_State* L, int code) {
  (void)L;
  ENGINE_LOG_FATAL(log_scripting, "unprotected luau error", log::field("status", code));
}

// The message handler every call runs under (stack index 1 of the main thread). Luau calls it with
// the failing frame still on the stack, so the first interpreted frame above it is where the
// script was when it failed: that is the chunk and line last_error() reports, whatever the
// message says or does not say. It copies into a fixed buffer and allocates nothing, because it
// also runs for out-of-memory errors.
int error_handler(lua_State* L) {
  Impl& impl = impl_of(L);
  lua_Debug ar;
  for (int level = 1; lua_getinfo(L, level, "sl", &ar) != 0; ++level) {
    if (ar.what == nullptr || ar.what[0] == 'C') continue;
    const char* source = ar.source != nullptr ? ar.source : "?";
    if (source[0] == '=' || source[0] == '@') ++source;
    const usize n = std::min<usize>(std::strlen(source), sizeof(impl.error_chunk) - 1);
    std::memcpy(impl.error_chunk, source, n);
    impl.error_chunk[n] = '\0';
    impl.error_line = ar.currentline > 0 ? static_cast<u32>(ar.currentline) : 0;
    impl.error_located = true;
    break;
  }
  return 1;
}

// ---- the engine API ----------------------------------------------------------------------------
//
// The fixed surface every script sees as the global `engine`. Typed in emit_engine_api_definitions
// (type_definitions.cpp); the two are kept in step by the typed API test.

bool parse_script_level(std::string_view name, log::Level& out) noexcept {
  if (name == "trace")
    out = log::Level::Trace;
  else if (name == "debug")
    out = log::Level::Debug;
  else if (name == "info")
    out = log::Level::Info;
  else if (name == "warn")
    out = log::Level::Warn;
  else if (name == "error")
    out = log::Level::Error;
  else
    return false;
  return true;
}

// engine.log(level, message, fields?) — a record in the `scripting` category carrying the
// script's chunk and line as the fields `script` and `line`, then up to 14 typed fields from a
// table of strings, numbers and booleans. The position is a field and not the record's `file`:
// sinks keep `file` as a pointer, expecting __FILE__, and a chunk name is not static. There is
// no "fatal": a script does not get to stop the engine.
int api_log(lua_State* L) {
  constexpr int k_max_fields = 14;
  size_t level_len = 0;
  const char* level_text = luaL_checklstring(L, 1, &level_len);
  log::Level level = log::Level::Info;
  if (!parse_script_level(std::string_view(level_text, level_len), level)) {
    luaL_error(L,
               "engine.log: level must be \"trace\", \"debug\", \"info\", \"warn\" or \"error\", "
               "not \"%s\"",
               level_text);
  }
  size_t message_len = 0;
  const char* message = luaL_checklstring(L, 2, &message_len);
  if (!lua_isnoneornil(L, 3)) luaL_checktype(L, 3, LUA_TTABLE);
  if (!log_scripting.enabled(level)) return 0;

  lua_Debug ar;
  const bool located = lua_getinfo(L, 1, "sl", &ar) != 0;
  log::Field fields[k_max_fields + 2];
  int count = 0;
  fields[count++] = log::field("script", located ? ar.short_src : "?");
  fields[count++] = log::field("line", located && ar.currentline > 0 ? ar.currentline : 0);
  if (lua_istable(L, 3)) {
    lua_rawcheckstack(L, 2);
    lua_pushnil(L);
    while (lua_next(L, 3) != 0) {
      if (count == k_max_fields + 2) {
        lua_pop(L, 2);
        luaL_error(L, "engine.log: at most %d fields", k_max_fields);
      }
      if (lua_type(L, -2) != LUA_TSTRING) {
        lua_pop(L, 2);
        luaL_error(L, "engine.log: field names must be strings");
      }
      size_t key_len = 0;
      const char* key = lua_tolstring(L, -2, &key_len);
      const std::string_view name(key, key_len);
      switch (lua_type(L, -1)) {
        case LUA_TBOOLEAN: fields[count++] = log::field(name, lua_toboolean(L, -1) != 0); break;
        case LUA_TNUMBER: fields[count++] = log::field(name, lua_tonumber(L, -1)); break;
        case LUA_TSTRING: {
          size_t value_len = 0;
          const char* value = lua_tolstring(L, -1, &value_len);
          fields[count++] = log::field(name, std::string_view(value, value_len));
          break;
        }
        default:
          lua_pop(L, 2);
          luaL_error(L, "engine.log: field '%s' must be a string, number or boolean", key);
      }
      lua_pop(L, 1);
    }
  }
  log::emit(log_scripting, level, nullptr, 0, std::string_view(message, message_len),
            std::span<const log::Field>(fields, static_cast<usize>(count)));
  return 0;
}

// engine.tunable(name) — a tunable's current value: numbers for Int and Float, a boolean for Bool,
// the choice's name for Enum. A lookup by name scans the registry, so read it once, at load or at
// the top of a call, not inside a loop.
int api_tunable(lua_State* L) {
  size_t len = 0;
  const char* name = luaL_checklstring(L, 1, &len);
  const tunables::Tunable* tunable = tunables::find(std::string_view(name, len));
  if (tunable == nullptr) luaL_error(L, "engine.tunable: no tunable named '%s'", name);
  switch (tunable->kind()) {
    case tunables::Kind::Int:
      lua_pushnumber(L, static_cast<f64>(static_cast<const tunables::Int*>(tunable)->get()));
      break;
    case tunables::Kind::Float:
      lua_pushnumber(L, static_cast<const tunables::Float*>(tunable)->get());
      break;
    case tunables::Kind::Bool:
      lua_pushboolean(L, static_cast<const tunables::Bool*>(tunable)->get() ? 1 : 0);
      break;
    case tunables::Kind::Enum: {
      const auto* e = static_cast<const tunables::EnumBase*>(tunable);
      lua_pushstring(L, e->choice_name(e->index()));
      break;
    }
  }
  return 1;
}

int api_tick(lua_State* L) {
  lua_pushnumber(L, static_cast<f64>(impl_of(L).tick));
  return 1;
}

int api_fixed_step(lua_State* L) {
  lua_pushnumber(L, impl_of(L).fixed_step);
  return 1;
}

// engine.random(seed, index) — the index-th number of the stream named by seed, in [0, 1). A pure
// function of its arguments, which is the point: a script gets randomness only from a seed its
// caller gave it, and the same seed and index give the same number on every machine and in
// every replay. There is no hidden state for a call order to disturb.
u64 checked_integer(lua_State* L, int arg, const char* what) {
  const f64 n = luaL_checknumber(L, arg);
  if (!(n >= 0.0 && n <= 9007199254740992.0) || static_cast<f64>(static_cast<u64>(n)) != n)
    luaL_error(L, "engine.random: %s must be a whole number in [0, 2^53]", what);
  return static_cast<u64>(n);
}

int api_random(lua_State* L) {
  const u64 seed = checked_integer(L, 1, "seed");
  const u64 index = checked_integer(L, 2, "index");
  const u64 bits = mix64(hash_combine(mix64(seed ^ 0x5cf1a3d9e7b24c61ull), index));
  lua_pushnumber(L, static_cast<f64>(bits >> 11) * (1.0 / 9007199254740992.0));
  return 1;
}

// engine.object(id) — the schema object the host's resolver knows by this id, visible for the
// rest of the call, or nil. The way a condition follows a reference (`quest.giver`) to the object
// it names.
int api_object(lua_State* L) {
  Impl& impl = impl_of(L);
  size_t len = 0;
  const char* text = luaL_checklstring(L, 1, &len);
  if (impl.resolver.resolve == nullptr)
    luaL_error(L, "engine.object: this host gives scripts no object lookup");
  Id128 id;
  if (!Id128::from_hex(std::string_view(text, len), id))
    luaL_error(L, "engine.object: '%s' is not an id (32 hex characters)", text);
  ObjectRef object;
  if (!impl.resolver.resolve(impl.resolver.user, id, object) || object.type == nullptr ||
      object.object == nullptr || object.type->kind != schema::Kind::Struct) {
    lua_pushnil(L);
    return 1;
  }
  const u32 slot = acquire_slot(impl, object, true);
  push_root_view(L, impl, slot);
  return 1;
}

void register_engine_api(lua_State* L) {
  lua_createtable(L, 0, 6);
  lua_pushcfunction(L, api_log, "engine.log");
  lua_setfield(L, -2, "log");
  lua_pushcfunction(L, api_tunable, "engine.tunable");
  lua_setfield(L, -2, "tunable");
  lua_pushcfunction(L, api_tick, "engine.tick");
  lua_setfield(L, -2, "tick");
  lua_pushcfunction(L, api_fixed_step, "engine.fixed_step");
  lua_setfield(L, -2, "fixed_step");
  lua_pushcfunction(L, api_random, "engine.random");
  lua_setfield(L, -2, "random");
  lua_pushcfunction(L, api_object, "engine.object");
  lua_setfield(L, -2, "object");
  lua_setglobal(L, "engine");
}

// ---- the sandbox -------------------------------------------------------------------------------
//
// Opened: base, coroutine, table, string, math, bit32, utf8, buffer, vector. Never opened: os
// (the wall clock, os.clock, os.date), debug (stack and upvalue inspection, the way out of every
// sandbox), and nothing that reaches a file exists in Luau's VM to begin with (no io, no require,
// no loadstring, no dofile). Removed after opening: print (stdout is not a channel; engine.log
// is), gcinfo (heap size is allocator-dependent), getfenv and setfenv (another function's
// environment), math.random and math.randomseed (seeded from an address, so different every run).
// Then luaL_sandbox freezes every library table, the string metatable and the global table, and
// each script runs with a global table of its own that reads through to the frozen one.

void open_library(lua_State* L, lua_CFunction open, const char* name) {
  lua_pushcfunction(L, open, nullptr);
  lua_pushstring(L, name);
  lua_call(L, 1, 0);
}

void open_sandboxed_libraries(lua_State* L) {
  open_library(L, luaopen_base, "");
  open_library(L, luaopen_coroutine, LUA_COLIBNAME);
  open_library(L, luaopen_table, LUA_TABLIBNAME);
  open_library(L, luaopen_string, LUA_STRLIBNAME);
  open_library(L, luaopen_math, LUA_MATHLIBNAME);
  open_library(L, luaopen_bit32, LUA_BITLIBNAME);
  open_library(L, luaopen_utf8, LUA_UTF8LIBNAME);
  open_library(L, luaopen_buffer, LUA_BUFFERLIBNAME);
  open_library(L, luaopen_vector, LUA_VECLIBNAME);

  for (const char* name : {"print", "gcinfo", "getfenv", "setfenv"}) {
    lua_pushnil(L);
    lua_setglobal(L, name);
  }
  lua_getglobal(L, LUA_MATHLIBNAME);
  for (const char* name : {"random", "randomseed"}) {
    lua_pushnil(L);
    lua_setfield(L, -2, name);
  }
  lua_pop(L, 1);
}

}  // namespace

// ---- calls -------------------------------------------------------------------------------------
//
// Shared with program.cpp through context_impl.h: a program runs under the same handler, the same
// budget accounting and the same error report as a call.

void begin_call(Impl& impl) noexcept {
  impl.depth = 1;
  impl.steps = 0;
  impl.step_limit = impl.limits.step_budget;
  impl.deadline_ns = impl.limits.time_budget_ns != 0
                         ? time::monotonic_ns() + static_cast<i64>(impl.limits.time_budget_ns)
                         : 0;
  impl.cause = Cause::None;
  impl.error_located = false;
  impl.error_line = 0;
}

void end_call(Impl& impl) noexcept {
  impl.depth = 0;
  impl.last_call_steps = impl.steps;
  ++impl.calls;
}

namespace {

// Strips the "chunk:line: " Luau puts at the front of a message raised from `chunk`, which
// last_error() already carries as fields.
std::string_view strip_position(std::string_view message, std::string_view chunk) noexcept {
  if (chunk.empty() || message.size() <= chunk.size() || message.substr(0, chunk.size()) != chunk ||
      message[chunk.size()] != ':')
    return message;
  usize i = chunk.size() + 1;
  const usize digits_start = i;
  while (i < message.size() && message[i] >= '0' && message[i] <= '9')
    ++i;
  if (i == digits_start || i + 1 >= message.size() || message[i] != ':' || message[i + 1] != ' ')
    return message;
  return message.substr(i + 2);
}

}  // namespace

// A failed protected call: the status from the cause the interrupt or the allocator recorded (a
// script cannot fake those by raising the same text), the position from the error handler, the
// message from the error object. The error object is on the top of the stack.
Status fail_call(Impl& impl, int code, std::string_view default_chunk) {
  lua_State* L = impl.L;
  Status status = Status::RuntimeError;
  switch (impl.cause) {
    case Cause::Budget:
    case Cause::Time: status = Status::BudgetExceeded; break;
    case Cause::Memory: status = Status::MemoryExceeded; break;
    case Cause::None: status = code == LUA_ERRMEM ? Status::MemoryExceeded : Status::RuntimeError;
  }
  size_t len = 0;
  const char* text = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &len) : nullptr;
  const std::string_view chunk =
      impl.error_located ? std::string_view(impl.error_chunk) : default_chunk;
  std::string_view message = text != nullptr ? std::string_view(text, len)
                                             : std::string_view("(error object is not a string)");
  message = strip_position(message, chunk);
  impl.error.status = status;
  impl.error.chunk.assign(chunk.data(), chunk.size());
  impl.error.line = impl.error_located ? impl.error_line : 0;
  impl.error.message.assign(message.data(), message.size());
  if (status == Status::MemoryExceeded) {
    // What the failed call built is garbage now; collect it before the next call starts near the
    // limit.
    lua_gc(L, LUA_GCCOLLECT, 0);
  }
  return status;
}

void set_error(Impl& impl, Status status, std::string_view chunk, u32 line,
               std::string_view message) {
  impl.error.status = status;
  impl.error.chunk.assign(chunk.data(), chunk.size());
  impl.error.line = line;
  impl.error.message.assign(message.data(), message.size());
}

Status compile_chunk(Impl& impl, std::string_view name, std::string_view source,
                     PrepareEnv prepare) {
  lua_CompileOptions options{};
  options.optimizationLevel = 1;  // no inlining: line numbers in errors stay the source's
  options.debugLevel = 1;         // line info and function names, for errors
  size_t size = 0;
  char* bytecode = luau_compile(source.data(), source.size(), &options, &size);
  if (bytecode == nullptr) {
    set_error(impl, Status::CompileError, name, 0, "the compiler produced nothing");
    return Status::CompileError;
  }
  if (size == 0 || bytecode[0] == 0) {
    // An encoded compile error: a zero byte, then ":<line>: <message>".
    std::string_view text =
        size > 1 ? std::string_view(bytecode + 1, size - 1) : std::string_view();
    u32 line = 0;
    if (!text.empty() && text[0] == ':') {
      usize i = 1;
      while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        line = line * 10 + static_cast<u32>(text[i] - '0');
        ++i;
      }
      if (i + 1 < text.size() && text[i] == ':' && text[i + 1] == ' ') text = text.substr(i + 2);
    }
    set_error(impl, Status::CompileError, name, line, text);
    std::free(bytecode);
    return Status::CompileError;
  }

  lua_State* L = impl.L;
  lua_State* thread = lua_newthread(L);  // L: [.., thread]
  luaL_sandboxthread(thread);            // its own global table, reading through to the frozen one
  if (prepare != nullptr) {
    // Before luau_load: the loader resolves a chunk's imports (`engine.doc.apply`) against the
    // environment as it is then, and caches what it finds.
    lua_pushvalue(thread, LUA_GLOBALSINDEX);
    prepare(thread, impl, lua_gettop(thread));
    lua_pop(thread, 1);
  }
  std::string chunkname;
  chunkname.reserve(name.size() + 1);
  chunkname.push_back('=');
  chunkname.append(name.data(), name.size());
  const int loaded = luau_load(thread, chunkname.c_str(), bytecode, size, 0);
  std::free(bytecode);
  if (loaded != 0) {
    size_t len = 0;
    const char* text = lua_tolstring(thread, -1, &len);
    set_error(impl, Status::CompileError, name, 0,
              text != nullptr ? std::string_view(text, len) : std::string_view("load failed"));
    lua_pop(L, 1);
    return Status::CompileError;
  }
  lua_pushvalue(thread, LUA_GLOBALSINDEX);  // thread: [fn, env]
  lua_xmove(thread, L, 2);                  // L: [.., thread, fn, env]
  lua_remove(L, -3);                        // L: [.., fn, env]; the closure keeps env alive
  lua_insert(L, -2);                        // L: [.., env, fn]
  return Status::Ok;
}

namespace {

// Compiles `source` and runs its top level in a fresh sandboxed global table, which is frozen
// afterwards and returned as a registry reference. Nothing is replaced here; the caller swaps.
Status compile_and_run(Impl& impl, std::string_view name, std::string_view source, int& env_ref) {
  if (const Status compiled = compile_chunk(impl, name, source); compiled != Status::Ok)
    return compiled;
  lua_State* L = impl.L;
  begin_call(impl);
  const int code = lua_pcall(L, 0, 0, k_handler_index);
  end_call(impl);
  if (code != 0) {
    const Status status = fail_call(impl, code, name);
    lua_settop(L, k_handler_index);
    return status;
  }
  // A script's globals are its entry points, defined by its top level. Freezing them turns the
  // classic Lua bug — assigning a global from inside a function — into an error at the line that
  // does it, and keeps one call from leaving state in the table another call reads.
  lua_setreadonly(L, -1, true);
  env_ref = lua_ref(L, -1);
  lua_settop(L, k_handler_index);
  return Status::Ok;
}

bool push_arg(Impl& impl, const Arg& arg) {
  lua_State* L = impl.L;
  switch (arg.kind()) {
    case Arg::Kind::Nil: lua_pushnil(L); return true;
    case Arg::Kind::Boolean: lua_pushboolean(L, arg.as_boolean() ? 1 : 0); return true;
    case Arg::Kind::Number: lua_pushnumber(L, arg.as_number()); return true;
    case Arg::Kind::String: {
      const std::string_view s = arg.as_string();
      lua_pushlstring(L, s.data(), s.size());
      return true;
    }
    case Arg::Kind::Object: {
      const ObjectRef object = arg.as_object();
      if (object.type == nullptr || object.object == nullptr ||
          object.type->kind != schema::Kind::Struct) {
        set_error(impl, Status::InvalidArgument, {}, 0,
                  "call: an object argument is not a schema struct");
        return false;
      }
      const u32 slot = acquire_slot(impl, object, true);
      push_root_view(L, impl, slot);
      return true;
    }
    case Arg::Kind::Binding: {
      const BindingId binding = arg.as_binding();
      if (!binding.valid() || binding.slot >= impl.slots.size() || !impl.slots[binding.slot].live ||
          impl.slots[binding.slot].serial != binding.serial || impl.slots[binding.slot].temporary) {
        set_error(impl, Status::InvalidArgument, {}, 0, "call: a binding argument is not bound");
        return false;
      }
      lua_getref(L, impl.slots[binding.slot].view_ref);
      return true;
    }
    case Arg::Kind::Json: push_json(L, arg.as_json()); return true;
  }
  lua_pushnil(L);
  return true;
}

// Resolves a function slot against its script's current global table.
void resolve_function(Impl& impl, FunctionSlot& fn) {
  lua_State* L = impl.L;
  if (fn.ref != LUA_NOREF) {
    lua_unref(L, fn.ref);
    fn.ref = LUA_NOREF;
  }
  const Script& script = impl.scripts[fn.script];
  if (script.env_ref == LUA_NOREF) return;
  lua_getref(L, script.env_ref);
  lua_rawgetfield(L, -1, fn.name.c_str());
  if (lua_isfunction(L, -1)) fn.ref = lua_ref(L, -1);
  lua_pop(L, 2);
}

}  // namespace

// ---- small public types ------------------------------------------------------------------------

const char* status_name(Status status) noexcept {
  switch (status) {
    case Status::Ok: return "ok";
    case Status::CompileError: return "compile_error";
    case Status::RuntimeError: return "runtime_error";
    case Status::BudgetExceeded: return "budget_exceeded";
    case Status::MemoryExceeded: return "memory_exceeded";
    case Status::NotFound: return "not_found";
    case Status::InvalidArgument: return "invalid_argument";
    case Status::IoError: return "io_error";
  }
  return "?";
}

ScriptLimits ScriptLimits::from_tunables() noexcept {
  ScriptLimits limits;
  limits.memory_bytes = static_cast<u64>(tunable_memory_limit_mb.get()) << 20;
  limits.step_budget = static_cast<u64>(tunable_step_budget.get());
  limits.time_budget_ns = static_cast<u64>(tunable_time_budget_us.get()) * 1000u;
  return limits;
}

Arg Arg::boolean(bool value) noexcept {
  Arg a;
  a.kind_ = Kind::Boolean;
  a.boolean_ = value;
  return a;
}

Arg Arg::number(f64 value) noexcept {
  Arg a;
  a.kind_ = Kind::Number;
  a.number_ = value;
  return a;
}

Arg Arg::string(std::string_view value) noexcept {
  Arg a;
  a.kind_ = Kind::String;
  a.string_ = Str{value.data(), value.size()};
  return a;
}

Arg Arg::object(ObjectRef value) noexcept {
  Arg a;
  a.kind_ = Kind::Object;
  a.object_ = value;
  return a;
}

Arg Arg::binding(BindingId value) noexcept {
  Arg a;
  a.kind_ = Kind::Binding;
  a.binding_ = value;
  return a;
}

Arg Arg::json(const JsonValue& value) noexcept {
  Arg a;
  a.kind_ = Kind::Json;
  a.json_ = &value;
  return a;
}

// ---- ScriptContext -----------------------------------------------------------------------------

ScriptContext::ScriptContext(const ScriptLimits& limits) : impl_(std::make_unique<Impl>()) {
  Impl& impl = *impl_;
  impl.limits = limits;
  impl.tag = scripting_tag();
  build_atoms(impl);

  impl.L = lua_newstate(allocate, &impl);
  ENGINE_VERIFY(impl.L != nullptr, "scripting: could not create a Luau state");
  lua_State* L = impl.L;
  lua_Callbacks* callbacks = lua_callbacks(L);
  callbacks->userdata = &impl;
  callbacks->interrupt = interrupt;
  callbacks->panic = panic;
  callbacks->useratom = useratom;

  open_sandboxed_libraries(L);
  register_views(L);
  register_engine_api(L);
  luaL_sandbox(L);

  lua_pushcfunction(L, error_handler, "error_handler");
  ENGINE_VERIFY(lua_gettop(L) == k_handler_index, "scripting: the handler must be stack slot 1");

  // The budget errors are made once so that raising one allocates nothing.
  std::string message =
      "script exceeded its step budget (" + std::to_string(limits.step_budget) + " steps per call)";
  lua_pushlstring(L, message.data(), message.size());
  impl.budget_message_ref = lua_ref(L, -1);
  lua_pop(L, 1);
  message = "script exceeded its time budget (" + std::to_string(limits.time_budget_ns / 1000u) +
            " microseconds per call)";
  lua_pushlstring(L, message.data(), message.size());
  impl.time_message_ref = lua_ref(L, -1);
  lua_pop(L, 1);
}

ScriptContext::~ScriptContext() {
  if (impl_ != nullptr && impl_->L != nullptr) {
    impl_->gc_pauses = nullptr;
    lua_close(impl_->L);
    impl_->L = nullptr;
  }
}

const ScriptLimits& ScriptContext::limits() const noexcept { return impl_->limits; }

Status ScriptContext::load(std::string_view name, std::string_view source, ScriptId& out) {
  Impl& impl = *impl_;
  out = ScriptId{};
  if (impl.depth != 0) {
    set_error(impl, Status::InvalidArgument, name, 0, "load: called from inside a script call");
    return Status::InvalidArgument;
  }
  int env_ref = LUA_NOREF;
  const Status status = compile_and_run(impl, name, source, env_ref);
  if (status != Status::Ok) return status;
  Script script;
  script.name.assign(name.data(), name.size());
  script.env_ref = env_ref;
  script.generation = 1;
  script.content_hash = hash_bytes(source.data(), source.size());
  out = ScriptId{impl.scripts.size()};
  impl.scripts.push_back(std::move(script));
  return Status::Ok;
}

Status ScriptContext::load_file(std::string_view path, ScriptId& out) {
  Impl& impl = *impl_;
  out = ScriptId{};
  io::FileInfo info;
  std::string text;
  if (io::stat_file(path, info) != io::Status::Ok || io::read_file(path, text) != io::Status::Ok) {
    set_error(impl, Status::IoError, path, 0, "the script file could not be read");
    return Status::IoError;
  }
  const Status status = load(path, text, out);
  if (status != Status::Ok) return status;
  Script& script = impl.scripts[out.index];
  script.path.assign(path.data(), path.size());
  script.mtime_ms = info.modified_unix_ms;
  script.file_size = info.size;
  return Status::Ok;
}

Status ScriptContext::reload(ScriptId id, std::string_view source) {
  Impl& impl = *impl_;
  if (!id.valid() || id.index >= impl.scripts.size()) {
    set_error(impl, Status::NotFound, {}, 0, "reload: no such script");
    return Status::NotFound;
  }
  if (impl.depth != 0) {
    set_error(impl, Status::InvalidArgument, impl.scripts[id.index].name, 0,
              "reload: called from inside a script call");
    return Status::InvalidArgument;
  }
  int env_ref = LUA_NOREF;
  const Status status = compile_and_run(impl, impl.scripts[id.index].name, source, env_ref);
  if (status != Status::Ok) return status;  // the last good chunk stays
  Script& script = impl.scripts[id.index];
  if (script.env_ref != LUA_NOREF) lua_unref(impl.L, script.env_ref);
  script.env_ref = env_ref;
  script.content_hash = hash_bytes(source.data(), source.size());
  ++script.generation;
  for (FunctionSlot& fn : impl.functions) {
    if (fn.script == id.index) resolve_function(impl, fn);
  }
  return Status::Ok;
}

u32 ScriptContext::poll_changes(Vector<ScriptId>* reloaded) {
  Impl& impl = *impl_;
  u32 count = 0;
  std::string text;
  for (u32 i = 0; i < impl.scripts.size(); ++i) {
    if (impl.scripts[i].path.empty()) continue;
    io::FileInfo info;
    // A file that is missing for a moment (an editor saving by rename) keeps the loaded chunk.
    if (io::stat_file(impl.scripts[i].path, info) != io::Status::Ok) continue;
    if (info.modified_unix_ms == impl.scripts[i].mtime_ms && info.size == impl.scripts[i].file_size)
      continue;
    // Remember this version before trying it, so a broken save is reported once rather than on
    // every poll until the next save.
    impl.scripts[i].mtime_ms = info.modified_unix_ms;
    impl.scripts[i].file_size = info.size;
    if (io::read_file(impl.scripts[i].path, text) != io::Status::Ok) continue;
    if (hash_bytes(text.data(), text.size()) == impl.scripts[i].content_hash) continue;
    const Status status = reload(ScriptId{i}, text);
    if (status == Status::Ok) {
      ++count;
      if (reloaded != nullptr) reloaded->push_back(ScriptId{i});
      ENGINE_LOG_INFO(log_scripting, "script reloaded", log::field("script", impl.scripts[i].name),
                      log::field("generation", impl.scripts[i].generation));
    } else {
      ENGINE_LOG_WARN(log_scripting, "script reload failed; the previous version stays",
                      log::field("script", impl.error.chunk), log::field("line", impl.error.line),
                      log::field("status", status_name(status)),
                      log::field("message", impl.error.message));
    }
  }
  return count;
}

u32 ScriptContext::generation(ScriptId id) const noexcept {
  const Impl& impl = *impl_;
  return id.valid() && id.index < impl.scripts.size() ? impl.scripts[id.index].generation : 0;
}

Status ScriptContext::find_function(ScriptId id, std::string_view name, FunctionId& out) {
  Impl& impl = *impl_;
  out = FunctionId{};
  if (!id.valid() || id.index >= impl.scripts.size()) {
    set_error(impl, Status::NotFound, {}, 0, "find_function: no such script");
    return Status::NotFound;
  }
  for (u32 i = 0; i < impl.functions.size(); ++i) {
    const FunctionSlot& fn = impl.functions[i];
    if (fn.script == id.index && fn.name == name && fn.ref != LUA_NOREF) {
      out = FunctionId{i};
      return Status::Ok;
    }
  }
  FunctionSlot fn;
  fn.script = id.index;
  fn.name.assign(name.data(), name.size());
  resolve_function(impl, fn);
  if (fn.ref == LUA_NOREF) {
    set_error(impl, Status::NotFound, impl.scripts[id.index].name, 0,
              "the script defines no global function '" + fn.name + "'");
    return Status::NotFound;
  }
  out = FunctionId{impl.functions.size()};
  impl.functions.push_back(std::move(fn));
  return Status::Ok;
}

Status ScriptContext::call(FunctionId function, std::span<const Arg> args, JsonValue* result) {
  Impl& impl = *impl_;
  lua_State* L = impl.L;
  if (!function.valid() || function.index >= impl.functions.size()) {
    set_error(impl, Status::NotFound, {}, 0, "call: no such function");
    return Status::NotFound;
  }
  const FunctionSlot& fn = impl.functions[function.index];
  const std::string_view chunk = impl.scripts[fn.script].name;
  if (fn.ref == LUA_NOREF) {
    set_error(impl, Status::NotFound, chunk, 0,
              "the script no longer defines '" + fn.name + "' (it was reloaded without it)");
    return Status::NotFound;
  }
  if (impl.depth != 0) {
    set_error(impl, Status::InvalidArgument, chunk, 0,
              "call: re-entered from inside a script call");
    return Status::InvalidArgument;
  }
  if (!lua_checkstack(L, static_cast<int>(args.size()) + 4)) {
    set_error(impl, Status::InvalidArgument, chunk, 0, "call: too many arguments");
    return Status::InvalidArgument;
  }

  lua_getref(L, fn.ref);
  for (const Arg& arg : args) {
    if (!push_arg(impl, arg)) {
      impl.error.chunk.assign(chunk.data(), chunk.size());
      lua_settop(L, k_handler_index);
      release_temporaries(impl);
      return Status::InvalidArgument;
    }
  }

  begin_call(impl);
  const int code =
      lua_pcall(L, static_cast<int>(args.size()), result != nullptr ? 1 : 0, k_handler_index);
  end_call(impl);

  Status status = Status::Ok;
  if (code != 0) {
    status = fail_call(impl, code, chunk);
  } else if (result != nullptr) {
    // Converted before the call's temporary bindings are released, so a script may return one of
    // the objects it was given.
    std::string problem;
    if (!to_json(L, impl, -1, *result, problem)) {
      set_error(impl, Status::InvalidArgument, chunk, 0, problem);
      status = Status::InvalidArgument;
    }
  }
  lua_settop(L, k_handler_index);
  release_temporaries(impl);
  return status;
}

Status ScriptContext::call(ScriptId script, std::string_view function, std::span<const Arg> args,
                           JsonValue* result) {
  FunctionId id;
  const Status status = find_function(script, function, id);
  if (status != Status::Ok) return status;
  return call(id, args, result);
}

void ScriptContext::set_resolver(ObjectResolver resolver) noexcept { impl_->resolver = resolver; }

void ScriptContext::set_time(u64 tick, f64 fixed_step) noexcept {
  impl_->tick = tick;
  impl_->fixed_step = fixed_step;
}

const ScriptError& ScriptContext::last_error() const noexcept { return impl_->error; }

ContextStats ScriptContext::stats() const noexcept {
  const Impl& impl = *impl_;
  ContextStats stats;
  stats.heap_bytes = impl.heap;
  stats.heap_peak_bytes = impl.heap_peak;
  stats.live_bytes = static_cast<u64>(lua_gc(impl.L, LUA_GCCOUNT, 0)) * 1024u +
                     static_cast<u64>(lua_gc(impl.L, LUA_GCCOUNTB, 0));
  stats.allocated_bytes = impl.allocated;
  stats.calls = impl.calls;
  stats.last_call_steps = impl.last_call_steps;
  stats.scripts = impl.scripts.size();
  u32 bindings = 0;
  for (const Slot& slot : impl.slots)
    bindings += slot.live && !slot.temporary ? 1u : 0u;
  stats.bindings = bindings;
  return stats;
}

void ScriptContext::collect_garbage() noexcept { lua_gc(impl_->L, LUA_GCCOLLECT, 0); }

void ScriptContext::record_gc_pauses(Vector<u64>* pauses_ns) noexcept {
  impl_->gc_pauses = pauses_ns;
  impl_->in_gc_step = false;
}

}  // namespace engine::scripting
