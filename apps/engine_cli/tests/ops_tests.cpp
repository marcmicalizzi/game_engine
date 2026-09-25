// End to end: the rest of plan 06 §6.9's day-one operations — content.build, session.events,
// engine.budgets, session.run_headless and engine.run_tests — through engine-cli against
// engine-host, one process per call where the answer lives on disk, and one engine-host fed a
// series of requests where it lives in the host (a runtime world's tick carries across calls).
//
// Nothing here needs a GPU, a sample asset or the repository's derived-data cache: the mesh is a
// grid written as a .gltf and a .bin at test time, and every cache the calls use is the test's own
// scratch directory. A case that needs an optional capability (the store, the tissue validators)
// checks the answer the host gives without it — 1006 Unavailable, or a result that says so —
// when this configuration left it out.
#include <core/json/json.h>
#include <core/platform/process.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(ENGINE_CLI_TESTS_STORE)
#include <foundation/store/database.h>
#include <foundation/store/event_log.h>
#endif

using namespace engine;

namespace {

constexpr i32 k_session_not_found = 1000;
constexpr i32 k_invalid_argument = 1004;
// Used only where this configuration left the store or the tissue validators out.
[[maybe_unused]] constexpr i32 k_unavailable = 1006;

const std::string& cli_exe() {
  static const std::string path = test::app_path(ENGINE_APP_PATH);
  return path;
}
const std::string& host_exe() {
  static const std::string path = test::app_path(ENGINE_HOST_PATH);
  return path;
}

struct Run {
  i32 exit_code = -1;
  std::string stdout_text;
  JsonValue result;
};

Run cli(std::vector<std::string> args) {
  std::vector<std::string_view> argv;
  argv.push_back(cli_exe());
  argv.push_back("--host");
  argv.push_back(host_exe());
  for (const std::string& a : args)
    argv.push_back(a);
  platform::Process p;
  std::string error;
  Run run;
  if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error)) {
    FAIL("cannot spawn engine-cli: " << error);
    return run;
  }
  p.close_stdin();
  p.read_all(run.stdout_text);
  run.exit_code = p.wait();
  if (!run.stdout_text.empty()) (void)parse_json(run.stdout_text, run.result);
  return run;
}

// One engine-host fed a series of requests: a session's runtime world lives in the host.
struct Host {
  platform::Process process;
  u32 next_id = 1;
  bool ok = false;

  Host() {
    const std::string_view argv[2] = {host_exe(), "--stdio"};
    std::string error;
    if (!process.spawn(std::span<const std::string_view>(argv, 2), &error)) {
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
    JsonValue response;
    const std::string line = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(next_id++) +
                             ",\"method\":\"" + std::string(method) +
                             "\",\"params\":" + std::string(params) + "}\n";
    if (!process.write(line)) {
      FAIL("cannot write to engine-host");
      return response;
    }
    std::string text;
    if (!process.read_line(text)) {
      FAIL("engine-host closed the connection");
      return response;
    }
    REQUIRE_MESSAGE(parse_json(text, response).ok, text);
    return response;
  }
};

i32 error_code(const JsonValue& response) {
  const JsonValue* error = response.find("error");
  if (error == nullptr) return 0;
  const JsonValue* code = error->find("code");
  i64 value = -1;
  return code != nullptr && code->get_i64(value) ? static_cast<i32>(value) : -1;
}

const JsonValue& at(const JsonValue& o, std::string_view key) {
  const JsonValue* v = o.find(key);
  REQUIRE_MESSAGE(v != nullptr, "missing '" << std::string(key) << "'");
  return *v;
}

const JsonValue& result_of(const JsonValue& response) {
  const JsonValue* error = response.find("error");
  REQUIRE_MESSAGE(error == nullptr,
                  (error != nullptr ? write_json(*error, JsonWriteOptions{}) : std::string()));
  return at(response, "result");
}

u64 number(const JsonValue& o, std::string_view key) {
  u64 out = 0;
  const JsonValue* v = o.find(key);
  return v != nullptr && v->get_u64(out) ? out : ~u64{0};
}

f64 real(const JsonValue& o, std::string_view key) {
  f64 out = std::nan("");
  const JsonValue* v = o.find(key);
  return v != nullptr && v->get_f64(out) ? out : std::nan("");
}

std::string text(const JsonValue& o, std::string_view key) {
  std::string_view out;
  const JsonValue* v = o.find(key);
  return v != nullptr && v->get_string(out) ? std::string(out) : std::string();
}

// A JSON string literal for a path: backslashes and quotes escaped.
std::string json_string(std::string_view s) {
  std::string out = "\"";
  for (const char c : s) {
    if (c == '\\' || c == '"') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

const char* k_a = "00000000000000100000000000000001";
const char* k_b = "00000000000000100000000000000002";
const char* k_type = "engine.content.AssetProvenance";

std::string apply_params(const std::string& commands, const char* actor = "ops-test") {
  return "{\"commands\":" + commands + ",\"attribution\":{\"actor\":\"" + actor +
         "\",\"role\":\"environment\",\"task\":\"t\",\"rationale\":\"day-one operations\"}}";
}

std::string create(const char* id, const char* generator) {
  return std::string("{\"kind\":\"CreateObject\",\"id\":\"") + id + "\",\"type\":\"" + k_type +
         "\",\"value\":{\"generator\":\"" + generator + "\"}}";
}

void put_f32(std::vector<u8>& out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((bits >> (8 * i)) & 0xffu));
}

void put_u32(std::vector<u8>& out, u32 v) {
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((v >> (8 * i)) & 0xffu));
}

// A bumpy n x n grid of quads as `<stem>.gltf` and `<stem>.bin`: positions, normals, UVs and 32-bit
// indices under one material. At n = 16 it is 512 triangles, several clusters and a coarser level.
bool write_grid(const std::string& dir, const std::string& stem, u32 n) {
  std::vector<u8> bin;
  const u32 side = n + 1;
  for (u32 j = 0; j < side; ++j) {
    for (u32 i = 0; i < side; ++i) {
      put_f32(bin, static_cast<f32>(i));
      put_f32(bin, (i + j) % 2 == 0 ? 0.25f : 0.0f);
      put_f32(bin, static_cast<f32>(j));
    }
  }
  const u32 normals = static_cast<u32>(bin.size());
  for (u32 v = 0; v < side * side; ++v) {
    put_f32(bin, 0.0f);
    put_f32(bin, 1.0f);
    put_f32(bin, 0.0f);
  }
  const u32 uvs = static_cast<u32>(bin.size());
  for (u32 j = 0; j < side; ++j) {
    for (u32 i = 0; i < side; ++i) {
      put_f32(bin, static_cast<f32>(i) / static_cast<f32>(n));
      put_f32(bin, static_cast<f32>(j) / static_cast<f32>(n));
    }
  }
  const u32 indices = static_cast<u32>(bin.size());
  for (u32 j = 0; j < n; ++j) {
    for (u32 i = 0; i < n; ++i) {
      const u32 a = j * side + i;
      const u32 b = (j + 1) * side + i;
      const u32 c = j * side + i + 1;
      const u32 d = (j + 1) * side + i + 1;
      for (const u32 v : {a, b, c, c, b, d})
        put_u32(bin, v);
    }
  }
  const u32 total = static_cast<u32>(bin.size());
  const auto view = [](u32 offset, u32 length) {
    return "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) +
           ",\"byteLength\":" + std::to_string(length) + "}";
  };
  const std::string count = std::to_string(side * side);
  const std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"" + stem +
      ".bin\",\"byteLength\":" + std::to_string(total) + "}],\"bufferViews\":[" + view(0, normals) +
      "," + view(normals, uvs - normals) + "," + view(uvs, indices - uvs) + "," +
      view(indices, total - indices) +
      "],\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":" + count +
      ",\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[" + std::to_string(n) + ",0.25," +
      std::to_string(n) + "]},{\"bufferView\":1,\"componentType\":5126,\"count\":" + count +
      ",\"type\":\"VEC3\"},{\"bufferView\":2,\"componentType\":5126,\"count\":" + count +
      ",\"type\":\"VEC2\"},{\"bufferView\":3,\"componentType\":5125,\"count\":" +
      std::to_string(n * n * 6) +
      ",\"type\":\"SCALAR\"}],\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,"
      "1,1,1]}}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
      "\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],\"nodes\":[{\"mesh\":0}],"
      "\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
  std::ofstream gltf(dir + "/" + stem + ".gltf", std::ios::binary);
  std::ofstream data(dir + "/" + stem + ".bin", std::ios::binary);
  if (!gltf.good() || !data.good()) return false;
  gltf << json;
  data.write(reinterpret_cast<const char*>(bin.data()), static_cast<std::streamsize>(bin.size()));
  return gltf.good() && data.good();
}

bool write_text(const std::string& path, const std::string& body) {
  std::ofstream file(path, std::ios::binary);
  file << body;
  return file.good();
}

// The session id engine-cli's --doc would inject, for a Host-driven case: opens (or creates) the
// document and returns the id.
std::string open_session(Host& host, const std::string& dir, bool create) {
  const JsonValue opened = host.call(
      "session.open", "{\"path\":" + json_string(dir) + (create ? ",\"create\":true}" : "}"));
  return text(result_of(opened), "session");
}

}  // namespace

