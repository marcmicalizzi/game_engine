#include <core/profiling/profile.h>

#include <doctest/doctest.h>

using namespace engine;

namespace {

int instrumented_work(int n) {
  ENGINE_PROFILE_ZONE();
  int sum = 0;
  for (int i = 0; i < n; ++i) {
    ENGINE_PROFILE_ZONE_NAMED("iteration");
    sum += i;
  }
  ENGINE_PROFILE_PLOT("sum", static_cast<double>(sum));
  return sum;
}

}  // namespace

TEST_CASE("profiling: macros compile in both modes and cost nothing when off") {
  ENGINE_PROFILE_THREAD("profiling tests");
  CHECK(instrumented_work(10) == 45);
  ENGINE_PROFILE_FRAME();
  ENGINE_PROFILE_FRAME_NAMED("test frame");
  const char text[] = "message";
  ENGINE_PROFILE_MESSAGE(text, sizeof(text) - 1);
  int object = 0;
  ENGINE_PROFILE_ALLOC(&object, sizeof(object));
  ENGINE_PROFILE_FREE(&object);
  CHECK(profiling::enabled() == (ENGINE_PROFILING != 0));
  CHECK_FALSE(profiling::connected());  // no profiler attached to the test process
}
