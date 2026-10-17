// End to end: a --game module's `nearest_placement` is a function of the scene, not of what has
// streamed in (docs/subsystems/apps.md "The game contract"; world.md "The consumers"; roadmap P44).
// The ruins-walk scene's kit of boxes, its one building moved to four tiles 160-220 m from where a
// walker stands, streamed with a world block of one 48 m ring and again with three rings to 320 m:
// the fixture's HUD line naming the nearest ruin and its count of found queries are the same in
// both, where answering from the held tiles gave the one-ring world no ruin at all. Compiled where
// the ruins capability is; needs a device, and skips without.

#include "../fly_camera.h"
#include "view_process.h"

#include <core/json/json.h>
#include <domain/ruins/synthetic_kit.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <string>
#include <vector>

using namespace engine;

#if ENGINE_VIEW_GAME && ENGINE_VIEW_WORLD && defined(WALK_POC_PATH)

namespace {

struct Run {
  i32 exit_code = -1;
  std::string output;
};

Run run_view(const std::vector<std::string>& args) {
  static const std::string exe = test::app_path(ENGINE_APP_PATH);
  std::vector<std::string_view> argv;
  argv.push_back(exe);
  for (const std::string& a : args)
    argv.push_back(a);
  Run run;
  run.exit_code = view_test::run_program(
      std::span<const std::string_view>(argv.data(), argv.size()), run.output);
  return run;
}

bool summary_of(const Run& run, JsonValue& out) {
  usize end = run.output.size();
  while (end > 0) {
    usize begin = run.output.find_last_of('\n', end - 1);
    begin = begin == std::string::npos ? 0 : begin + 1;
    const std::string line = run.output.substr(begin, end - begin);
    if (!line.empty() && line[0] == '{' && parse_json(line, out).ok && out.is_object()) return true;
    if (begin == 0) break;
    end = begin - 1;
  }
  return false;
}

const JsonValue* at(const JsonValue& v, std::initializer_list<const char*> path) {
  const JsonValue* p = &v;
  for (const char* key : path) {
    p = p != nullptr && p->is_object() ? p->find(key) : nullptr;
  }
  return p;
}

f64 number_at(const JsonValue& v, std::initializer_list<const char*> path) {
  const JsonValue* p = at(v, path);
  f64 out = -1.0;
  return p != nullptr && p->get_f64(out) ? out : -1.0;
}

// The fixture's HUD line 1: "water: a ruin N m away, the spring M m", or the spring alone.
std::string water_line(const JsonValue& summary) {
  const JsonValue* lines = at(summary, {"interactive", "game", "hud", "lines"});
  if (lines == nullptr || !lines->is_array() || lines->size() < 2) return {};
  const JsonValue& line = (*lines)[1u];
  return line.is_string() ? std::string(line.as_string()) : std::string();
}

JsonValue ring(f64 radius) {
  JsonValue r = JsonValue::object();
  r.set("radius", radius);
  return r;
}

// The ruins-walk scene with its building on four tiles 5 and 6 along both axes (160-224 m from
// the origin) and a world block of `rings`, written beside the kit in the case's directory.
std::string far_ruins_scene(const test::TempDir& tmp, const char* name, JsonValue rings) {
  const std::string committed =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/ruins-walk/scene.json",
                      "content/test-scenes/ruins-walk/scene.json");
  if (!test::path_exists(committed)) return {};
  std::string source;
  REQUIRE(io::read_file(committed, source) == io::Status::Ok);
  JsonValue scene;
  REQUIRE(parse_json(source, scene).ok);
  JsonValue* ruins = scene.find("ruins");
  REQUIRE(ruins != nullptr);
  REQUIRE(ruins->is_array());
  JsonValue& entry = (*ruins)[0u];
  JsonValue min = JsonValue::array();
  min.push_back(JsonValue(5));
  min.push_back(JsonValue(5));
  JsonValue max = JsonValue::array();
  max.push_back(JsonValue(6));
  max.push_back(JsonValue(6));
  entry.set("tile_min", std::move(min));
  entry.set("tile_max", std::move(max));
  entry.set("count", static_cast<u64>(4));
  JsonValue world = JsonValue::object();
  world.set("tile_size", static_cast<u64>(32));
  world.set("rings", std::move(rings));
  scene.set("world", std::move(world));
  const std::string path = tmp.file(name);
  REQUIRE(io::write_file(path, write_json(scene, JsonWriteOptions{.pretty = true})) ==
          io::Status::Ok);
  return path;
}

}  // namespace

