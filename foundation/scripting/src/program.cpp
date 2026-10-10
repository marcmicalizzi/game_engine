// Programs (ScriptContext::run_program; docs/subsystems/scripting.md, "Programs: the console"): a
// script a host runs once, top to bottom, which drives the host through the methods it offers.
// engine-host's console is the host this exists for: the methods are the protocol's, and a call
// goes through the dispatcher exactly as a JSON-RPC request would.
//
// Every function here that a script calls keeps context_impl.h's rule: nothing with a destructor is
// alive where it can raise. The C++ half of a call — reading the params table, the host's method,
// keeping its answer — happens in invoke(), a frame that has returned before anything is pushed or
// raised, and the values it hands across live in Impl::program.

#include "context_impl.h"

#include <core/json/json.h>

#include <string>
#include <string_view>

namespace engine::scripting {
namespace {

// While the host's half of a call runs, the VM's memory limit does not apply, as for any host-side
// work between calls (script_context.cpp, the allocator): the conversion may grow the Luau stack,
// and a refused growth would raise through a frame that holds strings and JSON. What the program
// itself allocates — the result's tables, its own data — is under the limit as before.
class HostSide {
 public:
  explicit HostSide(Impl& impl) noexcept : impl_(impl), depth_(impl.depth) { impl_.depth = 0; }
  ~HostSide() { impl_.depth = depth_; }
  ENGINE_NON_COPYABLE(HostSide);