TEST_CASE("ops: the five methods are in the catalogue with schema-typed params and results") {
  Run methods = cli({"--compact", "engine.methods"});
  REQUIRE(methods.exit_code == 0);
  const JsonValue& list = at(methods.result, "methods");
  struct Expected {
    const char* name;
    const char* params;
    const char* result;
  };
  const Expected expected[] = {
      {"content.build", "engine.protocol.ContentBuildParams", "engine.protocol.ContentBuildResult"},
      {"session.events", "engine.protocol.SessionEventsParams",
       "engine.protocol.SessionEventsResult"},
      {"engine.budgets", "engine.protocol.BudgetsParams", "engine.protocol.BudgetsResult"},
      {"session.run_headless", "engine.protocol.RunHeadlessParams",
       "engine.protocol.RunHeadlessResult"},
      {"engine.run_tests", "engine.protocol.RunTestsParams", "engine.protocol.RunTestsResult"},
      // The materialization report beside them (docs/subsystems/protocol.md).
      {"session.materialize", "engine.protocol.MaterializeParams",
       "engine.protocol.MaterializeResult"},
  };
  for (const Expected& e : expected) {
    CAPTURE(e.name);
    bool found = false;
    for (usize i = 0; i < list.size(); ++i) {
      if (text(list[i], "name") != e.name) continue;
      found = true;
      CHECK(text(list[i], "params_type") == e.params);
      CHECK(text(list[i], "result_type") == e.result);
      CHECK(text(list[i], "doc").size() > 40);
    }
    CHECK(found);
    // And each type is described, which is what the MCP bridge builds a tool's schema from.
    Run described =
        cli({"--compact", "schema.describe", std::string("{\"type\":\"") + e.params + "\"}"});
    CHECK(described.exit_code == 0);
    CHECK(at(at(described.result, "description"), "fields").size() >= 1);
  }
}

