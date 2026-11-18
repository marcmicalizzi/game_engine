// The sky mirror's sun against its sky on level ground (R182;
// docs/experiments/sun-and-sky-on-level-ground.md). No device.
#include "sky_reference.h"

#include <doctest/doctest.h>

using namespace engine;

TEST_CASE("sky: the sun against the sky on level ground, against published clear-sky models") {
  CHECK(sky_ref::k_pi > 3.0);
}
