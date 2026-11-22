// Shared doctest entry point linked into every engine_<module>_tests executable.
//
// A main() of our own rather than DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN, for two reasons. The CPU
// baseline check of ADR-0031 has to be the first thing a binary of this tree does, and a test
// binary is a binary of this tree — the one that would otherwise be the first to meet a machine
// too old for the build, since `ctest` is what a new machine runs first. And a test is what takes
// the machine-wide GPU lock when it opens a GPU device (ADR-0049), which is decided here, once,
// for every test executable and every process one starts.
// engine-lint: allow-exceptions test harness
#define DOCTEST_CONFIG_IMPLEMENT
#include <core/platform/cpu_baseline.h>

#include <doctest/doctest.h>
#include <test_environment.h>
#include <test_temp_dir.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

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

// No test reads the machine's material pack (docs/subsystems/materials.md, "Where the hosts find
// it"; T103). The default lookup is a host's whatever its derived-data root, so a load in this
// process, or in any `engine-view`, `engine-host` or `engine-content` it starts, would otherwise
// draw with whatever pack this checkout has fetched or generated, and a test would pass or fail by
// the machine. `materials::default_pack_dir` answers none while this is `0`; a test that wants a
// pack names one, or holds `test::ScopedEnv(k_default_materials_env, "1")` for its case. Always
// set, never inherited: a developer's shell does not decide what a test reads.
void read_no_default_materials() { set_environment(engine::test::k_default_materials_env, "0"); }

// No test plays through the machine's speakers (S112-3; docs/subsystems/audio.md, "No test opens
// the speakers"): every engine-view a case starts inherits this, and its `--audio auto` answers
// the null backend however it was started — a window, `--interactive`, a game module. Always set,
// never inherited, like the pack's switch.
void open_no_audio_device() { set_environment(engine::test::k_audio_auto_env, "null"); }

// A test's label against its devices (roadmap T170; foundation/gpu_lock/device_hold.h). The
// configure gives a test labelled `gpu` ENGINE_TEST_DEVICES=report; here it becomes a file in a
// scratch directory of this run's own, which `gfx::Device::create` in this process or any process
// it starts writes a line to. A run that passed and left no file never tried to open a device, and
// says so: a report, never a failure, since a part may hold only CPU cases and a weaker machine's
// skips can end a case before it asks. `forbidden` (a test without the label) is device_hold's to
// enforce and needs nothing here.
struct DeviceReport {
  std::optional<engine::test::TempDir> dir;
  std::string marker;

  void begin() {
    if (environment("ENGINE_TEST_DEVICES") != "report") return;
    dir.emplace("devices");
    marker = dir->file("opened");
    set_environment("ENGINE_TEST_DEVICES", marker);
  }

  void end(int code) const {
    if (marker.empty() || code != 0) return;
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::path(marker), ec)) return;
    std::fprintf(stderr,
                 "engine-test: labelled gpu and opened no device: nothing in this test's tree "
                 "called gfx::Device::create (docs/ci/what-to-run.md, \"Tests that open no "
                 "device\")\n");
  }
};

// The proof that a process runs under a test's main (roadmap T181; docs/subsystems/apps.md,
// "Running as a weaker card"): `ENGINE_TEST_RUN` names a scratch directory this main made and
// removes as it returns, so `engine-view` honours `ENGINE_GFX_TEST_DEVICE` only while the test that
// started it is alive. Always this run's own, never inherited, as the variables above.
struct TestRun {
  std::optional<engine::test::TempDir> dir;

  void begin() {
    dir.emplace("run");
    set_environment("ENGINE_TEST_RUN", dir->path());
  }
};

}  // namespace

int main(int argc, char** argv) {
  engine::platform::require_cpu_baseline();
  take_part_in_the_gpu_lock();
  read_no_default_materials();
  open_no_audio_device();
  TestRun run;
  run.begin();
  DeviceReport devices;
  devices.begin();
  const int code = doctest::Context(argc, argv).run();
  devices.end(code);
  return code;
}
