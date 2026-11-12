// The resolve by pixel class (roadmap R151; docs/subsystems/renderer.md, "The resolve by pixel
// class"). Under `renderer.resolve.classes` a classify pass writes each pixel's class as depth and
// the resolve is drawn once per class, depth-equal. What must hold, and is held here: the picture
// and every capture channel are the bytes the one draw draws; every pixel of the target is given
// exactly one class; every class a pixel holds is drawn (a class left out leaves its pixels the
// clear, and the picture says so); and a renderer made with the split off builds none of it.

#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/gfx/device.h>
#include <domain/gfx/resolve_classes.h>
#include <domain/scene_gen/scene_gen.h>
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

// A tunable set for the length of a scope: `renderer.resolve.classes` is read when a renderer's
// pipelines are made, and again every frame.
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
};

// Hills from the renderer's own heightfield, under the stand-in sky or a physical one.
SceneDesc hills(bool sky) {
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.heightfield_grid = 65;
  desc.cache = false;
  if (sky) {
    scene::Sky entry;
    entry.latitude_deg = 31.1f;
    entry.day_of_year = 100.0f;
    entry.turbidity = 1.6f;
    desc.sky = entry;
  }
  return desc;
}

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  std::string error;

  bool build(const gfx::Device& device, bool sky, ShadowMode shadows) {
    if (!load_scene(hills(sky), data, error)) return false;
    RenderSettings settings;
    settings.shadows = shadows;
    resolve_settings(settings, device.features(), &data, resolved);
    if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
      error = "unavailable";
      return false;
    }
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = k_width;
    rd.height = k_height;
    return renderer.create(device, scene, resolved, rd, &error);
  }
};

// Low over the hills towards the horizon, so the frame has ground and an uncovered sky.
FrameDesc looking() {
  FrameDesc frame;
  const Vec3 eye{0.0f, 3.0f, 0.0f};
  const Vec3 towards = normalize(Vec3{0.35f, -0.12f, -1.0f});
  frame.camera.position = absolute(WorldPos::origin(), eye);
  frame.camera.target = absolute(WorldPos::origin(), eye + towards * 100.0f);
  frame.camera.fov_y = 60.0f * 3.14159265f / 180.0f;
  frame.camera.znear = 0.05f;
  frame.dt_s = 1.0 / 60.0;
  return frame;
}

bool shoot(Rig& rig, CapturedFrame& out) {
  CaptureChannels channels;
  channels.color = true;
  channels.ids = true;
  channels.depth = true;
  return rig.renderer.capture(looking(), channels, out, &rig.error);
}

u32 pixels_apart(const CapturedFrame& a, const CapturedFrame& b) {
  u32 out = 0;
  for (u32 p = 0; p * 4 + 3 < a.color.size(); ++p) {
    bool differs = false;
    for (u32 c = 0; c < 4; ++c)
      differs = differs || a.color[p * 4 + c] != b.color[p * 4 + c];
    out += differs ? 1u : 0u;
  }
  return out;
}

bool null_pass(const FramePassContext&, void*, std::string*) { return true; }

constexpr u32 bit(gfx::ResolveClass c) { return 1u << static_cast<u32>(c); }

