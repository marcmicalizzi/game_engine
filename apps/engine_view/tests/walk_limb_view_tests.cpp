// End to end: a walk along a megaflora's major limb (docs/subsystems/flora.md, "Megaflora";
// content/test-scenes/elder-grove). The committed scene and the flora recipes it names are copied
// into the test's scratch directory; each ancient tree's recipe is solved here (no device) to find
// its lowest major limb's straight run from the structural table, and a session written by the
// test starts a walker standing on that run 2.5 m out from the bark, facing along it, and holds W —
// replayed **offscreen**, so no window opens. Where the build has physics and the scene's collision
// the tree is a static body and the walker walks along the limb; the test holds the first 15 m (or
// as much of the run as its wood left, G79) and
// reports the rest: where the walker is every second, what it does where the run ends, and on the
// second tree's limb, which rises at 20 degrees. Compiled where the flora capability is; needs a
// device, and skips without.

#include "../fly_camera.h"
#include "../walk.h"
#include "view_process.h"

#include <core/json/json.h>
#include <domain/flora/flora.h>
#include <domain/flora/structure.h>
#include <domain/flora/trees.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/scene.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
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

// The committed scene and every file its meshes name, copied into `root` at the same relative
// places, so the copy reads as the committed one does and the test owns what it reads.
bool copy_scene(const std::string& committed, const std::string& root, std::string& scene_out) {
  std::string text;
  if (io::read_file(committed, text) != io::Status::Ok) return false;
  const std::string from_dir = committed.substr(0, committed.find_last_of("/\\") + 1);
  const std::string to_dir = root + "/test-scenes/elder-grove/";
  scene_out = to_dir + "scene.json";
  if (io::make_directories(to_dir) != io::Status::Ok) return false;
  if (io::make_directories(root + "/flora") != io::Status::Ok) return false;
  if (io::write_file(scene_out, text) != io::Status::Ok) return false;
  std::string path_text;
  if (io::read_file(from_dir + "camera-path.json", path_text) == io::Status::Ok)
    (void)io::write_file(to_dir + "camera-path.json", path_text);
  JsonValue scene;
  if (!parse_json(text, scene).ok) return false;
  const JsonValue* meshes = scene.find("meshes");
  if (meshes == nullptr || !meshes->is_array()) return false;
  for (u32 i = 0; i < meshes->size(); ++i) {
    const JsonValue* path = (*meshes)[i].find("path");
    if (path == nullptr || !path->is_string()) return false;
    std::string bytes;
    if (io::read_file(from_dir + std::string(path->as_string()), bytes) != io::Status::Ok)
      return false;
    if (io::write_file(to_dir + std::string(path->as_string()), bytes) != io::Status::Ok)
      return false;
  }
  return true;
}

// A major limb's run in the world: the instance's place and turn applied to the structural row.
struct Limb {
  Vec3 pivot;
  Vec3 direction;
  f32 length = 0.0f;
  f32 r0 = 0.0f;
  f32 r1 = 0.0f;
  f32 core = 0.0f;  // the trunk's core radius at the limb's height
  Vec3 door;        // the first hollow's sill, on the trunk's surface, and the way into it
  Vec3 inward;
  f32 radius_at(f32 s) const { return r0 + (r1 - r0) * (s / length); }
  // The top of the run's bark at arc `s`, straight above the axis.
  Vec3 top(f32 s) const {
    const f32 c = std::sqrt(direction.x * direction.x + direction.z * direction.z);
    return pivot + direction * s + Vec3{0.0f, radius_at(s) / c, 0.0f};
  }
};

