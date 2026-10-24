// End to end: a streamed world's own trees (renderer.md, "Authored instances per tile";
// authored.md). The scene's instances — small oaks with their card levels and spruces with none —
// handed to the `authored` generator a tile at a time draw the whole read's picture where every
// tile is in the inner ring, and a tile handed from one ring's form to the next changes no pixel of
// a still frame (engine-view's handover pass, with the cross-fade off). Compiled with the flora,
// world and authored capabilities; skips (exit 3) on a machine with no Vulkan device.
#include "view_process.h"

#include <core/json/json.h>
#include <foundation/image/decode.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace engine;

#if ENGINE_VIEW_WORLD && ENGINE_VIEW_TESTS_FLORA

namespace {

struct Run {
  i32 exit_code = -1;
  std::string output;
};

Run view(const std::vector<std::string>& args) {
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

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

// authored_scene_tests.cpp's trees, small enough for a debug build.
constexpr const char* k_oak =
    R"({"species":"broadleaf","broadleaf":{"seed":3,"height_m":8,"trunk_radius_m":0.25,)"
    R"("triangle_budget":8000,"card_bake_px":16,"impostor_px":32}})";
constexpr const char* k_spruce =
    R"({"species":"conifer","conifer":{"seed":5,"height_m":6,"trunk_radius_m":0.1,)"
    R"("whorls":6,"branches_per_whorl":4,"crown_radius_m":1.5,"triangle_budget":8000}})";

std::string scene_text(const char* world) {
  std::string text =
      R"({"format":"engine.scene.v1","name":"authored-view",)"
      R"("terrain":{"size":65,"extent":400,"seed":7,"dune_height":0.3,"dune_wavelength":60,)"
      R"("generator":"Waves"},)"
      R"("meshes":[{"name":"oak","path":"oak.flora.json"},)"
      R"({"name":"spruce","path":"spruce.flora.json"}],)"
      R"("scatters":[{"mesh":0,"center":[0,0,0],"radius_min":2,"radius_max":120,"count":70,)"
      R"("seed":11,"scale_min":0.8,"scale_max":1.2},)"
      R"({"mesh":1,"center":[0,0,0],"radius_min":2,"radius_max":120,"count":70,"seed":12}])";
  if (world != nullptr) text += std::string(R"(,"world":)") + world;
  return text + "}";
}

std::vector<std::string> base_args(const test::TempDir& tmp, const char* scene, const char* path) {
  return {"--scene",
          tmp.file(scene),
          "--camera-path",
          tmp.file(path),
          "--offscreen",
          "--width",
          "320",
          "--height",
          "180",
          "--shadows",
          "off",
          "--tunable",
          "renderer.flora.motion=false",
          "--ddc",
          tmp.file("ddc")};
}

JsonValue summary_of(const std::string& output) {
  const usize line_start = output.find_last_of('\n', output.size() - 2);
  const std::string last = output.substr(line_start == std::string::npos ? 0 : line_start + 1);
  JsonValue summary;
  REQUIRE_MESSAGE(parse_json(last, summary).ok, last);
  return summary;
}

u64 field_u64(const JsonValue& object, const char* name) {
  const JsonValue* v = object.find(name);
  u64 out = 0;
  if (v != nullptr) v->get_u64(out);
  return out;
}

}  // namespace

TEST_CASE("engine-view: a world's own trees streamed per tile draw the whole read's picture") {
  const test::TempDir tmp("engine_view_authored");
  REQUIRE(write_text(tmp.file("oak.flora.json"), k_oak));
  REQUIRE(write_text(tmp.file("spruce.flora.json"), k_spruce));
  REQUIRE(write_text(tmp.file("whole.json"), scene_text(nullptr)));
  // One ring of 30 tiles: every tile the trees stand on is in it, in the full mesh.
  // The 32 m tiles also contain the enlarged three-plane foliage bounds (G87).
  REQUIRE(write_text(tmp.file("streamed.json"),
                     scene_text(R"({"tile_size":32,"rings":[{"radius":30}]})")));
  REQUIRE(write_text(tmp.file("path.json"),
                     R"({"format":"engine.camera-path.v1","name":"over","fps":10,"keys":[)"
                     R"({"time":0,"position":[110,30,80],"target":[0,4,0]}]})"));
  std::vector<std::string> whole_args = base_args(tmp, "whole.json", "path.json");
  for (const char* a : {"--frames", "3", "--capture"})
    whole_args.push_back(a);
  whole_args.push_back(tmp.file("whole.png"));
  const Run whole = view(whole_args);
  if (whole.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << whole.output);
    return;
  }
  REQUIRE_MESSAGE(whole.exit_code == 0, whole.output);
  std::vector<std::string> streamed_args = base_args(tmp, "streamed.json", "path.json");
  for (const char* a : {"--frames", "3", "--capture"})
    streamed_args.push_back(a);
  streamed_args.push_back(tmp.file("streamed.png"));
  const Run streamed = view(streamed_args);
  REQUIRE_MESSAGE(streamed.exit_code == 0, streamed.output);
  const JsonValue summary = summary_of(streamed.output);
  const JsonValue* world = summary.find("world");
  REQUIRE(world != nullptr);
  CHECK(field_u64(*world, "authored_streamed") == 140);
  CHECK(field_u64(*world, "tail_pairs") > 0);
  CHECK(field_u64(*world, "refused") == 0);

