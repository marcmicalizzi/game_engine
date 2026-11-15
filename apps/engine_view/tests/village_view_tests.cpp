// End to end: a village on a tile set's ground, its houses the rural filler's, flown from the lane
// in at a door, through the rooms, up the stair and to a window (docs/subsystems/village.md, "In a
// scene"; roadmap G69, S89). engine-content writes the ground, plans the village and writes the
// flight; engine-view draws a picture at every marker of it with the ids beside. What a regression
// would break is what is held: every house the entry asks for stands as an instance of a mesh
// with clusters, a frame the flight calls inside is filled by one building seen from within (most
// of its pixels are one instance's, over several of its clusters, and almost none are sky), and
// the load dropped and withheld nothing. Compiled where the village, the dwellings, the structures
// and the tile set are; skips (exit 3) on a machine with no Vulkan device.
#include "view_process.h"

#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;

namespace {

struct Run {
  i32 exit_code = -1;
  std::string output;
};

Run run_app(const std::string& app, const std::vector<std::string>& args) {
  std::vector<std::string_view> argv;
  argv.push_back(app);
  for (const std::string& a : args)
    argv.push_back(a);
  Run run;
  run.exit_code = view_test::run_program(
      std::span<const std::string_view>(argv.data(), argv.size()), run.output);
  return run;
}

const std::string& view_exe() {
  static const std::string path = test::app_path(ENGINE_APP_PATH);
  return path;
}

// engine-content is built beside engine-view, and in a bundle sits beside it too.
std::string content_exe() {
  const std::filesystem::path app(view_exe());
  return (app.parent_path() / ("engine-content" + app.extension().string())).generic_string();
}

// The whole number after the first `"key":` of a summary, or `missing`.
u64 number_after(const std::string& text, const std::string& key, u64 missing) {
  const usize at = text.find("\"" + key + "\":");
  if (at == std::string::npos) return missing;
  usize k = at + key.size() + 3;
  if (k >= text.size() || text[k] < '0' || text[k] > '9') return missing;
  u64 v = 0;
  for (; k < text.size() && text[k] >= '0' && text[k] <= '9'; ++k)
    v = v * 10 + static_cast<u64>(text[k] - '0');
  return v;
}

// What an ids capture shows: three u32 a pixel (instance, cluster, triangle), all ones for none.
struct Seen {
  u64 pixels = 0, empty = 0, most = 0;  // of the commonest instance
  u32 instance = 0xffffffffu;
  u32 clusters = 0;  // distinct clusters of that instance
};

Seen read_ids(const std::string& path) {
  Seen seen;
  std::ifstream f(path, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  REQUIRE(bytes.size() % 12 == 0);
  seen.pixels = bytes.size() / 12;
  std::vector<u32> instances, clusters;
  instances.reserve(seen.pixels);
  for (u64 p = 0; p < seen.pixels; ++p) {
    u32 word = 0;
    std::memcpy(&word, bytes.data() + p * 12, 4);
    if (word == 0xffffffffu)
      ++seen.empty;
    else
      instances.push_back(word);
  }
  std::sort(instances.begin(), instances.end());
  for (usize a = 0; a < instances.size();) {
    usize b = a;
    while (b < instances.size() && instances[b] == instances[a])
      ++b;
    if (b - a > seen.most) {
      seen.most = b - a;
      seen.instance = instances[a];
    }
    a = b;
  }
  for (u64 p = 0; p < seen.pixels; ++p) {
    u32 words[2] = {0, 0};
    std::memcpy(words, bytes.data() + p * 12, 8);
    if (words[0] == seen.instance) clusters.push_back(words[1]);
  }
  std::sort(clusters.begin(), clusters.end());
  seen.clusters =
      static_cast<u32>(std::unique(clusters.begin(), clusters.end()) - clusters.begin());
  return seen;
}

}  // namespace

TEST_CASE("engine-view: a village's flight goes in at a door and sees the house from within") {
  const test::TempDir tmp("engine_view_village");
  // Level ground 64 m square at 50 cm, so the plan is the entry's alone.
  const Run synth =
      run_app(content_exe(), {"terrain-synth", tmp.file("ground.json"), "--kind", "flat", "--size",
                              "129", "--spacing-mm", "500", "--origin-mm", "-32000,-32000"});
  REQUIRE_MESSAGE(synth.exit_code == 0, synth.output);
  // One street of two rows, two houses of two storeys, plots two perches deep so both rows stand
  // on the ground there is.
  const std::string scene = tmp.file("scene.json");
  REQUIRE(io::write_file(scene, R"({"format": "engine.scene.v1", "name": "village-view",
  "terrain": {"size": 17, "extent": 32, "provider": "tiles",
              "tiles": {"heightfield": "ground.json", "cells": 32}},
  "placements": [{"generator": "village", "params": {
    "name": "test", "seed": 7, "houses": 2, "storeys": 2, "depth_perches": 2,
    "lanes": [{"name": "street", "waypoints": [[-28, 0, 0], [28, 0, 0]]}]}}]}
)") == io::Status::Ok);
  const std::string flight = tmp.file("flight.json");
  const Run planned = run_app(content_exe(), {"village", scene, tmp.file("plan.json"), "--ddc",
                                              tmp.file("ddc"), "--flight", flight});
  REQUIRE_MESSAGE(planned.exit_code == 0, planned.output);
  CHECK_MESSAGE(number_after(planned.output, "houses", 0) == 2, planned.output);
  CHECK_MESSAGE(number_after(planned.output, "boundaries", 0) >= 2, planned.output);
  REQUIRE(io::exists(flight));

  const std::string shots = tmp.file("shots");
  const Run drawn =
      run_app(view_exe(), {"--scene", scene, "--camera-path", flight, "--offscreen", "--width",
                           "320", "--height", "180", "--shadows", "off", "--marker-captures", shots,
                           "--capture-channels", "ids", "--ddc", tmp.file("ddc")});
  if (drawn.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << drawn.output);
    return;
  }
  REQUIRE_MESSAGE(drawn.exit_code == 0, drawn.output);
  // The houses are meshes with clusters, and nothing the scene asked for was dropped or withheld.
  const usize house = drawn.output.find("\"name\":\"house");
  CHECK_MESSAGE(house != std::string::npos, drawn.output);
  CHECK(drawn.output.find("\"clusters\":0,") == std::string::npos);
  CHECK(number_after(drawn.output, "dropped_instances", 1) == 0);
  // (A scene with no flora has no tail to withhold from, and its summary names none.)
  CHECK(number_after(drawn.output, "tail_withheld_instances", 0) == 0);
  CHECK(number_after(drawn.output, "instances_floating", 1) == 0);
  CHECK(number_after(drawn.output, "instances_buried", 1) == 0);

