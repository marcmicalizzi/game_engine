// End to end: save and load of a running world, and the replay assertion of plan 05 §5.10 ("input
// log + event log => replay; CI runs replays and asserts identical persistent-state hashes";
// docs/subsystems/world.md, "Save and load"; protocol.md, "session.save_game").
//
// One document, three ways to the same tick:
//
//   (a) continuous  one engine-host runs it for K ticks, saves, and runs on to N
//   (b) loaded      a fresh engine-host loads the tick-K save and runs on to N
//   (c) replayed    a third runs a copy of the starting document from tick 0 to N in one call
//
// with the same scripted observer (a walk at a constant velocity), the same player (an observer an
// input log steers through an action map) and the same write-back cadence. The persistent-state
// hash (`session.state_hash`) of all three at N must be one number, and the save loaded and saved
// again must be the same files byte for byte. The world streams a partitioned document of places
// and — where the host has the kinematics capability — carts that drive across tile boundaries
// while the tiles round them come and go, which is what made a record's tile history-dependent
// before records followed their tile (world.md, "Records that move").
#include <core/json/json.h>
#include <core/platform/process.h>
#include <foundation/input/input.h>
#include <foundation/input/input_log.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace engine;

namespace {

const std::string& host_exe() {
  static const std::string path = test::app_path(ENGINE_HOST_PATH);
  return path;
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

std::string json_string(std::string_view s) {
  std::string out = "\"";
  for (const char c : s) {
    if (c == '\\' || c == '"') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

}  // namespace

#if defined(ENGINE_CLI_TESTS_WORLD)

namespace {

constexpr i32 k_invalid_argument = 1004;
constexpr i32 k_validation_failed = 1002;
constexpr i32 k_session_not_found = 1000;

std::string error_message(const JsonValue& response) {
  const JsonValue* error = response.find("error");
  const JsonValue* message = error != nullptr ? error->find("message") : nullptr;
  std::string_view text;
  return message != nullptr && message->get_string(text) ? std::string(text) : std::string();
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

std::string text(const JsonValue& o, std::string_view key) {
  std::string_view out;
  const JsonValue* v = o.find(key);
  return v != nullptr && v->get_string(out) ? std::string(out) : std::string();
}

std::string file_bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string open_session(Host& host, const std::string& dir, bool create) {
  const JsonValue opened = host.call(
      "session.open", "{\"path\":" + json_string(dir) + (create ? ",\"create\":true}" : "}"));
  return text(result_of(opened), "session");
}

std::string attribution() {
  return R"("attribution":{"actor":"save-test","role":"environment","task":"t","rationale":"a world to replay"})";
}

std::string num(f64 v) {
  std::string out;
  write_json(JsonValue(v), out, JsonWriteOptions{false});
  return out;
}

std::string create(const std::string& id, const char* type, const std::string& value,
                   const std::string& parent = std::string()) {
  return "{\"kind\":\"CreateObject\",\"id\":\"" + id + "\",\"type\":\"" + type + "\"" +
         (parent.empty() ? "" : ",\"parent\":\"" + parent + "\"") + ",\"value\":" + value + "}";
}

std::string id_of(u32 n) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "0000000000000050%016x", n);
  return buf;
}

// The starting document: a partitioned layer of places on a strip of tiles along +x, and — where
// the host can move them — carts set to drive across the tiles' edges, one of them a place's child.
void make_world(Host& host, const std::string& dir) {
  const std::string s = "{\"session\":\"" + open_session(host, dir, true) + "\"";
  REQUIRE(at(result_of(host.call(
                 "doc.add_layer",
                 s + R"(,"name":"places","partition":{"property":"position","tile_size":32}})")),
             "layers")
              .is_array());
  std::string commands = "[";
  u32 n = 1;
  for (i32 x = -2; x <= 13; ++x) {
    for (i32 z = -1; z <= 1; ++z) {
      if (n > 1) commands += ",";
      commands += create(id_of(n++), "engine.world.Node",
                         "{\"name\":\"place\",\"position\":[" + num(x * 32.0 + 7.5) + ",0.0," +
                             num(z * 32.0 + 11.25) + "]}");
    }
  }
#if defined(ENGINE_CLI_TESTS_KINEMATICS)
  struct Cart {
    f64 x, z, vx, vz, spin;
  };
  const Cart carts[] = {{30.0, 5.0, 3.0, 0.0, 0.0},     {35.0, 30.0, 0.0, 2.5, 0.0},
                        {95.0, -3.0, -4.0, 1.5, 45.0},  {160.0, 20.0, 6.0, 0.0, 0.0},
                        {200.0, 33.0, 1.0, -3.0, 90.0}, {250.0, -30.0, 0.5, 5.0, 0.0}};
  const std::string parent = id_of(4);  // a place in tile (-1, 0): the first cart's parent
  u32 c = 0;
  for (const Cart& cart : carts) {
    commands += "," + create(id_of(1000 + c), "engine.kinematics.Mover",
                             "{\"name\":\"cart\",\"position\":[" + num(cart.x) + ",0.0," +
                                 num(cart.z) + "],\"velocity\":[" + num(cart.vx) + ",0.0," +
                                 num(cart.vz) + "],\"spin\":[0.0," + num(cart.spin) + ",0.0]}",
                             c == 0 ? parent : std::string());
    ++c;
  }
#endif
  commands += "]";
  const JsonValue applied =
      host.call("doc.apply", s + ",\"commands\":" + commands + "," + attribution() + "}");
  REQUIRE(at(result_of(applied), "committed") == JsonValue(true));
}

// The player's map and log: WASD on one Axis2, and a walk that turns four times over ten seconds.
void make_input(const std::string& map_path, const std::string& log_path) {
  input::ActionMap map;
  const input::ActionId move = map.add_action("move", input::ActionKind::Axis2);
  map.bind(move, input::Binding{input::Source::Key, 7, 1.0f, 0.0f}, 0);    // D
  map.bind(move, input::Binding{input::Source::Key, 4, -1.0f, 0.0f}, 0);   // A
  map.bind(move, input::Binding{input::Source::Key, 26, 1.0f, 0.0f}, 1);   // W
  map.bind(move, input::Binding{input::Source::Key, 22, -1.0f, 0.0f}, 1);  // S
  {
    std::ofstream out(map_path, std::ios::binary);
    out << write_json(map.to_json()) << "\n";
  }
  input::InputLog log;
  log.set_map(map);
  auto key = [&](u64 tick, u32 code, f32 value) {
    input::RawEvent e;
    e.tick = SimTick{tick};
    e.source = input::Source::Key;
    e.code = code;
    e.value = value;
    log.record(e);
  };
  key(30, 26, 1.0f);   // W: forward, -z
  key(90, 7, 1.0f);    // and D, the diagonal; D is still held when the save is taken at 300
  key(160, 26, 0.0f);  // D alone
  key(360, 22, 1.0f);  // S with D
  key(420, 7, 0.0f);
  key(480, 4, 1.0f);  // A with S
  key(520, 22, 0.0f);
  key(590, 4, 0.0f);
  REQUIRE(log.save(log_path) == io::Status::Ok);
}

std::string stream_params(const std::string& map_path, const std::string& log_path) {
  return ",\"stream\":{\"tile_size\":32,\"rings\":[1.5,3],\"simulated\":1,\"observers\":["
         "{\"position\":[0.0,1.7,0.0],\"velocity\":[20.0,0.0,0.0]},"
         "{\"position\":[16.0,1.7,16.0],\"velocity\":[0.0,0.0,0.0]}],"
         "\"player\":{\"log\":" +
         json_string(log_path) + ",\"map\":" + json_string(map_path) +
         ",\"action\":\"move\",\"speed\":10,\"observer\":1}}";
}

struct Hash {
  std::string value;
  JsonValue whole;
};

Hash state_hash(Host& host, const std::string& session) {
  const JsonValue r = host.call("session.state_hash", "{\"session\":\"" + session + "\"}");
  Hash h;
  h.whole = result_of(r);
  h.value = text(h.whole, "hash");
  return h;
}

void same_parts(const Hash& a, const Hash& b) {
  for (const char* part : {"hash", "clock", "document", "events", "projections", "snapshots"}) {
    CAPTURE(part);
    CHECK(text(a.whole, part) == text(b.whole, part));
  }
  for (const char* count : {"records", "event_count", "projection_count", "snapshot_count"}) {
    CAPTURE(count);
    CHECK(number(a.whole, count) == number(b.whole, count));
  }
}

}  // namespace

TEST_CASE(
    "save: a world saved at tick K and loaded runs on to the same state as one never stopped") {
  const test::TempDir tmp("cli_save");
  REQUIRE(tmp.ok());
  const std::string start = tmp.file("start");
  const std::string map_path = tmp.file("move-map.json");
  const std::string log_path = tmp.file("walk.jsonl");
  make_input(map_path, log_path);
  {
    Host maker;
    REQUIRE(maker.ok);
    make_world(maker, start);
  }
  // Two copies of the starting document, before anything has run in it.
  const std::string continuous = tmp.file("continuous");
  const std::string replayed = tmp.file("replayed");
  std::filesystem::copy(start, continuous, std::filesystem::copy_options::recursive);
  std::filesystem::copy(start, replayed, std::filesystem::copy_options::recursive);
  const std::string stream = stream_params(map_path, log_path);
  const std::string save = tmp.file("save-at-300");

  // (a) Continuous: 300 ticks, a save, 300 more with nothing declared — the world goes on.
  Hash a;
  Hash at_save;
  JsonValue saved_files;
  {
    Host host;
    REQUIRE(host.ok);
    const std::string session = open_session(host, continuous, false);
    const std::string s = "{\"session\":\"" + session + "\"";
    const JsonValue first = host.call("session.run_headless", s + ",\"seconds\":5" + stream + "}");
    const JsonValue& run = result_of(first);
    REQUIRE(number(run, "tick") == 300);
    CHECK(number(at(run, "streamed"), "activated") > 0);
    at_save = state_hash(host, session);
    CHECK(number(at_save.whole, "tick") == 300);

    const JsonValue saved =
        host.call("session.save_game", s + ",\"path\":" + json_string(save) + "}");
    const JsonValue& manifest = result_of(saved);
    CHECK(text(manifest, "state_hash") == at_save.value);
    CHECK(number(manifest, "tick") == 300);
    CHECK(at(manifest, "streamed") == JsonValue(true));
    CHECK(at(manifest, "player") == JsonValue(true));
    CHECK(number(manifest, "observers") == 2);
    CHECK(number(manifest, "tiles") > 0);
    CHECK(std::filesystem::exists(save + "/save.json"));
    CHECK(std::filesystem::exists(save + "/world.db"));
    CHECK(std::filesystem::exists(save + "/input.jsonl"));
    saved_files = at(manifest, "files");
    // Saving changed nothing.
    CHECK(state_hash(host, session).value == at_save.value);

    const JsonValue second = host.call("session.run_headless", s + ",\"seconds\":5}");
    const JsonValue& on = result_of(second);
    CHECK(number(on, "tick") == 600);
    // Continued, not refilled: one update a tick and none before the first.
    CHECK(number(at(on, "streamed"), "updates") == 300);
    a = state_hash(host, session);
  }
  MESSAGE("state hash at tick 300: " << at_save.value << ", at tick 600: " << a.value);
  CHECK(number(a.whole, "tick") == 600);
  // Every tile that went was written, whatever the build: a snapshot per tile.
  CHECK(number(a.whole, "snapshot_count") > 0);
  CHECK(text(a.whole, "store") == "read");
#if defined(ENGINE_CLI_TESTS_KINEMATICS)
  // With an entity store the tiles held entities, so their projections were written, and the carts
  // moved, so the write-back logged events. Without one (the *-no-ecs presets) nothing
  // materializes and both tables stay empty — the three runs must still agree, on less.
  CHECK(number(a.whole, "event_count") > 0);
  CHECK(number(a.whole, "projection_count") > 0);
#endif

  // (b) Loaded: a fresh host, the save loaded into a new directory, the same log handed over.
  Hash b;
  {
    Host host;
    REQUIRE(host.ok);
    const std::string loaded_dir = tmp.file("loaded");
    const JsonValue loaded =
        host.call("session.load_game",
                  "{\"path\":" + json_string(save) + ",\"dir\":" + json_string(loaded_dir) + "}");
    const JsonValue& load = result_of(loaded);
    const std::string session = text(load, "session");
    CHECK(number(load, "tick") == 300);
    CHECK(text(load, "saved_state_hash") == at_save.value);
    CHECK(text(load, "state_hash") == at_save.value);
    CHECK(at(load, "migrations").size() == 0);
    CHECK(at(load, "player") == JsonValue(true));
    const Hash loaded_hash = state_hash(host, session);
    same_parts(loaded_hash, at_save);

    // Saved again at once: the same files, byte for byte — the manifest among them.
    const std::string again = tmp.file("save-again");
    const JsonValue resaved =
        host.call("session.save_game",
                  "{\"session\":\"" + session + "\",\"path\":" + json_string(again) + "}");
    const JsonValue& files = at(result_of(resaved), "files");
    REQUIRE(files.size() == saved_files.size());
    CHECK(files.size() > 3);
    for (usize i = 0; i < files.size(); ++i) {
      const std::string path = text(files[i], "path");
      CAPTURE(path);
      CHECK(path == text(saved_files[i], "path"));
      CHECK(text(files[i], "hash") == text(saved_files[i], "hash"));
      CHECK(file_bytes(save + "/" + path) == file_bytes(again + "/" + path));
    }
    CHECK(file_bytes(save + "/save.json") == file_bytes(again + "/save.json"));

    // On to tick 600 with the whole log handed over again, observers where the save left them.
    const JsonValue run =
        host.call("session.run_headless",
                  "{\"session\":\"" + session +
                      "\",\"seconds\":5,\"stream\":{\"player\":{\"log\":" + json_string(log_path) +
                      ",\"map\":" + json_string(map_path) +
                      ",\"action\":\"move\",\"speed\":10,\"observer\":1}}}");
    CHECK(number(result_of(run), "tick") == 600);
    CHECK(number(at(result_of(run), "streamed"), "updates") == 300);
    b = state_hash(host, session);
  }

  // (c) Replayed: from tick 0, in one call, the same logs.
  Hash c;
  {
    Host host;
    REQUIRE(host.ok);
    const std::string session = open_session(host, replayed, false);
    const JsonValue run = host.call(
        "session.run_headless", "{\"session\":\"" + session + "\",\"seconds\":10" + stream + "}");
    CHECK(number(result_of(run), "tick") == 600);
    c = state_hash(host, session);
  }
  MESSAGE("continuous " << a.value << ", loaded " << b.value << ", replayed " << c.value);
  same_parts(a, b);
  same_parts(a, c);
  CHECK(a.value == b.value);
  CHECK(a.value == c.value);
#if defined(ENGINE_CLI_TESTS_KINEMATICS)
  // And the same number on every compiler and standard library the tree builds with: MSVC, and
  // Clang 18 and GCC 13 in the Linux container (docs/subsystems/world.md, "The persistent-state
  // hash"). A change that moves the world — the fixture, a system's arithmetic, the write-back, the
  // hash itself — moves this, and says so here; a change that moves it on one compiler only is a
  // determinism bug (ADR-0035's kind), not an expectation to update.
  // c2168e9307bca23b until 2026-10-05, when the carts began to integrate in f64 (ADR-0053).
  CHECK(a.value == "034b0fdc9cda96f0");
#endif
}

TEST_CASE("save: a save that is not what its manifest says is refused, naming what is wrong") {
  const test::TempDir tmp("cli_save_refused");
  REQUIRE(tmp.ok());
  const std::string doc = tmp.file("doc");
  Host host;
  REQUIRE(host.ok);
  make_world(host, doc);
  const std::string session = open_session(host, doc, false);
  const std::string s = "{\"session\":\"" + session + "\"";
  REQUIRE(number(result_of(host.call("session.run_headless", s + ",\"seconds\":0.5}")), "tick") ==
          30);
  const std::string save = tmp.file("save");
  REQUIRE(at(result_of(host.call("session.save_game", s + ",\"path\":" + json_string(save) + "}")),
             "streamed") == JsonValue(false));

  // A whole-document world saved and loaded: the same state, and the next half second the same.
  const JsonValue loaded =
      host.call("session.load_game", "{\"path\":" + json_string(save) +
                                         ",\"dir\":" + json_string(tmp.file("loaded")) + "}");
  const std::string other = text(result_of(loaded), "session");
  CHECK(other != session);
  CHECK(text(result_of(loaded), "state_hash") == text(result_of(loaded), "saved_state_hash"));
  REQUIRE(number(result_of(host.call("session.run_headless", s + ",\"seconds\":0.5}")), "tick") ==
          60);
  REQUIRE(number(result_of(host.call("session.run_headless",
                                     "{\"session\":\"" + other + "\",\"seconds\":0.5}")),
                 "tick") == 60);
  CHECK(state_hash(host, session).value == state_hash(host, other).value);

  // Refusals: a save inside the document; a directory that is not empty; a file changed since;
  // a load into a directory that is not empty; an unknown session.
  const JsonValue inside =
      host.call("session.save_game", s + ",\"path\":" + json_string(doc + "/saves/one") + "}");
  CHECK(error_code(inside) == k_invalid_argument);
  CHECK(error_message(inside).find("inside the document") != std::string::npos);
  const JsonValue occupied =
      host.call("session.save_game", s + ",\"path\":" + json_string(save) + "}");
  CHECK(error_code(occupied) == k_invalid_argument);
  {
    std::ofstream touch(save + "/document/manifest.json", std::ios::binary | std::ios::app);
    touch << " ";
  }
  const JsonValue changed =
      host.call("session.load_game",
                "{\"path\":" + json_string(save) + ",\"dir\":" + json_string(tmp.file("x")) + "}");
  CHECK(error_code(changed) == k_validation_failed);
  CHECK(error_message(changed).find("document/manifest.json") != std::string::npos);
  CHECK_FALSE(std::filesystem::exists(tmp.file("x") + "/manifest.json"));
  const JsonValue into = host.call(
      "session.load_game", "{\"path\":" + json_string(save) + ",\"dir\":" + json_string(doc) + "}");
  CHECK(error_code(into) != 0);
  CHECK(error_code(host.call("session.state_hash", "{\"session\":\"nope\"}")) ==
        k_session_not_found);
}

// ---- the migration corpus (content/migration-corpus/README.md; plan 03 section 3.8)
// --------------

namespace {

std::string environment(const char* name) {
#if defined(_MSC_VER)
  char* value = nullptr;
  size_t size = 0;
  if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return {};
  std::string out(value);
  std::free(value);
  return out;
#else
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string{};
#endif
}

// One save of the corpus and what loading it must produce, measured when it was committed: the tick
// it continues from, the state hash its manifest recorded and the one the loaded world has — which
// differ exactly when something migrated — every migration step, and the state hash after running
// on one game second with no new input (the player keeps the log its save kept). The hashes are the
// same on every compiler; a change that moves one of them on one compiler only is a determinism
// bug.
struct CorpusSave {
  const char* name;
  u64 tick;
  const char* saved;
  const char* loaded;
  std::vector<std::string> migrations;
  const char* continued;
};

const std::vector<CorpusSave>& corpus() {
  static const std::vector<CorpusSave> saves = {
      // ADR-0053 moved `Node` and `Mover` to version 2 (their positions are f64): a step each, and
      // nothing to rewrite, since the JSON of a float32 position is the double it widens to. The
      // loaded hashes did not move; the hash a second later did, because the carts now move in
      // f64 from where the save left them.
      //
      // 2026-10-06: `LayerFile` 2 (a layer names its record types' versions), `TileProjection` 2
      // and `SaveObserver` 2 (their positions are f64) are three more steps for every older save,
      // and rewrite nothing: a file with no type table reads as before, and a float32 position's
      // JSON is the double it widens to.
      {"v1",
       300,
       "8a6d7d078e00bcbf",
       "3d528309eea2b9df",
       {"engine.doc.LayerFile 1 -> 2", "engine.kinematics.Mover 1 -> 2", "engine.world.Node 1 -> 2",
        "engine.world.SaveManifest 1 -> 2", "engine.world.SaveObserver 1 -> 2",
        "engine.world.TileProjection 1 -> 2", "store.tables 1 -> 2"},
       "232aa2a53b31c9ac"},
      {"v2",
       300,
       "3d528309eea2b9df",
       "3d528309eea2b9df",
       {"engine.doc.LayerFile 1 -> 2", "engine.kinematics.Mover 1 -> 2", "engine.world.Node 1 -> 2",
        "engine.world.SaveObserver 1 -> 2", "engine.world.TileProjection 1 -> 2"},
       "232aa2a53b31c9ac"},
      // Written by the build that made them version 2: the same world, its carts moved in f64 for
      // the 300 ticks before the save, which is why its hashes are not v2's.
      {"v3",
       300,
       "8cdae985e86ab1d5",
       "8cdae985e86ab1d5",
       {"engine.doc.LayerFile 1 -> 2", "engine.world.SaveObserver 1 -> 2",
        "engine.world.TileProjection 1 -> 2"},
       "c9b72a04d230351f"},
      // Written by the build that made those three version 2: the same world and the same state
      // at tick 300 as v3 — nothing the 300 ticks wrote depended on the change — in files that
      // name their versions.
      {"v4", 300, "8cdae985e86ab1d5", "8cdae985e86ab1d5", {}, "c9b72a04d230351f"},
  };
  return saves;
}

}  // namespace

// The save the current version writes, into `ENGINE_SAVE_CORPUS_OUT/<name>` when that is set
// (content/migration-corpus/README.md, "Adding a version"): the replay test's world, run to tick
// 300 and saved. Without the variable it does nothing, so the suite never writes into the
// repository.
TEST_CASE("save corpus: write the current version's save when asked") {
  const std::string out = environment("ENGINE_SAVE_CORPUS_OUT");
  if (out.empty()) return;
  const test::TempDir tmp("cli_save_corpus_write");
  REQUIRE(tmp.ok());
  const std::string doc = tmp.file("doc");
  const std::string map_path = tmp.file("move-map.json");
  const std::string log_path = tmp.file("walk.jsonl");
  make_input(map_path, log_path);
  Host host;
  REQUIRE(host.ok);
  make_world(host, doc);
  const std::string s = "{\"session\":\"" + open_session(host, doc, false) + "\"";
  REQUIRE(
      number(result_of(host.call("session.run_headless",
                                 s + ",\"seconds\":5" + stream_params(map_path, log_path) + "}")),
             "tick") == 300);
  const JsonValue saved = host.call("session.save_game", s + ",\"path\":" + json_string(out) + "}");
  MESSAGE("wrote " << out << ": " << write_json(result_of(saved), JsonWriteOptions{false}));
}

TEST_CASE("save corpus: every save loads, migrates, and runs on as it was measured to") {
  const std::string root = test::data_path(ENGINE_SOURCE_DIR "/content/migration-corpus/saves",
                                           "content/migration-corpus/saves");
  if (!test::path_exists(root)) {
    // Only a test bundle may leave it out; a build's source tree always has it.
    REQUIRE_MESSAGE(!test::bundle_root().empty(), "the migration corpus is missing: " << root);
    MESSAGE("not in this bundle: " << root);
    return;
  }
  // Every directory is a row of the table and every row a directory: a save nobody checks is not
  // in the corpus, and a row with no save is a save somebody lost.
  std::vector<std::string> on_disk;
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    if (entry.is_directory()) on_disk.push_back(entry.path().filename().string());
  }
  std::sort(on_disk.begin(), on_disk.end());
  std::vector<std::string> listed;
  for (const CorpusSave& save : corpus())
    listed.push_back(save.name);
  std::sort(listed.begin(), listed.end());
  CHECK(on_disk == listed);

  const test::TempDir tmp("cli_save_corpus");
  REQUIRE(tmp.ok());
  Host host;
  REQUIRE(host.ok);
  for (const CorpusSave& save : corpus()) {
    CAPTURE(save.name);
    const std::string path = root + "/" + save.name;
    const JsonValue loaded =
        host.call("session.load_game", "{\"path\":" + json_string(path) +
                                           ",\"dir\":" + json_string(tmp.file(save.name)) + "}");
#if defined(ENGINE_CLI_TESTS_KINEMATICS)
    const JsonValue& load = result_of(loaded);
    CHECK(number(load, "tick") == save.tick);
    CHECK(text(load, "saved_state_hash") == save.saved);
    CHECK(text(load, "state_hash") == save.loaded);
    const JsonValue& steps = at(load, "migrations");
    REQUIRE(steps.size() == save.migrations.size());
    for (usize i = 0; i < steps.size(); ++i)
      CHECK(steps[i].as_string() == save.migrations[i]);
    const std::string session = text(load, "session");
    const JsonValue run =
        host.call("session.run_headless", "{\"session\":\"" + session + "\",\"seconds\":1}");
    CHECK(number(result_of(run), "tick") == save.tick + 60);
    const Hash on = state_hash(host, session);
    MESSAGE(std::string(save.name)
            << " loaded " << text(load, "state_hash") << ", continued " << on.value);
    CHECK(on.value == std::string(save.continued));
#else
    // Its carts are the kinematics capability's, which this build does not have: the load is
    // refused before it touches anything, naming the type, rather than loading a world whose carts
    // would not move.
    CHECK(error_code(loaded) == k_validation_failed);
    CHECK(error_message(loaded).find("engine.kinematics.Mover") != std::string::npos);
#endif
  }
}

#else

// A build without the world capability has no save format, no state hash and no ring to restore:
// the three methods are in the catalogue and answer 1006 Unavailable, naming the switch.
TEST_CASE("save: without the world capability the save methods say so") {
  const test::TempDir tmp("cli_save_absent");
  REQUIRE(tmp.ok());
  Host host;
  REQUIRE(host.ok);
  const JsonValue opened =
      host.call("session.open", "{\"path\":" + json_string(tmp.file("doc")) + ",\"create\":true}");
  const JsonValue* result = opened.find("result");
  REQUIRE(result != nullptr);
  const JsonValue* id = result->find("session");
  std::string_view session;
  REQUIRE((id != nullptr && id->get_string(session)));
  const std::string s = "{\"session\":\"" + std::string(session) + "\"";
  constexpr i32 k_unavailable = 1006;
  CHECK(error_code(host.call("session.state_hash", s + "}")) == k_unavailable);
  CHECK(error_code(host.call("session.save_game", s + ",\"path\":" + json_string(tmp.file("save")) +
                                                      "}")) == k_unavailable);
  CHECK(error_code(host.call("session.load_game", "{\"path\":" + json_string(tmp.file("save")) +
                                                      ",\"dir\":" + json_string(tmp.file("x")) +
                                                      "}")) == k_unavailable);
}

#endif