// One scene, the one draw and the split alternated on one renderer: the same bytes, a whole
// census, every class with a pixel drawn, and a class left out seen in the picture.
void split_is_the_one_draw(bool sky, ShadowMode shadows, bool frame_pass) {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  if (shadows == ShadowMode::RayTraced && !gpu.device.features().ray_query) {
    MESSAGE("skipped: no ray queries on " << gpu.device.adapter().name);
    return;
  }
  ScopedTunable mode("renderer.resolve.classes", "draws");  // the pipelines are made under it
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, sky, shadows), rig.error);
  if (frame_pass) {
    REQUIRE_MESSAGE(rig.renderer.add_frame_pass(FramePassStage::AfterResolve, null_pass, nullptr,
                                                "null pass", &rig.error),
                    rig.error);
  }

  mode.set("off");
  CapturedFrame one;
  REQUIRE_MESSAGE(shoot(rig, one), rig.error);
  CHECK(rig.renderer.resolve_classes_drawn() == 0);
  SceneRenderer::ResolveClassCensus none;
  CHECK_FALSE(rig.renderer.resolve_class_census(none));

  mode.set("draws");
  CapturedFrame split;
  REQUIRE_MESSAGE(shoot(rig, split), rig.error);
  const u32 drawn = rig.renderer.resolve_classes_drawn();
  // No water in the scene: the five classes a scene without it can hold, by its own module.
  CHECK(drawn == (gfx::k_resolve_class_all & ~gfx::k_resolve_class_water_mask));
  CHECK(rig.renderer.resolve_classes_plain() == 0);
  REQUIRE(split.color.size() == one.color.size());
  CHECK(pixels_apart(split, one) == 0);
  CHECK(split.color == one.color);
  CHECK(split.ids == one.ids);
  CHECK(split.depth == one.depth);

  // Every pixel of the target has exactly one class, and every class that has a pixel is drawn.
  SceneRenderer::ResolveClassCensus census;
  REQUIRE_MESSAGE(rig.renderer.resolve_class_census(census, &rig.error), rig.error);
  u64 classed = 0;
  for (u32 c = 0; c < gfx::k_resolve_classes; ++c) {
    classed += census.pixels[c];
    if (census.pixels[c] > 0) {
      CHECK_MESSAGE((drawn & (1u << c)) != 0, gfx::resolve_class_name(c) << " has pixels, undrawn");
    }
  }
  CHECK(census.unclassed == 0);
  CHECK(classed == u64{k_width} * k_height);
  const u64 sky_pixels = census.pixels[static_cast<u32>(gfx::ResolveClass::Sky)];
  const u64 rigid_pixels = census.pixels[static_cast<u32>(gfx::ResolveClass::Rigid)];
  CHECK(sky_pixels > 1000);
  CHECK(rigid_pixels > 1000);
  CHECK(sky_pixels + rigid_pixels == classed);  // hills and sky, nothing else
  CHECK(census.quads_total == u64{k_width / 2} * (k_height / 2));
  CHECK(census.quads_mixed > 0);  // the horizon
  CHECK(census.quads_mixed < census.quads_total / 8);
  // The covered pixels are the visibility buffer's: the classes agree with the id channel.
  u64 covered = 0;
  for (u32 p = 0; p < k_width * k_height; ++p)
    covered += split.depth[p] > 0.0f ? 1u : 0u;
  CHECK(covered == rigid_pixels);

  // A class left undrawn leaves its pixels the clear: the picture is not the one draw's, by that
  // class's pixels and no others. (Without frame passes and under the stand-in sky the clear is
  // the sky's own colour, so the sky's class is the one whose absence shows nothing.)
  {
    ScopedTunable mask("renderer.resolve.class_mask", "29");  // every class but the rigid one
    CapturedFrame holed;
    REQUIRE_MESSAGE(shoot(rig, holed), rig.error);
    CHECK((rig.renderer.resolve_classes_drawn() & bit(gfx::ResolveClass::Rigid)) == 0);
    const u32 apart = pixels_apart(holed, one);
    CHECK(apart > 0);
    CHECK(apart <= rigid_pixels);
    CHECK(apart * 10 >= rigid_pixels * 9);  // but for a hill pixel that is the clear's colour
  }

  // Back to the one draw: the bytes it drew before the split ran.
  mode.set("off");
  CapturedFrame again;
  REQUIRE_MESSAGE(shoot(rig, again), rig.error);
  CHECK(rig.renderer.resolve_classes_drawn() == 0);
  CHECK(again.color == one.color);
}

}  // namespace

TEST_CASE("resolve classes: the split draws the bytes the one draw draws, under the stand-in") {
  split_is_the_one_draw(false, ShadowMode::Off, false);
}

TEST_CASE("resolve classes: the split draws the bytes the one draw draws, under a sky") {
  split_is_the_one_draw(true, ShadowMode::Off, false);
}

TEST_CASE("resolve classes: the split draws the bytes the one draw draws, with traced shadows") {
  split_is_the_one_draw(true, ShadowMode::RayTraced, false);
}

TEST_CASE("resolve classes: the split draws the bytes the one draw draws, into a frame pass") {
  split_is_the_one_draw(true, ShadowMode::Off, true);
}

TEST_CASE("resolve classes: a renderer made with the split off builds none of it") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  ScopedTunable mode("renderer.resolve.classes", "off");
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, false, ShadowMode::Off), rig.error);
  CapturedFrame one;
  REQUIRE_MESSAGE(shoot(rig, one), rig.error);
  CHECK(rig.renderer.resolve_classes_drawn() == 0);
  // Asked for a frame later, there are no pipelines to draw it with: the one draw, as it was.
  mode.set("modules");
  CapturedFrame still;
  REQUIRE_MESSAGE(shoot(rig, still), rig.error);
  CHECK(rig.renderer.resolve_classes_drawn() == 0);
  CHECK(still.color == one.color);
}

TEST_CASE("resolve classes: every class has its own exact depth, and none is 0 or 1") {
  for (u32 c = 0; c < gfx::k_resolve_classes; ++c) {
    const f32 depth = gfx::resolve_class_depth(c);
    CHECK(depth > 0.0f);
    CHECK(depth < 1.0f);
    CHECK(depth * 16.0f == static_cast<f32>(c + 1));
    CHECK(std::string(gfx::resolve_class_name(c)) != "none");
  }
  CHECK(std::string(gfx::resolve_class_name(gfx::k_resolve_classes)) == "none");
  CHECK((gfx::k_resolve_class_water_mask & gfx::k_resolve_class_all) ==
        gfx::k_resolve_class_water_mask);
}
