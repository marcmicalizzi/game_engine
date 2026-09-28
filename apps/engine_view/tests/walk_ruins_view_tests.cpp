// End to end: the walk mode into a ruin's wall (docs/subsystems/apps.md, "Walking";
// content/test-scenes/ruins-walk). The committed scene is copied into the test's scratch directory
// and the kit of boxes written beside it; the scene is read (no device) to find one of the
// building's full-height walls, and a live window fed an injected session starts a walker five
// metres outside it, facing it, and holds W for six seconds — nine metres at a walk. Where the
// build has physics and the scene's collision, the walker stops at the wall's face, its radius and
// the backend's padding short of it; where it follows the ground, it walks through. The recording
// then replays offscreen to the same trajectory. Compiled where the ruins capability is; needs a
// display and a device, and skips without.

#include "../fly_camera.h"

#include <core/json/json.h>
#include <core/platform/process.h>
#include <domain/ruins/synthetic_kit.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/scene.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstdio>
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
  platform::Process p;
  std::string error;
  Run run;
  if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error,
               /*merge_stderr=*/true)) {
    FAIL("cannot spawn engine-view: " << error);
    return run;
  }
  p.close_stdin();
  p.read_all(run.output);
  run.exit_code = p.wait();
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

std::string text(const JsonValue* v, const char* key) {
  const JsonValue* f = v != nullptr ? v->find(key) : nullptr;
  return f != nullptr && f->is_string() ? std::string(f->as_string()) : std::string();
}

bool marker_at(const JsonValue* trajectory, u32 index, Vec3& out) {
  const JsonValue* markers = trajectory != nullptr ? trajectory->find("markers") : nullptr;
  if (markers == nullptr || !markers->is_array() || markers->size() <= index) return false;
  const JsonValue* position = (*markers)[index].find("position");
  f64 c[3] = {};
  if (position == nullptr || !position->is_array() || position->size() != 3) return false;
  for (u32 k = 0; k < 3; ++k)
    if (!(*position)[k].get_f64(c[k])) return false;
  out = Vec3{static_cast<f32>(c[0]), static_cast<f32>(c[1]), static_cast<f32>(c[2])};
  return true;
}

std::string number_text(f32 v) {
  char out[32];
  std::snprintf(out, sizeof(out), "%.4f", static_cast<f64>(v));
  return out;
}

}  // namespace

