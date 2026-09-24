// End to end: engine-mcp driven the way an MCP client drives it — one JSON-RPC message per line
// on its stdin, one answer per line on its stdout — over the engine-host it spawns
// (docs/subsystems/apps.md, "engine-mcp"; plan 06 §6.3).
//
// Every case gets its own workspace and documents inside a TempDir. The render cases need no
// fixture: they load the procedural heightfield. Where a case renders, it accepts either answer a
// machine can honestly give — a picture, or "no GPU on this machine" — and says which it got; the
// no-GPU answer itself is pinned on every machine by hiding the Vulkan drivers from the host.
#include <core/json/json.h>
#include <core/platform/process.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#if ENGINE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#endif

using namespace engine;

namespace {

const char* k_type = "engine.content.AssetProvenance";
const char* k_b = "00000000000000100000000000000002";
const char* k_c = "00000000000000100000000000000003";

// The built executables, or the bundle's copies of them (tests/support/test_paths.h).
const std::string& bridge_exe() {
  static const std::string path = test::app_path(ENGINE_APP_PATH);
  return path;
}
const std::string& host_exe() {
  static const std::string path = test::app_path(ENGINE_HOST_PATH);
  return path;
}

const JsonValue& at(const JsonValue& o, std::string_view key) {
  const JsonValue* v = o.find(key);
  REQUIRE_MESSAGE(v != nullptr, "no '" << key << "' in " << write_json(o));
  return *v;
}

std::string str(const JsonValue& o, std::string_view key) {
  const JsonValue* v = o.find(key);
  std::string_view s;
  return v != nullptr && v->get_string(s) ? std::string(s) : std::string();
}

u64 num(const JsonValue& o, std::string_view key) {
  const JsonValue* v = o.find(key);
  u64 n = 0;
  return v != nullptr && v->get_u64(n) ? n : ~u64{0};
}

// One engine-mcp process, spoken to as an MCP client speaks to it.
struct Mcp {
  platform::Process process;
  i64 next_id = 1;
  bool ok = false;

  explicit Mcp(const std::string& workspace, std::vector<std::string> extra = {}) {
    std::vector<std::string> args = {bridge_exe(), "--host",  host_exe(), "--workspace",
                                     workspace,    "--actor", "mcp-test"};
    for (std::string& e : extra)
      args.push_back(std::move(e));
    std::vector<std::string_view> argv(args.begin(), args.end());
    std::string error;
    if (!process.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error)) {
      FAIL("cannot spawn engine-mcp: " << error);
      return;
    }
    ok = true;
  }
  ~Mcp() {
    if (!ok) return;
    process.close_stdin();
    (void)process.wait();
  }

  void send(std::string_view line) {
    std::string text(line);
    text.push_back('\n');
    REQUIRE(process.write(text));
  }

  // The next line the bridge writes, parsed. Every line on its stdout must be one JSON message.
  JsonValue receive() {
    std::string text;
    REQUIRE_MESSAGE(process.read_line(text), "engine-mcp closed its output");
    JsonValue message;
    REQUIRE_MESSAGE(parse_json(text, message).ok, text);
    return message;
  }

  JsonValue raw(std::string_view line) {
    send(line);
    return receive();
  }

  JsonValue request(std::string_view method, std::string_view params) {
    const i64 id = next_id++;
    JsonValue response =
        raw("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"method\":\"" +
            std::string(method) + "\",\"params\":" + std::string(params) + "}");
    CHECK(at(response, "id") == JsonValue(id));
    return response;
  }

  void notify(std::string_view method) {
    send("{\"jsonrpc\":\"2.0\",\"method\":\"" + std::string(method) + "\"}");
  }

  void initialize() {
    const JsonValue r =
        request("initialize", R"({"protocolVersion":"2025-06-18","capabilities":{},)"
                              R"("clientInfo":{"name":"mcp-tests","version":"1"}})");
    REQUIRE(r.find("result") != nullptr);
    notify("notifications/initialized");
  }

  // A tools/call's result object, which carries `isError` rather than a JSON-RPC error.
  JsonValue tool(std::string_view name, std::string_view arguments) {
    const JsonValue response =
        request("tools/call", "{\"name\":\"" + std::string(name) +
                                  "\",\"arguments\":" + std::string(arguments) + "}");
    const JsonValue* result = response.find("result");
    REQUIRE_MESSAGE(result != nullptr, write_json(response));
    return *result;
  }
};

