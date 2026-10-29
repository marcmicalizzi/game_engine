#include <core/platform/process.h>
#include <domain/geometry/cluster_file.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <fstream>
using namespace engine;

TEST_CASE("engine-content: cooked litter preserves the needle terminal form") {
  test::TempDir tmp("litter_cook");
  REQUIRE(tmp.ok());
  const auto recipe = tmp.file("needles.litter.json"), output = tmp.file("needles.clusters");
  std::ofstream(recipe) << R"({"kind":"needles","seed":17})";
  const auto exe = test::app_path(ENGINE_APP_PATH);
  const std::string_view args[] = {exe, "build", recipe, output, "--page-bytes", "65536"};
  platform::Process process;
  std::string error, log;
  REQUIRE_MESSAGE(process.spawn(args, &error, true), error);
  process.close_stdin();
  process.read_all(log);
  REQUIRE_MESSAGE(process.wait() == 0, log);
  geometry::ClusterFileData cooked;
  REQUIRE_MESSAGE(geometry::read_cluster_file(output, cooked, &error), error);
  CHECK(cooked.generator == "litter");
  CHECK(cooked.mesh.leaf_triangle_count == 2400);
  u32 triangles = 0;
  for (u32 i = 0; i < cooked.mesh.lod.size(); ++i)
    if (cooked.mesh.lod[i].parent_error == geometry::k_lod_terminal_error)
      triangles += cooked.mesh.mesh.clusters[i].triangle_count;
  CHECK(triangles == 800);
}
