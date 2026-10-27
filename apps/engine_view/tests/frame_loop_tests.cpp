// The frame loop allocates nothing in steady state, on the repository's three path scenes
// (AGENTS.md, "No allocations in the frame loop in steady state"; plan 11 §11.2; docs/subsystems/
// renderer.md, "The frame loop allocates nothing"). engine-view flies each scene's committed camera
// path offscreen with `--benchmark`, and every frame's record says what the frame loop allocated on
// the frame's thread — engine allocations (`mem::allocations_on_thread`) and device buffers
// (`gfx::buffers_created_on_thread`) over the host's turn, `begin_frame` and `submit_frame`, less
// what the host records for the benchmark (`renderer::FlightBookkeeping`).
//
// **Steady state is the second flight over the path.** The path is flown twice (`--repeat 2`), each
// time after `k_warmup` frames at its first camera, and a streamed world starts over at each. The
// first flight is the warm-up: the lists that grow with what a frame holds — the world's tile
// ring and its tiers, the terrain's staging records — reach the most the path needs in it, a
// growth at a time, and on the endless desert that is a few dozen allocations over its first
// frames (2026-10-07). **The second flight allocates nothing**, which is the rule: an allocation
// made every frame, or by a list that keeps growing, shows in it as it does in the first.
//
// **The scenes are the committed ones made small enough for a debug build**: the overlook's and the
// erg's ground at 257 samples over their own extents, the endless desert's world in three rings
// and two far levels, each path resampled to `k_frames` frames so the whole of it is flown in
// coarse steps (which moves more tiles a frame than the path's own 60 fps would), at 160x96, into a
// derived-data cache in the test's scratch directory. What they keep is what allocates: the
// overlook's meshes, palms and terrain with occlusion culling, the erg's rings round a walker, the
// endless desert's tiles and far squares made and let go, with the shadows each scene draws by
// default. The full-size flights are the frame budgets' (tools/frame-budget.ps1), whose summaries
// carry the same `frame_loop` block.
//
// **They are a CTest test of their own**, `engine_view.frame_loop`
// (apps/engine_view/CMakeLists.txt): the three flights take about 220 s in `msvc-debug` and 60 s in
// `msvc-release`, which inside `engine_view` would double its run and bring it near its timeout. So
// they are marked `doctest::skip()`, which the `engine_view` run honours, and
// `engine_view.frame_loop` runs the same executable with `--no-skip` and these cases alone.
#include "view_process.h"

#include <core/json/json.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using namespace engine;