bool lowest_limb(const renderer::SceneDesc& desc, const std::string& scene_dir, const char* recipe,
                 Limb& out, std::string& name) {
  for (u32 i = 0; i < desc.instances.size(); ++i) {
    const std::string& mesh = desc.meshes[desc.instances[i].mesh];
    if (mesh.size() < std::string(recipe).size() ||
        mesh.compare(mesh.size() - std::string(recipe).size(), std::string::npos, recipe) != 0) {
      continue;
    }
    std::string text;
    const std::string path =
        mesh.find(':') != std::string::npos || mesh[0] == '/' ? mesh : scene_dir + mesh;
    if (io::read_file(path, text) != io::Status::Ok) return false;
    flora::FloraFile file;
    std::string error;
    if (!flora::read_flora_file(text, file, error)) return false;
    assets::MeshData tree;
    flora::Motion motion;
    if (!flora::solve_ancient(file.ancient, tree, nullptr, &error, &motion)) return false;
    // Part 1 is the lowest limb's run (flora.md, "Megaflora": the trunk, then each run and the
    // limb past it).
    const flora::MotionPart& run = motion.parts[1];
    const flora::StructuralPart& row = motion.structure[1];
    if ((row.flags & flora::k_straight) == 0u) return false;
    Transform3 t = desc.instances[i].transform;
    t.position = relative(desc.instances[i].origin, WorldPos::origin());
    out.pivot = t.position + rotate(t.rotation, run.pivot);
    out.direction = rotate(t.rotation, run.direction);
    out.length = run.length;
    out.r0 = row.base_radius;
    out.r1 = row.tip_radius;
    out.core =
        flora::structural_radius(motion.structure[0], motion.parts[0],
                                 dot(run.pivot - motion.parts[0].pivot, motion.parts[0].direction));
    Vector<flora::Hollow> hollows;
    if (flora::ancient_hollows(file.ancient, hollows) && !hollows.empty()) {
      out.door = t.position + rotate(t.rotation, hollows[0].sill);
      out.inward = rotate(t.rotation, hollows[0].inward);
    }
    name = std::string(recipe + 1) + " at (" + std::to_string(static_cast<i32>(t.position.x)) +
           ", " + std::to_string(static_cast<i32>(t.position.z)) + ")";
    return true;
  }
  return false;
}

struct Walked {
  bool ran = false;
  std::vector<Vec3> markers;  // the eye, once a second
  u64 drops = 0;
  std::string collision;
};

// A walker started standing on the limb `start` metres out along it, facing along it, W held for
// `seconds`, a marker every second; replayed offscreen.
Walked walk_limb(const test::TempDir& tmp, const std::string& scene, const Limb& limb, f32 start,
                 u32 seconds, const char* tag) {
  Walked w;
  const view::WalkParams params = view::walk_params_from_tunables();
  const Vec3 feet = limb.top(start) + Vec3{0.0f, 0.4f, 0.0f};
  const f64 yaw =
      std::atan2(-static_cast<f64>(limb.direction.x), -static_cast<f64>(limb.direction.z));
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  constexpr u32 k_w = 26;
  constexpr u32 k_m = 16;
  const auto key = [&](u64 tick, u32 code, bool down) {
    keys.record(input::RawEvent{SimTick{tick}, input::Source::Key, code, down ? 1.0f : 0.0f, 0});
  };
  constexpr u64 k_tick_hz = 240;
  key(10, k_w, true);
  for (u32 s = 1; s <= seconds; ++s) {
    key(10 + s * k_tick_hz, k_m, true);
    key(11 + s * k_tick_hz, k_m, false);
  }
  key(12 + seconds * k_tick_hz, k_w, false);
  view::SessionHeader header;
  header.ticks = 20 + seconds * k_tick_hz;
  header.scene = scene;
  header.start.position =
      WorldPos{static_cast<f64>(feet.x), static_cast<f64>(feet.y + params.eye_height),
               static_cast<f64>(feet.z)};
  header.start.yaw = static_cast<f32>(yaw);
  header.start.pitch = 0.0f;
  header.has_walk = true;
  header.walking = true;
  header.walk = params;
  keys.set_session(view::session_to_json(header));
  const std::string session = tmp.file(std::string("limb-") + tag + ".jsonl");
  REQUIRE(keys.save(session) == io::Status::Ok);
  const Run run = run_view({"--replay-input", session, "--scene", scene, "--offscreen", "--width",
                            "160", "--height", "96", "--ddc", tmp.file("ddc")});
  if (run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << run.output);
    return w;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  JsonValue summary;
  REQUIRE_MESSAGE(summary_of(run, summary), run.output);
  const JsonValue* interactive = summary.find("interactive");
  REQUIRE(interactive != nullptr);
  const JsonValue* trajectory = interactive->find("trajectory");
  for (u32 m = 0; m < seconds; ++m) {
    Vec3 at{};
    if (!marker_at(trajectory, m, at)) break;
    w.markers.push_back(at);
  }
  if (const JsonValue* walk = interactive->find("walk"); walk != nullptr) {
    if (const JsonValue* c = walk->find("collision"); c != nullptr && c->is_string())
      w.collision = std::string(c->as_string());
    if (const JsonValue* d = walk->find("drops"); d != nullptr) {
      f64 drops = 0.0;
      if (d->get_f64(drops)) w.drops = static_cast<u64>(drops);
    }
  }
  w.ran = true;
  return w;
}

