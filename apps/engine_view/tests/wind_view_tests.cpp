// End to end, with nobody at the keyboard (docs/subsystems/apps.md, "The wind on three keys";
// gfx.md, "The live wind"; roadmap W151): an input log written here — calm, `'` pressed six times
// to a gale, G held three seconds and let go — replayed offscreen over an open basin under a
// scene's light wind, with `--benchmark` writing a record a frame. Every record says the live wind
// and what its readers took from it, and the case checks them frame by frame: unset before the
// first press, the mean on the Beaufort ladder, the gust on its envelope, the trees' wind moving
// with the gust, and the basin's sea growing from the sea it drew. Offscreen (view_process.h).

#include "../fly_camera.h"
#include "view_process.h"

#include <core/json/json.h>
#include <domain/gfx/wind.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>
#if defined(WALK_POC_WIND_PATH)
#include <foundation/hotload/module_abi.h>
#endif
#include <systems/renderer/scene_wind.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;

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

f64 number(const JsonValue* v) {
  f64 out = -1.0;
  return v != nullptr && v->get_f64(out) ? out : -1.0;
}

input::RawEvent key(u64 tick, u32 code, bool down) {
  return input::RawEvent{SimTick{tick}, input::Source::Key, code, down ? 1.0f : 0.0f, 0};
}

constexpr u32 k_hz = 240;
constexpr u64 k_ticks = 1440;  // six seconds
constexpr u64 k_gust_down = 240;
constexpr u64 k_gust_up = 960;  // held three seconds

// A frame's record as the case reads it.
struct Frame {
  u64 tick = 0;
  bool set = false;
  f64 mean = 0.0;
  f64 gust = 0.0;
  f64 speed = 0.0;
  f64 trees = 0.0;
  f64 sea = 0.0;
};

// The basin under a scene's fixed 3 m/s, its body open over 20 km.
std::string basin_scene(const test::TempDir& tmp) {
  const std::string scene = tmp.file("basin.json");
  REQUIRE(io::write_file(scene, R"({
  "format": "engine.scene.v1",
  "name": "wind-keys",
  "terrain": {"size": 129, "extent": 64, "seed": 2, "dune_height": 0,
              "basins": [{"center": [0, 0], "radius": 14, "depth": 4}]},
  "sky": {"wind": {"speed_m_s": 3, "to_deg": 30}},
  "water": [{"name": "basin", "level_m": -1.5, "open": true,
             "shape": {"fill": {"seed": [0, 0, 0], "max_extent_m": 30}},
             "wet_m": 0.3, "wind_m_s": 2, "fetch_km": 20}]
})") == io::Status::Ok);
  return scene;
}

