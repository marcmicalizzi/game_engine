#pragma once

// engine-mcp: the Model Context Protocol bridge of docs/plan/06-agent-tooling.md §6.3
// (docs/subsystems/apps.md, "engine-mcp").
//
// MCP is the cognitive interface and the engine protocol the mechanical one. This process speaks
// MCP on its own stdio and the engine protocol to one engine-host it spawns, exactly as engine-cli
// does; everything in between is here: the curated tools, their JSON Schema (generated at startup
// from the host's own `schema.describe`, so no field list is copied by hand), attribution rules,
// pagination, workspace files for bulk results, and error messages that say what to do next.
//
// It is a separate process rather than a second front end on the dispatcher because the methods
// it needs are not all in one library: `render.*` is registered by engine-host over
// systems/renderer, and the only thing that has every method is the host. Being a client also
// keeps the plan's shape — the editor, the CLI and this bridge are equal clients of one server —
// and one host per bridge keeps session ownership simple: every session and scene id the agent
// sees was made by this bridge's host, and they all go when it does.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/flat_map.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/platform/process.h>

#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace engine::mcp {

// The deadlines' defaults (docs/subsystems/apps.md, "Deadlines"): a quick call is bookkeeping over
// a document or the host's own state, a long one does work that grows with the content or the
// request — a render, a content build, a headless run, the validators.
inline constexpr u64 k_default_call_timeout_ms = 120u * 1000u;
inline constexpr u64 k_default_long_call_timeout_ms = 3600u * 1000u;

struct BridgeOptions {
  std::string host_path;
  Vector<std::string> mounts;
  // Appended to engine-host's command line as given (`--host-arg`, repeatable): `--log` or
  // `--tunables` for the host, or the test hook `--debug-hang`.
  Vector<std::string> host_args;
  // Absolute, forward slashes, no trailing slash. Created on the first bulk write.
  std::string workspace;
  // The attribution a mutation carries when the caller leaves a field out: the actor always has
  // one, the role and the task only when the bridge was started with them.
  std::string actor;
  std::string role;
  std::string task;
  // A roles file for the host (`--roles`, absolute), or empty. With one, `role` also names the
  // role configuration every call runs in: the host is started with `--roles`, `--actor`, `--role`
  // and `--task`, the tools the role may not call are not offered, and a call that names another
  // role is refused (docs/subsystems/apps.md, "Roles, leases and proposals").
  std::string roles;
  // How long one protocol call may go unanswered before the host is taken for hung, in
  // milliseconds; 0 waits for as long as it takes. `long_call_timeout_ms` is for the methods
  // `is_long_call` names.
  u64 call_timeout_ms = k_default_call_timeout_ms;
  u64 long_call_timeout_ms = k_default_long_call_timeout_ms;
};

// Whether a protocol method is in the long class: render.load, render.capture, render.benchmark,
// render.evaluate, render.compare, content.build, session.run_headless, engine.run_tests, and
// session.save_game and session.load_game, which copy a world's store and document.
bool is_long_call(std::string_view method) noexcept;
// The deadline a call to `method` gets under these options, in milliseconds; 0 is none.
u64 call_deadline_ms(const BridgeOptions& options, std::string_view method) noexcept;
// Seconds as a reader writes them: "120", "0.5".
std::string seconds_text(u64 milliseconds);

// ---- the host --------------------------------------------------------------------------------

// One engine-host behind the bridge, spoken to one JSON-RPC line at a time. A host that has died
// is noticed on the call that finds its pipes closed — its stdin refuses the write, or its stdout
// ends before the answer. A host that is alive and never answers is noticed by the call's
// **deadline**: every read of an answer waits at most the deadline of the method's class, and a
// host that stays silent past it is killed and reaped, so the next `start` makes a new one. The
// read itself happens on a reader thread of the host's own (host_client.cpp says why a thread).
class HostClient {
 public:
  // `timed_out`: the host did not answer within the deadline and was killed; `error.message` names
  // the method and the seconds waited.
  enum class Status : u8 { ok, error, gone, timed_out };
  struct Error {
    i32 code = 0;
    std::string message;
    JsonValue data;
    // For `timed_out`: the deadline that ran out, in milliseconds.
    u64 deadline_ms = 0;
  };

  HostClient();
  ~HostClient();
  ENGINE_NON_COPYABLE(HostClient);

