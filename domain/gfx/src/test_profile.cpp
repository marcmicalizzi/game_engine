#include <core/base/macros.h>
#include <core/log/log.h>
#include <domain/gfx/device_lost.h>
#include <domain/gfx/test_profile.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace engine::gfx {

ENGINE_LOG_CATEGORY_DEFINE(log_test_profile, "gfx.test_profile");

namespace {

// getenv, spelled the way MSVC compiles without a deprecation warning.
std::string read_environment(const char* name) {
#if ENGINE_COMPILER_MSVC
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

// `a` is `root` or lies under it, compared as lexically normal paths (no file system read: the
// existence of the run's directory is the caller's to say).
bool lies_under(std::string_view path, std::string_view root) {
  const std::filesystem::path p = std::filesystem::path(path).lexically_normal();
  const std::filesystem::path r = std::filesystem::path(root).lexically_normal();
  const std::filesystem::path rel = p.lexically_relative(r);
  if (rel.empty()) return false;
  const std::string first = rel.begin()->string();
  return first != ".." && first != ".";
}

}  // namespace

TestProfile judge_test_profile(std::string_view profile, std::string_view test_run,
                               std::string_view scratch_root, bool run_exists) {
  if (profile.empty()) return TestProfile::none;
  if (test_run.empty()) return TestProfile::no_test;
  if (scratch_root.empty() || !run_exists || !lies_under(test_run, scratch_root)) {
    return TestProfile::stale_run;
  }
  return TestProfile::honoured;
}

DeviceOverrides test_profile_overrides(std::string_view profile) {
  DeviceOverrides overrides;
  if (profile == "baseline") {
    overrides.absent.push_back("VK_EXT_mesh_shader");
  } else if (profile == "titanxp") {
    for (const char* row :
         {"VK_EXT_mesh_shader", "VK_KHR_ray_query", "VK_NV_cluster_acceleration_structure",
          "VK_EXT_memory_decompression", "VK_KHR_fragment_shading_rate"}) {
      overrides.absent.push_back(row);
    }
  } else {
    usize start = 0;
    while (start < profile.size()) {
      usize end = profile.find(',', start);
      if (end == std::string_view::npos) end = profile.size();
      usize first = start;
      usize last = end;
      while (first < last && profile[first] == ' ')
        ++first;
      while (last > first && profile[last - 1] == ' ')
        --last;
      if (last > first)
        overrides.absent.push_back(std::string(profile.substr(first, last - first)));
      start = end + 1;
    }
  }
  return overrides;
}

const DeviceOverrides& test_device_overrides() {
  static const DeviceOverrides overrides = [] {
    const std::string profile = read_environment(k_test_device_env);
    if (profile.empty()) return DeviceOverrides{};
    const std::string run = read_environment(k_test_run_env);
    // The root the test main's scratch directories live under (tests/support/test_temp_dir.h).
    std::error_code temp_error;
    const std::filesystem::path temp = std::filesystem::temp_directory_path(temp_error);
    const std::string root = temp_error ? std::string() : (temp / "engine-tests").string();
    std::error_code run_error;
    const bool exists = !run.empty() && std::filesystem::is_directory(run, run_error);
    switch (judge_test_profile(profile, run, root, exists)) {
      case TestProfile::honoured:
        ENGINE_LOG_INFO(log_test_profile, "the device opens as a test's profile",
                        log::field("profile", profile));
        return test_profile_overrides(profile);
      case TestProfile::no_test:
        ENGINE_LOG_WARN(log_test_profile,
                        "a device profile is set and ignored: it is honoured only under a test's "
                        "main, and no test run is named",
                        log::field("profile", profile));
        break;
      case TestProfile::stale_run:
        ENGINE_LOG_WARN(log_test_profile,
                        "a device profile is set and ignored: the test run named is not a live one",
                        log::field("profile", profile));
        break;
      case TestProfile::none: break;
    }
    return DeviceOverrides{};
  }();
  return overrides;
}

// The lost-device seam (device_lost.h, T212) under the same rule as a profile: a test's variable
// does nothing outside a live test's main.
u64 test_lose_device_at() {
  static const u64 at = [] {
    const std::string text = read_environment(k_test_lose_device_env);
    if (text.empty()) return u64{0};
    const std::string run = read_environment(k_test_run_env);
    std::error_code temp_error;
    const std::filesystem::path temp = std::filesystem::temp_directory_path(temp_error);
    const std::string root = temp_error ? std::string() : (temp / "engine-tests").string();
    std::error_code run_error;
    const bool exists = !run.empty() && std::filesystem::is_directory(run, run_error);
    if (judge_test_profile(text, run, root, exists) != TestProfile::honoured) {
      ENGINE_LOG_WARN(log_test_profile,
                      "the lost-device seam is set and ignored: it is honoured only under a live "
                      "test's main",
                      log::field("value", text));
      return u64{0};
    }
    const u64 value = parse_lose_device_at(text);
    ENGINE_LOG_INFO(log_test_profile, "the device will answer lost at a frame wait (test seam)",
                    log::field("frame_value", value));
    return value;
  }();
  return at;
}

}  // namespace engine::gfx
