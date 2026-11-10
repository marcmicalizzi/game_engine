// Tests for the landform capability (ADR-0027). TODO(landform): the invariants listed on the docs
// page
// belong here, one test each, before the capability ships.
#include <domain/landform/landform.h>

#include <doctest/doctest.h>

#include <string_view>

using namespace engine;

namespace {

// Named once so the assertions below stay inside the column limit whatever the capability is
// called, which is also how they read best.
using System = landform::LandformSystem;

}  // namespace

TEST_CASE("landform system runs a tick") {
  System system;
  system.begin_tick(7);
  system.tick(1.0f / 60.0f);
  system.end_tick();
  CHECK(system.tick_index() == 7u);
  CHECK(system.elapsed() == doctest::Approx(1.0f / 60.0f));
}

TEST_CASE("landform LOD policy never coarsens as the observer gets nearer") {
  // The tier is monotonic in the observer score (plan 05 §5.4); hysteresis, once it exists,
  // widens the boundaries but must not break this.
  CHECK(System::lod_tier(0.0f) <= System::lod_tier(10.0f));
  CHECK(System::lod_tier(10.0f) <= System::lod_tier(1000.0f));
}

TEST_CASE("landform declares a determinism stance") {
  const std::string_view determinism{landform::k_determinism};
  CHECK((determinism == "hashed" || determinism == "derived"));
}