// Everything below is for the cases at the end, and every one of them is behind
// ENGINE_VIEW_TESTS_DUNES: with the dunes off this file has no cases, and Clang would otherwise
// refuse the helpers as unused functions (-Werror), as linux-clang-minimal did on 2026-10-08.
#if ENGINE_VIEW_TESTS_DUNES
namespace {

constexpr u32 k_frames = 96;
constexpr u32 k_warmup = 12;
constexpr u32 k_repeats = 2;

struct Run {
  i32 exit_code = -1;
  std::string output;
};

Run view(const std::vector<std::string>& args) {
  std::vector<std::string_view> argv;
  const std::string exe = test::app_path(ENGINE_APP_PATH);
  argv.push_back(exe);
  for (const std::string& a : args)
    argv.push_back(a);
  Run run;
  run.exit_code = view_test::run_program(
      std::span<const std::string_view>(argv.data(), argv.size()), run.output);
  return run;
}

std::string committed_dir(const char* scene) {
  const std::string relative = std::string("content/test-scenes/") + scene;
  return test::data_path(std::string(ENGINE_SOURCE_DIR "/") + relative, relative);
}

bool read_json(const std::string& file, JsonValue& out) {
  std::string text;
  if (io::read_file(file, text) != io::Status::Ok) return false;
  return parse_json(text, out).ok;
}

std::string forward_slashes(std::string s) {
  for (char& c : s)
    if (c == '\\') c = '/';
  return s;
}

u64 number(const JsonValue& object, const char* key) {
  const JsonValue* value = object.find(key);
  u64 v = 0;
  return value != nullptr && value->get_u64(v) ? v : ~u64{0};
}

// What one flight's records said, frame by frame.
struct Flown {
  bool ran = false;
  u64 allocations = 0;
  u64 buffers = 0;
  u32 records = 0;
  // The first flight's, the warm-up: reported, not asserted.
  u64 first_allocations = 0;
  u64 first_buffers = 0;
  std::string first;             // the first frames that allocated, for the message
  std::string allocation_sites;  // debug child output, printed only when zero is violated
  JsonValue summary;
};

// Flies `scene` along `path` and reads the `.jsonl` back. False (and a MESSAGE) when this machine
// cannot draw it, which is a skip and not a failure.
Flown fly(const test::TempDir& tmp, const std::string& name, const std::string& scene,
          const std::string& path, const std::vector<std::string>& extra) {
  Flown out;
  const std::string jsonl = tmp.file(name + ".jsonl");
  std::vector<std::string> args = {"--scene",
                                   scene,
                                   "--camera-path",
                                   path,
                                   "--offscreen",
                                   "--benchmark",
                                   jsonl,
                                   "--frames",
                                   std::to_string(k_frames),
                                   "--repeat",
                                   std::to_string(k_repeats),
                                   "--warmup",
                                   std::to_string(k_warmup),
                                   "--warmup-seconds",
                                   "0",
                                   "--width",
                                   "160",
                                   "--height",
                                   "96",
                                   "--ddc",
                                   tmp.file("ddc")};
  args.insert(args.end(), extra.begin(), extra.end());
  const Run run = view(args);
  if (run.exit_code == 3) {
    MESSAGE(name << ": engine-view cannot draw it here: " << run.output);
    return out;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, name << ": " << run.output);
  std::string text;
  REQUIRE(io::read_file(jsonl, text) == io::Status::Ok);
  std::istringstream lines(text);
  u32 listed = 0;
  for (std::string line; std::getline(lines, line);) {
    if (line.empty()) continue;
    JsonValue value;
    REQUIRE_MESSAGE(parse_json(line, value).ok, line);
    if (value.find("format") != nullptr) {
      out.summary = std::move(value);
      continue;
    }
    ++out.records;
    const u64 a = number(value, "allocations");
    const u64 b = number(value, "buffers");
    REQUIRE_MESSAGE(a != ~u64{0}, "a record without `allocations`: " << line.substr(0, 200));
    REQUIRE(b != ~u64{0});
    if (number(value, "repeat") == 0) {
      out.first_allocations += a;
      out.first_buffers += b;
      continue;
    }
    out.allocations += a;
    out.buffers += b;
    if ((a > 0 || b > 0) && listed < 8) {
      out.first += (listed++ > 0 ? ", " : "") + std::string("frame ") +
                   std::to_string(number(value, "frame")) + ": " + std::to_string(a) +
                   " allocations, " + std::to_string(b) + " buffers";
    }
  }
  REQUIRE(out.records == k_frames * k_repeats);
  const JsonValue* loop = out.summary.find("frame_loop");
  REQUIRE_MESSAGE(loop != nullptr, "the summary has no frame_loop block");
  // The summary's block is the records' totals.
  CHECK(number(*loop, "allocations") == out.first_allocations + out.allocations);
  CHECK(number(*loop, "buffers") == out.first_buffers + out.buffers);
  const JsonValue* device = out.summary.find("device");
  REQUIRE(device != nullptr);
  CHECK_FALSE(device->as_string().empty());
  MESSAGE(name << " on " << device->as_string() << ", " << k_frames
               << " frames a flight: the first " << out.first_allocations << " allocations and "
               << out.first_buffers << " device buffers on the frame's thread, the second "
               << out.allocations << " and " << out.buffers
               << (out.first.empty() ? std::string() : " (" + out.first + ")"));
  if (out.allocations != 0) out.allocation_sites = run.output;
  out.ran = true;
  return out;
}

}  // namespace

