// The targets a frame reads (roadmap R4 stage 1; docs/experiments/render-graph-aliasing.md, "7.
// Stages, in build order"; docs/subsystems/renderer.md, "The targets a frame reads"). The Hi-Z
// pyramid is made only under two-pass occlusion or the occlusion term of the LOD threshold, and the
// depth image only for the direct path or the resolve by pixel class: under traced shadows neither
// is read on the defaults, and a renderer holds neither. What must hold, and is held here:
//
// - a renderer holds each only where its settings read it, in every raster and shadow mode the
//   device has;
// - holding them changes no byte: a renderer made to hold both (the occlusion term's histogram,
//   which builds the Hi-Z and changes nothing, and the split resolve) draws the colour and ids of
//   one that holds neither;
// - a setting turned on mid-session makes its target at that frame, and the frames after it are
//   the bytes of a session started with it on (the occlusion term at k = 4, whose first frame
//   reads an empty pyramid either way; the split resolve, named and then made by a frame pass's
//   registration, against a session started with both).
//
// A small scene (the renderer's own heightfield, 320 x 180) and a few frames each, so the cases
// hold the GPU for seconds. On the Titan Xp profile (no ray queries, no mesh shaders) the modes
// that trace are skipped by name and the rest run on the vertex path.

#include "test_device.h"

#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/gfx/device.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/frame_pass.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr u32 k_width = 320;
constexpr u32 k_height = 180;

