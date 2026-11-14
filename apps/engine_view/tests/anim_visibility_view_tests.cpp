// End to end: an actor keeps animating as far as it can be seen to move (roadmap A44;
// docs/subsystems/animation.md, "The LOD policy: what the eye can see"). The skinned bar of
// mesh_view_tests.cpp, scaled to a small thing, 100 m from a camera that holds still, run with
// `--animate --offscreen` — which A44 also opened (the forest's need 93): the tier rule reads how
// many pixels the bar's limbs travel, so the same bar at the same distance draws its rest pose at
// 960 x 540, keeps moving at 1920 x 1080, and keeps moving at 960 x 540 again once its scene block
// gives it a bias. The summary says the tier, the travel the rule read and the pose hash; a pose
// that moves hashes differently four steps later, and the rest pose hashes as nothing.

#include "view_process.h"

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <foundation/io/vfs.h>
#include <systems/animation/animation.h>
#include <systems/animation/visibility.h>
#include <systems/renderer/scene.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
#include <string>
#include <vector>

using namespace engine;

// Compiled only where the animation capability is (CMakeLists.txt), so it needs no guard of its
// own.
namespace engine::view_test {
bool write_skinned_bar_glb(const std::string& path);  // mesh_view_tests.cpp
}  // namespace engine::view_test

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

// The one animated instance's entry of a run's summary.
struct Seen {
  u32 tier = 99;
  f64 travel_m = -1.0;
  f64 lod_bias = -1.0;
  std::string pose_hash;
};

Seen seen(const Run& run) {
  Seen out;
  JsonValue summary;
  REQUIRE_MESSAGE(summary_of(run, summary), run.output);
  const JsonValue* animation = summary.find("animation");
  REQUIRE(animation != nullptr);
  const JsonValue* animated = animation->find("animated");
  REQUIRE(animated != nullptr);
  REQUIRE(animated->is_array());
  REQUIRE(animated->size() == 1);
  const JsonValue& entry = (*animated)[usize{0}];
  u64 tier = 99;
  REQUIRE(entry.find("tier") != nullptr);
  CHECK(entry.find("tier")->get_u64(tier));
  out.tier = static_cast<u32>(tier);
  REQUIRE(entry.find("travel_m") != nullptr);
  CHECK(entry.find("travel_m")->get_f64(out.travel_m));
  REQUIRE(entry.find("lod_bias") != nullptr);
  CHECK(entry.find("lod_bias")->get_f64(out.lod_bias));
  REQUIRE(entry.find("pose_hash") != nullptr);
  out.pose_hash = std::string(entry.find("pose_hash")->as_string());
  return out;
}

// The bar at 7 % of its size in a scene of its own, its block with `bias`, and a path
// holding the camera 100 m in front of it at the bar's height.
constexpr f32 k_scale = 0.07f;
constexpr f64 k_distance_m = 100.0;

void write_scene(const test::TempDir& tmp, const char* name, f32 bias) {
  REQUIRE(view_test::write_skinned_bar_glb(tmp.file("bar.glb")));
  std::string body = R"({"format":"engine.scene.v1","name":"far-bar","meshes":[{"path":"bar.glb"}],
      "instances":[{"mesh":0,"translation":[0,0,0],"scale":[)";
  const std::string s = std::to_string(k_scale);
  body += s + "," + s + "," + s + R"(],"animation":{"clip":"bend","lod_bias":)" +
          std::to_string(bias) + "}}]}";
  REQUIRE(io::write_file(tmp.file(name), body) == io::Status::Ok);
  const std::string path = R"({"format":"engine.camera-path.v1","name":"still","fps":60,"keys":[
      {"time":0,"position":[0,0.08,100],"target":[0,0.08,0]},
      {"time":1,"position":[0,0.08,100],"target":[0,0.08,0]}]})";
  REQUIRE(io::write_file(tmp.file("still.json"), path) == io::Status::Ok);
}

Seen run_at(const test::TempDir& tmp, const char* scene, u32 width, u32 height, u32 frames) {
  const Run run = run_view({"--scene", tmp.file(scene), "--camera-path", tmp.file("still.json"),
                            "--animate", "--offscreen", "--frames", std::to_string(frames),
                            "--width", std::to_string(width), "--height", std::to_string(height)});
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  return seen(run);
}

// cot(fov / 2) * rows / 2 at the renderer's default 55 degrees: the lens the rule reads.
f64 lens(u32 rows) { return static_cast<f64>(rows) * 0.5 / std::tan(0.9599310886 * 0.5); }

}  // namespace

TEST_CASE("anim visibility view: the same bar at 100 m moves with the projection and with a bias") {
  test::TempDir tmp("anim-visibility-view");
  write_scene(tmp, "plain.json", 1.0f);
  write_scene(tmp, "biased.json", 4.0f);

  // Five runs, each its own GPU device and so its own turn at the machine's GPU lock: the case is
  // kept to what it must show. 960 x 540: the bar's limbs travel under a pixel at 100 m, so it is
  // held at LOD2 and draws its rest pose, whose run is empty and hashes as nothing (`k_hash_seed`,
  // animation.cpp `animation_summary`) at any step.
  const Seen small_a = run_at(tmp, "plain.json", 960, 540, 20);
  // The geometry is chosen for the bend clip's bound on this bar (2.12 m at scale 1, measured
  // 2026-11-14: 0.77 px at 540 rows, 1.54 at 1080 at this scale): the case
  // means what it says only while the travel the rule read keeps the 540-row run under the band's
  // pixel widened by the hysteresis and the 1080-row run over it.
  const f64 travel = small_a.travel_m;
  MESSAGE("the bar's travel at scale " << k_scale << ": " << travel << " m; "
                                       << travel * lens(540) / k_distance_m << " px at 540 rows, "
                                       << travel * lens(1080) / k_distance_m << " px at 1080");
  const f64 widen = 1.0 + static_cast<f64>(animation::tier_params().hysteresis);
  REQUIRE(travel * lens(540) / k_distance_m < 1.0 / widen);
  REQUIRE(travel * lens(1080) / k_distance_m > 1.0);
  CHECK(small_a.lod_bias == doctest::Approx(1.0));
  CHECK(small_a.tier == 2);
  CHECK(small_a.pose_hash == renderer::hash_hex(k_hash_seed));

  // 1920 x 1080: twice the lens, twice the pixels, over the band — skinned, and moving.
  const Seen large_a = run_at(tmp, "plain.json", 1920, 1080, 20);
  const Seen large_b = run_at(tmp, "plain.json", 1920, 1080, 24);
  CHECK(large_a.travel_m == doctest::Approx(travel));
  CHECK(large_a.tier <= 1);
  CHECK(large_b.tier <= 1);
  CHECK(large_a.pose_hash != large_b.pose_hash);
  CHECK(large_a.pose_hash != small_a.pose_hash);

  // 960 x 540 with the scene block's bias of 4: four times the pixels — skinned and moving again.
  const Seen biased_a = run_at(tmp, "biased.json", 960, 540, 20);
  const Seen biased_b = run_at(tmp, "biased.json", 960, 540, 24);
  CHECK(biased_a.lod_bias == doctest::Approx(4.0));
  CHECK(biased_a.tier <= 1);
  CHECK(biased_a.pose_hash != biased_b.pose_hash);
  CHECK(biased_a.pose_hash != renderer::hash_hex(k_hash_seed));
  // A bias outside [0, 64] is refused by the scene reader with a sentence, not clamped:
  // systems/renderer's flythrough_tests.cpp ("scene file: the oldest spelling ...") holds it with
  // no GPU, which this case would otherwise spend a turn at the lock on.
}