TEST_CASE(
    "ops: content.build builds the grid, skips it, rebuilds it by force, and builds a manifest") {
  const test::TempDir tmp("ops_content");
  REQUIRE(tmp.ok());
  REQUIRE(write_grid(tmp.path(), "grid", 16));
  const std::string source = tmp.file("grid.gltf");
  const std::string output = tmp.file("grid.clusters");
  const std::string params =
      "{\"source\":" + json_string(source) + ",\"output\":" + json_string(output) + ",\"jobs\":2}";

  Run built = cli({"--compact", "content.build", params});
  REQUIRE(built.exit_code == 0);
  CHECK(number(built.result, "built") == 1);
  REQUIRE(at(built.result, "outputs").size() == 1);
  const JsonValue& first = at(built.result, "outputs")[0];
  CHECK(text(first, "status") == "built");
  CHECK(text(first, "path") == output);
  CHECK(number(first, "triangles") == 512);
  CHECK(number(first, "clusters") > number(first, "leaf_clusters"));
  CHECK(number(first, "bytes") > 0);
  CHECK(number(first, "source_hash") != 0);
  CHECK(number(first, "build_key") != 0);
  CHECK(real(first, "build_ms") > 0.0);
  CHECK(at(first, "repairs").is_array());
  CHECK(text(at(first, "atlas"), "mode") == "keep");
  // No derived-data root (no cache, no ddc named): the texture step does not run, and says so.
  CHECK(at(at(first, "textures"), "enabled") == JsonValue(false));
  CHECK(at(at(built.result, "textures"), "enabled") == JsonValue(false));
  // The metrics engine-content stats prints, for the container just written.
  const JsonValue& stats = at(first, "stats");
  CHECK(number(stats, "clusters") == number(first, "clusters"));
  CHECK(at(stats, "level_clusters").size() == number(first, "lod_levels"));
  CHECK(at(at(stats, "bytes"), "total").is_number());
  CHECK(std::filesystem::exists(output));

  // Again: the container records this source and these options, so it is the answer already.
  Run again = cli({"--compact", "content.build", params});
  REQUIRE(again.exit_code == 0);
  CHECK(number(again.result, "skipped") == 1);
  const JsonValue& skipped = at(again.result, "outputs")[0];
  CHECK(text(skipped, "status") == "skipped");
  CHECK(number(skipped, "build_key") == number(first, "build_key"));
  CHECK(number(skipped, "bytes") == number(first, "bytes"));
  CHECK(number(at(skipped, "stats"), "clusters") == number(first, "clusters"));

  // force rebuilds, and the bytes are the same whatever the thread count.
  Run forced = cli({"--compact", "content.build",
                    "{\"source\":" + json_string(source) + ",\"output\":" + json_string(output) +
                        ",\"jobs\":1,\"force\":true,\"stats\":false}"});
  REQUIRE(forced.exit_code == 0);
  const JsonValue& rebuilt = at(forced.result, "outputs")[0];
  CHECK(text(rebuilt, "status") == "built");
  CHECK(number(rebuilt, "hash") == number(first, "hash"));
  CHECK(at(rebuilt, "stats").is_null());

  // A different option is a different build key.
  Run other = cli({"--compact", "content.build",
                   "{\"source\":" + json_string(source) +
                       ",\"output\":" + json_string(tmp.file("64.clusters")) +
                       ",\"options\":{\"max_triangles\":64},\"stats\":false}"});
  REQUIRE(other.exit_code == 0);
  CHECK(number(at(other.result, "outputs")[0], "build_key") != number(first, "build_key"));

  // A manifest: one entry to a named output (already built, so skipped), one to the cache, and
  // one whose source is not there, which fails on its own without stopping the others.
  const std::string ddc = tmp.file("ddc");
  const std::string manifest = tmp.file("meshes.json");
  REQUIRE(write_text(manifest,
                     "{\"meshes\":[{\"source\":\"grid.gltf\",\"output\":\"grid.clusters\"},"
                     "{\"source\":\"grid.gltf\",\"options\":{\"max_vertices\":32}},"
                     "{\"source\":\"missing.gltf\",\"output\":\"missing.clusters\"}]}"));
  Run listed = cli({"--compact", "content.build",
                    "{\"manifest\":" + json_string(manifest) +
                        ",\"cache\":true,\"ddc\":" + json_string(ddc) + ",\"stats\":false}"});
  REQUIRE(listed.exit_code == 0);
  CHECK(number(listed.result, "built") == 1);
  CHECK(number(listed.result, "skipped") == 1);
  CHECK(number(listed.result, "failed") == 1);
  CHECK(text(listed.result, "ddc") == ddc);
  const JsonValue& outputs = at(listed.result, "outputs");
  REQUIRE(outputs.size() == 3);
  CHECK(text(outputs[0], "status") == "skipped");
  CHECK(text(outputs[1], "status") == "built");
  CHECK(at(outputs[1], "cached") == JsonValue(true));
  CHECK(text(outputs[1], "path").rfind(ddc, 0) == 0);
  CHECK(std::filesystem::exists(text(outputs[1], "path")));
  CHECK(text(outputs[2], "status") == "failed");
  CHECK(text(outputs[2], "rule") == "source.unreadable");
  // With a root the texture step runs; the grid samples no image, so it has nothing to build.
  CHECK(at(at(listed.result, "textures"), "enabled") == JsonValue(true));
  CHECK(number(at(listed.result, "textures"), "built") == 0);
  CHECK(at(at(outputs[1], "textures"), "detail").size() == 0);

  // Refusals are errors, not failed outputs: nothing named, both named, two destinations.
  CHECK(cli({"content.build", "{}"}).exit_code == 1);
  CHECK(cli({"content.build",
             "{\"source\":" + json_string(source) + ",\"manifest\":" + json_string(manifest) + "}"})
            .exit_code == 1);
  CHECK(cli({"content.build", "{\"source\":" + json_string(source) + "}"}).exit_code == 1);
  CHECK(cli({"content.build", "{\"source\":" + json_string(source) +
                                  ",\"output\":" + json_string(output) +
                                  ",\"options\":{\"uv_seams\":\"sometimes\"}}"})
            .exit_code == 1);
  Host host;
  REQUIRE(host.ok);
  CHECK(error_code(host.call("content.build", "{\"source\":" + json_string(source) +
                                                  ",\"output\":" + json_string(output) +
                                                  ",\"options\":{\"max_triangles\":2}}")) ==
        k_invalid_argument);
}

TEST_CASE("ops: session.events pages the journal's commits after two apply commands") {
  const test::TempDir tmp("ops_events");
  REQUIRE(tmp.ok());
  const std::string dir = tmp.file("world");
  REQUIRE(cli({"--doc", dir, "--create", "--name", "Events", "session.info"}).exit_code == 0);
  REQUIRE(
      cli({"--doc", dir, "doc.apply", apply_params("[" + create(k_a, "rock") + "]")}).exit_code ==
      0);
  REQUIRE(cli({"--doc", dir, "doc.apply",
               apply_params("[" + create(k_b, "tree") + "]", "second-actor")})
              .exit_code == 0);

  Run all = cli({"--doc", dir, "--compact", "session.events"});
  REQUIRE(all.exit_code == 0);
  const JsonValue& events = at(all.result, "events");
  REQUIRE(events.size() == 2);
  CHECK(text(events[0], "source") == "journal");
  CHECK(number(events[0], "sequence") == 0);
  CHECK(text(events[0], "kind") == "commit");
  CHECK(text(events[0], "actor") == "ops-test");
  CHECK(text(at(events[0], "attribution"), "rationale") == "day-one operations");
  CHECK(number(at(events[0], "attribution"), "timestamp_unix_ms") > 0);
  CHECK(text(events[0], "layer") == "base");
  CHECK(number(events[0], "commands") == 1);
  CHECK(at(events[0], "applied") == JsonValue(true));
  CHECK(text(events[1], "actor") == "second-actor");
  CHECK(number(all.result, "journal_total") == 2);
  CHECK(at(all.result, "more") == JsonValue(false));
  const std::string status = text(all.result, "store_status");
  CHECK((status == "absent" || status == "not built"));

  // One at a time: the cursor carries the place, and polling at the end returns nothing new.
  Run one = cli({"--doc", dir, "--compact", "session.events", "{\"limit\":1}"});
  REQUIRE(one.exit_code == 0);
  REQUIRE(at(one.result, "events").size() == 1);
  CHECK(at(one.result, "more") == JsonValue(true));
  const std::string cursor = text(one.result, "next");
  CHECK_FALSE(cursor.empty());
  Run two = cli({"--doc", dir, "--compact", "session.events",
                 "{\"limit\":1,\"cursor\":" + json_string(cursor) + "}"});
  REQUIRE(two.exit_code == 0);
  REQUIRE(at(two.result, "events").size() == 1);
  CHECK(number(at(two.result, "events")[0], "sequence") == 1);
  Run end = cli({"--doc", dir, "--compact", "session.events",
                 "{\"cursor\":" + json_string(text(two.result, "next")) + "}"});
  REQUIRE(end.exit_code == 0);
  CHECK(at(end.result, "events").size() == 0);

  // Filters and since; an undone commit is still an event, and says it is not applied.
  Run by_actor = cli({"--doc", dir, "--compact", "session.events",
                      "{\"actor\":\"second-actor\",\"source\":\"journal\"}"});
  REQUIRE(at(by_actor.result, "events").size() == 1);
  CHECK(text(by_actor.result, "store_status") == "not asked");
  Run since = cli({"--doc", dir, "--compact", "session.events", "{\"since\":1}"});
  REQUIRE(at(since.result, "events").size() == 1);
  REQUIRE(cli({"--doc", dir, "doc.undo"}).exit_code == 0);
  Run undone = cli({"--doc", dir, "--compact", "session.events"});
  REQUIRE(at(undone.result, "events").size() == 2);
  CHECK(at(at(undone.result, "events")[1], "applied") == JsonValue(false));
  CHECK(number(undone.result, "journal_position") == 1);

  // A cursor this method did not write, and a source it does not know, are refused.
  Host host;
  REQUIRE(host.ok);
  const std::string session = open_session(host, dir, false);
  CHECK(error_code(
            host.call("session.events", "{\"session\":\"" + session + "\",\"cursor\":\"x=1\"}")) ==
        k_invalid_argument);
  CHECK(error_code(
            host.call("session.events", "{\"session\":\"" + session + "\",\"source\":\"log\"}")) ==
        k_invalid_argument);
  CHECK(error_code(host.call("session.events", "{\"session\":\"nope\"}")) == k_session_not_found);
}

