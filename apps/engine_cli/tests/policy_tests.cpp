// End to end: plan 06 §6.5 through the real executables (docs/subsystems/protocol.md, "Roles,
// leases and proposals"; apps.md, engine-host's `--roles`). engine-host refuses a roles file it
// cannot use, restricts what a role may change, and a lease or a proposal granted by one process
// holds in the next, because both are kept beside the document — which is what makes them usable
// from engine-cli, which starts a host for every call.
#include <core/json/json.h>
#include <core/platform/process.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <fstream>
#include <string>
#include <vector>

using namespace engine;

namespace {

const std::string& cli_exe() {
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

struct Run {
  i32 exit_code = -1;
  std::string out;
  JsonValue result;
};

Run run(std::vector<std::string> args) {
  std::vector<std::string_view> argv(args.begin(), args.end());
  platform::Process p;
  std::string error;
  Run r;
  if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error)) {
    FAIL("cannot spawn " << args[0] << ": " << error);
    return r;
  }
  p.close_stdin();
  p.read_all(r.out);
  r.exit_code = p.wait();
  if (!r.out.empty()) (void)parse_json(r.out, r.result);
  return r;
}

Run cli(std::vector<std::string> args) {
  args.insert(args.begin(), {cli_exe(), "--host", host_exe()});
  return run(std::move(args));
}

// One engine-host with the given flags, spoken to a line at a time.
struct Host {
  platform::Process process;
  u32 next_id = 1;
  bool ok = false;

  explicit Host(std::vector<std::string> flags) {
    std::vector<std::string> args = {host_exe(), "--stdio"};
    args.insert(args.end(), flags.begin(), flags.end());
    std::vector<std::string_view> argv(args.begin(), args.end());
    std::string error;
    if (!process.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error)) {
      FAIL("cannot spawn engine-host: " << error);
      return;
    }
    ok = true;
  }
  ~Host() {
    if (!ok) return;
    process.close_stdin();
    (void)process.wait();
  }
  JsonValue call(std::string_view method, std::string_view params) {
    const std::string line = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(next_id++) +
                             ",\"method\":\"" + std::string(method) +
                             "\",\"params\":" + std::string(params) + "}\n";
    REQUIRE(process.write(line));
    std::string text;
    REQUIRE(process.read_line(text));
    JsonValue response;
    REQUIRE_MESSAGE(parse_json(text, response).ok, text);
    return response;
  }
};

i32 code_of(const JsonValue& response) {
  const JsonValue* e = response.find("error");
  return e != nullptr ? static_cast<i32>(e->find("code")->as_int()) : 0;
}

void write_file(const std::string& path, std::string_view text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
}

const char* k_roles = R"({
  "default_role": "qa",
  "roles": [
    {"name": "director", "methods": ["*"], "layers": ["*"], "object_types": ["*"]},
    {"name": "qa", "methods": []}
  ]
})";

const char* k_type = "engine.world.Node";

// A node standing in tile (tx, 0) of a grid 10 m a side. The type declares `position`, so the
// promotion's validation has nothing to say about it.
std::string create(const char* id, int tx) {
  return std::string(R"({"kind":"CreateObject","id":")") + id + R"(","type":")" + k_type +
         R"(","value":{"position":[)" + std::to_string(tx * 10 + 5) + ",0,5]}}";
}

std::string apply(const std::string& command, const char* actor, const char* layer) {
  return R"({"commands":[)" + command + R"(],"attribution":{"actor":")" + actor +
         R"(","rationale":"e2e"},"layer":")" + layer + "\"}";
}

}  // namespace

TEST_CASE("host: a roles file it cannot use is a usage error") {
  test::TempDir tmp("engine_cli_roles");
  const std::string good = tmp.file("roles.json");
  write_file(good, k_roles);
  const std::string bad = tmp.file("bad.json");
  write_file(bad, R"({"roles":[{"name":"a"},{"name":"a"}]})");
  const std::string request = R"({"jsonrpc":"2.0","id":1,"method":"engine.roles"})";
  CHECK(run({host_exe(), "--roles", tmp.file("missing.json"), "--request", request}).exit_code ==
        2);
  CHECK(run({host_exe(), "--roles", bad, "--request", request}).exit_code == 2);
  CHECK(run({host_exe(), "--roles", good, "--role", "nobody", "--request", request}).exit_code ==
        2);
  const Run roles = run(
      {host_exe(), "--roles", good, "--actor", "me", "--role", "director", "--request", request});
  CHECK(roles.exit_code == 0);
  const JsonValue& result = at(roles.result, "result");
  CHECK(at(result, "loaded") == JsonValue(true));
  CHECK(at(result, "actor") == JsonValue("me"));
  CHECK(at(result, "role") == JsonValue("director"));
  CHECK(at(result, "roles").size() == 2);
  // Without one: nothing loaded, nothing restricted.
  const Run none = run({host_exe(), "--request", request});
  CHECK(at(at(none.result, "result"), "loaded") == JsonValue(false));
}

