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

std::vector<std::string> base_args(const test::TempDir& tmp, const char* scene, const char* path,
                                   bool sway = false) {
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
          sway ? "renderer.flora.motion=true" : "renderer.flora.motion=false",
          "--ddc",
          tmp.file("ddc")};
}

// The pixels whose colour differs between two captures of one size.
u32 differing_pixels(const std::string& a_path, const std::string& b_path) {
  image::Image a;
  image::Image b;
  REQUIRE(image::read_image(a_path, a) == io::Status::Ok);
  REQUIRE(image::read_image(b_path, b) == io::Status::Ok);
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
  return differing;
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

// Roadmap W74 (renderer.md, "Streamed trees sway"): the trees a tile brings sway as the whole
// read's do, at the same world time — in the inner ring's full mesh, and in the next ring's
// distance form, which is cut for the sway's padded spheres and sways with its source's parts.
TEST_CASE("engine-view: a world's streamed trees sway as the whole read's do") {
  const test::TempDir tmp("engine_view_authored_sway");
  REQUIRE(write_text(tmp.file("oak.flora.json"), k_oak));
  REQUIRE(write_text(tmp.file("spruce.flora.json"), k_spruce));
  REQUIRE(write_text(tmp.file("whole.json"), scene_text(nullptr)));
  REQUIRE(write_text(tmp.file("inner.json"),
                     scene_text(R"({"tile_size":32,"rings":[{"radius":30}]})")));
  // Two rings: the trees within two tiles of the camera are full meshes, the rest their ring-1
  // form, whose floor (33 m) is inside the sway's range.
  REQUIRE(write_text(tmp.file("rings.json"),
                     scene_text(R"({"tile_size":32,"rings":[{"radius":2},{"radius":30}]})")));
  REQUIRE(write_text(tmp.file("path.json"),
                     R"({"format":"engine.camera-path.v1","name":"over","fps":10,"keys":[)"
                     R"({"time":0,"position":[110,30,80],"target":[0,4,0]}]})"));
  // Thirty frames: the sway's clock at half a second, every tree off its rest pose.
  const auto capture = [&](const char* scene, bool sway, const char* png,
                           const std::vector<const char*>& extra = {}) {
    std::vector<std::string> args = base_args(tmp, scene, "path.json", sway);
    for (const char* a : {"--frames", "30", "--lod-fade", "0", "--capture"})
      args.push_back(a);
    args.push_back(tmp.file(png));
    for (const char* a : extra)
      args.push_back(a);
    return view(args);
  };
  const Run whole = capture("whole.json", true, "whole.png");
  if (whole.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << whole.output);
    return;
  }
  REQUIRE_MESSAGE(whole.exit_code == 0, whole.output);
  const Run rigid = capture("whole.json", false, "rigid.png");
  REQUIRE_MESSAGE(rigid.exit_code == 0, rigid.output);
  const Run inner = capture("inner.json", true, "inner.png");
  REQUIRE_MESSAGE(inner.exit_code == 0, inner.output);
  // The second ring sways only when the range reaches its floor (the default, 0, keeps forms
  // rigid).
  const Run rings =
      capture("rings.json", true, "rings.png", {"--tunable", "renderer.flora.sway_m=100"});
  REQUIRE_MESSAGE(rings.exit_code == 0, rings.output);
  // Before W74: the same streamed world with its tail drawn rigid.
  const Run before =
      capture("inner.json", true, "before.png", {"--tunable", "renderer.flora.tail=false"});
  REQUIRE_MESSAGE(before.exit_code == 0, before.output);

  for (const Run* run : {&inner, &rings}) {
    const JsonValue summary = summary_of(run->output);
    const JsonValue* flora = summary.find("flora");
    REQUIRE(flora != nullptr);
    REQUIRE(flora->is_object());
    CHECK(field_u64(*flora, "tail_instances") == 140);
    CHECK(field_u64(*flora, "tail_records") >= 140);
  }
  const u32 swayed = differing_pixels(tmp.file("whole.png"), tmp.file("rigid.png"));
  const u32 inner_diff = differing_pixels(tmp.file("whole.png"), tmp.file("inner.png"));
  const u32 rings_diff = differing_pixels(tmp.file("whole.png"), tmp.file("rings.png"));
  const u32 before_diff = differing_pixels(tmp.file("whole.png"), tmp.file("before.png"));
  MESSAGE("the sway moves " << swayed << " pixels of the whole read; streamed in one ring "
                            << inner_diff << " differ, in two " << rings_diff
                            << ", with the tail rigid " << before_diff);
  CHECK(swayed > 20);  // 76 at 320 x 180 from 136 m, half a second in (2026-10-24)
  CHECK(inner_diff == 0);
  CHECK(rings_diff == 0);
  CHECK(before_diff > 20);
}