bool is_error(const JsonValue& result) {
  const JsonValue* e = result.find("isError");
  bool out = false;
  return e != nullptr && e->get_bool(out) && out;
}

// Every text block of a tool result, joined.
std::string text_of(const JsonValue& result) {
  std::string out;
  const JsonValue* content = result.find("content");
  for (usize i = 0; content != nullptr && i < content->size(); ++i) {
    if (str((*content)[i], "type") == "text") out += str((*content)[i], "text") + "\n";
  }
  return out;
}

const JsonValue& data_of(const JsonValue& result) { return at(result, "structuredContent"); }

// A tool result that must have succeeded; a copy of its structured data, so a caller may pass the
// temporary a call returned.
JsonValue ok(const JsonValue& result) {
  REQUIRE_MESSAGE(!is_error(result), text_of(result));
  return data_of(result);
}

std::string path_of_uri(std::string_view uri) {
  std::string_view rest = uri;
  if (rest.starts_with("file:///")) {
    rest.remove_prefix(8);
    if (rest.size() > 1 && rest[1] == ':') return std::string(rest);
    return "/" + std::string(rest);
  }
  if (rest.starts_with("file://")) rest.remove_prefix(7);
  return std::string(rest);
}

std::string attribution(const char* rationale) {
  return std::string(R"("attribution":{"role":"environment","task":"t1","rationale":")") +
         rationale + "\"}";
}

// Sets an environment variable for the processes this one starts, and puts it back.
class ScopedEnv {
 public:
  ScopedEnv(const char* name, const std::string& value)
      : name_(name), previous_(test::detail::environment(name)) {
    set(value.c_str());
  }
  ~ScopedEnv() { set(previous_.empty() ? nullptr : previous_.c_str()); }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  void set(const char* value) {
#if ENGINE_PLATFORM_WINDOWS
    // The OS environment block is what CreateProcess hands a child.
    ::SetEnvironmentVariableA(name_, value);
    (void)_putenv_s(name_, value != nullptr ? value : "");
#else
    if (value != nullptr) {
      ::setenv(name_, value, 1);
    } else {
      ::unsetenv(name_);
    }
#endif
  }
  const char* name_;
  std::string previous_;
};

bool kill_process(u64 pid) {
#if ENGINE_PLATFORM_WINDOWS
  HANDLE process = ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  if (process == nullptr) return false;
  const BOOL killed = ::TerminateProcess(process, 9);
  ::WaitForSingleObject(process, 10000);
  ::CloseHandle(process);
  return killed != 0;
#else
  return ::kill(static_cast<pid_t>(pid), SIGKILL) == 0;
#endif
}

