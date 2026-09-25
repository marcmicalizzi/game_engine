// End to end: a scene with a `ruins` entry over the procedural terrain, drawn by engine-view
// offscreen (docs/subsystems/ruins.md, "Where it runs"). The kit of boxes is written into the
// test's scratch directory; the run must place exactly the assembler's pieces plus the terrain, and
// its capture must show them — the same camera over the same terrain without the ruins draws a
// picture that differs in a real share of its pixels. Compiled only with the ruins capability;
// skips (exit 3) on a machine with no Vulkan device.
#include <core/json/json.h>
#include <core/platform/process.h>
#include <domain/ruins/assembler.h>
#include <domain/ruins/kit.h>
#include <domain/ruins/synthetic_kit.h>
#include <foundation/image/decode.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;

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

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

std::string scene_text(bool with_ruins) {
  std::string text = R"({"format":"engine.scene.v1","name":"ruins-view",)"
                     R"("terrain":{"size":65,"extent":60,"seed":4,"dune_height":1.0})";
  if (with_ruins) {
    text += R"(,"ruins":[{"kit":"kit/kit.json","seed":5,"tile_size":24,"tile_min":[-1,-1],)"
            R"("tile_max":[0,0],"count":4,"wind_deg":0}])";
  }
  return text + "}";
}

}  // namespace

TEST_CASE("engine-view: a scene's ruins draw over the terrain, offscreen") {
  const test::TempDir tmp("engine_view_ruins");
  std::string error;
  std::string kit_path;
  REQUIRE_MESSAGE(
      ruins::write_synthetic_kit(tmp.file("kit"), ruins::SyntheticKitOptions{}, &error, &kit_path),
      error);
  REQUIRE(write_text(tmp.file("ruins.json"), scene_text(true)));
  REQUIRE(write_text(tmp.file("bare.json"), scene_text(false)));
  // One camera, a little above the four tiles and looking across them.
  REQUIRE(write_text(tmp.file("path.json"),
                     R"({"format":"engine.camera-path.v1","name":"over","fps":10,"keys":[)"
                     R"({"time":0,"position":[22,9,30],"target":[-2,1,-4],"fov_deg":60}]})"));

  // What the assembler puts on those tiles: the count does not depend on the ground.
  ruins::Kit kit;
  REQUIRE_MESSAGE(ruins::read_kit_file(kit_path, kit, error), error);
  Vector<ruins::TileCoord> tiles;
  ruins::choose_tiles(5, ruins::TileCoord{-1, -1}, ruins::TileCoord{0, 0}, 4, 0.0f, tiles);
  ruins::Placement placement;
  placement.world_seed = 5;
  placement.tile_cm = 2400;
  ruins::Output built;
  REQUIRE(ruins::assemble_tiles(kit, placement,
                                std::span<const ruins::TileCoord>(tiles.data(), tiles.size()),
                                nullptr, built, &error));
  REQUIRE(built.sites.size() == 4);

  auto draw = [&](const char* scene, const char* capture) {
    return view({"--scene", tmp.file(scene), "--camera-path", tmp.file("path.json"), "--offscreen",
                 "--frames", "3", "--capture", tmp.file(capture), "--width", "320", "--height",
                 "180", "--shadows", "off", "--ddc", tmp.file("ddc")});
  };
  const Run run = draw("ruins.json", "ruins.png");
  if (run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << run.output);
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  const usize line_start = run.output.find_last_of('\n', run.output.size() - 2);
  const std::string last = run.output.substr(line_start == std::string::npos ? 0 : line_start + 1);
  JsonValue summary;
  REQUIRE_MESSAGE(parse_json(last, summary).ok, last);
  u64 instances = 0;
  REQUIRE(summary.find("instances") != nullptr);
  REQUIRE(summary.find("instances")->get_u64(instances));
  CHECK(instances == 1 + built.instances.size());  // the terrain and every piece

  const Run bare = draw("bare.json", "bare.png");
  REQUIRE_MESSAGE(bare.exit_code == 0, bare.output);
  image::Image with;
  image::Image without;
  REQUIRE(image::read_image(tmp.file("ruins.png"), with) == io::Status::Ok);
  REQUIRE(image::read_image(tmp.file("bare.png"), without) == io::Status::Ok);
  REQUIRE(with.width == without.width);
  REQUIRE(with.height == without.height);
  u32 differing = 0;
  const u32 pixels = with.width * with.height;
  for (u32 p = 0; p < pixels; ++p) {
    u32 d = 0;
    for (u32 c = 0; c < 3; ++c)
      d += static_cast<u32>(std::abs(static_cast<i32>(with.pixels[p * 4 + c]) -
                                     static_cast<i32>(without.pixels[p * 4 + c])));
    differing += d > 24 ? 1u : 0u;
  }
  MESSAGE("ruins: " << built.instances.size() << " pieces in 4 buildings; " << differing << " of "
                    << pixels << " pixels differ from the bare terrain");
  CHECK(differing > pixels / 50);  // more than 2% of the picture is ruins
}