 private:
  Impl& impl_;
  u32 depth_;
};

enum class Outcome : u8 { Ok, HostError, NoJson };

const HostMethod* find_method(const ProgramHost& host, std::string_view name) noexcept {
  for (const HostMethod& method : host.methods) {
    if (method.name == name) return &method;
  }
  return nullptr;
}

// The C++ half of one call. Never raises.
Outcome invoke(lua_State* L, Impl& impl, std::string_view name, const schema::TypeInfo* type,
               int params_index) {
  HostSide host_side(impl);
  ProgramState& p = impl.program;
  p.method.assign(name.data(), name.size());
  p.params = JsonValue();
  p.result = JsonValue();
  p.error = HostError{};
  if (lua_type(L, params_index) == LUA_TTABLE) {
    std::string problem;
    if (!to_json_typed(L, impl, params_index, type, p.params, problem)) {
      p.text = p.method + ": params: " + problem;
      return Outcome::NoJson;
    }
  }
  if (!p.host->call(p.host->user, p.method, p.params, p.result, p.error)) return Outcome::HostError;
  return Outcome::Ok;
}

// Raises the failed call's error as a table, `{method, code, message, data}`, whose metatable
// gives it a string form. A table rather than a string so that a script that catches it with
// pcall reads the code and the data without parsing a message.
void raise_host_error(lua_State* L, Impl& impl) {
  const ProgramState& p = impl.program;
  luaL_checkstack(L, 3, "no stack for an error");
  lua_createtable(L, 0, 4);
  lua_pushlstring(L, p.method.data(), p.method.size());
  lua_rawsetfield(L, -2, "method");
  lua_pushnumber(L, static_cast<f64>(p.error.code));
  lua_rawsetfield(L, -2, "code");
  lua_pushlstring(L, p.error.message.data(), p.error.message.size());
  lua_rawsetfield(L, -2, "message");
  push_json(L, p.error.data, /*read_only=*/false);
  lua_rawsetfield(L, -2, "data");
  lua_getref(L, p.error_meta_ref);
  lua_setmetatable(L, -2);
  lua_error(L);
}

int host_call(lua_State* L, Impl& impl, std::string_view name, const schema::TypeInfo* type,
              int params_index) {
  const int kind = lua_type(L, params_index);
  if (kind != LUA_TNIL && kind != LUA_TNONE && kind != LUA_TTABLE)
    luaL_typeerror(L, params_index, "table or nil");
  switch (invoke(L, impl, name, type, params_index)) {
    case Outcome::Ok: push_json(L, impl.program.result, /*read_only=*/false); return 1;
    case Outcome::HostError: raise_host_error(L, impl); break;
    case Outcome::NoJson: luaL_error(L, "%s", impl.program.text.c_str());
  }
  return 0;
}

// engine.call(method, params) — any method by name, the ones the host did not list included: the
// host answers those itself (the console's dispatcher says "unknown method").
int api_call(lua_State* L) {
  Impl& impl = impl_of(L);
  size_t len = 0;
  const char* name = luaL_checklstring(L, 1, &len);
  const std::string_view method(name, len);
  const HostMethod* known = find_method(*impl.program.host, method);
  return host_call(L, impl, method, known != nullptr ? known->params : nullptr, 2);
}

// engine.doc.apply(params) and every other generated function: upvalue 1 is the method's index.
int api_method(lua_State* L) {
  Impl& impl = impl_of(L);
  const usize index = static_cast<usize>(lua_tointeger(L, lua_upvalueindex(1)));
  const HostMethod& method = impl.program.host->methods[index];
  return host_call(L, impl, method.name, method.params, 1);
}

bool json_text(lua_State* L, Impl& impl, int index, bool pretty) {
  HostSide host_side(impl);
  JsonValue value;
  std::string problem;
  if (!to_json(L, impl, index, value, problem)) {
    impl.program.text = "engine.json: " + problem;
    return false;
  }
  impl.program.text = write_json(value, JsonWriteOptions{.pretty = pretty});
  return true;
}

// engine.json(value [, pretty]) — the value's JSON text, the way a result is printed: `print`
// shows a table as its address.
int api_json(lua_State* L) {
  Impl& impl = impl_of(L);
  luaL_checkany(L, 1);
  const bool pretty = lua_toboolean(L, 2) != 0;
  if (!json_text(L, impl, 1, pretty)) luaL_error(L, "%s", impl.program.text.c_str());
  lua_pushlstring(L, impl.program.text.data(), impl.program.text.size());
  return 1;
}

// print(...) — the program's own output: each argument through tostring, tab-separated, and a
// newline, handed to the host whole. Built on the Luau stack, so a __tostring that prints too
// cannot clobber a line half made.
int api_print(lua_State* L) {
  Impl& impl = impl_of(L);
  const int n = lua_gettop(L);
  luaL_checkstack(L, 2 * n + 2, "too many arguments to print");
  for (int i = 1; i <= n; ++i) {
    luaL_tolstring(L, i, nullptr);
    if (i < n) lua_pushliteral(L, "\t");
  }
  lua_pushliteral(L, "\n");
  lua_concat(L, n == 0 ? 1 : 2 * n);
  size_t len = 0;
  const char* text = lua_tolstring(L, -1, &len);
  if (impl.program.host->print != nullptr)
    impl.program.host->print(impl.program.host->user, std::string_view(text, len));
  return 0;
}

// The error table's string form: "doc.apply: error 1008: ...".
int error_tostring(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_rawgetfield(L, 1, "method");
  lua_rawgetfield(L, 1, "code");
  lua_rawgetfield(L, 1, "message");
  const char* method = lua_tostring(L, -3);
  const char* message = lua_tostring(L, -1);
  lua_pushfstring(L, "%s: error %d: %s", method != nullptr ? method : "?",
                  static_cast<int>(lua_tonumber(L, -2)), message != nullptr ? message : "");
  return 1;
}

// Puts `engine` and `print` into the program's own global table (stack index `env`). Host-side
// setup before the run, like the context's own engine API: it raises only on a real
// out-of-memory, which panics.
void install_api(lua_State* L, Impl& impl, int env) {
  ProgramState& p = impl.program;
  const ProgramHost& host = *p.host;

  lua_createtable(L, 0, 1);
  lua_pushcfunction(L, error_tostring, "__tostring");
  lua_rawsetfield(L, -2, "__tostring");
  lua_setreadonly(L, -1, true);
  p.error_meta_ref = lua_ref(L, -1);
  lua_pop(L, 1);

  lua_createtable(L, 0, 16);
  const int engine = lua_gettop(L);
  lua_pushcfunction(L, api_call, "engine.call");
  lua_rawsetfield(L, engine, "call");
  lua_pushcfunction(L, api_json, "engine.json");
  lua_rawsetfield(L, engine, "json");

  // One function per method, in nested tables named by the dots: "doc.apply" is engine.doc.apply,
  // "engine.ping" is engine.ping. A name whose place is taken — by `call`, `json`, a method of the
  // same name or a namespace — is left to engine.call rather than replacing what is there.
  p.names.clear();
  p.names.reserve(static_cast<u32>(host.methods.size()));
  for (const HostMethod& method : host.methods)
    p.names.push_back(std::string(method.name));
  for (u32 i = 0; i < p.names.size(); ++i) {
    std::string_view rest = p.names[i];
    if (rest.starts_with("engine.")) rest.remove_prefix(7);
    lua_pushvalue(L, engine);
    bool placed = !rest.empty();
    for (usize dot = rest.find('.'); placed && dot != std::string_view::npos;
         dot = rest.find('.')) {
      const std::string_view segment = rest.substr(0, dot);
      lua_pushlstring(L, segment.data(), segment.size());
      lua_rawget(L, -2);
      if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_createtable(L, 0, 8);
        lua_pushlstring(L, segment.data(), segment.size());
        lua_pushvalue(L, -2);
        lua_rawset(L, -4);
      } else if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        placed = false;
        break;
      }
      lua_remove(L, -2);
      rest.remove_prefix(dot + 1);
    }
    if (placed && !rest.empty()) {
      lua_pushlstring(L, rest.data(), rest.size());
      lua_rawget(L, -2);
      const bool taken = !lua_isnil(L, -1);
      lua_pop(L, 1);
      if (!taken) {
        lua_pushlstring(L, rest.data(), rest.size());
        lua_pushinteger(L, static_cast<int>(i));
        lua_pushcclosure(L, api_method, p.names[i].c_str(), 1);
        lua_rawset(L, -3);
      }
    }
    lua_pop(L, 1);
  }
  lua_rawsetfield(L, env, "engine");
  lua_pushcfunction(L, api_print, "print");
  lua_rawsetfield(L, env, "print");
}