bool has(const JsonValue& array, const JsonValue& value) {
  for (usize i = 0; i < array.size(); ++i) {
    if (array[i] == value) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("mcp: --help, --version, and a host that cannot start") {
  const test::TempDir tmp("mcp_bridge_usage");
  {
    const std::string_view argv[] = {bridge_exe(), "--help"};
    platform::Process p;
    REQUIRE(p.spawn(std::span<const std::string_view>(argv, std::size(argv))));
    p.close_stdin();
    std::string out;
    p.read_all(out);
    CHECK(p.wait() == 0);
    CHECK(out.find("usage: engine-mcp") != std::string::npos);
  }
  {
    const std::string_view argv[] = {bridge_exe(), "--version"};
    platform::Process p;
    REQUIRE(p.spawn(std::span<const std::string_view>(argv, std::size(argv))));
    p.close_stdin();
    std::string out;
    p.read_all(out);
    CHECK(p.wait() == 0);
    JsonValue v;
    REQUIRE(parse_json(out, v).ok);
    CHECK(str(v, "tool") == "engine-mcp");
  }
  {
    const std::string_view argv[] = {bridge_exe(), "--no-such-flag"};
    platform::Process p;
    REQUIRE(p.spawn(std::span<const std::string_view>(argv, std::size(argv))));
    p.close_stdin();
    std::string out;
    p.read_all(out);
    CHECK(p.wait() == 2);
    CHECK(out.empty());
  }
  {
    // stdout belongs to MCP: a bridge that cannot start its host says so on stderr and writes
    // nothing a client could mistake for a message.
    const std::string missing = tmp.file("no-such-engine-host");
    const std::string ws = tmp.file("ws");
    const std::string_view argv[] = {bridge_exe(), "--host", missing, "--workspace", ws};
    platform::Process p;
    REQUIRE(p.spawn(std::span<const std::string_view>(argv, std::size(argv))));
    p.close_stdin();
    std::string out;
    p.read_all(out);
    CHECK(p.wait() == 2);
    CHECK(out.empty());
  }
}

TEST_CASE("mcp: initialize agrees a protocol version and advertises tools") {
  const test::TempDir tmp("mcp_bridge_init");
  Mcp mcp(tmp.file("ws"));
  REQUIRE(mcp.ok);

  const JsonValue init =
      mcp.request("initialize", R"({"protocolVersion":"2025-06-18","capabilities":{},)"
                                R"("clientInfo":{"name":"mcp-tests","version":"1"}})");
  const JsonValue& r = at(init, "result");
  CHECK(str(r, "protocolVersion") == "2025-06-18");
  CHECK(at(r, "capabilities").find("tools") != nullptr);
  CHECK(at(r, "capabilities").find("resources") == nullptr);
  CHECK(str(at(r, "serverInfo"), "name") == "engine-mcp");
  CHECK_FALSE(str(at(r, "serverInfo"), "version").empty());
  CHECK(str(r, "instructions").find("open_session") != std::string::npos);

  // A notification gets no answer: the next line the bridge writes is the ping's.
  mcp.notify("notifications/initialized");
  const JsonValue ping = mcp.request("ping", "{}");
  CHECK(at(ping, "result") == JsonValue::object());

  // An older version the bridge speaks is echoed; one it does not know gets its newest, and the
  // client decides.
  CHECK(str(at(mcp.request("initialize", R"({"protocolVersion":"2024-11-05","capabilities":{}})"),
               "result"),
            "protocolVersion") == "2024-11-05");
  const std::string newest =
      str(at(mcp.request("initialize", R"({"protocolVersion":"1999-01-01","capabilities":{}})"),
             "result"),
          "protocolVersion");
  CHECK(newest >= "2025-06-18");
}

TEST_CASE("mcp: tools/list gives every tool a JSON Schema generated from the engine's schemas") {
  const test::TempDir tmp("mcp_bridge_tools");
  Mcp mcp(tmp.file("ws"));
  REQUIRE(mcp.ok);
  mcp.initialize();

  const JsonValue list = mcp.request("tools/list", "{}");
  const JsonValue& tools = at(at(list, "result"), "tools");
  REQUIRE(tools.size() >= 25);
  JsonValue names = JsonValue::array();
  const JsonValue* by_name[64] = {};
  const char* wanted[] = {"open_session",  "close_session",  "list_sessions", "layers",
                          "add_layer",     "set_edit_layer", "objects",       "get",
                          "create_object", "set_property",   "reparent",      "delete_object",
                          "apply",         "undo",           "redo",          "journal",
                          "diff",          "merge_layers",   "validate",      "describe",
                          "list_schema",   "capture",        "benchmark",     "compare",
                          "evaluate",      "get_logs",       "adapters",      "host_info"};
  for (usize i = 0; i < tools.size(); ++i) {
    const JsonValue& t = tools[i];
    const std::string name = str(t, "name");
    CAPTURE(name);
    CHECK_FALSE(name.empty());
    CHECK(str(t, "description").size() > 20);
    const JsonValue& schema = at(t, "inputSchema");
    CHECK(str(schema, "type") == "object");
    const JsonValue& properties = at(schema, "properties");
    CHECK(properties.is_object());
    for (u32 p = 0; p < properties.as_object().size(); ++p) {
      CHECK(properties.as_object().value_at(p).is_object());
    }
    if (const JsonValue* required = schema.find("required"); required != nullptr) {
      for (usize k = 0; k < required->size(); ++k) {
        std::string_view r;
        REQUIRE((*required)[k].get_string(r));
        CHECK_MESSAGE(properties.find(r) != nullptr, name << " requires " << r);
      }
    }
    names.push_back(JsonValue(name));
    for (usize w = 0; w < std::size(wanted); ++w) {
      if (name == wanted[w]) by_name[w] = &t;
    }
  }
  for (usize w = 0; w < std::size(wanted); ++w) {
    CHECK_MESSAGE(by_name[w] != nullptr, "no tool " << wanted[w]);
  }
  const auto schema_of = [&](const char* name) -> const JsonValue& {
    for (usize w = 0; w < std::size(wanted); ++w) {
      if (std::string_view(name) == wanted[w] && by_name[w] != nullptr)
        return at(*by_name[w], "inputSchema");
    }
    FAIL("no tool " << name);
    return *by_name[0];
  };
  const auto property = [&](const char* tool, const char* field) -> const JsonValue& {
    return at(at(schema_of(tool), "properties"), field);
  };

  // What makes these generated rather than copied: the types, the documentation and the defaults
  // all come from the IDL through the host's schema.describe.
  CHECK(str(property("open_session", "create"), "type") == "boolean");
  CHECK(at(property("open_session", "create"), "default") == JsonValue(false));
  CHECK(str(property("open_session", "path"), "description").find("manifest.json") !=
        std::string::npos);
  CHECK(at(schema_of("open_session"), "required") == [] {
    JsonValue r = JsonValue::array();
    r.push_back(JsonValue("path"));
    return r;
  }());
  CHECK(at(property("capture", "width"), "default") == JsonValue(u32{1280}));
  CHECK(str(at(property("capture", "channels"), "items"), "type") == "string");
  CHECK(at(schema_of("capture"), "properties").find("out_dir") == nullptr);
  const JsonValue& settings = at(at(property("capture", "load"), "properties"), "settings");
  CHECK(at(at(at(settings, "properties"), "raster"), "default") == JsonValue("hw"));
  // An f32 default reads as the number the schema wrote, not its double widening.
  CHECK(at(at(at(property("capture", "orbit"), "properties"), "pitch_deg"), "default") ==
        JsonValue(24.2277));
  const JsonValue& kind = at(at(at(property("apply", "commands"), "items"), "properties"), "kind");
  CHECK(has(at(kind, "enum"), JsonValue("CreateObject")));
  CHECK(str(kind, "description").find("CreateObject:") != std::string::npos);
  const JsonValue& attribution_schema = property("create_object", "attribution");
  CHECK(has(at(attribution_schema, "required"), JsonValue("rationale")));
  CHECK(at(at(at(attribution_schema, "properties"), "actor"), "default") == JsonValue("mcp-test"));
  CHECK(at(attribution_schema, "properties").find("timestamp_unix_ms") == nullptr);
  CHECK(at(property("objects", "limit"), "default") == JsonValue(u32{50}));
  CHECK(at(schema_of("objects"), "properties").find("offset") == nullptr);
  CHECK(at(schema_of("objects"), "properties").find("cursor") != nullptr);
  CHECK(str(property("create_object", "id"), "pattern") == "^[0-9a-fA-F]{32}$");

  // Annotations, which a client uses to decide what to ask the user about.
  for (usize i = 0; i < tools.size(); ++i) {
    if (str(tools[i], "name") == "objects") {
      CHECK(at(at(tools[i], "annotations"), "readOnlyHint") == JsonValue(true));
    }
    if (str(tools[i], "name") == "delete_object") {
      CHECK(at(at(tools[i], "annotations"), "destructiveHint") == JsonValue(true));
    }
  }
}

TEST_CASE("mcp: a scripted editing session") {
  const test::TempDir tmp("mcp_bridge_session");
  Mcp mcp(tmp.file("ws"));
  REQUIRE(mcp.ok);
  mcp.initialize();
  const std::string doc = tmp.file("world");

  const JsonValue& opened =
      ok(mcp.tool("open_session", "{\"path\":\"" + doc + "\",\"create\":true,\"name\":\"World\"}"));
  const std::string session = str(opened, "session");
  REQUIRE_FALSE(session.empty());
  CHECK(std::filesystem::exists(std::filesystem::path(doc) / "manifest.json"));
  const std::string s = "\"session\":\"" + session + "\"";

  // An id the bridge generates.
  const JsonValue created =
      mcp.tool("create_object", "{" + s + ",\"type\":\"" + k_type +
                                    "\",\"properties\":{\"generator\":\"dune-gen\"}," +
                                    attribution("the first object") + "}");
  const std::string a = str(ok(created), "id");
  CHECK(a.size() == 32);
  CHECK(text_of(created).find("committed") != std::string::npos);
  // And two the caller names, one a child of the first.
  ok(mcp.tool("create_object", "{" + s + ",\"id\":\"" + k_b + "\",\"type\":\"" + k_type +
                                   "\",\"parent\":\"" + a + "\"," + attribution("a child") + "}"));
  ok(mcp.tool("create_object", "{" + s + ",\"id\":\"" + k_c + "\",\"type\":\"" + k_type +
                                   "\",\"parent\":\"" + a + "\"," + attribution("another") + "}"));

  // Two pages of objects.
  const JsonValue& first = ok(mcp.tool("objects", "{" + s + ",\"limit\":2}"));
  CHECK(at(first, "objects").size() == 2);
  CHECK(num(first, "total") == 3);
  CHECK(str(first, "next_cursor") == "2");
  const JsonValue& second = ok(mcp.tool("objects", "{" + s + ",\"limit\":2,\"cursor\":\"2\"}"));
  CHECK(at(second, "objects").size() == 1);
  CHECK(second.find("next_cursor") == nullptr);
  const JsonValue& summary = ok(mcp.tool("objects", "{" + s + ",\"detail\":\"summary\"}"));
  const JsonValue& one = at(summary, "objects")[0];
  CHECK(one.find("properties") == nullptr);
  CHECK(one.find("property_names") != nullptr);
  CHECK(num(ok(mcp.tool("objects", "{" + s + ",\"parent\":\"" + a + "\"}")), "total") == 2);

  // Set, read, undo, read again.
  ok(mcp.tool("set_property", "{" + s + ",\"id\":\"" + a +
                                  "\",\"name\":\"generator\",\"value\":\"quest-gen\"," +
                                  attribution("rename the generator") + "}"));
  CHECK(at(at(ok(mcp.tool("get", "{" + s + ",\"id\":\"" + a + "\"}")), "properties"),
           "generator") == JsonValue("quest-gen"));
  const JsonValue& undone = ok(mcp.tool("undo", "{" + s + "}"));
  CHECK(num(undone, "stepped") == 1);
  CHECK(num(undone, "position") == 3);
  CHECK(at(at(ok(mcp.tool("get", "{" + s + ",\"id\":\"" + a + "\"}")), "properties"),
           "generator") == JsonValue("dune-gen"));
  CHECK(num(ok(mcp.tool("redo", "{" + s + "}")), "position") == 4);

  // The journal carries the attribution, with the actor the bridge was started with.
  const JsonValue& journal = ok(mcp.tool("journal", "{" + s + "}"));
  CHECK(num(journal, "total") == 4);
  const JsonValue& patch = at(journal, "patches")[0];
  CHECK(str(at(patch, "attribution"), "actor") == "mcp-test");
  CHECK(str(at(patch, "attribution"), "rationale") == "the first object");
  CHECK(num(patch, "forward_commands") == 1);

  // A layer, an override in it, and the diff between the two.
  const JsonValue& layered = ok(mcp.tool("add_layer", "{" + s + ",\"name\":\"quest\"}"));
  CHECK(at(layered, "layers").size() == 2);
  ok(mcp.tool("set_property", "{" + s + ",\"id\":\"" + a +
                                  "\",\"name\":\"generator\",\"value\":\"quest-only\"," +
                                  attribution("the quest renames it") + "}"));
  const JsonValue& diff =
      ok(mcp.tool("diff", "{" + s + ",\"from_layer\":\"base\",\"to_layer\":\"quest\"}"));
  CHECK(at(diff, "commands").size() >= 1);
  CHECK(num(diff, "total") == at(diff, "commands").size());
  const JsonValue& got = ok(mcp.tool("get", "{" + s + ",\"id\":\"" + a + "\"}"));
  CHECK(at(at(got, "properties"), "generator") == JsonValue("quest-only"));
  CHECK(str(got, "defining_layer") == "base");
  ok(mcp.tool("set_edit_layer", "{" + s + ",\"layer\":\"base\"}"));

  // Structure: move one to the root, delete the other, and the document still validates.
  ok(mcp.tool("reparent", "{" + s + ",\"id\":\"" + k_c + "\"," + attribution("promote") + "}"));
  ok(mcp.tool("delete_object",
              "{" + s + ",\"id\":\"" + k_b + "\"," + attribution("not needed") + "}"));
  CHECK(num(ok(mcp.tool("objects", "{" + s + ",\"parent\":\"" + a + "\"}")), "total") == 0);
  CHECK(at(ok(mcp.tool("validate", "{" + s + "}")), "ok") == JsonValue(true));

  // A batch through apply, atomic by default.
  ok(mcp.tool("apply", "{" + s + ",\"commands\":[{\"kind\":\"SetProperty\",\"id\":\"" + a +
                           "\",\"name\":\"license\",\"value\":\"MIT\"},{\"kind\":\"SetProperty\","
                           "\"id\":\"" +
                           k_c + "\",\"name\":\"license\",\"value\":\"CC0\"}]," +
                           attribution("licenses") + "}"));

  // Sessions, and closing one.
  CHECK(at(ok(mcp.tool("list_sessions", "{}")), "sessions").size() == 1);
  ok(mcp.tool("close_session", "{" + s + "}"));
  CHECK(at(ok(mcp.tool("list_sessions", "{}")), "sessions").size() == 0);
  const JsonValue gone = mcp.tool("get", "{" + s + ",\"id\":\"" + a + "\"}");
  CHECK(is_error(gone));
  CHECK(text_of(gone).find("SessionNotFound") != std::string::npos);
  CHECK(text_of(gone).find("Hint:") != std::string::npos);
  CHECK(num(at(data_of(gone), "error"), "code") == 1000);

  // A document that is not there says how to make one.
  const JsonValue missing =
      mcp.tool("open_session", "{\"path\":\"" + tmp.file("nothing-here") + "\"}");
  CHECK(is_error(missing));
  CHECK(text_of(missing).find("\"create\": true") != std::string::npos);
}

TEST_CASE("mcp: a mutation with no rationale is refused, with what to add") {
  const test::TempDir tmp("mcp_bridge_rationale");
  Mcp mcp(tmp.file("ws"));
  REQUIRE(mcp.ok);
  mcp.initialize();
  const std::string session =
      str(ok(mcp.tool("open_session", "{\"path\":\"" + tmp.file("doc") + "\",\"create\":true}")),
          "session");
  const std::string s = "\"session\":\"" + session + "\"";

  const JsonValue bare = mcp.tool("create_object", "{" + s + ",\"type\":\"" + k_type + "\"}");
  CHECK(is_error(bare));
  CHECK(text_of(bare).find("missing required argument 'attribution'") != std::string::npos);

  const JsonValue blank = mcp.tool("create_object", "{" + s + ",\"type\":\"" + k_type +
                                                        "\",\"attribution\":{\"task\":\"t1\","
                                                        "\"rationale\":\"   \"}}");
  CHECK(is_error(blank));
  const std::string text = text_of(blank);
  CHECK(text.find("no rationale") != std::string::npos);
  CHECK(text.find("Hint: Add attribution.rationale") != std::string::npos);
  CHECK(str(at(data_of(blank), "error"), "hint").find("rationale") != std::string::npos);

  const JsonValue no_why =
      mcp.tool("set_property",
               "{" + s + ",\"id\":\"" + k_b +
                   "\",\"name\":\"generator\",\"value\":1,\"attribution\":{\"actor\":\"x\"}}");
  CHECK(is_error(no_why));
  CHECK(text_of(no_why).find("no rationale") != std::string::npos);

  // Nothing reached the journal.
  CHECK(num(ok(mcp.tool("journal", "{" + s + "}")), "total") == 0);

  // A transaction the host refuses commits nothing and names every problem.
  const JsonValue refused = mcp.tool(
      "set_property", "{" + s + ",\"id\":\"" + k_b + "\",\"name\":\"generator\",\"value\":1," +
                          attribution("an object that is not there") + "}");
  CHECK(is_error(refused));
  CHECK(text_of(refused).find("Nothing was committed") != std::string::npos);
  CHECK(text_of(refused).find("does not exist") != std::string::npos);
  CHECK(num(ok(mcp.tool("journal", "{" + s + "}")), "total") == 0);
}

TEST_CASE("mcp: bad messages get JSON-RPC errors and the bridge keeps serving") {
  const test::TempDir tmp("mcp_bridge_errors");
  Mcp mcp(tmp.file("ws"));
  REQUIRE(mcp.ok);
  mcp.initialize();

  const JsonValue unknown = mcp.request("tools/call", R"({"name":"no_such_tool","arguments":{}})");
  i64 code = 0;
  REQUIRE(at(at(unknown, "error"), "code").get_i64(code));
  CHECK(code == -32602);
  CHECK(str(at(unknown, "error"), "message").find("no_such_tool") != std::string::npos);
  CHECK(has(at(at(at(unknown, "error"), "data"), "tools"), JsonValue("capture")));

  const JsonValue garbage = mcp.raw("{oops");
  REQUIRE(at(at(garbage, "error"), "code").get_i64(code));
  CHECK(code == -32700);
  CHECK(at(garbage, "id").is_null());

  const JsonValue batch = mcp.raw(R"([{"jsonrpc":"2.0","id":99,"method":"ping"}])");
  REQUIRE(at(at(batch, "error"), "code").get_i64(code));
  CHECK(code == -32600);

  const JsonValue no_method = mcp.request("resources/list", "{}");
  REQUIRE(at(at(no_method, "error"), "code").get_i64(code));
  CHECK(code == -32601);

  const JsonValue bad_args = mcp.request("tools/call", R"({"name":"layers","arguments":[1]})");
  REQUIRE(at(at(bad_args, "error"), "code").get_i64(code));
  CHECK(code == -32602);

  // Argument problems are tool results the model can read and correct, with the right names.
  const JsonValue misspelt = mcp.tool("open_session", R"({"pth":"x"})");
  CHECK(is_error(misspelt));
  CHECK(text_of(misspelt).find("unknown argument 'pth'") != std::string::npos);
  CHECK(text_of(misspelt).find("path") != std::string::npos);
  const JsonValue missing = mcp.tool("open_session", "{}");
  CHECK(is_error(missing));
  CHECK(text_of(missing).find("missing required argument 'path'") != std::string::npos);
  // And a protocol error keeps its code, its diagnostics and a hint.
  const JsonValue typed = mcp.tool("open_session", R"({"path":5})");
  CHECK(is_error(typed));
  CHECK(text_of(typed).find("InvalidParams") != std::string::npos);
  CHECK(text_of(typed).find("path: expected a string") != std::string::npos);

  // Still serving.
  CHECK(at(mcp.request("ping", "{}"), "result") == JsonValue::object());
  CHECK(ok(mcp.tool("list_sessions", "{}")).find("sessions") != nullptr);
}

TEST_CASE("mcp: render tools on a machine with no GPU say so, and the rest still works") {
  const test::TempDir tmp("mcp_bridge_no_gpu");
  // Hide every Vulkan driver from the host: the loader then has no device to offer, which is the
  // state of every hosted CI runner, made on purpose so the answer is pinned on a GPU machine too.
  const std::string nothing = tmp.file("no-such-driver.json");
  const ScopedEnv driver_files("VK_DRIVER_FILES", nothing);
  const ScopedEnv icd_filenames("VK_ICD_FILENAMES", nothing);
  const ScopedEnv disable("VK_LOADER_DRIVERS_DISABLE", "*");
  Mcp mcp(tmp.file("ws"));
  REQUIRE(mcp.ok);
  mcp.initialize();

  const JsonValue capture = mcp.tool("capture", R"({"load":{"grid":33},"width":64,"height":48})");
  if (!is_error(capture)) {
    // A loader that ignores the override (one running elevated does) renders anyway.
    MESSAGE("the Vulkan loader ignored the hidden drivers and rendered: " << text_of(capture));
  } else {
    MESSAGE("no-GPU answer: " << text_of(capture));
    CHECK(text_of(capture).find("No GPU on this machine") != std::string::npos);
    CHECK(text_of(capture).find("1007") != std::string::npos);
    CHECK(num(at(data_of(capture), "error"), "code") == 1007);
    CHECK(str(at(data_of(capture), "error"), "hint").find("no GPU on this machine") !=
          std::string::npos);
    const JsonValue adapters = mcp.tool("adapters", "{}");
    CHECK(text_of(adapters).find("No GPU on this machine") != std::string::npos);
  }
  // The document tools do not need a GPU.
  const JsonValue& opened =
      ok(mcp.tool("open_session", "{\"path\":\"" + tmp.file("doc") + "\",\"create\":true}"));
  CHECK_FALSE(str(opened, "session").empty());
}

// No commas in this name: doctest splits a -tc/-tce filter at them, and this is the one case a
// CPU-only run excludes (-tce="*renders into*").
TEST_CASE("mcp: capture and benchmark and compare renders into the workspace or answers no GPU") {
  const test::TempDir tmp("mcp_bridge_render");
  const std::string ws = tmp.file("ws");
  Mcp mcp(ws);
  REQUIRE(mcp.ok);
  mcp.initialize();

  const char* load = R"("load":{"grid":33,"settings":{"raster":"hw","shadows":"off"}})";
  const JsonValue first = mcp.tool("capture", std::string("{") + load +
                                                  R"(,"width":64,"height":48,)"
                                                  R"("channels":["color","ids"]})");
  if (is_error(first)) {
    CHECK(text_of(first).find("No GPU on this machine") != std::string::npos);
    MESSAGE("SKIPPED the rendering half: this machine has no GPU the renderer can use: "
            << text_of(first));
    return;
  }
  const JsonValue& shot = data_of(first);
  MESSAGE("RENDERED on " << str(at(shot, "loaded"), "adapter"));
  const std::string scene = str(shot, "scene");
  CHECK_FALSE(scene.empty());
  CHECK(str(shot, "name") == "capture-001");
  CHECK(at(at(shot, "loaded"), "reused") == JsonValue(false));
  const JsonValue& files = at(shot, "files");
  const std::string color = path_of_uri(str(files, "color"));
  const std::string ids = path_of_uri(str(files, "ids"));
  CHECK(color.starts_with(ws + "/captures/"));
  CHECK(std::filesystem::exists(color));
  CHECK(std::filesystem::exists(path_of_uri(str(files, "result"))));
  REQUIRE(std::filesystem::exists(ids));
  CHECK(std::filesystem::file_size(ids) == 64u * 48u * 3u * 4u);
  // Bulk results are links, never bytes: nothing in the answer is anywhere near a picture's size.
  CHECK(write_json(first).size() < 8192);
  bool linked = false;
  const JsonValue& content = at(first, "content");
  for (usize i = 0; i < content.size(); ++i) {
    if (str(content[i], "type") == "resource_link" && str(content[i], "uri") == str(files, "color"))
      linked = true;
  }
  CHECK(linked);

  // The same load again is the same scene, not a second import; a second name is generated.
  const JsonValue& again =
      ok(mcp.tool("capture", std::string("{") + load + R"(,"width":64,"height":48})"));
  CHECK(str(again, "scene") == scene);
  CHECK(at(at(again, "loaded"), "reused") == JsonValue(true));
  CHECK(str(again, "name") == "capture-002");
  // And by id.
  const JsonValue& by_id = ok(mcp.tool(
      "capture", "{\"scene\":\"" + scene + "\",\"width\":64,\"height\":48,\"name\":\"by-id\"}"));
  CHECK(str(by_id, "name") == "by-id");

  // compare takes the URIs capture returned.
  const JsonValue& compared =
      ok(mcp.tool("compare", "{\"a\":\"" + str(files, "color") + "\",\"b\":\"" +
                                 str(at(again, "files"), "color") + "\"}"));
  CHECK(num(compared, "width") == 64);
  CHECK(std::filesystem::exists(path_of_uri(str(at(compared, "files"), "flip"))));

  // A benchmark writes its whole result to the workspace and summarizes it.
  const JsonValue bench = mcp.tool(
      "benchmark", "{\"scene\":\"" + scene + "\",\"width\":64,\"height\":48,\"frames\":4}");
  const JsonValue& measured = ok(bench);
  CHECK(num(measured, "frames") == 4);
  CHECK(std::filesystem::exists(path_of_uri(str(at(measured, "files"), "result"))));
  CHECK(text_of(bench).find("ms a frame") != std::string::npos);

  // Neither a scene nor a load is refused with what to pass.
  const JsonValue nothing = mcp.tool("capture", R"({"width":64,"height":48})");
  CHECK(is_error(nothing));
  CHECK(text_of(nothing).find("load") != std::string::npos);
}

TEST_CASE("mcp: a host that dies is reported, and the next call starts another") {
  const test::TempDir tmp("mcp_bridge_host_death");
  Mcp mcp(tmp.file("ws"));
  REQUIRE(mcp.ok);
  mcp.initialize();

  const JsonValue& before = ok(mcp.tool("host_info", "{}"));
  const u64 pid = num(at(before, "host"), "pid");
  REQUIRE(pid != ~u64{0});
  CHECK(num(at(before, "bridge"), "host_generation") == 1);
  REQUIRE(kill_process(pid));

  const JsonValue lost = mcp.tool("list_sessions", "{}");
  CHECK(is_error(lost));
  CHECK(text_of(lost).find("engine-host exited") != std::string::npos);
  CHECK(at(at(data_of(lost), "error"), "host_exited") == JsonValue(true));

  // The next call has a host again, a new one, with nothing open.
  CHECK(at(ok(mcp.tool("list_sessions", "{}")), "sessions").size() == 0);
  const JsonValue& after = ok(mcp.tool("host_info", "{}"));
  CHECK(num(at(after, "host"), "pid") != pid);
  CHECK(num(at(after, "bridge"), "host_generation") == 2);
}
