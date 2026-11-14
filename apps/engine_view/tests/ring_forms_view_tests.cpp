// End to end: a placement's form by ring (G197; renderer.md, "A placement's form by ring"). A
// streamed litter entry — rigid patches a generator places as one resident mesh each, in every
// ring — drawn as each ring's cut past the first: fewer pairs than the same walk with the forms
// off, and a tile handed from the mesh to its ring's form changes no pixel of a still frame
// (engine-view's handover pass, with the cross-fade off). Compiled with the litter and world
// capabilities; skips (exit 3) on a machine with no Vulkan device.
#include "view_process.h"

#include <core/json/json.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <fstream>
#include <string>
#include <vector>

using namespace engine;

// Compiled only where the world and the litter are (CMakeLists.txt), so no `#if` here.
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

// Litter over 16 x 16 tiles of 16 m, rings of 1.5, 3 and 6 tiles: the floors are 8.7 m and 33 m.
constexpr const char* k_scene =
    R"({"format":"engine.scene.v1","name":"ring-forms-view",)"
    R"("terrain":{"size":65,"extent":200,"seed":7,"dune_height":0.3,"dune_wavelength":30,)"
    R"("generator":"Waves"},)"
    R"("world":{"tile_size":16,"rings":[{"radius":1.5},{"radius":3},{"radius":6}]},)"
    R"("placements":[{"generator":"litter","params":{"meshes":["leaf.litter.json",)"
    R"("bark.litter.json"],"seed":29,"min_x":-8,"max_x":7,"min_z":-8,"max_z":7,"cell_m":4,)"
    R"("extent_m":1.3,"background":1.0}}]})";

std::vector<std::string> base_args(const test::TempDir& tmp) {
  return {"--scene", tmp.file("scene.json"), "--camera-path", tmp.file("path.json"), "--offscreen",
          "--width", "320", "--height", "180",
          // No `--shadows`: the default traces where the device traces, where a static form's
          // frontier would change at a handover (renderer.md, "A placement's form by ring"), and
          // the litter's forms are not static.
          "--lod-fade", "0", "--ddc", tmp.file("ddc")};
}

}  // namespace

TEST_CASE("engine-view: litter handed to its ring's form changes no pixel and holds fewer pairs") {
  const test::TempDir tmp("engine_view_ring_forms");
  REQUIRE(write_text(tmp.file("leaf.litter.json"), R"({"seed":17,"kind":"leaf"})"));
  REQUIRE(write_text(tmp.file("bark.litter.json"), R"({"seed":19,"kind":"bark"})"));
  REQUIRE(write_text(tmp.file("scene.json"), k_scene));
  // Low across the field from one side to the other, so tiles cross every ring both ways.
  REQUIRE(write_text(tmp.file("path.json"),
                     R"({"format":"engine.camera-path.v1","name":"across","fps":10,"keys":[)"
                     R"({"time":0,"position":[-120,3,4],"target":[0,0,0]},)"
                     R"({"time":4,"position":[120,3,-4],"target":[200,0,0]}]})"));
  std::vector<std::string> args = base_args(tmp);
  for (const char* a : {"--frames", "40", "--benchmark"})
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

  // The same flight with the forms off: every patch its whole mesh in every ring.
  const JsonValue summary = summary_of(run.output);
  const JsonValue* with = summary.find("world");
  REQUIRE(with != nullptr);
  std::vector<std::string> off = base_args(tmp);
  for (const char* a :
       {"--frames", "40", "--tunable", "renderer.forms.enabled=false", "--benchmark"})
    off.push_back(a);
  off.push_back(tmp.file("bench-off.jsonl"));
  const Run whole = view(off);
  REQUIRE_MESSAGE(whole.exit_code == 0, whole.output);
  const JsonValue summary_off = summary_of(whole.output);
  const JsonValue* without = summary_off.find("world");
  REQUIRE(without != nullptr);
  MESSAGE("tail pairs: " << field_u64(*with, "tail_pairs") << " with the forms, "
                         << field_u64(*without, "tail_pairs") << " without");
  CHECK(field_u64(*with, "refused") == 0);
  CHECK(field_u64(*with, "tail_pairs") < field_u64(*without, "tail_pairs"));
}