// After a failed run, with the error object on the top of the stack: a table says more than
// fail_call's "(error object is not a string)". A method's error is put in its own words with its
// data as JSON; any other table a script raised is its JSON. Host-side, after the call.
void describe_error_object(lua_State* L, Impl& impl) {
  if (lua_type(L, -1) != LUA_TTABLE) return;
  bool from_host = false;
  if (lua_getmetatable(L, -1) != 0) {
    lua_getref(L, impl.program.error_meta_ref);
    from_host = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
  }
  std::string problem;
  if (from_host) {
    lua_rawgetfield(L, -1, "method");
    lua_rawgetfield(L, -2, "code");
    lua_rawgetfield(L, -3, "message");
    lua_rawgetfield(L, -4, "data");
    const char* method = lua_tostring(L, -4);
    const char* message = lua_tostring(L, -2);
    std::string text = method != nullptr ? method : "?";
    text += ": error ";
    text += std::to_string(static_cast<i64>(lua_tonumber(L, -3)));
    text += ": ";
    text += message != nullptr ? message : "";
    JsonValue data;
    if (!lua_isnil(L, -1) && to_json(L, impl, -1, data, problem)) {
      text.push_back(' ');
      text += write_json(data, JsonWriteOptions{.pretty = false});
    }
    lua_pop(L, 4);
    impl.error.message = std::move(text);
    return;
  }
  JsonValue value;
  if (to_json(L, impl, -1, value, problem))
    impl.error.message = "error " + write_json(value, JsonWriteOptions{.pretty = false});
}

}  // namespace

Status ScriptContext::run_program(std::string_view name, std::string_view source,
                                  const ProgramHost& host, std::span<const std::string_view> args) {
  Impl& impl = *impl_;
  if (impl.depth != 0) {
    set_error(impl, Status::InvalidArgument, name, 0,
              "run_program: called from inside a script call");
    return Status::InvalidArgument;
  }
  if (host.call == nullptr) {
    set_error(impl, Status::InvalidArgument, name, 0, "run_program: the host has no call function");
    return Status::InvalidArgument;
  }
  lua_State* L = impl.L;
  impl.program.host = &host;
  // The API goes in before the chunk loads, so its imports resolve to it (compile_chunk).
  if (const Status compiled = compile_chunk(impl, name, source, &install_api);
      compiled != Status::Ok) {
    if (impl.program.error_meta_ref != LUA_NOREF) lua_unref(L, impl.program.error_meta_ref);
    impl.program = ProgramState{};
    return compiled;
  }
  // L: [handler, env, fn]
  if (lua_checkstack(L, static_cast<int>(args.size()) + 1) == 0) {
    set_error(impl, Status::InvalidArgument, name, 0, "run_program: too many arguments");
    lua_settop(L, k_handler_index);
    lua_unref(L, impl.program.error_meta_ref);
    impl.program = ProgramState{};
    return Status::InvalidArgument;
  }
  for (const std::string_view arg : args)
    lua_pushlstring(L, arg.data(), arg.size());

  begin_call(impl);
  const int code = lua_pcall(L, static_cast<int>(args.size()), 0, k_handler_index);
  end_call(impl);
  Status status = Status::Ok;
  if (code != 0) {
    status = fail_call(impl, code, name);
    describe_error_object(L, impl);
  }
  lua_settop(L, k_handler_index);
  lua_unref(L, impl.program.error_meta_ref);
  impl.program = ProgramState{};
  lua_gc(L, LUA_GCCOLLECT, 0);
  return status;
}

}  // namespace engine::scripting