TEST_CASE("engine-view: nearest_placement answers from the scene, whatever the ring holds") {
  const test::TempDir tmp("engine_view_game_placements");
  JsonValue one_ring = JsonValue::array();
  one_ring.push_back(ring(1.5));  // 48 m: the buildings 160 m off never stream in
  JsonValue three_rings = JsonValue::array();
  three_rings.push_back(ring(1.5));
  three_rings.push_back(ring(4.0));
  three_rings.push_back(ring(10.0));  // 320 m: they stream in, in the outer ring's walls
  const std::string small = far_ruins_scene(tmp, "small.json", std::move(one_ring));
  if (small.empty()) {
    MESSAGE("the ruins-walk scene is not in this bundle");
    return;
  }
  const std::string large = far_ruins_scene(tmp, "large.json", std::move(three_rings));
  std::string error;
  REQUIRE_MESSAGE(
      ruins::write_synthetic_kit(tmp.file("kit"), ruins::SyntheticKitOptions{}, &error, nullptr),
      error);
  // A walker standing a second near the origin, no keys: the fixture asks for the nearest ruin
  // within 6 m every tick and within 400 m once, on its first tick, for the HUD.
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  view::SessionHeader header;
  header.ticks = 240;
  header.start.position = WorldPos{8.0, 15.0, 8.0};
  header.start.pitch = -0.3f;
  header.has_walk = true;
  header.walking = true;
  keys.set_session(view::session_to_json(header));
  const std::string session = tmp.file("stand.jsonl");
  REQUIRE(keys.save(session) == io::Status::Ok);
  const std::string built = WALK_POC_PATH;
  const std::string module = tmp.file("walk_poc" + built.substr(built.rfind('.')));
  {
    std::string bytes;
    REQUIRE(io::read_file(built, bytes) == io::Status::Ok);
    REQUIRE(io::write_file(module, bytes) == io::Status::Ok);
  }
  const auto play = [&](const std::string& scene) {
    return run_view({"--replay-input", session, "--scene", scene, "--world", "--offscreen", "--game",
                     module, "--width", "160", "--height", "96", "--ddc", tmp.file("ddc")});
  };
  const Run a = play(small);
  if (a.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << a.output);
    return;
  }
  REQUIRE_MESSAGE(a.exit_code == 0, a.output);
  const Run b = play(large);
  REQUIRE_MESSAGE(b.exit_code == 0, b.output);
  JsonValue sa;
  JsonValue sb;
  REQUIRE_MESSAGE(summary_of(a, sa), a.output);
  REQUIRE_MESSAGE(summary_of(b, sb), b.output);
  const JsonValue* pa = at(sa, {"interactive", "game", "placements"});
  const JsonValue* pb = at(sb, {"interactive", "game", "placements"});
  REQUIRE(pa != nullptr);
  REQUIRE(pb != nullptr);
  MESSAGE("one ring: " << write_json(*pa) << " -> \"" << water_line(sa) << "\"; three rings: "
                       << write_json(*pb) << " -> \"" << water_line(sb) << "\"");
  // Both found the far ruin on the first tick's 400 m look, and name the same distance.
  CHECK(water_line(sa).find("water: a ruin ") == 0);
  CHECK(water_line(sa) == water_line(sb));
  CHECK(number_at(*pa, {"found"}) == 1.0);
  CHECK(number_at(*pa, {"found"}) == number_at(*pb, {"found"}));
  CHECK(number_at(*pa, {"queries"}) == 241.0);  // 240 near, one far
  CHECK(number_at(*pa, {"refused"}) == 0.0);
  // The far look generated the tiles out to the ruin and a square beyond, the near looks hit the
  // cache: the same count in both worlds, since the ring is not consulted.
  CHECK(number_at(*pa, {"generated"}) > 100.0);
  CHECK(number_at(*pa, {"generated"}) < 400.0);
  CHECK(number_at(*pa, {"generated"}) == number_at(*pb, {"generated"}));
  CHECK(at(*pa, {"world"})->as_bool());
}

#endif  // ENGINE_VIEW_GAME && ENGINE_VIEW_WORLD && WALK_POC_PATH