struct Gpu {
  gfx::Device device;
  std::string why;
  bool ok = false;
  Gpu() {
    std::string error;
    if (!device.create(renderer_test::device_options(), &error)) {
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

// A tunable set for the length of a scope, and set again inside it.
struct ScopedTunable {
  tunables::Tunable* tunable = nullptr;
  std::string previous;
  ScopedTunable(const char* name, const char* value) {
    tunable = tunables::find(name);
    REQUIRE_MESSAGE(tunable != nullptr, name);
    tunable->append_value(previous);
    REQUIRE(tunable->set_from_text(value));
  }
  void set(const char* value) { REQUIRE(tunable->set_from_text(value)); }
  ~ScopedTunable() {
    if (tunable != nullptr) (void)tunable->set_from_text(previous);
  }
  ScopedTunable(const ScopedTunable&) = delete;
  ScopedTunable& operator=(const ScopedTunable&) = delete;
};

SceneDesc hills() {
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.heightfield_grid = 65;
  desc.cache = false;
  return desc;
}

struct Mode {
  const char* name;
  RasterMode raster;
  ShadowMode shadows;
  bool occlusion;
};

// The settings matrix: the traced frame, the hardware rasterizer with the cascades and with traced
// shadows, occlusion on and off, and the direct path.
constexpr Mode k_modes[] = {
    {"rt/rt", RasterMode::RayTrace, ShadowMode::RayTraced, true},
    {"hw with the cascades, occlusion on", RasterMode::Hardware, ShadowMode::Cascaded, true},
    {"hw with the cascades, occlusion off", RasterMode::Hardware, ShadowMode::Cascaded, false},
    {"hw with traced shadows", RasterMode::Hardware, ShadowMode::RayTraced, true},
    {"direct", RasterMode::Direct, ShadowMode::Off, false},
};

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  std::string error;
  std::string skip;

  bool build(const gfx::Device& device, const Mode& mode) {
    if (!load_scene(hills(), data, error)) return false;
    RenderSettings settings;
    settings.raster = mode.raster;
    settings.shadows = mode.shadows;
    settings.occlusion = mode.occlusion;
    resolve_settings(settings, device.features(), &data, resolved);
    const RenderAvailability availability = check_availability(resolved, device.features());
    if (availability != RenderAvailability::Ok) {
      skip = unavailable_reason(availability, device);
      return false;
    }
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = k_width;
    rd.height = k_height;
    return renderer.create(device, scene, resolved, rd, &error);
  }
};

FrameDesc looking(u64 index) {
  FrameDesc frame;
  const Vec3 eye{0.0f, 3.0f, 0.0f};
  const Vec3 towards = normalize(Vec3{0.35f, -0.12f, -1.0f});
  frame.camera.position = absolute(WorldPos::origin(), eye);
  frame.camera.target = absolute(WorldPos::origin(), eye + towards * 100.0f);
  frame.camera.fov_y = 60.0f * 3.14159265f / 180.0f;
  frame.camera.znear = 0.05f;
  frame.dt_s = 1.0 / 60.0;
  frame.frame_index = index;
  return frame;
}

// `frames` frames from `first`, the last of them captured.
bool shoot(Rig& rig, u64 first, u32 frames, CapturedFrame& out) {
  for (u32 f = 0; f + 1 < frames; ++f) {
    rig.renderer.begin_frame();
    if (rig.renderer.submit_frame(looking(first + f), &rig.error) == 0) return false;
  }
  CaptureChannels channels;
  channels.color = true;
  channels.ids = !rig.resolved.direct;  // the direct path writes no visibility buffer
  return rig.renderer.capture(looking(first + frames - 1), channels, out, &rig.error);
}

bool null_pass(const FramePassContext&, void*, std::string*) { return true; }

void check_same(const CapturedFrame& a, const CapturedFrame& b) {
  REQUIRE(a.color.size() == b.color.size());
  REQUIRE(a.ids.size() == b.ids.size());
  CHECK(a.color == b.color);
  CHECK(a.ids == b.ids);
}

}  // namespace

TEST_CASE("frame targets: a renderer holds the Hi-Z and the depth image only where it reads them") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  for (const Mode& mode : k_modes) {
    const std::string name = mode.name;
    CAPTURE(name);
    Rig rig;
    if (!rig.build(gpu.device, mode)) {
      REQUIRE_MESSAGE(!rig.skip.empty(), rig.error);
      MESSAGE("skipped " << name << ": " << rig.skip);
      continue;
    }
    CapturedFrame shot;
    REQUIRE_MESSAGE(shoot(rig, 0, 2, shot), rig.error);
    MESSAGE(name << ": occlusion " << rig.resolved.occlusion << ", direct "
                      << rig.resolved.direct << "; Hi-Z " << rig.renderer.hiz_bytes()
                      << " bytes, depth " << rig.renderer.depth_bytes() << " bytes");
    CHECK((rig.renderer.hiz_bytes() > 0) == rig.resolved.occlusion);
    CHECK((rig.renderer.depth_bytes() > 0) == rig.resolved.direct);
    if (mode.raster == RasterMode::RayTrace) {  // the traced defaults: neither, by name
      CHECK(rig.renderer.hiz_bytes() == 0);
      CHECK(rig.renderer.depth_bytes() == 0);
    }
  }
}

TEST_CASE("frame targets: holding the Hi-Z and the depth image changes no byte") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  for (const Mode& mode : k_modes) {
    if (mode.raster == RasterMode::Direct) continue;  // no ids, and its depth is its own
    const std::string name = mode.name;
    CAPTURE(name);
    CapturedFrame lean;
    {
      Rig rig;
      if (!rig.build(gpu.device, mode)) {
        REQUIRE_MESSAGE(!rig.skip.empty(), rig.error);
        MESSAGE("skipped " << name << ": " << rig.skip);
        continue;
      }
      REQUIRE_MESSAGE(shoot(rig, 0, 3, lean), rig.error);
    }
    // The occlusion term's histogram builds the Hi-Z and counts, and at k = 0 moves no threshold;
    // the split resolve draws the one draw's bytes into the depth image's classes (R151).
    ScopedTunable histogram("renderer.lod.occluded_histogram", "true");
    ScopedTunable classes("renderer.resolve.classes", "draws");
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, mode), rig.error);
    CapturedFrame held;
    REQUIRE_MESSAGE(shoot(rig, 0, 3, held), rig.error);
    CHECK(rig.renderer.hiz_bytes() > 0);
    CHECK(rig.renderer.depth_bytes() > 0);
    check_same(lean, held);
  }
}

