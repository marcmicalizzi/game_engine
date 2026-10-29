#include <core/json/json.h>
#include <core/platform/process.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
using namespace engine;
namespace {
struct Output {
  i32 code = -1;
  std::string text;
};
Output run_soil(std::vector<std::string> args) {
  const std::string exe = test::app_path(ENGINE_APP_PATH);
  std::vector<std::string_view> argv{exe};
  for (const auto& a : args)
    argv.push_back(a);
  platform::Process p;
  std::string error;
  Output result;
  if (!p.spawn(argv, &error)) {
    FAIL(error);
    return result;
  }
  p.close_stdin();
  p.read_all(result.text);
  result.code = p.wait();
  return result;
}
}  // namespace
TEST_CASE("engine-content soil cook keeps source and publishes matching terrain and mesh") {
#if ENGINE_CONTENT_SOIL
  test::TempDir tmp("soil_cook");
  REQUIRE(tmp.ok());
  std::ofstream(tmp.file("throw.soil.json")) << "{\"kind\":\"TreeThrow\"}";
  const auto source = tmp.file("source.json"), recipe = tmp.file("throw.soil.json");
  const auto synth = run_soil({"terrain-synth", source, "--kind", "flat", "--size", "129",
                               "--spacing-mm", "250", "--origin-mm", "-16000,-16000"});
  REQUIRE_MESSAGE(synth.code == 0, synth.text);
  const auto dir = tmp.file("cooked");
  const auto cooked = run_soil({"soil-cook", recipe, source, dir, "0", "0", "0"});
  REQUIRE_MESSAGE(cooked.code == 0, cooked.text);
  CHECK(std::filesystem::exists(std::filesystem::path(dir) / "scene.json"));
  const auto duplicate = run_soil({"soil-cook", recipe, source, dir, "0", "0", "0"});
  CHECK(duplicate.code != 0);
  const auto bad = run_soil({"soil-cook", recipe, source, tmp.file("bad"), "100", "0", "0"});
  CHECK(bad.code != 0);
  CHECK_FALSE(std::filesystem::exists(tmp.file("bad")));
  const auto built = run_soil({"build", (std::filesystem::path(dir) / "feature.soil.json").string(),
                               tmp.file("feature.clusters")});
  REQUIRE_MESSAGE(built.code == 0, built.text);
#else
  const auto result = run_soil({"soil-cook"});
  CHECK(result.code != 0);
  CHECK(result.text.find("soil capability") != std::string::npos);
#endif
}