  image::Image a;
  image::Image b;
  REQUIRE(image::read_image(tmp.file("whole.png"), a) == io::Status::Ok);
  REQUIRE(image::read_image(tmp.file("streamed.png"), b) == io::Status::Ok);
  REQUIRE(a.width == b.width);
  REQUIRE(a.height == b.height);
  u32 differing = 0;
  const u32 pixels = a.width * a.height;
  for (u32 p = 0; p < pixels; ++p) {
    bool d = false;
    for (u32 c = 0; c < 3; ++c)
      d = d || a.pixels[p * 4 + c] != b.pixels[p * 4 + c];
    differing += d ? 1u : 0u;
  }
  MESSAGE("streamed against whole: " << differing << " of " << pixels << " pixels differ");
  CHECK(differing == 0);
}

TEST_CASE("engine-view: a tree handed to its next ring's distance form changes no pixel") {
  const test::TempDir tmp("engine_view_authored_handover");
  REQUIRE(write_text(tmp.file("oak.flora.json"), k_oak));
  REQUIRE(write_text(tmp.file("spruce.flora.json"), k_spruce));
  REQUIRE(write_text(
      tmp.file("streamed.json"),
      scene_text(R"({"tile_size":16,"rings":[{"radius":1.5},{"radius":3},{"radius":6}]})")));
  // Low across the trees from one side to the other, so tiles cross every ring both ways.
  REQUIRE(write_text(tmp.file("path.json"),
                     R"({"format":"engine.camera-path.v1","name":"across","fps":10,"keys":[)"
                     R"({"time":0,"position":[-150,12,10],"target":[0,4,0]},)"
                     R"({"time":4,"position":[150,12,-10],"target":[250,4,0]}]})"));
  std::vector<std::string> args = base_args(tmp, "streamed.json", "path.json");
  for (const char* a : {"--frames", "40", "--lod-fade", "0", "--benchmark"})
    args.push_back(a);
  args.push_back(tmp.file("bench.jsonl"));
  args.push_back("--world-handover");
  args.push_back(tmp.file("handover.jsonl"));
  const Run run = view(args);
  if (run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << run.output);
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  std::ifstream log(tmp.file("handover.jsonl"), std::ios::binary);
  std::string line;
  u32 handovers = 0;
  u64 changed = 0;
  u64 drawn = 0;
  while (std::getline(log, line)) {
    JsonValue record;
    if (!parse_json(line, record).ok || record.find("coverage_changed") == nullptr) continue;
    ++handovers;
    changed += field_u64(record, "coverage_changed") + field_u64(record, "depth_changed") +
               field_u64(record, "color_changed");
    drawn += field_u64(record, "tile_before");
  }
  MESSAGE(handovers << " handovers over " << drawn << " of their tiles' pixels, " << changed
                    << " changed");
  CHECK(handovers > 0);
  CHECK(drawn > 0);
  CHECK(changed == 0);
}

#endif
