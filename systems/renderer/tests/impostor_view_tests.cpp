// A tree's impostor chain drawn (docs/subsystems/flora.md, "The impostor chain"; renderer.md,
// "Impostors are masked clusters"; R89): from a plane's baked view, the whole-tree star drawn by
// the masked variants and lit through its baked normals matches the full geometry's picture within
// a stated error — the proof that the bake, the container, the texture step and the draw agree.

#include <domain/gfx/device.h>
#include <systems/renderer/camera_path.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
#include <fstream>
#include <string>

using namespace engine;
using namespace engine::renderer;

#if ENGINE_RENDERER_TESTS_FLORA

namespace {

struct Gpu {
  gfx::Device device;
  std::string why;
  bool ok = false;
  Gpu() {
    std::string error;
    if (!device.create(gfx::DeviceOptions{}, &error)) {
      why = "no Vulkan device: " + error;
      return;
    }
    if (!device.features().buffer_int64_atomics) {
      why = std::string(device.adapter().name) + " has no 64-bit buffer atomics";
      return;
    }
    ok = true;
  }
};

// The impostor tests' 8 m oak (domain/flora/tests/impostor_tests.cpp).
constexpr const char* k_small_oak =
    R"({"species":"broadleaf","broadleaf":{"height_m":8,"triangle_budget":30000}})";

}  // namespace

TEST_CASE("impostor view: the tree's star from its baked view matches the full tree") {
  const test::TempDir tmp("renderer_impostor_view");
  REQUIRE(tmp.ok());
  const std::string oak = tmp.file("oak.flora.json");
  {
    std::ofstream f(oak, std::ios::binary);
    f << k_small_oak;
  }
  SceneDesc desc;
  desc.meshes.push_back(oak);
  desc.ddc = tmp.file("ddc");
  desc.cache = true;
  SceneData data;
  std::string error;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  REQUIRE(data.sources.size() == 1);
  REQUIRE(data.sources[0].data.materials.size() >= 4);  // bark, leaf, a card level, the star

  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  RenderSettings settings;
  ResolvedSettings resolved;
  resolve_settings(settings, gpu.device.features(), &data, resolved);
  REQUIRE(check_availability(resolved, gpu.device.features()) == RenderAvailability::Ok);
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(gpu.device, data, resolved, &error), error);
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = 480;
  rd.height = 480;
  REQUIRE_MESSAGE(renderer.create(gpu.device, scene, resolved, rd, &error), error);

  // The star's third plane faces +z through the trunk: look at the tree from far along +z, where a
  // perspective view is nearly the bake's orthographic one, the tree about 200 pixels tall.
  CameraPath path;
  CameraPathKey key;
  key.position = WorldPos{0.0, 4.0, 120.0};
  key.target = WorldPos{0.0, 4.0, 0.0};
  key.fov_y = 0.08f;
  path.keys.push_back(key);
  key.time = 1.0;
  path.keys.push_back(key);
  FrameDesc frame;
  frame.camera = sample_camera_path(path, 0.0);
  CaptureChannels channels;
  channels.ids = true;
  CapturedFrame full;
  CapturedFrame star;
  frame.lod_px = 0.0f;  // the finest clusters
  REQUIRE_MESSAGE(renderer.render_offscreen(frame, &error), error);
  REQUIRE_MESSAGE(renderer.capture(frame, channels, full, &error), error);
  frame.lod_px = 1.0e6f;  // the chain's root
  REQUIRE_MESSAGE(renderer.render_offscreen(frame, &error), error);
  REQUIRE_MESSAGE(renderer.capture(frame, channels, star, &error), error);
  if (!test::detail::environment("ENGINE_FLORA_KEEP_CAPTURE").empty()) {
    CaptureFiles files;
    write_capture(tmp.path(), "full", full, files, error);
    write_capture(tmp.path(), "star", star, files, error);
    tmp.keep();
  }

  // Coverage: a pixel is the tree's when an instance drew it. Colour: over the pixels both drew.
  const u32 instances = data.instances.size();
  u32 both = 0, either = 0, full_only = 0, star_only = 0;
  f64 colour_error = 0.0;
  for (u32 p = 0; p < full.width * full.height; ++p) {
    const bool a = full.ids[p * k_id_words] < instances;
    const bool b = star.ids[p * k_id_words] < instances;
    either += (a || b) ? 1u : 0u;
    full_only += (a && !b) ? 1u : 0u;
    star_only += (b && !a) ? 1u : 0u;
    if (!(a && b)) continue;
    ++both;
    for (u32 k = 0; k < 3; ++k)
      colour_error += std::abs(static_cast<f64>(full.color[p * 4 + k]) -
                               static_cast<f64>(star.color[p * 4 + k]));
  }
  REQUIRE(either > 2000u);
  const f64 overlap = static_cast<f64>(both) / static_cast<f64>(either);
  const f64 mean = colour_error / (3.0 * static_cast<f64>(both > 0 ? both : 1u));
  MESSAGE("star against full tree from the baked view: "
          << either << " tree pixels, overlap " << overlap << " (" << full_only
          << " only the tree's, " << star_only << " only the star's), mean colour error " << mean
          << " of 255");
  // The stated bounds (flora.md, "The impostor chain"): the silhouettes share most of their
  // pixels — the oblique planes and the level one add a fringe the tree does not have — and where
  // both draw, the colour is the tree's to within a fifth of the range.
  CHECK(overlap > 0.5);
  CHECK(mean < 50.0);
}

#endif  // ENGINE_RENDERER_TESTS_FLORA