#if defined(ENGINE_CLI_TESTS_STORE)
TEST_CASE("ops: session.events reads the document's world.db when the store is built") {
  const test::TempDir tmp("ops_store");
  REQUIRE(tmp.ok());
  const std::string dir = tmp.file("world");
  REQUIRE(cli({"--doc", dir, "--create", "--name", "Stored", "session.info"}).exit_code == 0);
  REQUIRE(
      cli({"--doc", dir, "doc.apply", apply_params("[" + create(k_a, "rock") + "]")}).exit_code ==
      0);
  {
    store::Database db;
    REQUIRE(db.open(dir + "/world.db") == store::Status::Ok);
    store::EventLog event_log(db);
    REQUIRE(event_log.open() == store::Status::Ok);
    const u8 payload[3] = {1, 2, 3};
    store::EventRecord events[3];
    const u64 ticks[3] = {10, 12, 11};
    const store::TileId tiles[3] = {7, 7, 3};
    const store::EventOrigin origins[3] = {store::EventOrigin::Deterministic,
                                           store::EventOrigin::Player, store::EventOrigin::Agent};
    for (u32 i = 0; i < 3; ++i) {
      events[i].tile = tiles[i];
      events[i].sim_tick = ticks[i];
      events[i].game_time_us = static_cast<i64>(ticks[i]) * 16667;
      events[i].type = 40 + i;
      events[i].origin = origins[i];
      events[i].subject = Id128::from_seed(1, i);
      events[i].payload = std::span<const u8>(payload, 3);
    }
    REQUIRE(event_log.append(std::span<store::EventRecord>(events, 3)) == store::Status::Ok);
  }

  Run all = cli({"--doc", dir, "--compact", "session.events"});
  REQUIRE(all.exit_code == 0);
  CHECK(text(all.result, "store_status") == "read");
  const JsonValue& events = at(all.result, "events");
  REQUIRE(events.size() == 4);  // the journal's commit, then the log's three by tick
  CHECK(text(events[0], "source") == "journal");
  CHECK(text(events[1], "source") == "store");
  CHECK(number(events[1], "tick") == 10);
  CHECK(number(events[2], "tick") == 11);
  CHECK(number(events[2], "tile") == 3);
  CHECK(text(events[2], "actor") == "agent");
  CHECK(text(events[3], "kind") == "41");
  CHECK(number(events[3], "payload_bytes") == 3);

  // Only the log, filtered by origin; then paged by two with the cursor.
  Run players = cli(
      {"--doc", dir, "--compact", "session.events", "{\"source\":\"store\",\"actor\":\"player\"}"});
  REQUIRE(at(players.result, "events").size() == 1);
  CHECK(number(at(players.result, "events")[0], "tick") == 12);
  Run page =
      cli({"--doc", dir, "--compact", "session.events", "{\"source\":\"store\",\"limit\":2}"});
  REQUIRE(at(page.result, "events").size() == 2);
  CHECK(at(page.result, "more") == JsonValue(true));
  Run rest =
      cli({"--doc", dir, "--compact", "session.events",
           "{\"source\":\"store\",\"cursor\":" + json_string(text(page.result, "next")) + "}"});
  REQUIRE(at(rest.result, "events").size() == 1);
  CHECK(number(at(rest.result, "events")[0], "tick") == 12);
  Run late =
      cli({"--doc", dir, "--compact", "session.events", "{\"source\":\"store\",\"since\":11}"});
  CHECK(at(late.result, "events").size() == 2);
}
#else
TEST_CASE(
    "ops: session.events without the store capability answers the journal and refuses the log") {
  const test::TempDir tmp("ops_store");
  REQUIRE(tmp.ok());
  const std::string dir = tmp.file("world");
  Host host;
  REQUIRE(host.ok);
  const std::string session = open_session(host, dir, true);
  const JsonValue both = host.call("session.events", "{\"session\":\"" + session + "\"}");
  CHECK(text(result_of(both), "store_status") == "not built");
  CHECK(error_code(host.call("session.events", "{\"session\":\"" + session +
                                                   "\",\"source\":\"store\"}")) == k_unavailable);
}
#endif

TEST_CASE("ops: engine.budgets names every budget, with finite numbers, the same way twice") {
  Run first = cli({"--compact", "engine.budgets", "{\"scope\":\"process\"}"});
  REQUIRE(first.exit_code == 0);
  const JsonValue& budgets = at(first.result, "budgets");
  std::vector<std::string> names;
  for (usize i = 0; i < budgets.size(); ++i) {
    const JsonValue& b = budgets[i];
    const std::string name = text(b, "name");
    CAPTURE(name);
    names.push_back(name);
    CHECK_FALSE(text(b, "doc").empty());
    CHECK_FALSE(text(b, "unit").empty());
    CHECK_FALSE(text(b, "source").empty());
    CHECK(text(b, "scope") == "process");
    for (const char* key : {"limit", "used", "peak"}) {
      const JsonValue& v = at(b, key);
      if (v.is_null()) continue;
      f64 x = 0.0;
      REQUIRE(v.get_f64(x));
      CHECK(std::isfinite(x));
      CHECK(x >= 0.0);
    }
  }
  const auto has = [&](const char* name) {
    for (const std::string& n : names) {
      if (n == name) return true;
    }
    return false;
  };
  for (const char* name : {"renderer.deform_pool", "renderer.page_budget", "renderer.upload_budget",
                           "renderer.rt", "memory.allocations", "memory.heap"}) {
    CAPTURE(name);
    CHECK(has(name));
  }
  // The rt budget is a tunable, and says which one; the allocation counter is counted, not bounded.
  for (usize i = 0; i < budgets.size(); ++i) {
    if (text(budgets[i], "name") == "renderer.rt") {
      CHECK(text(budgets[i], "source") == "tunable");
      CHECK(text(budgets[i], "key") == "renderer.rt.budget_mib");
      CHECK(real(budgets[i], "limit") == 1024.0 * 1024.0 * 1024.0);
    }
    if (text(budgets[i], "name") == "memory.allocations") {
      CHECK(at(budgets[i], "limit").is_null());
      CHECK(real(budgets[i], "used") > 0.0);
    }
  }
  // Audio's two are there exactly when the host has the audio capability.
  const bool audio = cli({"audio.devices"}).exit_code == 0;
  CHECK(has("audio.clips") == audio);
  CHECK(has("audio.voices") == audio);

  // Stable: the same names in the same order from a second host.
  Run second = cli({"--compact", "engine.budgets", "{\"scope\":\"process\"}"});
  REQUIRE(second.exit_code == 0);
  REQUIRE(at(second.result, "budgets").size() == budgets.size());
  for (usize i = 0; i < budgets.size(); ++i)
    CHECK(text(at(second.result, "budgets")[i], "name") == names[i]);

  // Every scope at once still parses, and a GPU, where there is one, reports its memory.
  Run everything = cli({"--compact", "engine.budgets"});
  REQUIRE(everything.exit_code == 0);
  for (usize i = 0; i < at(everything.result, "budgets").size(); ++i) {
    const JsonValue& b = at(everything.result, "budgets")[i];
    if (text(b, "name") != "gpu.memory") continue;
    CHECK(text(b, "scope").rfind("adapter:", 0) == 0);
    CHECK(real(b, "limit") > 0.0);
  }
}