  bool start(const BridgeOptions& options, std::string& error);
  // `gone` fills `error.message` with what happened to the host and `error.code` with its exit
  // code; `timed_out` says which call it failed to answer and in how long. Either way the process
  // is reaped, and the next `start` makes a new one.
  Status call(std::string_view method, const JsonValue& params, JsonValue& result, Error& error);
  bool running() const noexcept { return process_ != nullptr; }
  // Closes the host's stdin and waits for it to exit, which a host does when its input ends; one
  // that is still there after the short deadline is killed.
  void stop();
  // Incremented by every start: a scene id from an earlier generation names nothing.
  u32 generation() const noexcept { return generation_; }

 private:
  struct Reader;
  enum class Next : u8 { line, end, timeout };

  // The next line the host wrote, waiting until `due` (null: for as long as it takes).
  Next next_line(std::string& line, const std::chrono::steady_clock::time_point* due);
  void reap(Error& error, std::string_view what);
  // Kills the host after a deadline ran out and reaps it.
  void abandon(Error& error, std::string_view method, u64 deadline_ms);
  // Joins the reader, cancelling its read first on Windows (host_client.cpp).
  void join_reader() noexcept;

  std::unique_ptr<platform::Process> process_;
  std::unique_ptr<Reader> reader_;
  u64 call_timeout_ms_ = k_default_call_timeout_ms;
  u64 long_call_timeout_ms_ = k_default_long_call_timeout_ms;
  u64 next_id_ = 1;
  u32 generation_ = 0;
};

// ---- JSON Schema from the host's schema descriptions ------------------------------------------

// The method catalogue and the schema types, as the host describes them, turned into JSON Schema.
// A type is described once and cached; the host is the one source of truth, so a field added to
// a params struct reaches the tool's input schema by rebuilding the host, not by editing here.
class SchemaGen {
 public:
  struct Method {
    std::string name;
    std::string doc;
    std::string params_type;
    std::string result_type;
  };

  explicit SchemaGen(HostClient& host) : host_(host) {}

  bool load_methods(std::string& error);
  const Method* method(std::string_view name) const noexcept;
  std::span<const Method> methods() const noexcept { return {methods_.data(), methods_.size()}; }

  // schema.describe's own answer for a qualified type, cached.
  bool describe(std::string_view qualified, JsonValue& out, std::string& error);
  // JSON Schema of a type as schema.describe spells it: "u32", "engine.doc.Command[]",
  // "engine.protocol.RenderCamera?", "map<string, json>".
  bool type_schema(std::string_view type, JsonValue& out, std::string& error);
  // The object schema of a method's params type; an empty object schema for a method with none.
  bool params_schema(std::string_view method, JsonValue& out, std::string& error);

 private:
  bool type_schema_at(std::string_view type, u32 depth, JsonValue& out, std::string& error);

  HostClient& host_;
  Vector<Method> methods_;
  FlatMap<std::string, JsonValue> described_;
};

// Moves every struct type that appears more than once, unchanged, in one tool's input schema into
// that schema's $defs, and puts {"$ref": "#/$defs/<qualified type>"} where each one stood,
// keeping the field's own description, default and deprecation beside the reference; then removes
// the type markers the generator left. Each inputSchema is a document of its own, so a definition
// is shared only inside one tool: RenderSettings, 10.7 KB with its documentation, stood twice in
// each of capture, benchmark and evaluate (`load.settings` and the per-call `settings`), and the
// second copies were a fifth of tools/list (docs/subsystems/apps.md, "Input schemas are generated,
// not written").
void share_definitions(JsonValue& schema);

// ---- tools -------------------------------------------------------------------------------------

// What a tool call produced: a short summary for a reader, the data as an object (the MCP
// `structuredContent`, and a compact JSON text block for clients that read only text), and links
// to files the call wrote.
struct ToolOutcome {
  struct Link {
    std::string uri;
    std::string name;
    std::string mime;
  };
  bool error = false;
  std::string summary;
  JsonValue data = JsonValue::object();
  Vector<Link> links;
};

class Bridge;

using ToolSchema = bool (*)(Bridge& bridge, JsonValue& schema, std::string& error);
using ToolRun = void (*)(Bridge& bridge, const JsonValue& args, ToolOutcome& out);

struct ToolDef {
  const char* name;
  const char* title;
  const char* description;
  // The protocol methods the tool calls, space-separated. A tool whose methods the host does not
  // serve is not offered, rather than offered and failing.
  const char* methods;
  bool read_only;
  bool destructive;
  bool idempotent;
  ToolSchema schema;
  ToolRun run;
};

std::span<const ToolDef> tool_table() noexcept;

// ---- the bridge --------------------------------------------------------------------------------

