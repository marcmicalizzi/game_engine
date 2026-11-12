// The resolve by pixel class on a scene with water (roadmap R151; docs/subsystems/renderer.md,
// "The resolve by pixel class"). Under `renderer.resolve.classes=modules` the pixels that cannot
// touch water are drawn by the resolve module that has none, and only the water class and the wet
// class by the scene's own. What is held here, on a basin with a wet band: `draws` is the one
// draw's bytes; `modules` is the one draw's picture to what two modules' arithmetic can differ by
// (a code on a handful of pixels, and none measured); the wet class holds every pixel the water
// module wets (drawn by the plain module it is not the picture, and only the wet class's pixels
// move); and every pixel has a class that is drawn.

#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/gfx/device.h>
#include <domain/gfx/resolve_classes.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstdlib>
#include <fstream>
#include <string>

using namespace engine;
using namespace engine::renderer;

#if ENGINE_WATER_TESTS_DUNES

namespace {

// The shore test's basin (shore_scene_tests.cpp): a smooth bowl 4 m deep in a plain, filled to
// 1.5 m below the plain, with a wet band a metre high round the waterline.
constexpr const char* k_basin = R"({
  "format": "engine.scene.v1",
  "name": "resolve-class-water-test",
  "terrain": {"size": 129, "extent": 64, "seed": 2, "dune_height": 0,
              "basins": [{"center": [0, 0], "radius": 14, "depth": 4}]},
  "water": [{"name": "basin", "level_m": -1.5,
             "shape": {"fill": {"seed": [0, 0, 0], "max_extent_m": 30}},
             "wind_follow": false, "wave_amplitude_m": 0.03, "wave_length_m": 1.5,
             "wet_m": 1.0, "foam_m": 0}]
})";

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

struct Apart {
  u32 pixels = 0;
  i32 worst = 0;
};

Apart apart(const CapturedFrame& a, const CapturedFrame& b) {
  Apart out;
  for (u32 p = 0; p * 4 + 3 < a.color.size(); ++p) {
    i32 worst = 0;
    for (u32 c = 0; c < 4; ++c) {
      const i32 d =
          std::abs(static_cast<i32>(a.color[p * 4 + c]) - static_cast<i32>(b.color[p * 4 + c]));
      worst = d > worst ? d : worst;
    }
    out.pixels += worst != 0 ? 1u : 0u;
    out.worst = worst > out.worst ? worst : out.worst;
  }
  return out;
}

}  // namespace

TEST_CASE("resolve classes: a basin's ground by the module without water is the one draw's") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("renderer unavailable here: " << error);
    return;
  }
  if (!device.features().buffer_int64_atomics) {
    MESSAGE("no 64-bit buffer atomics here");
    return;
  }
  const test::TempDir tmp("resolve_class_water");
  const std::string path = tmp.file("basin.json");
  {
    std::ofstream file(path, std::ios::binary);
    file << k_basin;
  }
  ScopedTunable mode("renderer.resolve.classes", "modules");  // the pipelines are made under it
  SceneDesc desc;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  desc.ddc = tmp.file("ddc");
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  RenderSettings settings;
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  REQUIRE(check_availability(resolved, device.features()) == RenderAvailability::Ok);
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  REQUIRE(scene.has_water());
  REQUIRE(scene.water_shore_count() > 0);
  constexpr u32 k_width = 480;
  constexpr u32 k_height = 320;
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = k_width;
  rd.height = k_height;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);

  constexpr u32 k_base = gfx::k_resolve_class_all & ~gfx::k_resolve_class_water_mask;
  const u32 water = static_cast<u32>(gfx::ResolveClass::Water);
  const u32 wet = static_cast<u32>(gfx::ResolveClass::Wet);
  // From 14 m up over the basin's rim looking down across it (the water, its wet band and dry
  // ground), and from the same place looking away over the plain (no water in view).
  struct Pose {
    const char* name;
    WorldPos target;
    bool water;
  };
  const Pose poses[2] = {{"across the basin", WorldPos{2.0, -2.0, 2.0}, true},
                         {"away from it", WorldPos{-60.0, 2.0, -60.0}, false}};
  for (const Pose& pose : poses) {
    const std::string name = pose.name;
    CAPTURE(name);
    FrameDesc frame;
    frame.camera.position = WorldPos{-14.0, 12.0, -14.0};
    frame.camera.target = pose.target;
    frame.sky_time_s = 0.0;
    frame.world_time_s = 3.0;
    const auto shoot = [&](CapturedFrame& out) {
      REQUIRE_MESSAGE(renderer.capture(frame, CaptureChannels{}, out, &error), error);
    };

    mode.set("off");
    CapturedFrame one;
    shoot(one);
    CHECK(renderer.resolve_classes_drawn() == 0);

    // Every class by the scene's own module: the one draw's bytes.
    mode.set("draws");
    CapturedFrame draws;
    shoot(draws);
    CHECK(renderer.resolve_classes_drawn() == gfx::k_resolve_class_all);
    CHECK(renderer.resolve_classes_plain() == 0);
    CHECK(draws.color == one.color);

    // The classes that cannot touch water by the module without it.
    mode.set("modules");
    CapturedFrame modules;
    shoot(modules);
    CHECK(renderer.resolve_classes_drawn() == gfx::k_resolve_class_all);
    CHECK(renderer.resolve_classes_plain() == k_base);
    SceneRenderer::ResolveClassCensus census;
    REQUIRE_MESSAGE(renderer.resolve_class_census(census, &error), error);
    u64 classed = 0;
    for (u32 c = 0; c < gfx::k_resolve_classes; ++c)
      classed += census.pixels[c];
    CHECK(census.unclassed == 0);
    CHECK(classed == u64{k_width} * k_height);
    if (pose.water) {
      CHECK(census.pixels[water] > 1000);
      CHECK(census.pixels[wet] > 1000);
    } else {
      CHECK(census.pixels[water] == 0);
    }
    // Two modules are the same source compiled twice, and the driver may arrange a float
    // expression differently in each: a code on a few pixels in a hundred thousand at most (2 of
    // 8.3 million at one pose of seven at 3840 x 2160, none at the others; none here).
    const Apart by_module = apart(modules, one);
    MESSAGE(name << ": modules against the one draw, " << by_module.pixels
                 << " pixels differ, worst " << by_module.worst << "; water "
                 << census.pixels[water] << ", wet " << census.pixels[wet]);
    CHECK(by_module.worst <= 1);
    CHECK(by_module.pixels * 20000 <= k_width * k_height);

    // The wet class is what makes that so: drawn by the plain module, the band is not darkened,
    // and nothing but the wet class's pixels moves.
    {
      ScopedTunable wet_plain("renderer.resolve.class_wet_plain", "true");
      CapturedFrame dry;
      shoot(dry);
      CHECK(renderer.resolve_classes_plain() == (k_base | (1u << wet)));
      const Apart band = apart(dry, one);
      CHECK(band.pixels <= census.pixels[wet] + by_module.pixels);
      if (pose.water) {
        CHECK(band.pixels > 200);
        CHECK(band.worst > 4);
      }
    }
  }
}

#endif  // ENGINE_WATER_TESTS_DUNES