// Six presses of `'` on ticks 10 to 60 and, with `gust`, G held from tick 240 to 960; replayed
// offscreen with a record a frame, read back. Empty when engine-view cannot run here.
std::vector<Frame> storm(const test::TempDir& tmp, const std::string& scene, bool gust,
                         const char* name) {
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  view::SessionHeader header;
  header.params.tick_hz = k_hz;
  header.ticks = k_ticks;
  header.scene = scene;
  header.start.position = WorldPos{0.0, 6.0, 24.0};
  header.start.pitch = -0.3f;
  header.has_wind = true;  // the scene's roughness's default and its fixed 3 m/s as the base
  header.wind_mean_m_s = 3.0;
  header.wind_to_deg = 30.0;
  keys.set_session(view::session_to_json(header));
  for (u64 i = 0; i < 6; ++i) {
    keys.record(key(10 + i * 10, 52, true));  // `'`
    keys.record(key(11 + i * 10, 52, false));
  }
  if (gust) {
    keys.record(key(k_gust_down, 10, true));  // G
    keys.record(key(k_gust_up, 10, false));
  }
  const std::string log = tmp.file(std::string(name) + ".jsonl");
  REQUIRE(keys.save(log) == io::Status::Ok);
  const std::string records = tmp.file(std::string(name) + "-records.jsonl");
  const Run run =
      run_view({"--replay-input", log, "--scene", scene, "--offscreen", "--width", "320",
                "--height", "200", "--ddc", tmp.file("ddc"), "--benchmark", records});
  std::vector<Frame> out;
  if (run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << run.output.substr(0, 300));
    return out;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output.substr(0, 3000));
  std::ifstream in(records);
  REQUIRE(in.good());
  std::string line;
  while (std::getline(in, line)) {
    JsonValue r;
    if (line.empty() || !parse_json(line, r).ok || !r.is_object()) continue;
    // The session's frames only: a record whose time is not one of its ticks (the drain's, past
    // the last frame) is not a frame the storm was drawn in.
    const f64 time = number(r.find("time"));
    if (!(time >= 0.0) || time * k_hz > static_cast<f64>(k_ticks) + 0.5) continue;
    Frame f;
    f.tick = static_cast<u64>(time * k_hz + 0.5);
    const JsonValue* w = r.find("wind");
    if (w != nullptr && w->is_object()) {
      f.set = true;
      f.mean = number(w->find("mean_m_s"));
      f.gust = number(w->find("gust"));
      f.speed = number(w->find("speed_m_s"));
      f.trees = number(w->find("trees_m_s"));
      f.sea = number(w->find("sea_height_m"));
    }
    out.push_back(f);
  }
  return out;
}

}  // namespace

// "Calm, a gale, a gust held three seconds, released", flown twice — with the gust and without it,
// the same presses otherwise — and read frame by frame: unset before the first press; from the
// sixth the mean at Beaufort 8 (from the scene's 3 m/s: forces 3 to 8); the gust on its envelope
// to the bit (smoothstep of the phase, rising while held, dying after); the wind with it the mean
// times 1 + level (G - 1); **the trees' wind** exactly the gustless flight's plus the forced
// gust's speed over the trees' mean, which follows the live mean through its lag (W154; the same
// turbulence at the same instants); and **the basin's sea** growing from
// the sea it drew, faster under the gust than without it.
TEST_CASE("engine-view: a replayed storm steps the live wind and its readers, frame by frame") {
  const test::TempDir tmp("engine_view_wind");
  const std::string scene = basin_scene(tmp);
  const std::vector<Frame> gusty = storm(tmp, scene, true, "gusty");
  if (gusty.empty()) return;
  const std::vector<Frame> plain = storm(tmp, scene, false, "plain");
  REQUIRE(plain.size() == gusty.size());
  const f64 gale = gfx::beaufort_speed(8);
  const f64 g = gfx::wind_gust_factor(0.03);
  // The trees' mean each frame's stats said: the lag stepped to that frame's time (the frame's wind
  // pass steps it and says it).
  std::vector<f64> trees_mean(gusty.size(), 0.0);
  {
    renderer::FloraWindLag lag;
    for (usize i = 0; i < gusty.size(); ++i) {
      gfx::WindLive live;
      live.set = gusty[i].set;
      live.mean_m_s = gusty[i].mean;
      trees_mean[i] = lag.follow(live, static_cast<f64>(gusty[i].tick) / k_hz).mean_m_s;
    }
  }
  u32 unset = 0;
  f64 worst_gust = 0.0;   // the envelope's largest miss
  f64 worst_trees = 0.0;  // the trees' largest miss of plain + forced
  f64 last_sea = -1.0;
  for (usize i = 0; i < gusty.size(); ++i) {
    const Frame& f = gusty[i];
    const Frame& p = plain[i];
    REQUIRE(f.tick == p.tick);
    if (!f.set) {
      CHECK(f.tick < 10);
      CHECK_FALSE(p.set);
      ++unset;
      continue;
    }
    if (f.tick >= 60) CHECK(f.mean == doctest::Approx(gale));
    f64 phase = 0.0;
    if (f.tick >= k_gust_down && f.tick < k_gust_up)
      phase = static_cast<f64>(f.tick - (k_gust_down - 1)) / (10.0 * k_hz);
    else if (f.tick >= k_gust_up)
      phase = std::max(0.3 - static_cast<f64>(f.tick - (k_gust_up - 1)) / (10.0 * k_hz), 0.0);
    worst_gust = std::max(worst_gust, std::abs(f.gust - gfx::wind_gust_level(phase)));
    CHECK(p.gust == 0.0);
    CHECK(f.speed == doctest::Approx(f.mean * (1.0 + f.gust * (g - 1.0))));
    // The trees take the live mean through their lag (W154, renderer::FloraWindLag): the forced gust
    // over the trees' mean, replayed here from the records' live means and times.
    worst_trees = std::max(
        worst_trees, std::abs(f.trees - (p.trees + trees_mean[i] * f.gust * (g - 1.0))));
    CHECK(f.sea > 0.0);
    // Each sea is redrawn when it moves by a part in a thousand, so the two drawn heights can sit
    // that far apart either way while the gust's lead is smaller.
    CHECK(f.sea >= p.sea * (1.0 - 2e-3));
    if (last_sea >= 0.0) CHECK(f.sea >= last_sea - 1e-9);  // under a rising wind it only grows
    last_sea = f.sea;
  }
  const Frame& end = gusty.back();
  MESSAGE(gusty.size() << " records, " << unset << " before the first press; the sea "
                       << gusty[unset].sea << " m at the first press, " << end.sea << " m at "
                       << end.tick << " ticks (" << plain.back().sea << " m without the gust); "
                       << "the envelope's largest miss " << worst_gust << ", the trees' "
                       << worst_trees << " m/s");
  CHECK(gusty.size() >= k_ticks / 4 - 2);
  CHECK(unset >= 1);
  CHECK(worst_gust < 1e-9);
  CHECK(worst_trees < 1e-4);
  CHECK(end.sea > gusty[unset].sea);
  CHECK(end.sea > plain.back().sea);
}

