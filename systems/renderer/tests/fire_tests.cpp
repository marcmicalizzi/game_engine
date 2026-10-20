// Fires (fire.h; docs/subsystems/renderer.md, "Fires"; roadmap R85): the scene file's block, the
// flicker as a function of the clock, the light array's nearest fires, a prop's fire carried by
// its placed instances — and on a GPU, the fire's light on the ground under traced shadows and
// under the cascaded maps.

#include <core/math/world.h>
#include <domain/gfx/device.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/fire.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/lighting.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

bool same_bytes(const FrameFire& a, const FrameFire& b) {
  // Field by field: FrameFire has no padding to compare, but its WorldPos and vectors are floats.
  return a.base == b.base && a.radius_m == b.radius_m && a.height_m == b.height_m &&
         a.intensity == b.intensity && a.color == b.color && a.reach_m == b.reach_m &&
         a.flame_color == b.flame_color && a.base_intensity == b.base_intensity;
}

}  // namespace

TEST_CASE("fire: the flicker is a function of the clock and the seed, and nothing else") {
  FireDesc desc;
  desc.flicker = 0.8f;
  desc.seed = 9;
  const WorldPos at{12.0, 1.0, -4.0};
  // The same clock twice, years in: the same light, to the bit.
  const f64 t = 94687200.0 + 0.25;
  const FrameFire a = burn_fire(desc, at, 1.0f, t);
  const FrameFire b = burn_fire(desc, at, 1.0f, t);
  CHECK(same_bytes(a, b));
  // Two clocks a fraction of a second apart: another intensity and another colour.
  const FrameFire c = burn_fire(desc, at, 1.0f, t + 0.13);
  CHECK(c.intensity != a.intensity);
  CHECK(!(c.color == a.color));
  // Within its swing: the block's intensity times 1 +- 0.35 flicker, whatever the clock.
  f32 lo = 1.0e30f, hi = 0.0f;
  for (u32 i = 0; i < 2000; ++i) {
    const FrameFire f = burn_fire(desc, at, 1.0f, t + 0.01 * i);
    lo = std::min(lo, f.intensity);
    hi = std::max(hi, f.intensity);
    CHECK(std::isfinite(f.intensity));
  }
  MESSAGE("over 20 s the light swung between " << lo << " and " << hi << " cd");
  CHECK(lo >= desc.intensity * (1.0f - k_fire_flicker_intensity * desc.flicker) - 1e-3f);
  CHECK(hi <= desc.intensity * (1.0f + k_fire_flicker_intensity * desc.flicker) + 1e-3f);
  CHECK(hi - lo > 0.3f * desc.intensity * k_fire_flicker_intensity * desc.flicker);
  // Another seed, another flicker; no flicker, the block's light.
  FireDesc other = desc;
  other.seed = 10;
  CHECK(burn_fire(other, at, 1.0f, t).intensity != a.intensity);
  FireDesc steady = desc;
  steady.flicker = 0.0f;
  CHECK(burn_fire(steady, at, 1.0f, t).intensity == steady.intensity);
  CHECK(burn_fire(steady, at, 1.0f, t + 3.0).intensity == steady.intensity);
  // With a sky the light is in sun units; the reach is the candela's either way.
  const FrameFire sky = burn_fire(desc, at, 1.0f, t, 1.0f / 128000.0f);
  CHECK(sky.reach_m == a.reach_m);
  CHECK(std::fabs(sky.intensity * 128000.0f - a.intensity) < 1e-2f);
}

TEST_CASE("fire: a blackbody's colour is orange at a campfire's temperature and white at noon's") {
  const Vec3 fire = blackbody_rgb(1800.0f);
  const Vec3 noon = blackbody_rgb(6500.0f);
  const auto luminance = [](Vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; };
  CHECK(std::fabs(luminance(fire) - 1.0f) < 1e-4f);
  CHECK(std::fabs(luminance(noon) - 1.0f) < 1e-4f);
  CHECK(fire.x > 2.0f * fire.y);
  CHECK(fire.y > fire.z);
  CHECK(std::fabs(noon.x - noon.z) < 0.1f);
}