// Where the eye is against the limb: how far along the run's horizontal direction from the start,
// how far to its side, and how high the feet are over the run's bark straight below the start
// line at that distance.
void report(const Limb& limb, f32 start, const Walked& w, const view::WalkParams& params,
            const char* what) {
  const Vec3 flat =
      Vec3{limb.direction.x, 0.0f, limb.direction.z} *
      (1.0f / std::sqrt(limb.direction.x * limb.direction.x + limb.direction.z * limb.direction.z));
  const Vec3 origin = limb.top(start);
  for (u32 m = 0; m < w.markers.size(); ++m) {
    const Vec3 d = w.markers[m] - origin;
    const f32 along = d.x * flat.x + d.z * flat.z;
    const f32 side = d.x * flat.z - d.z * flat.x;
    const f32 s = start + along / (flat.x * limb.direction.x + flat.z * limb.direction.z);
    const f32 above = w.markers[m].y - params.eye_height -
                      (s <= limb.length ? limb.top(s).y : limb.top(limb.length).y);
    MESSAGE(std::string(what) << " t=" << m + 1 << " s: " << along << " m along, " << side
                              << " m aside, feet " << above << " m over the run's bark (arc " << s
                              << " of " << limb.length << ")");
  }
}

}  // namespace

TEST_CASE("engine-view: a walker walks along a megaflora's major limb, offscreen") {
  const test::TempDir tmp("engine_view_walk_limb");
  const std::string committed =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/elder-grove/scene.json",
                      "content/test-scenes/elder-grove/scene.json");
  if (!test::path_exists(committed)) {
    MESSAGE("the elder-grove scene is not in this bundle");
    return;
  }
  std::string scene;
  REQUIRE(copy_scene(committed, tmp.path(), scene));
  renderer::SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(renderer::read_scene_file(scene, desc, error), error);
  const std::string scene_dir = scene.substr(0, scene.find_last_of("/\\") + 1);
  const view::WalkParams params = view::walk_params_from_tunables();

  // The first tree: its lowest limb runs out near level, 30 m up.
  Limb level;
  std::string level_name;
  REQUIRE(lowest_limb(desc, scene_dir, "/ancient.flora.json", level, level_name));
  MESSAGE(level_name << ": the lowest run leaves the trunk at " << level.pivot.y << " m, "
                     << std::asin(level.direction.y) * 57.29578f << " degrees above level, "
                     << level.length << " m long, " << 2.0f * level.r0 << " m thick tapering to "
                     << 2.0f * level.r1 << " m; the trunk's core there is " << level.core << " m");
  {
    // The README's walk start: six metres outside the first tree's ground-level hollow, facing in.
    const Vec3 eye = level.door - level.inward * 6.0f + Vec3{0.0f, params.eye_height, 0.0f};
    const f64 yaw =
        std::atan2(-static_cast<f64>(level.inward.x), -static_cast<f64>(level.inward.z));
    MESSAGE("a walk start at the first tree's hollow: --walk --start "
            << eye.x << "," << eye.y << "," << eye.z << " " << yaw * 57.29578 << ",0");
  }
  // 2.5 m out from the bark: the run starts half the core inside it.
  const f32 start = 0.5f * level.core + 2.5f;
  const u32 seconds = static_cast<u32>((level.length - start) / params.speed) + 40;
  const Walked walked = walk_limb(tmp, scene, level, start, seconds, "level");
  if (!walked.ran) return;
  MESSAGE("collision: " << walked.collision << ", drops " << walked.drops);
  report(level, start, walked, params, "level limb");
