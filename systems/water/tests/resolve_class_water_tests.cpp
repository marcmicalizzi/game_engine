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
  // The pipelines are made under it: `cycle` makes the one draw's, the split's and the plain
  // module's, and such a renderer follows the tunable frame by frame once it names a mode.
  ScopedTunable mode("renderer.resolve.classes", "cycle");
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
  // ground), and from the same place looking away over the plain (no water in view); then two
  // poses where the band is seen at a grazing angle, which is where the class's point (from the
  // pixel's depth) and the resolve's (from its triangle) are furthest apart along the ray: from
  // the rim at a walker's height, and from thirty metres back, a metre up, where the near rim
  // hides the water (the sight line over it reaches the level a metre short of the far bank's
  // waterline) and what shows is the far bank's band.
  struct Pose {
    const char* name;
    WorldPos position;
    WorldPos target;
    bool water;   // the band is in view
    u32 least;    // and at least this many pixels of it, and of the water where that is in view
    bool hidden;  // the water itself is not
  };
  const Pose poses[4] = {
      {"across the basin", WorldPos{-14.0, 12.0, -14.0}, WorldPos{2.0, -2.0, 2.0}, true, 1000,
       false},
      {"away from it", WorldPos{-14.0, 12.0, -14.0}, WorldPos{-60.0, 2.0, -60.0}, false, 0, true},
      {"from the rim", WorldPos{-17.0, 1.7, 0.0}, WorldPos{10.0, -1.5, 0.0}, true, 1, false},
      {"from thirty metres", WorldPos{-30.0, 1.0, -5.0}, WorldPos{0.0, -1.2, 0.0}, true, 1, true}};
  for (const Pose& pose : poses) {
    const std::string name = pose.name;
    CAPTURE(name);
    FrameDesc frame;
    frame.camera.position = pose.position;
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
      if (!pose.hidden) CHECK(census.pixels[water] >= pose.least);
      CHECK(census.pixels[wet] >= pose.least);
    } else {
      CHECK(census.pixels[water] == 0);
      // The class asks the record's level, not its square alone: the plain is 1.5 m above the
      // basin's level and its fringe a metre, and at this pose the class's slack in height is
      // under 0.4 m (resolve_class.slang, `rise`), so no pixel of the plain is wet-class. By the
      // square alone 21,102 of these pixels were.
      CHECK(census.pixels[wet] == 0);
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

    // The wet class is what makes that so: with no pixel classed wet the band's ground falls to
    // the plain module and is not darkened, and no more pixels move than the wet class held —
    // every pixel the water module wets was in it.
    {
      ScopedTunable wet_plain("renderer.resolve.class_wet_plain", "true");
      CapturedFrame dry;
      shoot(dry);
      CHECK(renderer.resolve_classes_plain() == k_base);
      SceneRenderer::ResolveClassCensus none;
      REQUIRE_MESSAGE(renderer.resolve_class_census(none, &error), error);
      CHECK(none.pixels[wet] == 0);
      CHECK(none.pixels[water] == census.pixels[water]);
      const Apart band = apart(dry, one);
      MESSAGE(name << ": with no wet class " << band.pixels << " pixels differ, worst "
                   << band.worst);
      CHECK(band.pixels <= census.pixels[wet] + by_module.pixels);
      if (pose.water) CHECK(band.pixels >= pose.least);
      if (pose.least >= 1000) {
        CHECK(band.pixels > 200);
        CHECK(band.worst > 4);
      }
    }
  }
}

#endif  // ENGINE_WATER_TESTS_DUNES