TEST_CASE("fire: the light array takes the nearest fires after the lights it has") {
  FrameFire fires[6];
  for (u32 i = 0; i < 6; ++i) {
    fires[i].base = WorldPos{100.0 + 10.0 * (5 - i), 2.0, 0.0};  // the last is the nearest
    fires[i].height_m = 1.0f;
    fires[i].intensity = 100.0f + static_cast<f32>(i);
    fires[i].reach_m = 30.0f;
  }
  FrameLighting lighting;
  lighting.light_count = 1;  // a moon
  const WorldPos eye{100.0, 2.0, 5.0};
  const u32 added = add_fire_lights(std::span<const FrameFire>(fires, 6), eye, lighting);
  CHECK(added == k_max_fire_lights);
  CHECK(lighting.light_count == 1 + k_max_fire_lights);
  // Nearest first: fire 5 (at x 100), then 4, 3, 2; in the eye's frame, at the light's height.
  CHECK(lighting.lights[1].color_intensity.w == 105.0f);
  CHECK(lighting.lights[2].color_intensity.w == 104.0f);
  CHECK(lighting.lights[4].color_intensity.w == 102.0f);
  CHECK(lighting.lights[1].position_radius.x == 0.0f);
  CHECK(lighting.lights[1].position_radius.y ==
        doctest::Approx(static_cast<f64>(k_fire_light_height)));
  CHECK(lighting.lights[1].position_radius.z == -5.0f);
  CHECK(lighting.lights[1].position_radius.w == 30.0f);
  CHECK(lighting.lights[1].direction_type.w == gfx::k_light_point);
  // A full array takes none.
  FrameLighting full;
  full.light_count = k_frame_lights;
  CHECK(add_fire_lights(std::span<const FrameFire>(fires, 6), eye, full) == 0);
}

TEST_CASE("fire: the scene file's fires and a prop's fire are read, and a bad one is refused") {
  const test::TempDir tmp("renderer_fire_scene");
  const std::string path = tmp.file("fire.json");
  REQUIRE(
      io::write_file(
          path,
          R"({"format":"engine.scene.v1","name":"fires","terrain":{"size":17,"extent":16,"seed":1},
      "fires":[{"name":"a","position":[1,2,3],"intensity":120,"seed":4,
                "smoke":{"density":0.2}}],
      "props":[{"name":"camp","mesh":"camp.glb",
                "fire":{"name":"camp","position":[0,0.1,0],"radius_m":0.2}}]})") == io::Status::Ok);
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  REQUIRE(desc.fires.size() == 1);
  CHECK(desc.fires[0].name == "a");
  CHECK(desc.fires[0].position == WorldPos{1.0, 2.0, 3.0});
  CHECK(desc.fires[0].intensity == 120.0f);
  CHECK(desc.fires[0].smoke_density == 0.2f);
  CHECK(desc.fires[0].height_m == 0.9f);  // the schema's default
  CHECK(desc.fires[0].seed == 4u);
  REQUIRE(desc.props.size() == 1);
  REQUIRE(desc.props[0].fire.has_value());
  CHECK(desc.props[0].fire->radius_m == 0.2f);

  REQUIRE(
      io::write_file(
          path,
          R"({"format":"engine.scene.v1","name":"bad","terrain":{"size":17,"extent":16,"seed":1},
      "fires":[{"name":"cold","position":[0,0,0],"color_k":200}]})") == io::Status::Ok);
  SceneDesc bad;
  CHECK_FALSE(read_scene_file(path, bad, error));
  MESSAGE(error);
  CHECK(error.find("fires 0") != std::string::npos);
  CHECK(error.find("color_k") != std::string::npos);
}

TEST_CASE("fire: a prop's fire stands where each placed instance of the prop stands") {
  SceneData scene;
  scene.parts.resize(3);
  FireDesc fire;
  fire.position = WorldPos{0.5, 0.25, 0.0};  // from the prop's origin
  SceneProp prop{"camp", 2u};
  prop.fire = fire;
  scene.props.push_back(prop);
  FireDesc fixed;
  fixed.position = WorldPos{-30.0, 0.0, 0.0};
  scene.fires.push_back(fixed);
  FireSet set;
  set.reset(scene);
  CHECK(set.count() == 1);
  SceneInstance tail[3];
  tail[0].mesh = 1;  // not a fire's
  tail[1].mesh = 2;
  tail[1].origin = WorldPos{1000.0, 5.0, 2000.0};
  tail[1].transform.rotation = quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, 0.5f * k_pi);
  tail[1].transform.scale = Vec3{2.0f, 2.0f, 2.0f};
  tail[2].mesh = 2;
  set.place(std::span<const SceneInstance>(tail, 3));
  CHECK(set.count() == 3);
  Vector<FrameFire> out;
  set.burn(0.0, 1.0f, out);
  REQUIRE(out.size() == 3);
  CHECK(out[0].base == fixed.position);
  // The offset turned a quarter about +y (x to -z) and doubled, from the instance's origin; the
  // fire doubled with it and its intensity four times.
  CHECK(out[1].base.x == doctest::Approx(1000.0));
  CHECK(out[1].base.y == doctest::Approx(5.5));
  CHECK(out[1].base.z == doctest::Approx(2000.0 - 1.0));
  CHECK(out[1].radius_m == doctest::Approx(static_cast<f64>(2.0f * fire.radius_m)));
  CHECK(out[1].base_intensity == doctest::Approx(static_cast<f64>(4.0f * fire.intensity)));
  CHECK(out[2].base == WorldPos{0.5, 0.25, 0.0});
  // The tail emptied, the props' fires go with it.
  set.place({});
  CHECK(set.count() == 1);
}