TEST_CASE("frame loop: the desert overlook allocates nothing in its second flight" *
          doctest::skip()) {
  const test::TempDir tmp("engine_view_frame_loop_overlook");
  const std::string dir = committed_dir("desert-overlook");
  JsonValue scene;
  if (!read_json(dir + "/scene.json", scene)) {
    MESSAGE("the desert overlook is not in this bundle");
    return;
  }
  // Its meshes are the Khronos samples, named relative to the committed file: made absolute so
  // the copy in the scratch directory finds them, and the flight skipped where they were not
  // fetched (tools/fetch-samples.ps1).
  JsonValue* meshes = scene.find("meshes");
  REQUIRE(meshes != nullptr);
  for (usize m = 0; m < meshes->size(); ++m) {
    JsonValue& mesh = (*meshes)[m];
    const JsonValue* relative = mesh.find("path");
    if (relative == nullptr) continue;
    const std::string absolute = forward_slashes(
        std::filesystem::weakly_canonical(std::filesystem::path(dir) / relative->as_string())
            .string());
    if (!test::path_exists(absolute)) {
      MESSAGE("the desert overlook's samples are not fetched here (" << absolute << "); skipped");
      return;
    }
    mesh.set("path", JsonValue(absolute));
  }
  JsonValue terrain = *scene.find("terrain");
  terrain.set("size", JsonValue(static_cast<u64>(257)));
  scene.set("terrain", std::move(terrain));
  scene.set("camera_path", JsonValue(forward_slashes(dir + "/camera-path.json")));
  const std::string file = tmp.file("overlook.json");
  REQUIRE(io::write_file(file, write_json(scene)) == io::Status::Ok);
  const Flown flown = fly(tmp, "desert-overlook", file, dir + "/camera-path.json", {});
  if (!flown.ran) return;
  CHECK_MESSAGE(flown.allocations == 0, flown.first << '\n' << flown.allocation_sites);
  CHECK(flown.buffers == 0);
}

TEST_CASE("frame loop: the erg's walk on its rings allocates nothing in its second pass" *
          doctest::skip()) {
  const test::TempDir tmp("engine_view_frame_loop_erg");
  const std::string dir = committed_dir("desert-erg");
  JsonValue scene;
  if (!read_json(dir + "/scene.json", scene)) {
    MESSAGE("the erg is not in this bundle");
    return;
  }
  JsonValue terrain = *scene.find("terrain");
  terrain.set("size", JsonValue(static_cast<u64>(257)));
  scene.set("terrain", std::move(terrain));
  scene.set("camera_path", JsonValue(forward_slashes(dir + "/walk-path.json")));
  const std::string file = tmp.file("erg.json");
  REQUIRE(io::write_file(file, write_json(scene)) == io::Status::Ok);
  // The walk is the erg's path scene (its README's benchmark line), on the rings round the walker.
  const Flown flown =
      fly(tmp, "desert-erg walk", file, dir + "/walk-path.json", {"--terrain-rings"});
  if (!flown.ran) return;
  CHECK_MESSAGE(flown.allocations == 0, flown.first << '\n' << flown.allocation_sites);
  CHECK(flown.buffers == 0);
}

#if ENGINE_VIEW_WORLD
TEST_CASE(
    "frame loop: the endless desert's flight over its tiles allocates nothing in its "
    "second flight" *
    doctest::skip()) {
  const test::TempDir tmp("engine_view_frame_loop_endless");
  const std::string dir = committed_dir("desert-endless");
  JsonValue scene;
  if (!read_json(dir + "/scene.json", scene)) {
    MESSAGE("the endless desert is not in this bundle");
    return;
  }
  // Its world's rings brought in to six tiles (as render_tests.cpp's flight does), so a debug
  // build lays a ring out in seconds; the tiles, their cells, the bands, the sky and the time are
  // the scene's own. Two far levels past them (`--terrain-far 2`) instead of six.
  JsonValue world = *scene.find("world");
  JsonValue rings = JsonValue::array();
  const f64 radii[3] = {1.5, 3.0, 6.0};
  const u64 cells[3] = {32, 16, 8};
  for (u32 k = 0; k < 3; ++k) {
    JsonValue ring = JsonValue::object();
    ring.set("radius", JsonValue(radii[k]));
    ring.set("ground_cells", JsonValue(cells[k]));
    rings.push_back(std::move(ring));
  }
  world.set("rings", std::move(rings));
  scene.set("world", std::move(world));
  scene.set("camera_path", JsonValue(forward_slashes(dir + "/camera-path.json")));
  const std::string file = tmp.file("endless.json");
  REQUIRE(io::write_file(file, write_json(scene)) == io::Status::Ok);
  const Flown flown =
      fly(tmp, "desert-endless", file, dir + "/camera-path.json", {"--terrain-far", "2"});
  if (!flown.ran) return;
  CHECK_MESSAGE(flown.allocations == 0, flown.first << '\n' << flown.allocation_sites);
  CHECK(flown.buffers == 0);
}