TEST_CASE("ops: session.run_headless steps 0.5 s at the fixed step, and stops on a predicate") {
  const test::TempDir tmp("ops_headless");
  REQUIRE(tmp.ok());
  const std::string dir = tmp.file("world");
  REQUIRE(cli({"--doc", dir, "--create", "--name", "Headless", "session.info"}).exit_code == 0);
  REQUIRE(
      cli({"--doc", dir, "doc.apply", apply_params("[" + create(k_a, "rock") + "]")}).exit_code ==
      0);

  // Through engine-cli: a fresh host, a fresh world, thirty ticks.
  Run half = cli({"--doc", dir, "--compact", "session.run_headless", "{\"seconds\":0.5}"});
  REQUIRE(half.exit_code == 0);
  CHECK(number(half.result, "ticks") == 30);
  CHECK(number(half.result, "tick") == 30);
  CHECK(number(half.result, "hz") == 60);
  CHECK(std::abs(real(half.result, "game_seconds") - 0.5) < 0.001);
  CHECK(text(half.result, "stopped") == "seconds");
  CHECK(at(half.result, "predicate").is_null());
  CHECK(real(half.result, "wall_ms") >= 0.0);
  CHECK(number(half.result, "materialized") == 0);
  const std::string world = text(half.result, "world");
  CHECK((world == "ecs" || world == "scheduler"));
  CHECK(at(half.result, "systems").is_array());

  // One host: the world persists, so a second call continues from tick 30.
  Host host;
  REQUIRE(host.ok);
  const std::string session = open_session(host, dir, false);
  const std::string s = "{\"session\":\"" + session + "\"";
  const JsonValue first = host.call("session.run_headless", s + ",\"seconds\":0.5}");
  CHECK(number(result_of(first), "tick") == 30);
  const JsonValue second = host.call("session.run_headless", s + ",\"seconds\":0.5}");
  CHECK(number(result_of(second), "ticks") == 30);
  CHECK(number(result_of(second), "tick") == 60);
  CHECK(number(result_of(second), "game_time_us") == 60u * 16667u);

  // A predicate that turns true: the object is as the document says, and a quarter of a second of
  // this call has passed — fifteen ticks, with ten seconds as the most it may run.
  const std::string rock =
      std::string("{\"object\":\"") + k_a + "\",\"property\":\"generator\",\"equals\":\"rock\"}";
  const JsonValue until =
      host.call("session.run_headless", s + ",\"seconds\":10,\"until\":{\"all\":[" + rock +
                                            ",{\"seconds_at_least\":0.25}]}}");
  const JsonValue& turned = result_of(until);
  CHECK(number(turned, "ticks") == 15);
  CHECK(at(turned, "predicate") == JsonValue(true));
  CHECK(text(turned, "stopped") == "predicate");

  // One that already holds runs nothing; one that never holds runs out the time and says false.
  const JsonValue already =
      host.call("session.run_headless", s + ",\"seconds\":1,\"until\":" + rock + "}");
  CHECK(number(result_of(already), "ticks") == 0);
  CHECK(at(result_of(already), "predicate") == JsonValue(true));
  const JsonValue never =
      host.call("session.run_headless", s + ",\"seconds\":0.1,\"until\":{\"any\":[{\"object\":\"" +
                                            k_b + "\",\"exists\":true},{\"not\":" + rock + "}]}}");
  CHECK(number(result_of(never), "ticks") == 6);
  CHECK(at(result_of(never), "predicate") == JsonValue(false));
  CHECK(text(result_of(never), "stopped") == "seconds");

  // Refusals: a term that is not one, a malformed id, a run past an hour.
  CHECK(error_code(
            host.call("session.run_headless", s + ",\"seconds\":1,\"until\":{\"sometime\":1}}")) ==
        k_invalid_argument);
  CHECK(
      error_code(host.call("session.run_headless",
                           s + ",\"seconds\":1,\"until\":{\"object\":\"xyz\",\"exists\":true}}")) ==
      k_invalid_argument);
  CHECK(error_code(host.call("session.run_headless", s + ",\"seconds\":4000}")) ==
        k_invalid_argument);
  CHECK(error_code(host.call("session.run_headless", "{\"session\":\"nope\",\"seconds\":1}")) ==
        k_session_not_found);
}

// ---- the materialized world (docs/subsystems/protocol.md, "session.run_headless") --------------

namespace {

// Three records of two types, one parent-child pair: a yard (a Node) with a cart in it (a Mover,
// one metre a second along x), and a buoy (a Mover at the root, turning a quarter turn a second).
const char* k_yard = "00000000000000200000000000000001";
const char* k_cart = "00000000000000200000000000000002";
const char* k_buoy = "00000000000000200000000000000003";

std::string yard_commands() {
  return std::string("[") + "{\"kind\":\"CreateObject\",\"id\":\"" + k_yard +
         "\",\"type\":\"engine.world.Node\",\"value\":{\"name\":\"yard\",\"position\":[0,0,0]}}," +
         "{\"kind\":\"CreateObject\",\"id\":\"" + k_cart +
         "\",\"type\":\"engine.kinematics.Mover\"," + "\"parent\":\"" + k_yard +
         "\",\"value\":{\"name\":\"cart\",\"position\":[1,0,0],\"velocity\":[1,0,0]}}," +
         "{\"kind\":\"CreateObject\",\"id\":\"" + k_buoy +
         "\",\"type\":\"engine.kinematics.Mover\",\"value\":{\"name\":\"buoy\",\"position\":[0,0,"
         "5],\"spin\":[0,90,0]}}]";
}

const JsonValue* type_row(const JsonValue& report, std::string_view type) {
  const JsonValue& types = at(report, "types");
  for (usize i = 0; i < types.size(); ++i) {
    if (text(types[i], "type") == type) return &types[i];
  }
  return nullptr;
}

}  // namespace

