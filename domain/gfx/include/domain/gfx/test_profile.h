#pragma once

// A test's device profile in the programs a test starts (roadmap T181 and R193;
// docs/subsystems/gfx.md, "A test's device profile reaches every host it starts"). The renderer's
// cases open their device as a weaker card under `ENGINE_GFX_TEST_DEVICE`
// (systems/renderer/tests/test_device.h, T153 and T162); an end-to-end test starts `engine-view`
// and `engine-host`, which open their own. **Every host a test starts opens its device through
// this one function**, because a test that compares two hosts' pictures compares two devices
// otherwise: from 2026-11-17 to 2026-11-19 `engine-view` took the profile and `engine-host` did
// not, and "the viewer and the host draw the same bytes" failed on the profile by a handful of
// bytes, the vertex path under the cascaded maps against the hardware path under traced shadows.
//
// The profile is honoured **only under a test's main**: the variable alone does nothing. The test
// main (tests/support/test_main.cpp) makes a scratch directory of its run's own under
// `<system temp>/engine-tests/`, names it in `ENGINE_TEST_RUN`, and removes it as it returns; a
// host takes the profile only while that variable names an existing directory under that root. A
// player's run cannot take it by accident: a profile left in a shell does nothing without the
// second variable, and a copied pair of variables names a directory that went with the test that
// made it. Either way the run logs a warning that it ignored the profile and why, and opens the
// card as it is.

#include <domain/gfx/requirements.h>

#include <string_view>

namespace engine::gfx {

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
DeviceOverrides test_profile_overrides(std::string_view profile);

// The overrides this process's device opens with: the profile's when it is honoured, none
// otherwise. Read once; the first call says in the log (`gfx.test_profile`) what it did with a
// profile it was given.
const DeviceOverrides& test_device_overrides();

}  // namespace engine::gfx