// A tile that leaves and comes back brings its trees back in the phase they had: a tree's seed is
// its place in the scene file, never the order the tiles came in, so a flight out of every ring
// and back draws, at the same world time, what a camera that never moved draws.
TEST_CASE("engine-view: a streamed tree that leaves and comes back sways in its own phase") {
  const test::TempDir tmp("engine_view_authored_return");
  REQUIRE(write_text(tmp.file("oak.flora.json"), k_oak));
  REQUIRE(write_text(tmp.file("spruce.flora.json"), k_spruce));
  REQUIRE(write_text(tmp.file("streamed.json"),
                     scene_text(R"({"tile_size":32,"rings":[{"radius":2},{"radius":30}]})")));
  REQUIRE(write_text(tmp.file("stay.json"),
                     R"({"format":"engine.camera-path.v1","name":"stay","fps":10,"keys":[)"
                     R"({"time":0,"position":[110,30,80],"target":[0,4,0]},)"
                     R"({"time":4,"position":[110,30,80],"target":[0,4,0]}]})"));
  // Out past the outer ring's 960 m, where every tile has gone, and back for the last two seconds.
  REQUIRE(write_text(tmp.file("away.json"),
                     R"({"format":"engine.camera-path.v1","name":"away","fps":10,"keys":[)"
                     R"({"time":0,"position":[110,30,80],"target":[0,4,0]},)"
                     R"({"time":1,"position":[2600,30,80],"target":[2500,4,0]},)"
                     R"({"time":2,"position":[110,30,80],"target":[0,4,0]},)"
                     R"({"time":4,"position":[110,30,80],"target":[0,4,0]}]})"));
  const auto fly = [&](const char* path, const char* png) {
    std::vector<std::string> args = base_args(tmp, "streamed.json", path, true);
    // Both rings swaying, so every one of the 140 trees leaves and comes back with a phase.
    for (const char* a : {"--tunable", "renderer.flora.sway_m=100", "--frames", "40", "--lod-fade",
                          "0", "--capture"})
      args.push_back(a);
    args.push_back(tmp.file(png));
    return view(args);
  };
  const Run stay = fly("stay.json", "stay.png");
  if (stay.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << stay.output);
    return;
  }
  REQUIRE_MESSAGE(stay.exit_code == 0, stay.output);
  const Run away = fly("away.json", "away.png");
  REQUIRE_MESSAGE(away.exit_code == 0, away.output);
  const JsonValue summary = summary_of(away.output);
  const JsonValue* flora = summary.find("flora");
  REQUIRE(flora != nullptr);
  REQUIRE(flora->is_object());
  CHECK(field_u64(*flora, "tail_instances") == 140);
  const u32 differing = differing_pixels(tmp.file("stay.png"), tmp.file("away.png"));
  MESSAGE("away and back against staying: " << differing << " pixels differ");
  CHECK(differing == 0);
}

// Roadmap R94 (renderer.md, "Static structures for rigid forms"): under the ray tracing chain a
// rigid distance form is drawn at its fixed cut — its frontier — and traced against structures
// built once, so the traced shadows of the raster picture and the ray-traced picture itself are
// the bytes the per-frame build of the same cut gives (`renderer.rt.static_build=false`), in two
// rings past the inner one; and the frame builds only what is not a form.
TEST_CASE("engine-view: a rigid form's structures built once trace the per-frame build's bytes") {
  const test::TempDir tmp("engine_view_rt_static_forms");
  REQUIRE(write_text(tmp.file("oak.flora.json"), k_oak));
  REQUIRE(write_text(tmp.file("spruce.flora.json"), k_spruce));
  REQUIRE(write_text(
      tmp.file("streamed.json"),
      scene_text(R"({"tile_size":16,"rings":[{"radius":1.5},{"radius":3},{"radius":6}]})")));
  // Low over the trees' edge, looking across them: the inner ring's full meshes, both outer
  // rings' forms, and the forms' traced shadows on the sand.
  REQUIRE(write_text(tmp.file("path.json"),
                     R"({"format":"engine.camera-path.v1","name":"across","fps":10,"keys":[)"
                     R"({"time":0,"position":[-60,14,10],"target":[30,2,0]}]})"));
  const auto capture = [&](const char* png, const std::vector<const char*>& extra) {
    std::vector<std::string> args = base_args(tmp, "streamed.json", "path.json");
    args.erase(args.begin() + 9, args.begin() + 11);  // the base's --shadows off
    for (const char* a : {"--frames", "3", "--capture"})
      args.push_back(a);
    args.push_back(tmp.file(png));
    for (const char* a : extra)
      args.push_back(a);
    return view(args);
  };
  struct Case {
    const char* name;
    std::vector<const char*> mode;
  };
  const Case cases[] = {{"traced shadows", {"--shadows", "rt"}},
                        {"ray path", {"--raster", "rt", "--shadows", "rt"}}};
  for (const Case& c : cases) {
    std::vector<const char*> per_frame = c.mode;
    for (const char* a : {"--tunable", "renderer.rt.static_build=false"})
      per_frame.push_back(a);
    const Run built_once = capture("once.png", c.mode);
    if (built_once.exit_code == 3) {
      MESSAGE("engine-view unavailable here: " << built_once.output);
      return;
    }
    REQUIRE_MESSAGE(built_once.exit_code == 0, built_once.output);
    const Run every_frame = capture("every.png", per_frame);
    REQUIRE_MESSAGE(every_frame.exit_code == 0, every_frame.output);
    const JsonValue once_summary = summary_of(built_once.output);
    const JsonValue every_summary = summary_of(every_frame.output);
    const JsonValue* once_rt = once_summary.find("rt");
    const JsonValue* every_rt = every_summary.find("rt");
    REQUIRE(once_rt != nullptr);
    REQUIRE(every_rt != nullptr);
    const u32 differing = differing_pixels(tmp.file("once.png"), tmp.file("every.png"));
    MESSAGE(std::string(c.name) << ": " << field_u64(*once_rt, "static_forms") << " forms of "
                   << field_u64(*once_rt, "static_clusters") << " clusters built once, "
                   << field_u64(*once_rt, "static_instances") << " instances referencing them; "
                   << differing << " pixels differ from the per-frame build");
    CHECK(field_u64(*once_rt, "static_forms") > 0);
    CHECK(field_u64(*once_rt, "static_instances") > 0);
    CHECK(field_u64(*once_rt, "static_bytes") > 0);
    CHECK(field_u64(*every_rt, "static_instances") == 0);
    CHECK(differing == 0);
  }
}

#endif