#if defined(ENGINE_CLI_TESTS_KINEMATICS)
TEST_CASE(
    "ops: run_headless materializes the document, ticks it, and a system's change comes back") {
  const test::TempDir tmp("ops_materialize");
  REQUIRE(tmp.ok());
  const std::string dir = tmp.file("yard");
  REQUIRE(cli({"--doc", dir, "--create", "--name", "Yard", "session.info"}).exit_code == 0);
#if defined(ENGINE_CLI_TESTS_STORE)
  {
    // The document keeps a world.db, so a write-back is also an event in the store's log.
    store::Database db;
    REQUIRE(db.open(dir + "/world.db") == store::Status::Ok);
    store::EventLog event_log(db);
    REQUIRE(event_log.open() == store::Status::Ok);
  }
#endif
  Host host;
  REQUIRE(host.ok);
  const std::string session = open_session(host, dir, false);
  const std::string s = "{\"session\":\"" + session + "\"";
  REQUIRE(at(result_of(host.call("doc.apply", s + ",\"commands\":" + yard_commands() +
                                                  ",\"attribution\":{\"actor\":\"ops-test\","
                                                  "\"role\":\"environment\",\"task\":\"t\"}}")),
             "committed") == JsonValue(true));

  // Half a second: thirty ticks of the materialized world, with the write-back at its default
  // cadence (a game second) and the flush every call ends with.
  const JsonValue first = host.call("session.run_headless", s + ",\"seconds\":0.5}");
  const JsonValue& run = result_of(first);
  CHECK(number(run, "ticks") == 30);
  CHECK(text(run, "world") == "ecs");
  CHECK(text(run, "executor") == "scheduler");
  CHECK(number(run, "materialized") == 3);
  CHECK(number(run, "entities") == 3);
  const JsonValue& pass = at(run, "materialize");
  CHECK(at(pass, "full") == JsonValue(true));
  CHECK(number(pass, "created") == 3);
  const JsonValue* movers = type_row(pass, "engine.kinematics.Mover");
  REQUIRE(movers != nullptr);
  CHECK(number(*movers, "materialized") == 2);
  bool integrates = false;
  const JsonValue& systems = at(run, "systems");
  for (usize i = 0; i < systems.size(); ++i)
    integrates = integrates || systems[i].as_string() == "kinematics.integrate";
  CHECK(integrates);
  // The cart moved and the buoy turned: two records, two writable fields, one commit.
  CHECK(number(run, "write_backs") == 1);
  CHECK(number(run, "written_fields") == 2);

  // The document says where the world stopped.
  const JsonValue cart = host.call("doc.get", s + ",\"id\":\"" + k_cart + "\"}");
  const f64 x = at(at(result_of(cart), "properties"), "position")[0].as_float();
  CHECK(std::abs(x - 1.5) < 1e-4);

  // And the journal says who wrote it: the system, not an agent.
  const JsonValue events =
      host.call("session.events", s + ",\"source\":\"journal\",\"actor\":\"system\"}");
  const JsonValue& written = at(result_of(events), "events");
  REQUIRE(written.size() == 1);
  CHECK(text(written[0], "kind") == "commit");
  CHECK(text(at(written[0], "attribution"), "task") == "sim.write_back");
  CHECK(number(written[0], "commands") == 2);
#if defined(ENGINE_CLI_TESTS_STORE)
  // One event per record it changed, in the store's log, from the deterministic simulation.
  const JsonValue logged = host.call("session.events", s + ",\"source\":\"store\"}");
  CHECK(text(result_of(logged), "store_status") == "read");
  const JsonValue& rows = at(result_of(logged), "events");
  REQUIRE(rows.size() == 2);
  CHECK(text(rows[0], "actor") == "deterministic");
  CHECK(number(rows[0], "tick") == 30);
  CHECK(text(rows[0], "subject") == k_cart);
  CHECK(text(rows[1], "subject") == k_buoy);
#endif

  // Again, with a write-back every tick and a predicate over both halves: the cart's position as
  // the document holds it and as its live Transform holds it. It turns true about twelve ticks in,
  // when the cart passes 1.7 m.
  const std::string past =
      std::string("{\"all\":[{\"object\":\"") + k_cart +
      "\",\"property\":\"position[0]\",\"at_least\":1.7},{\"object\":\"" + k_cart +
      "\",\"component\":\"engine.world.Transform\",\"field\":\"position[0]\",\"at_least\":1.7}]}";
  const JsonValue second = host.call(
      "session.run_headless", s + ",\"seconds\":1,\"write_back_every\":1,\"until\":" + past + "}");
  const JsonValue& until = result_of(second);
  CHECK(at(until, "predicate") == JsonValue(true));
  CHECK(text(until, "stopped") == "predicate");
  CHECK(number(until, "ticks") >= 11);
  CHECK(number(until, "ticks") <= 13);
  CHECK(number(until, "write_backs") == number(until, "ticks"));
  // The second call's materialization followed the document's change feed: the two records it saw
  // change were changed by this world's own write-back, so nothing was handed to a hook.
  const JsonValue& again = at(until, "materialize");
  CHECK(at(again, "full") == JsonValue(false));
  CHECK(number(again, "unchanged") == 2);
  CHECK(number(again, "created") + number(again, "updated") == 0);

  // An agent's edit between calls reaches the world on the next one.
  REQUIRE(at(result_of(host.call(
                 "doc.apply",
                 s + ",\"commands\":[{\"kind\":\"SetProperty\",\"id\":\"" + k_cart +
                     "\",\"name\":\"velocity\",\"value\":[0,0,0]}],\"attribution\":{\"actor\":"
                     "\"ops-test\",\"role\":\"environment\",\"task\":\"t\"}}")),
             "committed") == JsonValue(true));
  const JsonValue stopped = host.call("session.run_headless", s + ",\"seconds\":0.25}");
  const JsonValue& still = result_of(stopped);
  CHECK(number(at(still, "materialize"), "updated") == 1);
  // The buoy still turns; the cart no longer moves.
  CHECK(number(still, "written_fields") == 1);
}
#endif

// ---- a streamed world (docs/subsystems/world.md; protocol.md, "session.run_headless") ----------

