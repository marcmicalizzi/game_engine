#pragma once

// An environment variable set for this test process and the processes it starts, and put back
// when the scope ends. For tests of behaviour an environment variable switches — the GPU lock's
// switch, path and deadline (foundation/gpu_lock/device_hold.h) — that must not leak into the
// next case of the same executable.
//
// Header-only, like the rest of tests/support; on Windows it sets both the C runtime's copy,
// which getenv reads, and the OS block, which is what CreateProcess hands a child.

#include <test_paths.h>

#include <cstdlib>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace engine::test {

// The switch the test main and the bench main set to `0` for every test, bench and child process,
// so none reads the machine's material pack (`materials::k_default_materials_env`, spelled here
// because this directory sits below every module; docs/subsystems/materials.md, "Where the hosts
// find it"). A test that wants the default lookup — one that builds a repository of its own with a
// pack in it — holds `ScopedEnv(k_default_materials_env, "1")` for its case.
inline constexpr const char* k_default_materials_env = "ENGINE_DEFAULT_MATERIALS";

// The switch the test main and the bench main set to `null` for every test, bench and child
// process, so engine-view's `--audio auto` never opens a real audio device under one (S112-3;
// `view::k_audio_auto_env`, spelled here for the same reason; docs/subsystems/audio.md, "No test
// opens the speakers"). An explicit `--audio <device>` is then the only way a case reaches them,
// and only the opt-in device test (`ENGINE_AUDIO_DEVICE_TEST=1`) may pass one.
inline constexpr const char* k_audio_auto_env = "ENGINE_AUDIO_AUTO";

// Sets `name` to `value` (empty removes it) and restores what it was.
class ScopedEnv {
 public:
  ScopedEnv(const char* name, const std::string& value)
      : name_(name), previous_(detail::environment(name)) {
    set(value);
  }
  ~ScopedEnv() { set(previous_); }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  void set(const std::string& value) {
#if defined(_WIN32)
    (void)::SetEnvironmentVariableA(name_, value.empty() ? nullptr : value.c_str());
    (void)_putenv_s(name_, value.c_str());
#else
    if (value.empty()) {
      (void)::unsetenv(name_);
    } else {
      (void)::setenv(name_, value.c_str(), 1);
    }
#endif
  }

  const char* name_;
  std::string previous_;
};

}  // namespace engine::test