#if ENGINE_VIEW_TESTS_FLORA
// Roadmap W74 (renderer.md, "Streamed trees sway"): a streamed world's own trees sway with their
// tiles, each taking a record of its mesh's free list when its tile arrives and giving it back when
// it leaves. The first flight makes the records a mesh needs at most; the second (`--repeat`
// starts the world over, so every tile arrives and leaves again) reuses them. Tile changes are
// steady state: both the rigid and the swaying flight must allocate nothing.
TEST_CASE("frame loop: a streamed world's swaying trees allocate nothing in their second flight" *
          doctest::skip()) {
  const test::TempDir tmp("engine_view_frame_loop_trees");
  // authored_view_tests.cpp's trees, small enough for a debug build, in two rings of 32 m tiles.
  const std::string oak =
      R"({"species":"broadleaf","broadleaf":{"seed":3,"height_m":8,"trunk_radius_m":0.25,)"
      R"("triangle_budget":8000,"card_bake_px":16,"impostor_px":32}})";
  const std::string spruce =
      R"({"species":"conifer","conifer":{"seed":5,"height_m":6,"trunk_radius_m":0.1,)"
      R"("whorls":6,"branches_per_whorl":4,"crown_radius_m":1.5,"triangle_budget":8000}})";
  REQUIRE(io::write_file(tmp.file("oak.flora.json"), oak) == io::Status::Ok);
  REQUIRE(io::write_file(tmp.file("spruce.flora.json"), spruce) == io::Status::Ok);
  const std::string scene =
      R"({"format":"engine.scene.v1","name":"frame-loop-trees",)"
      R"("terrain":{"size":65,"extent":400,"seed":7,"dune_height":0.3,"dune_wavelength":60,)"
      R"("generator":"Waves"},)"
      R"("meshes":[{"name":"oak","path":"oak.flora.json"},)"
      R"({"name":"spruce","path":"spruce.flora.json"}],)"
      R"("scatters":[{"mesh":0,"center":[0,0,0],"radius_min":2,"radius_max":120,"count":70,)"
      R"("seed":11,"scale_min":0.8,"scale_max":1.2},)"
      R"({"mesh":1,"center":[0,0,0],"radius_min":2,"radius_max":120,"count":70,"seed":12}],)"
      R"("world":{"tile_size":32,"rings":[{"radius":1.5},{"radius":3}]}})";
  REQUIRE(io::write_file(tmp.file("trees.json"), scene) == io::Status::Ok);
  // Low across the grove, so tiles cross both rings both ways.
  const std::string path = R"({"format":"engine.camera-path.v1","name":"across","fps":60,"keys":[)"
                           R"({"time":0,"position":[-220,14,10],"target":[0,4,0]},)"
                           R"({"time":1.6,"position":[220,14,-10],"target":[400,4,0]}]})";
  REQUIRE(io::write_file(tmp.file("path.json"), path) == io::Status::Ok);
  const Flown rigid = fly(tmp, "streamed-trees-rigid", tmp.file("trees.json"),
                          tmp.file("path.json"), {"--tunable", "renderer.flora.tail=false"});
  if (!rigid.ran) return;
  const Flown flown = fly(tmp, "streamed-trees", tmp.file("trees.json"), tmp.file("path.json"), {});
  if (!flown.ran) return;
  MESSAGE("second flight, swaying: " << flown.allocations << " allocations, " << flown.buffers
                                     << " buffers; rigid: " << rigid.allocations << ", "
                                     << rigid.buffers);
  CHECK_MESSAGE(rigid.allocations == 0, rigid.first << '\n' << rigid.allocation_sites);
  CHECK(rigid.buffers == 0);
  CHECK_MESSAGE(flown.allocations == 0, flown.first << '\n' << flown.allocation_sites);
  CHECK(flown.buffers == 0);
}
#endif  // ENGINE_VIEW_TESTS_FLORA
#endif  // ENGINE_VIEW_WORLD

#endif  // ENGINE_VIEW_TESTS_DUNES