// ---- on a GPU
// ------------------------------------------------------------------------------------

namespace {

constexpr u32 k_width = 320;
constexpr u32 k_height = 180;

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  std::string error;

  // The heightfield, lit by the stand-in, with `fire` on it or not.
  bool build(const gfx::Device& device, ShadowMode shadows, const FireDesc* fire) {
    SceneDesc desc;
    desc.meshes.push_back("");
    desc.heightfield_grid = 65;
    desc.cache = false;
    if (!load_scene(desc, data, error)) return false;
    if (fire != nullptr) data.fires.push_back(*fire);
    RenderSettings settings;
    settings.shadows = shadows;
    settings.lights = false;  // the stand-in's two lights out: the fire is the only point light
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

void light_on_the_ground(ShadowMode shadows, const char* name) {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error) || !device.features().buffer_int64_atomics) {
    MESSAGE("skipped: no device with 64-bit buffer atomics: " << error);
    return;
  }
  // Looking down at the heightfield's middle; the fire stands a little over it.
  SceneData probe;
  {
    SceneDesc desc;
    desc.meshes.push_back("");
    desc.heightfield_grid = 65;
    desc.cache = false;
    REQUIRE_MESSAGE(load_scene(desc, probe, error), error);
  }
  const WorldPos centre = probe.center;
  const f64 r = static_cast<f64>(probe.radius);
  FireDesc fire;
  fire.position = centre + DVec3{0.0, 0.15 * r, 0.0};
  fire.intensity = 0.5f * probe.radius * probe.radius;  // the stand-in's units: its sun is 1
  fire.flicker = 0.0f;
  FrameDesc frame;
  frame.camera.position = centre + DVec3{0.0, 0.9 * r, 0.9 * r};
  frame.camera.target = centre;
  frame.camera.fov_y = 60.0f * k_pi / 180.0f;
  frame.camera.znear = 0.05f;

  Rig lit;
  if (!lit.build(device, shadows, &fire)) {
    REQUIRE_MESSAGE(lit.error == "unavailable", lit.error);
    MESSAGE("skipped: " << name << " is not available here");
    return;
  }
  Rig dark;
  REQUIRE_MESSAGE(dark.build(device, shadows, nullptr), dark.error);
  CaptureChannels channels;
  channels.color = true;
  CapturedFrame a, b;
  REQUIRE_MESSAGE(lit.renderer.capture(frame, channels, a, &lit.error), lit.error);
  REQUIRE_MESSAGE(dark.renderer.capture(frame, channels, b, &dark.error), dark.error);
  CHECK(lit.renderer.stats().fires == 1);
  CHECK(lit.renderer.stats().fire_lights == 1);
  CHECK(dark.renderer.stats().fires == 0);
  u32 brighter = 0, darker = 0;
  for (u32 i = 0; i + 3 < a.color.size(); i += 4) {
    const i32 da = (i32{a.color[i]} + a.color[i + 1] + a.color[i + 2]) -
                   (i32{b.color[i]} + b.color[i + 1] + b.color[i + 2]);
    brighter += da > 0 ? 1u : 0u;
    darker += da < 0 ? 1u : 0u;
  }
  const u32 pixels = static_cast<u32>(a.color.size() / 4);
  MESSAGE(std::string(name) << ": the fire brightened " << brighter << " of " << pixels
                            << " pixels, darkened " << darker);
  CHECK(brighter * 10 > pixels);
  CHECK(darker == 0);
  // The same frame again is the same bytes: the light is the clock's.
  CapturedFrame again;
  REQUIRE_MESSAGE(lit.renderer.capture(frame, channels, again, &lit.error), lit.error);
  CHECK(again.color == a.color);
}

}  // namespace

TEST_CASE("fire: the light lights the ground under traced shadows") {
  light_on_the_ground(ShadowMode::RayTraced, "--shadows rt");
}

TEST_CASE("fire: the light lights the ground under the cascaded maps, unshadowed") {
  light_on_the_ground(ShadowMode::Cascaded, "--shadows csm");
}
