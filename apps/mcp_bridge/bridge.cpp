// The MCP side of engine-mcp: initialize and the version handshake, ping, tools/list, tools/call,
// and the conversion of what the host answered into what an agent reads (docs/subsystems/apps.md,
// "engine-mcp").
#include "bridge.h"

#include <core/json/json.h>
#include <core/log/log.h>

#include <cstdio>

#ifndef ENGINE_VERSION
#define ENGINE_VERSION "0.0.0"
#endif

namespace engine::mcp {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_mcp, "mcp");

// Newest first. A client asking for one of these gets it back; a client asking for anything else
// gets the newest, and decides for itself whether it can speak it (MCP's lifecycle rule). Nothing
// the bridge uses changed shape between them: what differs is what a result may carry, which
// `structured_results` answers.
constexpr const char* k_versions[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

// JSON-RPC's own codes, for the errors that are about the message rather than about a tool.
constexpr i32 k_parse_error = -32700;
constexpr i32 k_invalid_request = -32600;
constexpr i32 k_method_not_found = -32601;
constexpr i32 k_invalid_params = -32602;

constexpr const char* k_instructions =
    "Tools over one engine-host: a headless game engine whose world is a layered document of "
    "typed objects on disk. Start with open_session (create: true makes a new document), then "
    "read with layers, objects, get and journal, and edit with create_object, set_property, "
    "reparent, delete_object or apply. Every edit is one undoable transaction and needs "
    "attribution.rationale, one sentence saying why. undo and redo step the journal; diff and "
    "merge_layers work on layers. list_schema and describe explain the object types and every "
    "protocol method. capture, benchmark, compare and evaluate render offscreen: they write "
    "their pictures and full results into the workspace directory and return file:// URIs to "
    "read with your own tools, never the bytes. The host keeps every scene a render tool loads "
    "until unload releases it: scenes lists them, and in a long session unload the ones you are "
    "done with. A render tool on a machine without a usable GPU answers 'no GPU on this machine' "
    "and the document tools keep working. get_logs reads the host's log.";

JsonValue rpc_error(const JsonValue& id, i32 code, std::string message, JsonValue data = {}) {
  JsonValue error = JsonValue::object();
  error.set("code", JsonValue(code));
  error.set("message", JsonValue(message));
  if (!data.is_null()) error.set("data", std::move(data));
  JsonValue response = JsonValue::object();
  response.set("jsonrpc", JsonValue("2.0"));
  response.set("id", id);
  response.set("error", std::move(error));
  return response;
}

JsonValue rpc_result(const JsonValue& id, JsonValue result) {
  JsonValue response = JsonValue::object();
  response.set("jsonrpc", JsonValue("2.0"));
  response.set("id", id);
  response.set("result", std::move(result));
  return response;
}

bool version_at_least(std::string_view version, std::string_view floor) {
  // The versions are dates, so they order as strings.
  return version >= floor;
}

}  // namespace

// ---- shared helpers
// ------------------------------------------------------------------------------

std::string text_of(const JsonValue& object, std::string_view key, std::string_view fallback) {
  const JsonValue* v = object.is_object() ? object.find(key) : nullptr;
  std::string_view s;
  return v != nullptr && v->get_string(s) ? std::string(s) : std::string(fallback);
}

u64 uint_of(const JsonValue& object, std::string_view key, u64 fallback) {
  const JsonValue* v = object.is_object() ? object.find(key) : nullptr;
  u64 out = 0;
  return v != nullptr && v->get_u64(out) ? out : fallback;
}

f64 real_of(const JsonValue& object, std::string_view key, f64 fallback) {
  const JsonValue* v = object.is_object() ? object.find(key) : nullptr;
  f64 out = 0.0;
  return v != nullptr && v->get_f64(out) ? out : fallback;
}

bool bool_of(const JsonValue& object, std::string_view key, bool fallback) {
  const JsonValue* v = object.is_object() ? object.find(key) : nullptr;
  bool out = false;
  return v != nullptr && v->get_bool(out) ? out : fallback;
}

