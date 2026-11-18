// engine-view's device profile is a test's alone (roadmap T174; docs/subsystems/apps.md, "Running as
// a weaker card"): `ENGINE_GFX_TEST_DEVICE` is honoured only while `ENGINE_TEST_RUN` names a live
// scratch directory of a test's main under `<system temp>/engine-tests/`, so a player's run cannot
// take it by accident. No GPU: the decision and the profile's rows.

#include "../test_device.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <string>

using namespace engine;
using view::TestProfile;

TEST_CASE("engine-view: a device profile is honoured only under a live test's main") {
  const std::string root = "C:/tmp/engine-tests";
  const std::string run = root + "/engine_view-run-3f2a";
  // No profile: nothing to decide, whatever else is set.
  CHECK(view::judge_test_profile("", run, root, true) == TestProfile::none);
  // A profile left in a player's shell: no test run named, so ignored.
  CHECK(view::judge_test_profile("titanxp", "", root, false) == TestProfile::no_test);
  // Both variables copied out of a test's environment after it ended: the directory went with it.
  CHECK(view::judge_test_profile("titanxp", run, root, false) == TestProfile::stale_run);
  // A directory that exists but is not a test's scratch directory.
  CHECK(view::judge_test_profile("titanxp", "C:/Users/someone/Desktop", root, true) ==
        TestProfile::stale_run);
  CHECK(view::judge_test_profile("titanxp", root + "/../elsewhere", root, true) ==
        TestProfile::stale_run);
  CHECK(view::judge_test_profile("titanxp", root, root, true) == TestProfile::stale_run);
  // No root to check against (the temp directory could not be read).
  CHECK(view::judge_test_profile("titanxp", run, "", true) == TestProfile::stale_run);
  // Under a live test's main.
  CHECK(view::judge_test_profile("titanxp", run, root, true) == TestProfile::honoured);
}

TEST_CASE("engine-view: a profile's rows are the renderer tests' table") {
  CHECK(view::test_profile_overrides("baseline").absent.size() == 1);
  CHECK(view::test_profile_overrides("baseline").absent[0] == "VK_EXT_mesh_shader");
  const gfx::DeviceOverrides titan = view::test_profile_overrides("titanxp");
  REQUIRE(titan.absent.size() == 5);
  CHECK(titan.absent[0] == "VK_EXT_mesh_shader");
  CHECK(titan.absent[1] == "VK_KHR_ray_query");
  const gfx::DeviceOverrides list = view::test_profile_overrides(" VK_KHR_ray_query , ,x ");
  REQUIRE(list.absent.size() == 2);
  CHECK(list.absent[0] == "VK_KHR_ray_query");
  CHECK(list.absent[1] == "x");
  CHECK(view::test_profile_overrides("").empty());
}

TEST_CASE("engine-view: this test's own main gives the run a live scratch directory") {
  // The test main names one for every test process (tests/support/test_main.cpp), which is what
  // lets the end-to-end tests' engine-view take a profile the gate's weaker-card leg sets.
  char* value = nullptr;
#if defined(_MSC_VER)
  size_t size = 0;
  REQUIRE(_dupenv_s(&value, &size, view::k_test_run_env) == 0);
  REQUIRE(value != nullptr);
  const std::string run(value);
  std::free(value);
#else
  value = std::getenv(view::k_test_run_env);
  REQUIRE(value != nullptr);
  const std::string run(value);
#endif
  CHECK_FALSE(run.empty());
  CHECK(run.find("engine-tests") != std::string::npos);
}