TEST_CASE("host: a role restricts what a call may change, never what it may read") {
  test::TempDir tmp("engine_cli_roles");
  const std::string roles = tmp.file("roles.json");
  write_file(roles, k_roles);
  const std::string doc = tmp.file("doc");
  REQUIRE(cli({"--doc", doc, "--create", "session.info"}).exit_code == 0);

  // A host for a QA agent: its identity makes every call QA's.
  {
    Host qa({"--roles", roles, "--actor", "q", "--role", "qa"});
    JsonValue open = qa.call("session.open", "{\"path\":" + write_json(JsonValue(doc)) + "}");
    REQUIRE(code_of(open) == 0);
    const JsonValue refused =
        qa.call("doc.apply",
                "{\"session\":\"s1\"," +
                    apply(create("00000000000000100000000000000001", 0), "q", "base").substr(1));
    CHECK(code_of(refused) == 1008);
    CHECK(at(at(at(refused, "error"), "data"), "role") == JsonValue("qa"));
    CHECK(code_of(qa.call("doc.objects", R"({"session":"s1"})")) == 0);
    CHECK(code_of(qa.call("doc.add_layer", R"({"session":"s1","name":"x"})")) == 1008);
  }
  // The director's host writes.
  {
    Host director({"--roles", roles, "--actor", "boss", "--role", "director"});
    REQUIRE(code_of(director.call("session.open",
                                  "{\"path\":" + write_json(JsonValue(doc)) + "}")) == 0);
    CHECK(
        code_of(director.call(
            "doc.apply",
            "{\"session\":\"s1\"," +
                apply(create("00000000000000100000000000000001", 0), "boss", "base").substr(1))) ==
        0);
  }
}

TEST_CASE("cli: leases and proposals hold from one process to the next") {
  test::TempDir tmp("engine_cli_leases");
  const std::string doc = tmp.file("doc");
  const auto call = [&](const char* method, const std::string& params) {
    return cli({"--doc", doc, "--compact", method, params});
  };
  REQUIRE(cli({"--doc", doc, "--create", "session.info"}).exit_code == 0);
  REQUIRE(call("doc.add_layer",
               R"({"name":"world","partition":{"property":"position","tile_size":10}})")
              .exit_code == 0);
  REQUIRE(call("lease.require", R"({"required":true,"attribution":{"actor":"a"}})").exit_code == 0);
  const Run acquired = call(
      "lease.acquire",
      R"({"layer":"world","tiles":[{"x0":0,"y0":0,"x1":0,"y1":0}],"attribution":{"actor":"a","task":"t1"}})");
  REQUIRE(acquired.exit_code == 0);
  CHECK(at(acquired.result, "actor") == JsonValue("a"));

  // Another process sees the lease, and the document's requirement.
  const Run listed = call("lease.list", "{}");
  REQUIRE(listed.exit_code == 0);
  CHECK(at(listed.result, "required") == JsonValue(true));
  REQUIRE(at(listed.result, "leases").size() == 1);
  CHECK(at(at(listed.result, "leases")[0], "task") == JsonValue("t1"));
  // b may not edit a's tile, nor a tile nobody holds; a may edit its own.
  CHECK(call("doc.apply", apply(create("00000000000000100000000000000001", 0), "b", "world"))
            .exit_code == 1);
  CHECK(call("doc.apply", apply(create("00000000000000100000000000000001", 3), "a", "world"))
            .exit_code == 1);
  CHECK(call("doc.apply", apply(create("00000000000000100000000000000001", 0), "a", "world"))
            .exit_code == 0);

  // A proposal opened by one process, written by another, promoted by a third.
  REQUIRE(
      call(
          "doc.propose_layer",
          R"({"name":"p.a","target":"world","edit":false,"attribution":{"actor":"a","rationale":"a second prop"}})")
          .exit_code == 0);
  CHECK(call("doc.apply", apply(create("00000000000000100000000000000002", 0), "a", "p.a"))
            .exit_code == 0);
  const Run promoted =
      call("doc.promote", R"({"proposal":"p.a","attribution":{"actor":"d","rationale":"ok"}})");
  REQUIRE(promoted.exit_code == 0);
  CHECK(at(promoted.result, "promoted") == JsonValue(true));
  const Run layers = call("doc.layers", "{}");
  const JsonValue& stack = at(layers.result, "layers");
  CHECK(at(stack[1], "records") == JsonValue(u32{2}));  // both props in world
  CHECK(at(at(stack[2], "proposal"), "state") == JsonValue("Promoted"));
  CHECK(at(at(stack[2], "proposal"), "closed_by") == JsonValue("d"));
}
