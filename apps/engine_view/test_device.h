#pragma once

// A test's device profile in `engine-view` (roadmap T174; docs/subsystems/apps.md, "Running as a
// weaker card"). The renderer's cases open their device as a weaker card under
// `ENGINE_GFX_TEST_DEVICE` (systems/renderer/tests/test_device.h, T153 and T162); the apps'
// end-to-end tests start `engine-view`, which opened the card as it is, so they could not. This is
// the same profile for `engine-view`'s device, and it is honoured **only under a test's main**:
// the variable alone does nothing. The test main (tests/support/test_main.cpp) makes a scratch
// directory of its run's own under `<system temp>/engine-tests/`, names it in `ENGINE_TEST_RUN`,
// and removes it as it returns; `engine-view` takes the profile only while that variable names an
// existing directory under that root. A player's run cannot take it by accident: a profile left in
// a shell does nothing without the second variable, and a copied pair of variables names a
// directory that went with the test that made it. Either way the run says on stderr that it
// ignored the profile and why, and opens the card as it is.

#include <domain/gfx/device.h>

#include <string>
#include <string_view>

namespace engine::view {

inline constexpr const char* k_test_device_env = "ENGINE_GFX_TEST_DEVICE";
inline constexpr const char* k_test_run_env = "ENGINE_TEST_RUN";

enum class TestProfile : u8 {
  none,       // no profile asked for
  honoured,   // asked for under a live test's main
  no_test,    // asked for with no test run named
  stale_run,  // the test run named is not a live test's scratch directory
};

// The decision, with what it reads passed in so a test can reach every branch: `profile` and
// `test_run` the two variables' values, `scratch_root` `<system temp>/engine-tests`, and
// `run_exists` whether `test_run` is a directory now.
TestProfile judge_test_profile(std::string_view profile, std::string_view test_run,
                               std::string_view scratch_root, bool run_exists);

// The requirement rows a profile reports absent: `baseline` the mesh shader, `titanxp` the TITAN
// Xp's absences, anything else a comma-separated list of rows — the renderer tests' table.
gfx::DeviceOverrides test_profile_overrides(std::string_view profile);

// The overrides this process's device opens with: the profile's when it is honoured, none
// otherwise. Read once; the first call says on stderr what it did with a profile it was given.
const gfx::DeviceOverrides& test_device_overrides();

}  // namespace engine::view