TEST_CASE("engine-view: a walk into a ruin's wall is stopped at the wall") {
  const test::TempDir tmp("engine_view_walk_ruins");
  const std::string committed =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/ruins-walk/scene.json",
                      "content/test-scenes/ruins-walk/scene.json");
  if (!test::path_exists(committed)) {
    MESSAGE("the ruins-walk scene is not in this bundle");
    return;
  }
  std::string source;
  REQUIRE(io::read_file(committed, source) == io::Status::Ok);
  const std::string scene = tmp.file("scene.json");
  REQUIRE(io::write_file(scene, source) == io::Status::Ok);
  std::string error;
  REQUIRE_MESSAGE(
      ruins::write_synthetic_kit(tmp.file("kit"), ruins::SyntheticKitOptions{}, &error, nullptr),
      error);

  // The building as the scene reader expands it: a full-height section, the longest there is. A
  // section is a box from its first socket along +x, `thickness` across its centre line, the
  // outside towards +z (ruins.md, "The member's frame").
  renderer::SceneDesc desc;
  REQUIRE_MESSAGE(renderer::read_scene_file(scene, desc, error), error);
  i32 best = -1;
  f32 best_length = 0.0f;
  for (u32 i = 0; i < desc.instances.size(); ++i) {
    const std::string& mesh = desc.meshes[desc.instances[i].mesh];
    const f32 length = mesh.find("section-10m-h100") != std::string::npos  ? 10.0f
                       : mesh.find("section-2m-h100") != std::string::npos ? 2.0f
                                                                           : 0.0f;
    if (length > best_length) {
      best_length = length;
      best = static_cast<i32>(i);
    }
  }
  REQUIRE_MESSAGE(best >= 0, "the building has no full-height section to walk into");
  const Transform3 wall = desc.instances[static_cast<u32>(best)].transform;
  const f32 half = 0.3f;  // the kit of boxes' wall is 0.6 m thick
  const Vec3 outward = rotate(wall.rotation, Vec3{0.0f, 0.0f, 1.0f});
  const Vec3 face = wall.position + rotate(wall.rotation, Vec3{0.5f * best_length, 0.0f, half});
  // Five metres out, facing the face: forward (-sin yaw, 0, -cos yaw) is -outward.
  const Vec3 from = face + outward * 5.0f;
  const f32 yaw_deg =
      static_cast<f32>(std::atan2(static_cast<f64>(outward.x), static_cast<f64>(outward.z)) *
                       180.0 / 3.14159265358979323846);

  constexpr u32 k_w = 26;
  constexpr u32 k_m = 16;
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  const auto key = [&](u64 tick, u32 code, bool down) {
    keys.record(input::RawEvent{SimTick{tick}, input::Source::Key, code, down ? 1.0f : 0.0f, 0});
  };
  key(10, k_w, true);
  key(1450, k_w, false);  // six seconds at a walk: nine metres, were nothing in the way
  key(1460, k_m, true);
  key(1461, k_m, false);
  view::SessionHeader header;
  header.ticks = 1480;
  keys.set_session(view::session_to_json(header));
  const std::string inject = tmp.file("walk.jsonl");
  REQUIRE(keys.save(inject) == io::Status::Ok);
  const std::string recorded = tmp.file("recorded.jsonl");
  const std::string ddc = tmp.file("ddc");
  const Run live =
      run_view({"--interactive", "--walk", "--start",
                number_text(from.x) + "," + number_text(from.y + 20.0f) + "," + number_text(from.z),
                number_text(yaw_deg) + ",0", "--inject-input", inject, "--record-input", recorded,
                "--scene", scene, "--width", "160", "--height", "96", "--no-vsync", "--ddc", ddc});
  if (live.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << live.output);
    return;
  }
  REQUIRE_MESSAGE(live.exit_code == 0, live.output);
  JsonValue summary;
  REQUIRE_MESSAGE(summary_of(live, summary), live.output);
  const JsonValue* interactive = summary.find("interactive");
  REQUIRE(interactive != nullptr);
  const JsonValue* walk = interactive->find("walk");
  REQUIRE(walk != nullptr);
  const JsonValue* trajectory = interactive->find("trajectory");
  Vec3 stopped{};
  REQUIRE(marker_at(trajectory, 0, stopped));
  // How far in front of the face the eye stopped, along the outward normal.
  const f32 ahead = (stopped.x - face.x) * outward.x + (stopped.z - face.z) * outward.z;
  const std::string collision = text(walk, "collision");
  MESSAGE("walked at a " << best_length << " m wall (" << collision << "): stopped " << ahead
                         << " m in front of its face, from 5 m");
#if ENGINE_VIEW_WALK_PHYSICS
  // The capsule's radius and the backend's 2 cm padding short of the face, and not past it.
  CHECK(collision == "physics");
  CHECK(ahead > 0.3f - 0.05f);
  CHECK(ahead < 0.3f + 0.2f);
#else
  // Following the ground, nothing stands in the way: it walked on through.
  CHECK(collision == "ground-follow");
  CHECK(ahead < -3.0f);
#endif

  // The recorded walk replays offscreen to the same camera at every tick.
  const Run replay = run_view({"--replay-input", recorded, "--offscreen", "--width", "160",
                               "--height", "96", "--ddc", ddc});
  REQUIRE_MESSAGE(replay.exit_code == 0, replay.output);
  JsonValue replayed;
  REQUIRE_MESSAGE(summary_of(replay, replayed), replay.output);
  const JsonValue* replay_interactive = replayed.find("interactive");
  REQUIRE(replay_interactive != nullptr);
  REQUIRE(replay_interactive->find("trajectory") != nullptr);
  CHECK(write_json(*replay_interactive->find("trajectory")) == write_json(*trajectory));
  CHECK(text(replay_interactive->find("walk"), "hash") == text(walk, "hash"));
}