#if defined(ENGINE_CLI_TESTS_WORLD)
TEST_CASE("ops: run_headless streams a partitioned document tile by tile round its observers") {
  const test::TempDir tmp("ops_world");
  REQUIRE(tmp.ok());
  const std::string dir = tmp.file("desert");
  REQUIRE(cli({"--doc", dir, "--create", "--name", "Desert", "session.info"}).exit_code == 0);
  REQUIRE(cli({"--doc", dir, "doc.add_layer",
               R"({"name":"places","partition":{"property":"position","tile_size":32}})"})
              .exit_code == 0);
  // Three places: one in the tile at the origin, one in the tile east of it, one two kilometres
  // away. Nodes, the engine's own record type, so they materialize wherever there is an entity
  // store.
  auto node = [](const char* id, f64 x, f64 z) {
    return std::string("{\"kind\":\"CreateObject\",\"id\":\"") + id +
           "\",\"type\":\"engine.world.Node\",\"value\":{\"name\":\"n\",\"position\":[" +
           std::to_string(x) + ",0.0," + std::to_string(z) + "]}}";
  };
  const char* k_near = "00000000000000300000000000000001";
  const char* k_east = "00000000000000300000000000000002";
  const char* k_far = "00000000000000300000000000000003";
  REQUIRE(cli({"--doc", dir, "doc.apply",
               apply_params("[" + node(k_near, 5, 5) + "," + node(k_east, 40, 5) + "," +
                            node(k_far, 2005, 5) + "]")})
              .exit_code == 0);

  Host host;
  REQUIRE(host.ok);
  const std::string session = open_session(host, dir, false);
  const std::string s = "{\"session\":\"" + session + "\"";
  // Rings of 1.5 and 3 tiles, the simulation in the inner one: from the origin, the four tiles
  // round it — the near place's among them; the east place's tile is in the outer ring only.
  auto stream = [](f64 x, f64 vx) {
    return std::string(",\"stream\":{\"tile_size\":32,\"rings\":[1.5,3],\"simulated\":1,") +
           "\"observers\":[{\"position\":[" + std::to_string(x) + ",1.7,0],\"velocity\":[" +
           std::to_string(vx) + ",0,0]}]}}";
  };
  const JsonValue first = host.call("session.run_headless", s + ",\"seconds\":0.1" + stream(0, 0));
  const JsonValue& run = result_of(first);
  CHECK(number(run, "ticks") == 6);
  const JsonValue& world = at(run, "streamed");
  REQUIRE(world.is_object());
  CHECK(number(world, "updates") == 7);  // the call's first fill, then one between every two ticks
  CHECK(number(world, "materialized_tiles") == 4);
  const JsonValue& tiles = at(world, "tiles");
  u32 inner = 0;
  for (usize i = 0; i < tiles.size(); ++i)
    inner += number(tiles[i], "ring") == 0 ? 1u : 0u;
  CHECK(inner == 4);
  CHECK(tiles.size() > 4);
  CHECK(at(world, "store") == JsonValue(true));
  CHECK(number(world, "reconciled") == 4);
  CHECK(number(world, "known") == 0);
#if defined(ENGINE_CLI_TESTS_KINEMATICS)
  // With an entity store: the near place is an entity, the other two are not.
  CHECK(number(world, "created") == 1);
  CHECK(number(run, "materialized") == 1);
#endif

  // Two kilometres east: the origin's tiles go (and are written to the store), the far place's
  // tile comes in.
  const JsonValue second =
      host.call("session.run_headless", s + ",\"seconds\":0.1" + stream(2000, 0));
  const JsonValue& away = at(result_of(second), "streamed");
  CHECK(number(away, "dematerialized_tiles") == 4);
  CHECK(number(away, "written") >= 4);
  CHECK(number(away, "materialized_tiles") == 6);  // on a tile boundary: three columns of two
#if defined(ENGINE_CLI_TESTS_KINEMATICS)
  CHECK(number(away, "dematerialized") == 1);
  CHECK(number(away, "created") == 1);
#endif

  // Walking back at 600 m/s: a second and a bit of ticks, the budget spreading the tiles over them,
  // and the origin's tiles reconciled from what the store kept of them.
  const JsonValue third =
      host.call("session.run_headless", s + ",\"seconds\":3.3" + stream(2000, -600));
  const JsonValue& back = at(result_of(third), "streamed");
  CHECK(number(back, "updates") == number(result_of(third), "ticks") + 1);
  CHECK(number(back, "activated") > 20);
  CHECK(number(back, "deactivated") > 20);
  CHECK(number(back, "known") >= 4);
  bool origin = false;
  const JsonValue& now = at(back, "tiles");
  for (usize i = 0; i < now.size(); ++i) {
    origin = origin ||
             (number(now[i], "x") == 0 && number(now[i], "z") == 0 && number(now[i], "ring") == 0);
  }
  CHECK(origin);

  // Rings that do not grow outward are refused with a sentence, on a session with no world yet
  // (the ring's parameters are its first call's, so the desert's would not be looked at again).
  const std::string other_dir = tmp.file("other");
  REQUIRE(cli({"--doc", other_dir, "--create", "--name", "Other", "session.info"}).exit_code == 0);
  const std::string other = open_session(host, other_dir, false);
  CHECK(error_code(host.call(
            "session.run_headless",
            "{\"session\":\"" + other + "\",\"seconds\":0.1,\"stream\":{\"rings\":[3,1.5]}}")) ==
        k_invalid_argument);
}
#endif

TEST_CASE("ops: session.materialize reports which types mapped and which records were skipped") {
  const test::TempDir tmp("ops_materialize");
  REQUIRE(tmp.ok());
  const std::string dir = tmp.file("yard");
  REQUIRE(cli({"--doc", dir, "--create", "--name", "Yard", "session.info"}).exit_code == 0);
  // The yard and its movers, and a provenance record: authored data no world needs as an entity.
  REQUIRE(cli({"--doc", dir, "doc.apply", apply_params(yard_commands())}).exit_code == 0);
  REQUIRE(
      cli({"--doc", dir, "doc.apply", apply_params("[" + create(k_a, "rock") + "]")}).exit_code ==
      0);

  Run report = cli({"--doc", dir, "--compact", "session.materialize", "{}"});
  REQUIRE(report.exit_code == 0);
  const JsonValue& r = report.result;
  CHECK(at(r, "full") == JsonValue(true));
  CHECK(number(r, "visited") == 4);
  const JsonValue* provenance = type_row(r, k_type);
  REQUIRE(provenance != nullptr);
  CHECK(at(*provenance, "mapped") == JsonValue(false));
  CHECK(text(*provenance, "reason") == "no mapping");

  // Every mapping this build compiled is listed, the world's own `Node` among them.
  bool node = false;
  const JsonValue& mappings = at(r, "mappings");
  for (usize i = 0; i < mappings.size(); ++i) {
    if (text(mappings[i], "record") != "engine.world.Node") continue;
    node = true;
    CHECK(text(mappings[i], "parent") == "ChildOf");
    CHECK(at(mappings[i], "components")[0].as_string() == "engine.world.Transform");
    CHECK(at(at(mappings[i], "fields")[0], "write_back") == JsonValue(true));
  }
  CHECK(node);

  const JsonValue* nodes = type_row(r, "engine.world.Node");
  REQUIRE(nodes != nullptr);
  CHECK(at(*nodes, "mapped") == JsonValue(true));
#if defined(ENGINE_CLI_TESTS_KINEMATICS)
  CHECK(number(r, "live") == 3);
  CHECK(number(r, "skipped") == 1);
  CHECK(number(*nodes, "materialized") == 1);
#else
  // Without the capability that declares Mover its records are of a type this build never heard
  // of, and without the entity store nothing becomes an entity at all: the report says which,
  // rather than returning an empty world.
  const JsonValue* movers = type_row(r, "engine.kinematics.Mover");
  REQUIRE(movers != nullptr);
  CHECK(number(*movers, "skipped") == 2);
  CHECK(text(*movers, "reason") == "unknown type");
  CHECK(number(*nodes, "materialized") + number(*nodes, "skipped") == 1);
  if (number(*nodes, "skipped") == 1) CHECK(text(*nodes, "reason") == "no entity store");
#endif

  // Refusals: a scope that is not one, and a session that is not open.
  Run bad = cli({"--doc", dir, "--compact", "session.materialize", "{\"scope\":\"everywhere\"}"});
  CHECK(bad.exit_code != 0);
}

