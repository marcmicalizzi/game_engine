// The static world end to end (content/test-scenes/static-world/, roadmap T130; docs/experiments/
// static-asset-world.md): imported meshes placed by hand, no generator, no terrain, no scatter, no
// sky — the scene a consumer who uses none of the solvers has. It is compiled in every
// configuration, `msvc-minimal` included, because that build is the one such a consumer links.
//
// engine-content writes the kit (`static-kit`: the ground, the branch-card tree, the shrub, the
// grass and the boulder as plain glTF with PNGs) into the test's scratch directory, the committed
// scene is copied beside it, and engine-view flies the committed camera path, resampled to a few
// frames, through **each of the four rasterizers** at 160x96, twice (`--repeat 2`), into a derived-
// data cache of the test's own. Each flight must load and draw (exit 0; 3 is a machine that cannot
// draw that mode, a skip), and its second pass must allocate nothing on the frame's thread, the
// rule every path scene holds (renderer.md, "The frame loop allocates nothing"). The Khronos
// samples the scene also places (the lanterns, the helmet) are git-ignored; where they were not
// fetched those meshes and their instances are left out and the rest is flown.
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

namespace {

constexpr u32 k_frames = 24;

std::string slashes(std::string s) {
  for (char& c : s)
    if (c == '\\') c = '/';
  return s;
}

i32 run(const std::string& exe, const std::vector<std::string>& args, std::string& output) {
  std::vector<std::string_view> argv;
  argv.push_back(exe);
  for (const std::string& a : args)
    argv.push_back(a);
  return view_test::run_program(std::span<const std::string_view>(argv.data(), argv.size()),
                                output);
}

u64 number(const JsonValue& object, const char* key) {
  const JsonValue* value = object.find(key);
  u64 v = 0;
  return value != nullptr && value->get_u64(v) ? v : ~u64{0};
}

}  // namespace

TEST_CASE("static world: imported meshes placed by hand fly through every rasterizer") {
  const test::TempDir tmp("engine_view_static_world");
  const std::string relative = "content/test-scenes/static-world";
  const std::string dir = test::data_path(std::string(ENGINE_SOURCE_DIR "/") + relative, relative);
  std::string text;
  if (io::read_file(dir + "/scene.json", text) != io::Status::Ok) {
    MESSAGE("the static world is not in this bundle");
    return;
  }
  JsonValue scene;
  REQUIRE(parse_json(text, scene).ok);

  // The kit, written by the engine-content built beside this engine-view.
  const std::filesystem::path view = test::app_path(ENGINE_APP_PATH);
  const std::string content =
      slashes((view.parent_path() / ("engine-content" + view.extension().string())).string());
  std::string output;
  REQUIRE_MESSAGE(run(content, {"static-kit", tmp.file("kit")}, output) == 0, output);

  // Kit meshes resolve beside the copy; a sample is made absolute, and where any is missing every
  // sample mesh (they are last in the file) and its instances are left out.
  JsonValue* meshes = scene.find("meshes");
  REQUIRE(meshes != nullptr);
  usize first_sample = meshes->size();
  bool samples = true;
  for (usize m = 0; m < meshes->size(); ++m) {
    JsonValue& mesh = (*meshes)[m];
    const std::string path(mesh.find("path")->as_string());
    if (path.rfind("kit/", 0) == 0) continue;
    first_sample = m < first_sample ? m : first_sample;
    const std::string absolute =
        slashes(std::filesystem::weakly_canonical(std::filesystem::path(dir) / path).string());
    if (!test::path_exists(absolute)) samples = false;
    mesh.set("path", JsonValue(absolute));
  }
  u64 placed = scene.find("instances")->size();
  if (!samples) {
    JsonValue kept_meshes = JsonValue::array();
    for (usize m = 0; m < first_sample; ++m)
      kept_meshes.push_back((*meshes)[m]);
    JsonValue kept = JsonValue::array();
    const JsonValue& instances = *scene.find("instances");
    for (usize i = 0; i < instances.size(); ++i)
      if (number(instances[i], "mesh") < first_sample) kept.push_back(instances[i]);
    MESSAGE("the Khronos samples are not fetched here: flying the kit's "
            << kept.size() << " instances of " << instances.size());
    placed = kept.size();
    scene.set("meshes", std::move(kept_meshes));
    scene.set("instances", std::move(kept));
  }
  const std::string path = slashes(dir + "/camera-path.json");
  scene.set("camera_path", JsonValue(path));
  const std::string file = tmp.file("scene.json");
  REQUIRE(io::write_file(file, write_json(scene)) == io::Status::Ok);
  CHECK(placed > 50u);

  for (const char* raster : {"hw", "vertex", "sw", "rt"}) {
    CAPTURE(raster);
    const std::string jsonl = tmp.file(std::string("static-") + raster + ".jsonl");
    output.clear();
    const i32 code = run(view.string(),
                         {"--scene",
                          file,
                          "--camera-path",
                          path,
                          "--offscreen",
                          "--benchmark",
                          jsonl,
                          "--frames",
                          std::to_string(k_frames),
                          "--repeat",
                          "2",
                          "--warmup",
                          "4",
                          "--warmup-seconds",
                          "0",
                          "--width",
                          "160",
                          "--height",
                          "96",
                          "--raster",
                          raster,
                          "--ddc",
                          tmp.file("ddc")},
                         output);
    if (code == 3) {
      MESSAGE(raster << ": engine-view cannot draw it here: " << output);
      continue;
    }
    REQUIRE_MESSAGE(code == 0, output);
    std::string lines;
    REQUIRE(io::read_file(jsonl, lines) == io::Status::Ok);
    std::istringstream stream(lines);
    u32 records = 0;
    u64 second = 0;
    JsonValue summary;
    for (std::string line; std::getline(stream, line);) {
      if (line.empty()) continue;
      JsonValue value;
      REQUIRE_MESSAGE(parse_json(line, value).ok, line);
      if (value.find("format") != nullptr) {
        summary = std::move(value);
        continue;
      }
      ++records;
      if (number(value, "repeat") == 1) second += number(value, "allocations");
    }
    CHECK(records == 2 * k_frames);
    REQUIRE_MESSAGE(summary.find("frame_loop") != nullptr, "no summary in " << jsonl);
    CHECK_MESSAGE(second == 0, raster << ": the second flight allocated\n" << output);
  }
}