#if defined(WALK_POC_WIND_PATH)

namespace {

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

f64 field(const JsonValue* v, const char* key) {
  return number(v != nullptr ? v->find(key) : nullptr);
}

}  // namespace

// **A game module drives the wind** (ABI 13's `wind_state`, `wind_set`, `wind_gust`; apps.md, "The
// wind a module reads and sets"), end to end: walk_poc_wind (game/walk_poc/wind_probe.cpp) reads
// the scene's wind as the session starts, has two out-of-range sets refused, sets 20 m/s to 90
// degrees, holds a gust three seconds and lets it go, reading the state at each step and ending the
// run with every answer. Nobody presses a key, so the frames' records are the module's wind alone:
// held against its answers, the mean the module set from the frame it set it, the gust rising while
// held and dying after.
TEST_CASE("engine-view: a game module reads, sets and gusts the live wind, frame by frame") {
  const test::TempDir tmp("engine_view_wind_game");
  const std::string scene = basin_scene(tmp);
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  view::SessionHeader header;
  header.params.tick_hz = k_hz;
  header.ticks = k_ticks;
  header.scene = scene;
  header.start.position = WorldPos{0.0, 6.0, 24.0};
  header.start.pitch = -0.3f;
  header.has_wind = true;
  header.wind_mean_m_s = 3.0;
  header.wind_to_deg = 30.0;
  keys.set_session(view::session_to_json(header));
  const std::string log = tmp.file("module.jsonl");
  REQUIRE(keys.save(log) == io::Status::Ok);
  const std::string records = tmp.file("module-records.jsonl");
  const Run run = run_view({"--replay-input", log, "--scene", scene, "--offscreen", "--width",
                            "320", "--height", "200", "--ddc", tmp.file("ddc"), "--benchmark",
                            records, "--game", WALK_POC_WIND_PATH});
  if (run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << run.output.substr(0, 300));
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output.substr(0, 3000));
  JsonValue summary;
  REQUIRE_MESSAGE(summary_of(run, summary), run.output.substr(0, 3000));
  const JsonValue* interactive = summary.find("interactive");
  REQUIRE(interactive != nullptr);
  const JsonValue* game = interactive->find("game");
  REQUIRE(game != nullptr);
  const JsonValue* result = game->find("result");
  const bool has_result = result != nullptr && result->is_object();
  REQUIRE_MESSAGE(has_result, write_json(*game));
  MESSAGE("the module's answers: " << write_json(*result));

  constexpr f64 k_ok = 0.0;
  const f64 invalid = static_cast<f64>(hotload::Result::InvalidArgument);
  const f64 live = static_cast<f64>(hotload::k_wind_live);
  const f64 held_flags = static_cast<f64>(hotload::k_wind_live | hotload::k_wind_gust_held);
  // As the session starts: the scene's 3 m/s, not live, no gust.
  const JsonValue* start = result->find("start");
  CHECK(field(start, "result") == k_ok);
  CHECK(field(start, "flags") == 0.0);
  CHECK(field(start, "mean_m_s") == doctest::Approx(3.0));
  CHECK(field(start, "gust") == 0.0);
  // The host's bounds, then the set, read back the next tick.
  CHECK(field(result, "set_negative") == invalid);
  CHECK(field(result, "set_past_max") == invalid);
  CHECK(field(result, "set") == k_ok);
  const JsonValue* after_set = result->find("after_set");
  CHECK(field(after_set, "result") == k_ok);
  CHECK(field(after_set, "flags") == live);
  CHECK(field(after_set, "mean_m_s") == 20.0);
  CHECK(field(after_set, "to_deg") == 90.0);
  CHECK(field(after_set, "gust") == 0.0);
  CHECK(field(after_set, "speed_m_s") == doctest::Approx(20.0));
  // The gust: rising while held, the wind with it above the mean; dying once let go.
  CHECK(field(result, "gust_on") == k_ok);
  CHECK(field(result, "gust_off") == k_ok);
  const JsonValue* held = result->find("held");
  const JsonValue* released = result->find("released");
  const JsonValue* after = result->find("after");
  CHECK(field(held, "flags") == held_flags);
  CHECK(field(released, "flags") == held_flags);
  CHECK(field(after, "flags") == live);
  const f64 g1 = field(held, "gust"), g3 = field(released, "gust"), g4 = field(after, "gust");
  CHECK(g1 > 0.0);
  CHECK(g3 > g1);
  CHECK(g3 < 1.0);
  CHECK(g4 > 0.0);
  CHECK(g4 < g3);
  CHECK(field(released, "speed_m_s") > field(held, "speed_m_s"));
  CHECK(field(held, "speed_m_s") > 20.0);
  CHECK(field(after, "mean_m_s") == 20.0);

  // The frames: every record the live wind is in is the module's 20 m/s, its gust no higher than
  // the module read at its release, and some frame draws the gust the module held.
  std::ifstream in(records);
  REQUIRE(in.good());
  std::string line;
  u32 frames = 0, set_frames = 0, gust_frames = 0;
  f64 top_gust = 0.0;
  while (std::getline(in, line)) {
    JsonValue r;
    if (line.empty() || !parse_json(line, r).ok || !r.is_object()) continue;
    const f64 time = number(r.find("time"));
    if (!(time >= 0.0) || time * k_hz > static_cast<f64>(k_ticks) + 0.5) continue;
    ++frames;
    const JsonValue* w = r.find("wind");
    if (w == nullptr || !w->is_object()) continue;
    ++set_frames;
    CHECK(number(w->find("mean_m_s")) == 20.0);
    const f64 gust = number(w->find("gust"));
    CHECK(gust <= g3 + 1e-12);
    if (gust > 0.0) ++gust_frames;
    top_gust = std::max(top_gust, gust);
  }
  MESSAGE(frames << " records, " << set_frames << " with the live wind, " << gust_frames
                 << " gusting, the largest gust " << top_gust);
  CHECK(set_frames >= 1);
  CHECK(gust_frames >= 1);
  CHECK(top_gust >= g1);
}

#endif  // WALK_POC_WIND_PATH
