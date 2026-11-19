// The serialized frame: the pass audit's guard check 3
// (docs/experiments/pass-declarations-audit.md, "The guard"; roadmap R189). Under
// `renderer.graph.serialize` the render graph records a full barrier before every pass, so no
// command runs beside another and every read waits for every write before it. A frame drawn that
// way and drawn as the graph's declarations order it must be the same bytes: a difference is a
// dependency no pass declared, which a driver may or may not happen to order. It sees what no
// declaration can name — bindless images, addresses inside GPU tables, a block a list got wrong —
// and the frame before, since the first pass's barrier orders it behind every command submitted
// earlier; it sees nothing inside one pass body.
//
// Two engine scenes (the grove and the oasis's palms), each drawn both ways in two configurations:
// the traced frame with the bounce
// (`--raster rt --shadows rt`, `renderer.gi.mode=bounce`), and the hardware rasterizer with the
// cascades (`--shadows csm`, the baseline tier's shadows). Each draw is a renderer of its own,
// three frames and a capture, so the two sides start from the same state.

#include "test_device.h"

#include <domain/gfx/device.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_paths.h>

#include <cstring>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr u32 k_width = 320;
constexpr u32 k_height = 180;
constexpr u32 k_frames = 3;

struct TunableScope {
  explicit TunableScope(const char* set, const char* restore) : restore_(restore) {
    REQUIRE(tunables::apply_overrides(set));
  }
  ~TunableScope() { (void)tunables::apply_overrides(restore_); }
  TunableScope(const TunableScope&) = delete;
  TunableScope& operator=(const TunableScope&) = delete;
  const char* restore_;
};

struct Config {
  const char* name;
  RasterMode raster;
  ShadowMode shadows;
  const char* gi;  // renderer.gi.mode
};

struct Shot {
  CapturedFrame frame;
  std::string skip;
};

Shot draw(const SceneData& data, const Config& config, bool serialize) {
  Shot out;
  const TunableScope gi((std::string("renderer.gi.mode=") + config.gi).c_str(),
                        "renderer.gi.mode=off");
  const TunableScope graph(
      serialize ? "renderer.graph.serialize=true" : "renderer.graph.serialize=false",
      "renderer.graph.serialize=false");
  gfx::Device device;
  std::string error;
  if (!device.create(renderer_test::device_options(), &error)) {
    out.skip = "device unavailable: " + error;
    return out;
  }
  RenderSettings settings;
  settings.raster = config.raster;
  settings.shadows = config.shadows;
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  const RenderAvailability availability = check_availability(resolved, device.features());
  if (availability != RenderAvailability::Ok) {
    out.skip = unavailable_reason(availability, device);
    device.destroy();
    return out;
  }
  GpuScene scene;
  SceneRenderer renderer;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), "gpu scene: " << error);
  SceneRenderer::Desc rd;
  rd.width = k_width;
  rd.height = k_height;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), "renderer: " << error);
  FrameDesc frame;
  const Vec3 back{0.6f * data.radius, 0.35f * data.radius, 0.6f * data.radius};
  frame.camera.position = absolute(data.center, back);
  frame.camera.target = data.center;
  frame.view_mode = settings.view_mode;
  for (u32 f = 0; f + 1 < k_frames; ++f) {
    frame.frame_index = f;
    renderer.begin_frame();
    REQUIRE_MESSAGE(renderer.submit_frame(frame, &error) != 0, error);
  }
  frame.frame_index = k_frames - 1;
  CaptureChannels channels;
  channels.ids = true;
  REQUIRE_MESSAGE(renderer.capture(frame, channels, out.frame, &error), error);
  renderer.destroy();
  scene.destroy();
  device.destroy();
  return out;
}

void check_same(const char* scene_dir) {
  // A device first: the scene's load is the case's longest part in a debug build, and a run with
  // no driver (`tools/dev.ps1 test -NoDriver`) has nothing to draw it with.
  {
    gfx::Device probe;
    std::string why;
    if (!probe.create(renderer_test::device_options(), &why)) {
      MESSAGE("skipped: device unavailable: " << why);
      return;
    }
    probe.destroy();
  }
  const std::string file = test::data_path(
      std::string(ENGINE_SOURCE_DIR "/content/test-scenes/") + scene_dir + "/scene.json",
      std::string("content/test-scenes/") + scene_dir + "/scene.json");
  const Config configs[] = {
      {"traced with the bounce", RasterMode::RayTrace, ShadowMode::RayTraced, "bounce"},
      {"hardware with the cascades", RasterMode::Hardware, ShadowMode::Cascaded, "off"},
  };
  // Loaded once: the four draws share the scene's data and differ in their GPU objects alone.
  SceneDesc desc;
  desc.cache = false;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(file, desc, error), error);
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), "scene: " << error);
  for (const Config& config : configs) {
    const std::string scene_name = scene_dir;
    const std::string config_name = config.name;
    CAPTURE(scene_name);
    CAPTURE(config_name);
    const Shot ordered = draw(data, config, false);
    if (!ordered.skip.empty()) {
      MESSAGE("skipped " << config.name << ": " << ordered.skip);
      continue;
    }
    const Shot serial = draw(data, config, true);
    REQUIRE(serial.skip.empty());
    REQUIRE(ordered.frame.color.size() == serial.frame.color.size());
    REQUIRE(ordered.frame.ids.size() == serial.frame.ids.size());
    u32 colour = 0;
    const u32 pixels = ordered.frame.width * ordered.frame.height;
    for (u32 p = 0; p < pixels; ++p) {
      if (std::memcmp(&ordered.frame.color[p * 4], &serial.frame.color[p * 4], 4) != 0) ++colour;
    }
    CHECK_MESSAGE(colour == 0, colour << " pixels differ in colour");
    CHECK(std::memcmp(ordered.frame.ids.data(), serial.frame.ids.data(),
                      ordered.frame.ids.size() * sizeof(ordered.frame.ids[0])) == 0);
  }
}

}  // namespace

TEST_CASE("serialized frame: the grove draws the same bytes with every pass serialized") {
  check_same("grove");
}

// The oasis's palms over their ground: a second generator mix, small enough for a debug build (the
// desert erg, its first second scene, regenerated its whole terrain in debug for over an hour, and
// a run with no driver timed out loading it before it found no device).
TEST_CASE("serialized frame: the oasis draws the same bytes with every pass serialized") {
  check_same("oasis-palms");
}