  // Every frame the flight calls inside is one building from within.
  u32 inside = 0;
  u32 building = 0xffffffffu;
  std::error_code ec;
  std::vector<std::string> files;
  for (const auto& entry : std::filesystem::directory_iterator(shots, ec))
    files.push_back(entry.path().generic_string());
  std::sort(files.begin(), files.end());
  for (const std::string& file : files) {
    const std::string_view name(file);
    if (!name.ends_with("-inside.ids.bin")) continue;
    const Seen seen = read_ids(file);
    REQUIRE(seen.pixels == 320u * 180u);
    INFO(file << ": " << seen.most << " of " << seen.pixels << " pixels are instance "
              << seen.instance << " over " << seen.clusters << " clusters; " << seen.empty
              << " empty");
    CHECK(seen.most * 10 >= seen.pixels * 7);  // seven tenths of the picture is the house
    CHECK(seen.clusters >= 3);
    CHECK(seen.empty * 20 <= seen.pixels);  // no more sky than a doorway or a window shows
    if (inside == 0) building = seen.instance;
    CHECK(seen.instance == building);  // the flight stays in the house it entered
    ++inside;
  }
  MESSAGE(inside << " inside frames of " << files.size() << " files in " << shots);
  CHECK(inside >= 4);  // the ground rooms, the stair's foot and head, the upper room
}