TEST_CASE("frame targets: a setting turned on mid-session draws the session started with it on") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  for (const Mode& mode : k_modes) {
    if (mode.raster == RasterMode::Direct) continue;  // it holds its depth from the start
    const std::string name = mode.name;
    CAPTURE(name);
    // The occlusion term (R118) at k = 4: three frames with it on, its first reading an empty
    // pyramid in both sessions (made and cleared at the switch, or cleared at the create). Under
    // two-pass occlusion the pyramid is held from the create and is last frame's, so a session that
    // switched has a history the other has not: there only the holding is checked.
    CapturedFrame switched;
    bool term_made_at_switch = false;
    {
      ScopedTunable k("renderer.lod.occluded_scale", "0");
      Rig rig;
      if (!rig.build(gpu.device, mode)) {
        REQUIRE_MESSAGE(!rig.skip.empty(), rig.error);
        MESSAGE("skipped " << name << ": " << rig.skip);
        continue;
      }
      CapturedFrame before;
      REQUIRE_MESSAGE(shoot(rig, 0, 1, before), rig.error);
      const u64 hiz_before = rig.renderer.hiz_bytes();
      CHECK((hiz_before > 0) == rig.resolved.occlusion);
      term_made_at_switch = hiz_before == 0;
      k.set("4");
      REQUIRE_MESSAGE(shoot(rig, 1, 3, switched), rig.error);
      CHECK(rig.renderer.hiz_bytes() > 0);  // made at the switch where nothing held it
    }
    if (term_made_at_switch) {
      CapturedFrame started;
      ScopedTunable k("renderer.lod.occluded_scale", "4");
      Rig rig;
      REQUIRE_MESSAGE(rig.build(gpu.device, mode), rig.error);
      CHECK(rig.renderer.hiz_bytes() > 0);  // made at the create: the setting is on
      REQUIRE_MESSAGE(shoot(rig, 1, 3, started), rig.error);
      check_same(switched, started);
    }

    // The split resolve, turned on mid-session the way a session can: the tunable names a mode and
    // then a frame pass is registered, which makes the split's pipelines (resolve_classes.cpp); the
    // next frame plans the classes and makes the depth image.
    CapturedFrame split_switched;
    {
      ScopedTunable classes("renderer.resolve.classes", "off");
      Rig rig;
      REQUIRE_MESSAGE(rig.build(gpu.device, mode), rig.error);
      CapturedFrame one;
      REQUIRE_MESSAGE(shoot(rig, 0, 1, one), rig.error);
      CHECK(rig.renderer.depth_bytes() == 0);
      classes.set("draws");
      REQUIRE_MESSAGE(rig.renderer.add_frame_pass(FramePassStage::AfterResolve, null_pass, nullptr,
                                                  "null pass", &rig.error),
                      rig.error);
      REQUIRE_MESSAGE(shoot(rig, 1, 2, split_switched), rig.error);
      CHECK(rig.renderer.resolve_classes_drawn() != 0);
      CHECK(rig.renderer.depth_bytes() > 0);
    }
    CapturedFrame split_started;
    {
      ScopedTunable classes("renderer.resolve.classes", "draws");
      Rig rig;
      REQUIRE_MESSAGE(rig.build(gpu.device, mode), rig.error);
      CHECK(rig.renderer.depth_bytes() > 0);
      REQUIRE_MESSAGE(rig.renderer.add_frame_pass(FramePassStage::AfterResolve, null_pass, nullptr,
                                                  "null pass", &rig.error),
                      rig.error);
      REQUIRE_MESSAGE(shoot(rig, 1, 2, split_started), rig.error);
      CHECK(rig.renderer.resolve_classes_drawn() != 0);
    }
    check_same(split_switched, split_started);
  }
}