#if ENGINE_VIEW_WALK_PHYSICS
  REQUIRE(walked.collision == "physics");
  // The first 15 m, or as much of the run as its wood left past the start less a metre (G79: a
  // limb is no longer than its wood could carry, run and all): on the limb at every second — the
  // feet on its bark within the capsule's padding, never more than a few centimetres off it, and
  // never beside it.
  const f32 held = std::min(15.0f, level.length - start - 1.0f);
  REQUIRE(held >= 8.0f);
  const u32 fifteen = static_cast<u32>(std::ceil(held / params.speed)) + 1;
  REQUIRE(walked.markers.size() > fifteen);
  const Vec3 flat = Vec3{level.direction.x, 0.0f, level.direction.z} *
                    (1.0f / std::sqrt(level.direction.x * level.direction.x +
                                      level.direction.z * level.direction.z));
  for (u32 m = 0; m < fifteen; ++m) {
    CAPTURE(m);
    const Vec3 d = walked.markers[m] - level.top(start);
    const f32 along = d.x * flat.x + d.z * flat.z;
    const f32 s = start + along / (flat.x * level.direction.x + flat.z * level.direction.z);
    const f32 above = walked.markers[m].y - params.eye_height - level.top(s).y;
    CHECK(above > -0.15f);
    CHECK(above < 0.3f);
    CHECK(std::abs(d.x * flat.z - d.z * flat.x) < 0.5f * level.radius_at(s));
  }
  const Vec3 d15 = walked.markers[fifteen - 1] - level.top(start);
  CHECK(d15.x * flat.x + d15.z * flat.z >= held - 0.5f);
#else
  CHECK(walked.collision == "ground-follow");
#endif

  // The second tree: its lowest limb rises at about 20 degrees.
  Limb rising;
  std::string rising_name;
  REQUIRE(lowest_limb(desc, scene_dir, "/ancient-rising.flora.json", rising, rising_name));
  MESSAGE(rising_name << ": the lowest run leaves the trunk at " << rising.pivot.y << " m, "
                      << std::asin(rising.direction.y) * 57.29578f << " degrees above level, "
                      << rising.length << " m long, " << 2.0f * rising.r0 << " m thick");
  const f32 rising_start = 0.5f * rising.core + 2.5f;
  const u32 rising_seconds = static_cast<u32>((rising.length - rising_start) / params.speed) + 20;
  const Walked climbed = walk_limb(tmp, scene, rising, rising_start, rising_seconds, "rising");
  if (!climbed.ran) return;
  MESSAGE("collision: " << climbed.collision << ", drops " << climbed.drops);
  report(rising, rising_start, climbed, params, "rising limb");
#if ENGINE_VIEW_WALK_PHYSICS
  // Twenty degrees is under the walker's slope limit: it climbs. Held to its first 8 m.
  REQUIRE(climbed.markers.size() > 6);
  const Vec3 c5 = climbed.markers[5] - rising.top(rising_start);
  CHECK(c5.y > 1.5f);
#endif
}