TEST_CASE("ops: engine.run_tests over a valid and an invalid document and two containers") {
  const test::TempDir tmp("ops_tests");
  REQUIRE(tmp.ok());
  const std::string good = tmp.file("good");
  const std::string bad = tmp.file("bad");
  REQUIRE(cli({"--doc", good, "--create", "--name", "Good", "session.info"}).exit_code == 0);
  REQUIRE(
      cli({"--doc", good, "doc.apply", apply_params("[" + create(k_a, "rock") + "]")}).exit_code ==
      0);
  REQUIRE(cli({"--doc", bad, "--create", "--name", "Bad", "session.info"}).exit_code == 0);
  // A property the type does not have: accepted by the edit (a document may be mid-change), and
  // exactly what the validator is for.
  REQUIRE(cli({"--doc", bad, "doc.apply",
               apply_params(std::string("[") + create(k_a, "rock") +
                            ",{\"kind\":\"SetProperty\",\"id\":\"" + k_a +
                            "\",\"name\":\"no_such_field\",\"value\":1}]")})
              .exit_code == 0);

  Run passed = cli({"--doc", good, "--compact", "engine.run_tests"});
  REQUIRE(passed.exit_code == 0);
  CHECK(at(passed.result, "ok") == JsonValue(true));
  CHECK(number(passed.result, "passed") == 2);
  CHECK(number(passed.result, "failed") == 0);
  const JsonValue& checks = at(passed.result, "checks");
  REQUIRE(checks.size() == 2);
  CHECK(text(checks[0], "id") == "doc.schema");
  CHECK(text(checks[0], "verdict") == "pass");
  CHECK(text(checks[1], "id") == "doc.index");

  Run failed = cli({"--doc", bad, "--compact", "engine.run_tests"});
  REQUIRE(failed.exit_code == 0);
  CHECK(at(failed.result, "ok") == JsonValue(false));
  CHECK(number(failed.result, "errors") == 1);
  const JsonValue& schema = at(failed.result, "checks")[0];
  CHECK(text(schema, "verdict") == "fail");
  REQUIRE(at(schema, "diagnostics").size() >= 1);
  CHECK(text(at(schema, "diagnostics")[0], "path").find("no_such_field") != std::string::npos);

  // The content checks over a container content.build writes, and over a file that is not one.
  REQUIRE(write_grid(tmp.path(), "grid", 16));
  const std::string container = tmp.file("grid.clusters");
  REQUIRE(cli({"content.build", "{\"source\":" + json_string(tmp.file("grid.gltf")) +
                                    ",\"output\":" + json_string(container) + ",\"stats\":false}"})
              .exit_code == 0);
  REQUIRE(write_text(tmp.file("junk.clusters"), "not a container"));
  Run content = cli({"--compact", "engine.run_tests",
                     "{\"containers\":[" + json_string(container) + "," +
                         json_string(tmp.file("junk.clusters")) + "]}"});
  REQUIRE(content.exit_code == 0);
  CHECK(at(content.result, "ok") == JsonValue(false));
  const JsonValue& rows = at(content.result, "checks");
  u32 good_passed = 0;
  u32 junk_failed = 0;
  u32 junk_skipped = 0;
  for (usize i = 0; i < rows.size(); ++i) {
    CHECK(text(rows[i], "suite") == "content");
    if (text(rows[i], "subject") == container) {
      CAPTURE(text(rows[i], "id"));
      CAPTURE(text(rows[i], "message"));
      CHECK(text(rows[i], "verdict") == "pass");
      ++good_passed;
    } else {
      if (text(rows[i], "verdict") == "fail") ++junk_failed;
      if (text(rows[i], "verdict") == "skipped") ++junk_skipped;
    }
  }
  CHECK(good_passed == 6);  // read, cluster budget, lod, pages, identity, images
  CHECK(junk_failed == 1);  // content.read
  CHECK(junk_skipped == 5);

  // The tissue validators, when the host has them; a clear refusal when it does not.
  Host host;
  REQUIRE(host.ok);
#if defined(ENGINE_CLI_TESTS_TISSUE)
  const std::string example = tmp.file("tissue");
  {
    const std::string content_path = test::app_path(ENGINE_CONTENT_PATH);
    const std::string_view argv[4] = {content_path, "tissue", "example", example};
    platform::Process p;
    std::string error;
    REQUIRE_MESSAGE(p.spawn(std::span<const std::string_view>(argv, 4), &error), error);
    p.close_stdin();
    std::string ignored;
    p.read_all(ignored);
    REQUIRE(p.wait() == 0);
  }
  const JsonValue tissue =
      host.call("engine.run_tests", "{\"tissue\":" + json_string(example + "/synthetic.tissue") +
                                        ",\"tissue_modes\":false}");
  const JsonValue& report = result_of(tissue);
  REQUIRE(at(report, "checks").size() > 1);
  CHECK(text(at(report, "checks")[0], "id") == "tissue.read");
  CHECK(text(at(report, "checks")[0], "verdict") == "pass");
  for (usize i = 0; i < at(report, "checks").size(); ++i)
    CHECK(text(at(report, "checks")[i], "suite") == "tissue");
#else
  CHECK(error_code(host.call("engine.run_tests", "{\"tissue\":\"any.tissue\"}")) == k_unavailable);
#endif
  CHECK(error_code(host.call("engine.run_tests", "{\"session\":\"nope\"}")) == k_session_not_found);
  // Nothing asked, nothing checked, and nothing failed.
  const JsonValue empty = host.call("engine.run_tests", "{}");
  CHECK(at(result_of(empty), "checks").size() == 0);
  CHECK(at(result_of(empty), "ok") == JsonValue(true));
}