// The same encoding engine-host's render methods write (apps/engine_host/render_methods.cpp):
// backslashes become slashes, a drive path gets the third slash, and the characters a URI cannot
// carry raw are percent-encoded. Repeated here because the bridge is a client and links no host
// code; the two agree because the end-to-end suites read both back with one decoder.
std::string file_uri(std::string_view path) {
  std::string out = path.starts_with("/") ? "file://" : "file:///";
  for (const char c : path) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (c == '\\') {
      out.push_back('/');
    } else if (u <= 0x20 || u >= 0x7f || c == '"' || c == '#' || c == '%' || c == '<' || c == '>' ||
               c == '?' || c == '{' || c == '}' || c == '|' || c == '^' || c == '`') {
      char escape[4] = {};
      std::snprintf(escape, sizeof(escape), "%%%02X", u);
      out += escape;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string path_of_uri(std::string_view uri) {
  if (!uri.starts_with("file://")) return std::string(uri);
  std::string_view rest = uri.substr(7);
  // file:///C:/x is a drive path; file:///home/x keeps its slash; file://host/x is not supported
  // and is left for the host to refuse.
  if (rest.size() > 3 && rest[0] == '/' && rest[2] == ':') rest.remove_prefix(1);
  std::string out;
  out.reserve(rest.size());
  const auto hex = [](char c) -> i32 {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (usize i = 0; i < rest.size(); ++i) {
    if (rest[i] == '%' && i + 2 < rest.size() && hex(rest[i + 1]) >= 0 && hex(rest[i + 2]) >= 0) {
      out.push_back(static_cast<char>(hex(rest[i + 1]) * 16 + hex(rest[i + 2])));
      i += 2;
    } else {
      out.push_back(rest[i]);
    }
  }
  return out;
}

const char* code_name(i32 code) noexcept {
  switch (code) {
    case -32700: return "ParseError";
    case -32600: return "InvalidRequest";
    case -32601: return "MethodNotFound";
    case -32602: return "InvalidParams";
    case -32603: return "InternalError";
    case 1000: return "SessionNotFound";
    case 1001: return "DocumentError";
    case 1002: return "ValidationFailed";
    case 1003: return "NotFound";
    case 1004: return "InvalidArgument";
    case 1005: return "IoError";
    case 1006: return "Unavailable";
    case 1007: return "RenderUnavailable";
    case 1008: return "Forbidden";
    case 1009: return "LeaseConflict";
    case 1010: return "LeaseRequired";
    default: return "Error";
  }
}

std::string hint_for(std::string_view method, i32 code, std::string_view message) {
  switch (code) {
    case -32602:
      return "An argument does not match the parameter type; the diagnostics name it. describe "
             "with {\"method\": \"" +
             std::string(method) + "\"} lists every field the method takes.";
    case -32601:
      return "This engine-host does not serve " + std::string(method) +
             "; it is older than the bridge. Rebuild both from the same tree.";
    case -32603: return "The host failed internally; get_logs shows what it logged just before.";
    case 1000:
      return "Session ids live as long as the host. list_sessions shows the open ones; "
             "open_session opens the document again (everything committed is on disk).";
    case 1001:
      return "The document on disk could not be read or written as asked; the message says "
             "which part. validate lists every problem in an open document.";
    case 1002: return "Fix what the diagnostics list, then try again.";
    case 1003:
      if (method == "session.open")
        return "Nothing is there yet: pass \"create\": true to make a new document at that path.";
      if (message.find("scene") != std::string_view::npos)
        return "Scene ids live until unload releases them or the host exits; scenes lists the "
               "loaded ones. Pass `load` instead of `scene` to load it again.";
      if (message.find("layer") != std::string_view::npos)
        return "layers lists the document's layer names.";
      return "Check the id or name: objects lists ids and layers lists layer names.";
    case 1004: return "Change the argument the message names and call again.";
    case 1005:
      return "A file could not be read or written: check the path, and that its directory "
             "exists and is writable.";
    case 1006: return "This host does not offer that service.";
    case 1007:
      return "There is no GPU on this machine that can render this. If the message names a "
             "missing ray-tracing extension, load again with settings {\"shadows\":\"off\"} and "
             "a raster mode other than \"rt\"; otherwise render tools cannot run here, and the "
             "document tools still work.";
    case 1008:
      if (message.find("needs review") != std::string_view::npos)
        return "This role's work is reviewed: write to a proposal layer of your own "
               "(propose_layer), and a director promotes it.";
      if (message.find("proposal") != std::string_view::npos)
        return "A proposal layer is written only by its owner while it is open; propose_layer "
               "opens one of your own.";
      return "The bridge's role configuration does not allow this; host_info names the role. "
             "Reading is never restricted.";
    case 1009:
      return "Another agent's lease covers that; leases lists who holds what and until when. "
             "Work on tiles nobody holds, or wait for the lease to be released or to expire.";
    case 1010:
      return "This document requires a lease before an edit: acquire_lease the tiles (or the "
             "object types) on the layer the message names, then call again.";
    default: return "get_logs may say more about what the host was doing.";
  }
}

void fail(ToolOutcome& out, std::string_view message, std::string_view hint) {
  out.error = true;
  out.summary = std::string(message);
  if (!hint.empty()) {
    out.summary += "\nHint: ";
    out.summary += hint;
  }
  JsonValue error = JsonValue::object();
  error.set("message", JsonValue(message));
  if (!hint.empty()) error.set("hint", JsonValue(hint));
  out.data = JsonValue::object();
  out.data.set("error", std::move(error));
}

void fail_from_host(std::string_view method, const HostClient::Status status,
                    const HostClient::Error& error, ToolOutcome& out) {
  out.error = true;
  out.links.clear();
  JsonValue e = JsonValue::object();
  e.set("method", JsonValue(method));
  if (status == HostClient::Status::timed_out) {
    // The same shape as a death — the host is gone and the next call starts another — with the
    // deadline named, because "it hung" and "it was slower than the deadline" look alike from
    // here and only the caller knows which one a render of that size is.
    const std::string hint =
        "The host was alive but gave no answer within the deadline of " + std::string(method) +
        "'s class, so the bridge stopped it; the next call starts a new engine-host. Open "
        "sessions and loaded scenes did not survive: call open_session again (every change the "
        "host confirmed is on disk) and pass `load` again to the render tools. If the call was a "
        "render or a build that needs longer, start engine-mcp with a larger " +
        std::string(is_long_call(method) ? "--long-call-timeout" : "--call-timeout") +
        " (0 is no deadline).";
    out.summary = error.message + "\nHint: " + hint;
    e.set("timed_out", JsonValue(true));
    e.set("seconds", JsonValue(static_cast<f64>(error.deadline_ms) / 1000.0));
    e.set("message", JsonValue(error.message));
    e.set("hint", JsonValue(hint));
  } else if (status == HostClient::Status::gone) {
    const std::string hint =
        "The bridge starts a new engine-host on the next call. Open sessions and loaded scenes "
        "did not survive: call open_session again (every change the host confirmed is on disk; "
        "one it was working on when it died may or may not be), and pass `load` again to the "
        "render tools.";
    out.summary = error.message + "\nHint: " + hint;
    e.set("host_exited", JsonValue(true));
    e.set("exit_code", JsonValue(error.code));
    e.set("message", JsonValue(error.message));
    e.set("hint", JsonValue(hint));
  } else {
    const std::string hint = hint_for(method, error.code, error.message);
    std::string text;
    if (error.code == 1007) {
      // The one error a whole class of machine answers, spelled so a reader cannot mistake it
      // for a mistake in the request.
      text = "No GPU on this machine can do this: " + std::string(method) +
             " answered error 1007 " + "RenderUnavailable: " + error.message;
    } else {
      text = std::string(method) + " failed: error " + std::to_string(error.code) + " " +
             code_name(error.code) + ": " + error.message;
    }
    // Diagnostics are {path, message} pairs for invalid params; anything else is shown as JSON.
    if (error.data.is_array()) {
      for (usize i = 0; i < error.data.size(); ++i) {
        const JsonValue& d = error.data[i];
        const std::string path = text_of(d, "path");
        text += "\n  " + (path.empty() ? std::string("(params)") : path) + ": " +
                text_of(d, "message", write_json(d, JsonWriteOptions{.pretty = false}));
      }
    } else if (!error.data.is_null()) {
      text += "\n  " + write_json(error.data, JsonWriteOptions{.pretty = false});
    }
    out.summary = text + "\nHint: " + hint;
    e.set("code", JsonValue(error.code));
    e.set("name", JsonValue(code_name(error.code)));
    e.set("message", JsonValue(error.message));
    if (!error.data.is_null()) e.set("data", error.data);
    e.set("hint", JsonValue(hint));
  }
  out.data = JsonValue::object();
  out.data.set("error", std::move(e));
}

// ---- the bridge
// ------------------------------------------------------------------------------------

Bridge::Bridge(BridgeOptions options) : options_(std::move(options)), schemas_(host_) {
  protocol_version_ = k_versions[0];
}

Bridge::~Bridge() { shutdown(); }

bool Bridge::start(std::string& error) {
  if (!host_.start(options_, error)) return false;
  if (!schemas_.load_methods(error)) return false;
  // The role configuration (plan 06 §6.5): when the host restricts calls, the role this bridge's
  // calls run in — `--role`, or the file's default for a bridge started without one — decides
  // which tools are offered at all. The host checks every call anyway; offering a tool the role
  // may not use would only teach a model to make calls that fail.
  if (schemas_.method("engine.roles") != nullptr) {
    JsonValue roles;
    HostClient::Error failure;
    if (host_.call("engine.roles", JsonValue(), roles, failure) != HostClient::Status::ok) {
      error = "engine.roles failed: " + failure.message;
      return false;
    }
    roles_loaded_ = bool_of(roles, "loaded");
    if (roles_loaded_) {
      role_name_ = options_.role.empty() ? text_of(roles, "default_role") : options_.role;
      const JsonValue* list = roles.find("roles");
      for (usize i = 0; list != nullptr && i < list->size(); ++i) {
        const JsonValue& r = (*list)[i];
        const JsonValue* role = r.find("role");
        if (role == nullptr || text_of(*role, "name") != role_name_) continue;
        const JsonValue* allowed = r.find("allowed_methods");
        for (usize k = 0; allowed != nullptr && k < allowed->size(); ++k)
          role_methods_.push_back(std::string((*allowed)[k].as_string()));
      }
    }
  }
  const auto allowed = [&](std::string_view method) {
    if (!roles_loaded_ || role_name_.empty()) return true;
    for (const std::string& m : role_methods_) {
      if (m == method) return true;
    }
    return false;
  };
  for (const ToolDef& def : tool_table()) {
    // Offered only when the host serves every method the tool calls, and the role may call them.
    std::string_view needs = def.methods;
    std::string missing;
    std::string forbidden;
    while (!needs.empty()) {
      const usize space = needs.find(' ');
      const std::string_view name = needs.substr(0, space);
      if (!name.empty() && schemas_.method(name) == nullptr) missing = std::string(name);
      if (!name.empty() && !allowed(name)) forbidden = std::string(name);
      if (space == std::string_view::npos) break;
      needs.remove_prefix(space + 1);
    }
    if (!missing.empty()) {
      ENGINE_LOG_WARN(log_mcp, "tool not offered: the host has no such method",
                      log::field("tool", def.name), log::field("method", missing));
      withheld_.push_back(Withheld{&def, missing, false});
      continue;
    }
    if (!forbidden.empty()) {
      ENGINE_LOG_INFO(log_mcp, "tool not offered: the role may not call its method",
                      log::field("tool", def.name), log::field("method", forbidden),
                      log::field("role", role_name_));
      ++tools_withheld_;
      withheld_.push_back(Withheld{&def, forbidden, true});
      continue;
    }
    JsonValue schema;
    std::string why;
    if (!def.schema(*this, schema, why)) {
      error = std::string("cannot build the input schema of ") + def.name + ": " + why;
      return false;
    }
    share_definitions(schema);
    tools_.push_back(Tool{&def, std::move(schema)});
  }
  ENGINE_LOG_INFO(log_mcp, "engine-mcp ready", log::field("tools", static_cast<u64>(tools_.size())),
                  log::field("workspace", options_.workspace));
  return true;
}

void Bridge::shutdown() { host_.stop(); }

bool Bridge::structured_results() const noexcept {
  return version_at_least(protocol_version_, "2025-06-18");
}

bool Bridge::call(std::string_view method, const JsonValue& params, JsonValue& result,
                  ToolOutcome& out) {
  if (!host_.running()) {
    // The last host died and a call reported it; this one starts the next.
    std::string error;
    if (!host_.start(options_, error)) {
      fail(out, error, "Check that engine-host is beside engine-mcp or named by --host.");
      return false;
    }
  }
  HostClient::Error error;
  const HostClient::Status status = host_.call(method, params, result, error);
  if (status == HostClient::Status::ok) return true;
  fail_from_host(method, status, error, out);
  return false;
}

bool Bridge::scene_for(const JsonValue& args, std::string& scene, JsonValue& info, bool& reused,
                       ToolOutcome& out) {
  reused = false;
  info = JsonValue();
  const JsonValue* load = args.find("load");
  const bool has_scene = !text_of(args, "scene").empty();
  if (has_scene && load != nullptr) {
    fail(out, "give `scene` or `load`, not both",
         "`scene` renders what an earlier call loaded; `load` loads (or reuses) a scene now.");
    return false;
  }
  if (host_.generation() != scenes_generation_) {
    // A new host: every scene id the old one handed out names nothing now.
    scenes_.clear();
    scenes_generation_ = host_.generation();
  }
  if (has_scene) {
    scene = text_of(args, "scene");
    return true;
  }
  if (load == nullptr || !load->is_object()) {
    fail(out, "nothing to render: neither `scene` nor `load` was given",
         "Pass `load`, e.g. {\"procedural\":\"heightfield\"}, {\"mesh\":\"path/to/model.gltf\"} "
         "or {\"scene\":\"path/to/scene.json\"}, or the `scene` id an earlier render tool "
         "returned.");
    return false;
  }
  // One load per distinct request per host: the host keeps every scene it loads until it exits,
  // so loading the same mesh again for every capture would cost the import and the memory twice.
  const std::string key = write_json(*load, JsonWriteOptions{.pretty = false});
  for (const LoadedScene& s : scenes_) {
    if (s.key == key && host_.generation() == scenes_generation_ && host_.running()) {
      scene = s.id;
      info = s.info;
      reused = true;
      return true;
    }
  }
  JsonValue result;
  if (!call("render.load", *load, result, out)) return false;
  if (host_.generation() != scenes_generation_) {
    scenes_.clear();
    scenes_generation_ = host_.generation();
  }
  scene = text_of(result, "scene");
  info = result;
  scenes_.push_back(LoadedScene{key, scene, result, *load});
  return true;
}

void Bridge::forget_scene(std::string_view scene) {
  for (u32 i = 0; i < scenes_.size(); ++i) {
    if (scenes_[i].id == scene) {
      scenes_.erase_at(i);
      return;
    }
  }
}

const JsonValue* Bridge::load_of(std::string_view scene) const noexcept {
  if (host_.generation() != scenes_generation_) return nullptr;
  for (const LoadedScene& s : scenes_) {
    if (s.id == scene) return &s.load;
  }
  return nullptr;
}

const Bridge::Tool* Bridge::find_tool(std::string_view name) const noexcept {
  for (const Tool& t : tools_) {
    if (name == t.def->name) return &t;
  }
  return nullptr;
}

JsonValue Bridge::handle_text(std::string_view line) {
  JsonValue message;
  const JsonParseResult parsed = parse_json(line, message);
  if (!parsed.ok) {
    return rpc_error(JsonValue(), k_parse_error,
                     std::string("Parse error: ") + parsed.message + " at line " +
                         std::to_string(parsed.line) + ", column " + std::to_string(parsed.column));
  }
  return handle(message);
}

JsonValue Bridge::handle(const JsonValue& message) {
  if (message.is_array()) {
    return rpc_error(JsonValue(), k_invalid_request,
                     "batches are not supported: send one JSON-RPC message per line");
  }
  if (!message.is_object()) {
    return rpc_error(JsonValue(), k_invalid_request, "a message must be a JSON object");
  }
  const JsonValue* id = message.find("id");
  const JsonValue* method = message.find("method");
  if (method == nullptr || !method->is_string()) {
    // A response to a request the bridge never sends, or nothing at all.
    if (id != nullptr && (message.contains("result") || message.contains("error"))) {
      return JsonValue();
    }
    return rpc_error(id != nullptr ? *id : JsonValue(), k_invalid_request,
                     "a request needs a string `method`");
  }
  if (text_of(message, "jsonrpc") != "2.0") {
    return rpc_error(id != nullptr ? *id : JsonValue(), k_invalid_request,
                     "`jsonrpc` must be \"2.0\"");
  }
  const std::string name(method->as_string());
  const JsonValue* params = message.find("params");
  if (id == nullptr) {
    // Notifications: initialized, cancelled (a call is answered before the next line is read, so
    // there is never anything in flight to cancel), roots and anything newer. None is answered.
    ENGINE_LOG_DEBUG(log_mcp, "notification", log::field("method", name));
    return JsonValue();
  }
  if (name == "initialize") return rpc_result(*id, on_initialize(params));
  if (name == "ping") return rpc_result(*id, JsonValue::object());
  if (name == "tools/list") return rpc_result(*id, on_tools_list());
  if (name == "tools/call") {
    JsonValue result;
    i32 code = 0;
    std::string why;
    if (!on_tools_call(params, result, code, why)) {
      JsonValue data;
      if (code == k_invalid_params) {
        JsonValue names = JsonValue::array();
        for (const Tool& t : tools_)
          names.push_back(JsonValue(t.def->name));
        data = JsonValue::object();
        data.set("tools", std::move(names));
      }
      return rpc_error(*id, code, why, std::move(data));
    }
    return rpc_result(*id, std::move(result));
  }
  return rpc_error(*id, k_method_not_found,
                   "method not found: " + name +
                       " (this server offers initialize, ping, tools/list and tools/call)");
}

JsonValue Bridge::on_initialize(const JsonValue* params) {
  const std::string asked = params != nullptr ? text_of(*params, "protocolVersion") : "";
  protocol_version_ = k_versions[0];
  for (const char* v : k_versions) {
    if (asked == v) protocol_version_ = v;
  }
  const std::string client = params != nullptr && params->find("clientInfo") != nullptr
                                 ? text_of(*params->find("clientInfo"), "name", "unknown")
                                 : std::string("unknown");
  ENGINE_LOG_INFO(log_mcp, "initialize", log::field("client", client), log::field("asked", asked),
                  log::field("version", protocol_version_));

  JsonValue tools = JsonValue::object();
  tools.set("listChanged", JsonValue(false));
  JsonValue capabilities = JsonValue::object();
  capabilities.set("tools", std::move(tools));

  JsonValue server = JsonValue::object();
  server.set("name", JsonValue("engine-mcp"));
  server.set("version", JsonValue(ENGINE_VERSION));
  if (version_at_least(protocol_version_, "2025-06-18")) {
    server.set("title", JsonValue("Game engine (engine-host over MCP)"));
  }

  JsonValue result = JsonValue::object();
  result.set("protocolVersion", JsonValue(protocol_version_));
  result.set("capabilities", std::move(capabilities));
  result.set("serverInfo", std::move(server));
  if (version_at_least(protocol_version_, "2025-03-26")) {
    result.set("instructions", JsonValue(k_instructions));
  }
  return result;
}

JsonValue Bridge::on_tools_list() const {
  const bool annotations = version_at_least(protocol_version_, "2025-03-26");
  const bool titles = version_at_least(protocol_version_, "2025-06-18");
  JsonValue list = JsonValue::array();
  for (const Tool& t : tools_) {
    JsonValue tool = JsonValue::object();
    tool.set("name", JsonValue(t.def->name));
    if (titles) tool.set("title", JsonValue(t.def->title));
    tool.set("description", JsonValue(t.def->description));
    tool.set("inputSchema", t.schema);
    if (annotations) {
      JsonValue a = JsonValue::object();
      a.set("title", JsonValue(t.def->title));
      a.set("readOnlyHint", JsonValue(t.def->read_only));
      if (!t.def->read_only) {
        a.set("destructiveHint", JsonValue(t.def->destructive));
        a.set("idempotentHint", JsonValue(t.def->idempotent));
      }
      // Everything a tool touches is this engine and its files: a closed world.
      a.set("openWorldHint", JsonValue(false));
      tool.set("annotations", std::move(a));
    }
    list.push_back(std::move(tool));
  }
  JsonValue result = JsonValue::object();
  result.set("tools", std::move(list));
  return result;
}

// A model that calls a tool by a name it remembers — from another session, another role, the
// documentation — is told why this bridge does not have it. "Unknown tool" for create_object under
// a QA role reads as a typo or a broken server; the role is the answer, and it is not one a call
// can change (docs/subsystems/apps.md, "Roles, leases and proposals").
std::string Bridge::unoffered_message(std::string_view name) const {
  const std::string offered =
      "tools/list names the " + std::to_string(tools_.size()) + " tools this bridge offers";
  for (const Withheld& w : withheld_) {
    if (name != w.def->name) continue;
    if (w.by_role) {
      return "tool '" + std::string(name) + "' is not offered to role '" + role_name_ +
             "': it calls " + w.method +
             ", which that role may not call. The role is chosen when engine-mcp is started "
             "(--role), not by a call; " +
             offered;
    }
    return "tool '" + std::string(name) + "' is not offered: this engine-host does not serve " +
           w.method + " (a build without it); " + offered;
  }
  return "unknown tool '" + std::string(name) + "': " + offered;
}

bool Bridge::on_tools_call(const JsonValue* params, JsonValue& result, i32& code,
                           std::string& message) {
  if (params == nullptr || !params->is_object()) {
    code = k_invalid_params;
    message = "tools/call needs params {name, arguments}";
    return false;
  }
  const std::string name = text_of(*params, "name");
  const Tool* tool = find_tool(name);
  if (tool == nullptr) {
    code = k_invalid_params;
    message = unoffered_message(name);
    return false;
  }
  JsonValue args = JsonValue::object();
  if (const JsonValue* a = params->find("arguments"); a != nullptr && !a->is_null()) {
    if (!a->is_object()) {
      code = k_invalid_params;
      message = "tools/call arguments must be a JSON object";
      return false;
    }
    args = *a;
  }

  ToolOutcome outcome;
  // The top level of every call is checked against the generated schema here, so a misspelt or
  // missing argument is named with the list of the right ones, rather than coming back from the
  // host as a field of a struct the caller never saw.
  const JsonValue* properties = tool->schema.find("properties");
  std::string problem;
  for (usize i = 0; i < args.as_object().size() && problem.empty(); ++i) {
    const std::string& key = args.as_object().key_at(static_cast<u32>(i));
    if (properties == nullptr || properties->find(key) == nullptr) {
      problem = "unknown argument '" + key + "' for " + name;
    }
  }
  if (const JsonValue* required = tool->schema.find("required");
      problem.empty() && required != nullptr && required->is_array()) {
    for (usize i = 0; i < required->size(); ++i) {
      std::string_view r;
      if ((*required)[i].get_string(r) && args.find(r) == nullptr) {
        problem = "missing required argument '" + std::string(r) + "' for " + name;
        break;
      }
    }
  }
  if (!problem.empty()) {
    std::string takes;
    if (properties != nullptr) {
      for (usize i = 0; i < properties->as_object().size(); ++i) {
        if (!takes.empty()) takes += ", ";
        takes += properties->as_object().key_at(static_cast<u32>(i));
      }
    }
    fail(outcome, problem,
         name + " takes: " + takes + ". Its input schema in tools/list describes each one.");
  } else {
    ENGINE_LOG_DEBUG(log_mcp, "tool call", log::field("tool", name));
    tool->def->run(*this, args, outcome);
  }
  result = tool_result(outcome);
  return true;
}

JsonValue Bridge::tool_result(const ToolOutcome& outcome) const {
  JsonValue content = JsonValue::array();
  const auto text_block = [](std::string text) {
    JsonValue block = JsonValue::object();
    block.set("type", JsonValue("text"));
    block.set("text", JsonValue(text));
    return block;
  };
  content.push_back(text_block(outcome.summary));
  const bool has_data = outcome.data.is_object() && outcome.data.size() > 0;
  // The data again as text: MCP asks a tool that returns structured content to serialize it into
  // a text block too, and a client that reads only text needs it anyway. An error's data is
  // already in its summary.
  if (has_data && !outcome.error) {
    content.push_back(text_block(write_json(outcome.data, JsonWriteOptions{.pretty = false})));
  }
  if (structured_results()) {
    for (const ToolOutcome::Link& link : outcome.links) {
      JsonValue block = JsonValue::object();
      block.set("type", JsonValue("resource_link"));
      block.set("uri", JsonValue(link.uri));
      block.set("name", JsonValue(link.name));
      if (!link.mime.empty()) block.set("mimeType", JsonValue(link.mime));
      content.push_back(std::move(block));
    }
  }
  JsonValue result = JsonValue::object();
  result.set("content", std::move(content));
  result.set("isError", JsonValue(outcome.error));
  if (structured_results() && has_data) result.set("structuredContent", outcome.data);
  return result;
}

}  // namespace engine::mcp
