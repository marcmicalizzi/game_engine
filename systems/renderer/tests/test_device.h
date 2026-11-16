#pragma once

// `ENGINE_GFX_TEST_DEVICE` for the renderer's cases that honour it (roadmap T153): the device a
// case opens lacks what the profile removes, through `DeviceOptions::overrides`, so a card with
// mesh shaders runs the case as the TITAN Xp would. The profiles are `gfx`'s
// (`domain/gfx/tests/raster_path.h`, which a renderer test cannot include: it names the Vulkan
// backend): `baseline` removes `VK_EXT_mesh_shader`, `titanxp` the TITAN Xp's absences, anything
// else is a comma-separated list of requirement rows. A forced profile that yields no device is
// the caller's to fail, not to skip (docs/subsystems/renderer.md, "Running a case as a weaker
// card").

#include <domain/gfx/device.h>

#include <test_paths.h>

#include <string>

namespace engine::renderer_test {

// The value of ENGINE_GFX_TEST_DEVICE, read once; empty for the GPU as it is.
inline const std::string& device_profile() {
  static const std::string profile = test::detail::environment("ENGINE_GFX_TEST_DEVICE");
  return profile;
}

// The options a case's device is created with: the profile's absences, or none.
inline gfx::DeviceOptions device_options() {
  gfx::DeviceOptions options;
  const std::string& profile = device_profile();
  if (profile == "baseline") {
    options.overrides.absent.push_back("VK_EXT_mesh_shader");
  } else if (profile == "titanxp") {
    for (const char* row :
         {"VK_EXT_mesh_shader", "VK_KHR_ray_query", "VK_NV_cluster_acceleration_structure",
          "VK_EXT_memory_decompression", "VK_KHR_fragment_shading_rate"}) {
      options.overrides.absent.push_back(row);
    }
  } else {
    usize start = 0;
    while (start < profile.size()) {
      usize end = profile.find(',', start);
      if (end == std::string::npos) end = profile.size();
      usize first = start;
      usize last = end;
      while (first < last && profile[first] == ' ')
        ++first;
      while (last > first && profile[last - 1] == ' ')
        --last;
      if (last > first) options.overrides.absent.push_back(profile.substr(first, last - first));
      start = end + 1;
    }
  }
  return options;
}

}  // namespace engine::renderer_test
