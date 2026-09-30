// Shared doctest entry point linked into every engine_<module>_tests executable.
//
// A main() of our own rather than DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN, for two reasons. The CPU
// baseline check of ADR-0031 has to be the first thing a binary of this tree does, and a test
// binary is a binary of this tree — the one that would otherwise be the first to meet a machine
// too old for the build, since `ctest` is what a new machine runs first. And a test is what takes
// the machine-wide GPU lock when it opens a GPU device (ADR-0050), which is decided here, once,
// for every test executable and every process one starts.
// engine-lint: allow-exceptions test harness
#define DOCTEST_CONFIG_IMPLEMENT
#include <core/platform/cpu_baseline.h>

#include <doctest/doctest.h>

#include <cstdlib>
#include <ctime>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#ifndef ENGINE_TEST_GPU_LOCK_WAIT_S
#error "cmake/EngineTesting.cmake defines ENGINE_TEST_GPU_LOCK_WAIT_S, the queue budget of a test"
#endif

namespace {

std::string environment(const char* name) {
#if defined(_MSC_VER)
  char* value = nullptr;
  size_t size = 0;
  if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return {};
  std::string out(value);
  std::free(value);
  return out;
#else
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string{};
#endif
}

void set_environment(const char* name, const std::string& value) {
#if defined(_WIN32)
  (void)::SetEnvironmentVariableA(name, value.c_str());
  (void)_putenv_s(name, value.c_str());
#else
  (void)::setenv(name, value.c_str(), 1);
#endif
}

// The GPU lock (foundation/gpu_lock/device_hold.h, docs/subsystems/gpu_lock.md). A GPU device
// opened by this process or by any process it starts takes the machine-wide lock for as long as
// the device lives, so a test run holds the GPU for exactly the tests that use it and nobody has
// to wrap `ctest` in `tools/gpu-lock.ps1 run`. Set only when unset, so `0` in the environment
// turns it off for a run. Nothing but a test sets it: the owner's own engine-view and engine-host
// sessions never take the lock and never wait for it.
//
// And the deadline of every wait for it in this test's tree: now plus the queue budget, which
// CMake also adds to the TIMEOUT of every test that can wait (cmake/EngineModule.cmake), so a
// test that runs out of queue ends with its own message and exit code 75, reported as skipped,
// before CTest's timeout could make it look like a hang. Always this test's own: a deadline
// inherited from whatever started the test would be somebody else's budget.
void take_part_in_the_gpu_lock() {
  if (environment("ENGINE_GPU_LOCK_ON_DEVICE").empty()) {
    set_environment("ENGINE_GPU_LOCK_ON_DEVICE", "1");
  }
  const long long deadline =
      static_cast<long long>(std::time(nullptr)) + (ENGINE_TEST_GPU_LOCK_WAIT_S);
  set_environment("ENGINE_GPU_LOCK_WAIT_UNTIL", std::to_string(deadline));
}

}  // namespace

int main(int argc, char** argv) {
  engine::platform::require_cpu_baseline();
  take_part_in_the_gpu_lock();
  return doctest::Context(argc, argv).run();
}