class Bridge {
 public:
  explicit Bridge(BridgeOptions options);
  ~Bridge();
  ENGINE_NON_COPYABLE(Bridge);

  // Starts the host and builds every tool's input schema. False with a reason when either fails.
  bool start(std::string& error);
  // One MCP message in; the response to write, or null when nothing is to be sent.
  JsonValue handle_text(std::string_view line);
  JsonValue handle(const JsonValue& message);
  void shutdown();

  // ---- for the tools ----

  const BridgeOptions& options() const noexcept { return options_; }
  SchemaGen& schemas() noexcept { return schemas_; }
  HostClient& host() noexcept { return host_; }

  // Calls the host, starting a new one first when the last one died. On failure `out` is the
  // error result — the code, the message, the diagnostics and a hint — and the call returns false.
  bool call(std::string_view method, const JsonValue& params, JsonValue& result, ToolOutcome& out);

  // A scene the render tools work on: `scene` as given, or `load` loaded now — or reused, when
  // this host already loaded exactly that. `info` is the load's answer when there was one.
  bool scene_for(const JsonValue& args, std::string& scene, JsonValue& info, bool& reused,
                 ToolOutcome& out);
  // The host no longer holds `scene` (the unload tool released it): a later `load` of the same
  // thing loads it again instead of reusing a dead id.
  void forget_scene(std::string_view scene);
  // The `load` a scene of this host was loaded with through the render tools, or null.
  const JsonValue* load_of(std::string_view scene) const noexcept;

  // The protocol version agreed at initialize: what the result is allowed to contain.
  std::string_view protocol_version() const noexcept { return protocol_version_; }
  bool structured_results() const noexcept;

  // The host restricts calls by a role configuration (engine.roles said so at startup), and the
  // role every call of this bridge runs in; empty when a call that names none is not restricted.
  bool roles_loaded() const noexcept { return roles_loaded_; }
  const std::string& role_name() const noexcept { return role_name_; }
  // Tools left out because the role may not call one of their methods.
  u32 tools_withheld() const noexcept { return tools_withheld_; }

 private:
  struct Tool {
    const ToolDef* def;
    JsonValue schema;
  };
  // A tool of the table this bridge does not offer, and why: kept so that a call to it by name is
  // told the reason (the role, or a host built without the method) rather than "unknown tool".
  struct Withheld {
    const ToolDef* def;
    std::string method;
    bool by_role;
  };
  struct LoadedScene {
    std::string key;
    std::string id;
    JsonValue info;
    JsonValue load;
  };

  JsonValue on_initialize(const JsonValue* params);
  JsonValue on_tools_list() const;
  bool on_tools_call(const JsonValue* params, JsonValue& result, i32& code, std::string& message);
  JsonValue tool_result(const ToolOutcome& outcome) const;
  const Tool* find_tool(std::string_view name) const noexcept;
  // The JSON-RPC error message for a tools/call naming a tool this bridge does not offer.
  std::string unoffered_message(std::string_view name) const;

  BridgeOptions options_;
  HostClient host_;
  SchemaGen schemas_;
  Vector<Tool> tools_;
  Vector<Withheld> withheld_;
  Vector<LoadedScene> scenes_;
  u32 scenes_generation_ = 0;
  std::string protocol_version_;
  bool roles_loaded_ = false;
  std::string role_name_;
  Vector<std::string> role_methods_;  // every method the role may call, reads included
  u32 tools_withheld_ = 0;
};

// ---- shared helpers
// ------------------------------------------------------------------------------

// Members of a JSON object, with a fallback when absent or of another kind.
std::string text_of(const JsonValue& object, std::string_view key, std::string_view fallback = {});
u64 uint_of(const JsonValue& object, std::string_view key, u64 fallback = 0);
f64 real_of(const JsonValue& object, std::string_view key, f64 fallback = 0.0);
bool bool_of(const JsonValue& object, std::string_view key, bool fallback = false);

// A `file://` URI of a native path, and back.
std::string file_uri(std::string_view path);
std::string path_of_uri(std::string_view uri);

// The protocol's error codes by name, and a sentence about what to do about one.
const char* code_name(i32 code) noexcept;
std::string hint_for(std::string_view method, i32 code, std::string_view message);

// Formats a failed protocol call as a tool result.
void fail_from_host(std::string_view method, const HostClient::Status status,
                    const HostClient::Error& error, ToolOutcome& out);
// A tool result for something the bridge itself refused, with the hint on its own line.
void fail(ToolOutcome& out, std::string_view message, std::string_view hint);

}  // namespace engine::mcp
