// The shore on a GPU (docs/subsystems/water.md, "The shore"; roadmap S97): a body that fills a
// basin of the renderer's ground, drawn through the renderer's reader and the resolve's water
// modules. The wet band darkens terrain pixels and nothing else, more of them the wider it is and
// none with it off; with its film off (`renderer.water.wet_film`, S99) no pixel it changes is
// lighter, and with the film on every lighter pixel is one the film changed, by no more than the
// film's own reflection of the sky there; the foam line changes the water's pixels and nothing
// else, and none with it off; the same frame twice is the same bytes.
#include <core/json/json.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/gfx/device.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/lighting.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <brdf_reference.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

using namespace engine;
using namespace engine::renderer;

#if ENGINE_WATER_TESTS_DUNES

namespace {

// A basin of the renderer's ground (no dunes: a smooth bowl 4 m deep in a plain), and a body that
// fills it from its middle at 1.5 m below the plain, with the shore's two fields given.
std::string basin_scene(f32 wet_m, f32 foam_m) {
  char text[1024];
  std::snprintf(text, sizeof(text), R"({
  "format": "engine.scene.v1",
  "name": "shore-test",
  "terrain": {"size": 129, "extent": 64, "seed": 2, "dune_height": 0,
              "basins": [{"center": [0, 0], "radius": 14, "depth": 4}]},
  "water": [{"name": "basin", "level_m": -1.5,
             "shape": {"fill": {"seed": [0, 0, 0], "max_extent_m": 30}},
             "wind_follow": false, "wave_amplitude_m": 0.03, "wave_length_m": 1.5,
             "wet_m": %g, "foam_m": %g}]
})",
                static_cast<f64>(wet_m), static_cast<f64>(foam_m));
  return text;
}

bool gpu_here() {
  gfx::Device probe;
  std::string why;
  if (!probe.create(gfx::DeviceOptions{}, &why)) {
    MESSAGE("renderer unavailable here: " << why);
    return false;
  }
  const bool atomics = probe.features().buffer_int64_atomics;
  probe.destroy();
  if (!atomics) MESSAGE("no 64-bit buffer atomics here");
  return atomics;
}

// Every shot's camera: from 14 m up over the basin's south-west rim, looking down across it.
constexpr f64 k_eye[3] = {-14.0, 12.0, -14.0};
constexpr f64 k_target[3] = {2.0, -2.0, 2.0};

struct Shot {
  CapturedFrame frame;
  u32 water_instance = ~0u;
  u32 shores = 0;
  f32 fov_y = 0.0f;
};

void shoot(const gfx::Device& device, const test::TempDir& tmp, const std::string& name, f32 wet_m,
           f32 foam_m, u32 shots, Shot* out, bool normals = false) {
  const std::string path = tmp.file(name + ".json");
  {
    std::ofstream file(path, std::ios::binary);
    file << basin_scene(wet_m, foam_m);
  }
  SceneDesc desc;
  std::string error;
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
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = 480;
  rd.height = 320;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
  FrameDesc frame;
  // From 14 m up over the basin's south-west rim, looking down across it.
  frame.camera.position = WorldPos{k_eye[0], k_eye[1], k_eye[2]};
  frame.camera.target = WorldPos{k_target[0], k_target[1], k_target[2]};
  frame.sky_time_s = 0.0;
  frame.world_time_s = 3.0;
  CaptureChannels channels;
  channels.ids = true;
  channels.normals = normals;
  u32 water_instance = ~0u;
  for (u32 i = 0; i < data.instances.size(); ++i) {
    const u32 mesh = data.instances[i].mesh;
    if (mesh < data.mesh_info.size() && data.mesh_info[mesh].water.has_value()) water_instance = i;
  }
  for (u32 s = 0; s < shots; ++s) {
    REQUIRE_MESSAGE(renderer.capture(frame, channels, out[s].frame, &error), error);
    out[s].water_instance = water_instance;
    out[s].shores = scene.water_shore_count();
    out[s].fov_y = frame.camera.fov_y;
  }
}

bool is_water(const CapturedFrame& f, u32 p, u32 water_instance) {
  return f.ids[p * k_id_words] == water_instance;
}

u32 luma(const CapturedFrame& f, u32 p) {
  return u32{f.color[p * 4]} + f.color[p * 4 + 1] + f.color[p * 4 + 2];
}

// Puts the tunables back whatever a case does.
struct Restore {
  ~Restore() {
    (void)tunables::apply_overrides("renderer.water.wet_film=true");
    (void)tunables::apply_overrides("renderer.water.reflect=1");
  }
};

// **The most the wet ground's film can add to pixel p's code in one channel** (S99; water.md, "The
// shore"), derived from what the resolve does there, not measured:
//   - the film adds film_w (sky F_film - ambient F_lobe) to the lit light and clamps at zero
//     (visibility_resolve.slang), with film_w <= 1 and ambient F_lobe >= 0, and with the water's
//     reflection off the light it reflects is the stand-in sky `k_sky`: at most k_sky F_film;
//   - F_film is brdf.slang's `brdf_env_specular` at water's F0, which falls as the roughness rises
//     (every roughness term of its fit lowers it) and as n.v rises, so it is at most its value at
//     the shader's least roughness and the least n.v this pixel's normal allows: the normals
//     channel's (the shading normal here — the basin's ground has no layers, no normal map and no
//     detail block), 8-bit and so at most 2 sqrt(3) / 255 off once renormalized, against the eye
//     ray through the pixel's centre, at most one pixel's angle off the rasterizer's;
//   - the encode (x^(1/2.2) after the SDR clip) is concave from 0, so it adds at most the encoded
//     gain; the dither moves both shots by the same noise, except within a code of black where its
//     amplitude narrows with the value and can double a step; and the store's rounding is a code.
u32 film_code_bound(const Shot& shot, u32 p, u32 channel, u32 code_off) {
  const CapturedFrame& f = shot.frame;
  const f64 x = static_cast<f64>(p % f.width) + 0.5;
  const f64 y = static_cast<f64>(p / f.width) + 0.5;
  f64 fwd[3] = {k_target[0] - k_eye[0], k_target[1] - k_eye[1], k_target[2] - k_eye[2]};
  const f64 fl = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
  for (f64& v : fwd)
    v /= fl;
  // look_at's frame: right = forward x up, up' = right x forward.
  f64 right[3] = {-fwd[2], 0.0, fwd[0]};
  const f64 rl = std::sqrt(right[0] * right[0] + right[2] * right[2]);
  right[0] /= rl;
  right[2] /= rl;
  const f64 up[3] = {right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2],
                     right[0] * fwd[1] - right[1] * fwd[0]};
  const f64 t = std::tan(0.5 * static_cast<f64>(shot.fov_y));
  const f64 sx = (x / f.width * 2.0 - 1.0) * t * static_cast<f64>(f.width) / f.height;
  const f64 sy = (1.0 - y / f.height * 2.0) * t;
  f64 dir[3];
  for (u32 i = 0; i < 3; ++i)
    dir[i] = fwd[i] + right[i] * sx + up[i] * sy;
  const f64 dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
  f64 n[3];
  for (u32 i = 0; i < 3; ++i)
    n[i] = f.normals[p * 3 + i] / 255.0 * 2.0 - 1.0;
  const f64 nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  const f64 n_dot_v = -(n[0] * dir[0] + n[1] * dir[1] + n[2] * dir[2]) / (nl * dl);
  const f64 slack = 2.0 * std::sqrt(3.0) / 255.0 + static_cast<f64>(shot.fov_y) / f.height;
  const f64 least = std::max(n_dot_v - slack, 0.0);
  const f64 f0 = 0.020059312;  // ((1.333 - 1) / (1.333 + 1))^2, wet_ground.slang's
  const f64 fresnel =
      brdf_ref::env_specular(brdf_ref::Dvec3{f0, f0, f0}, brdf_ref::k_min_roughness, least).x;
  const f64 sky = channel == 0 ? k_sky.x : (channel == 1 ? k_sky.y : k_sky.z);
  f64 step = 255.0 * std::pow(std::clamp(static_cast<f64>(sky) * fresnel, 0.0, 1.0), 1.0 / 2.2);
  if (code_off <= 1u) step *= 2.0;
  return static_cast<u32>(std::ceil(step)) + 1u;
}

}  // namespace

TEST_CASE("water shore: the wet band darkens the ground at the waterline, and only the ground") {
  if (!gpu_here()) return;
  const test::TempDir tmp("water_shore_wet");
  gfx::Device device;
  std::string error;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  Restore restore;
  // The film reflects the stand-in sky, the one light the bound below knows; what it does with the
  // water's traced reflection of the scene is reflect_scene_tests.cpp's.
  REQUIRE(tunables::apply_overrides("renderer.water.reflect=0"));
  Shot dry[1], narrow[2], wide[1], matte[1];
  shoot(device, tmp, "dry", 0.0f, 0.0f, 1, dry);
  shoot(device, tmp, "narrow", 0.3f, 0.0f, 2, narrow, true);
  shoot(device, tmp, "wide", 0.6f, 0.0f, 1, wide);
  REQUIRE(tunables::apply_overrides("renderer.water.wet_film=false"));
  shoot(device, tmp, "matte", 0.3f, 0.0f, 1, matte);
  REQUIRE(tunables::apply_overrides("renderer.water.wet_film=true"));
  device.destroy();
  CHECK(dry[0].shores == 0u);
  CHECK(narrow[0].shores == 1u);
  REQUIRE(narrow[0].water_instance != ~0u);
  const CapturedFrame& a = dry[0].frame;
  const CapturedFrame& b = narrow[0].frame;
  const CapturedFrame& c = wide[0].frame;
  const CapturedFrame& m = matte[0].frame;
  REQUIRE(a.color.size() == b.color.size());
  REQUIRE(a.color.size() == m.color.size());
  REQUIRE(a.ids.size() == b.ids.size());
  REQUIRE(b.normals.size() == size_t{a.width} * a.height * 3);
  CHECK(b.color == narrow[1].frame.color);
  CHECK(a.ids == b.ids);  // the band is shading: it moves no surface
  CHECK(a.ids == m.ids);
  const u32 pixels = a.width * a.height;
  u32 water = 0, wet_narrow = 0, wet_wide = 0, wet_on_water = 0;
  // Until S99 the band was a darkening alone and no pixel it changed could be lighter. Wet ground
  // now carries a film where it is saturated (wet_ground.slang), and a film reflects the sky:
  // brighter than the dark ground it lies on is what water on dark ground looks like. So the
  // darkening is held to "never lighter" with the film off (`matte`), exactly as before, and the
  // film on to what a film can do: every pixel lighter than the dry one is a pixel the film
  // changed, and no pixel the film changed gains more than the film's own reflection of the sky.
  u32 matte_lighter = 0, lighter = 0, lighter_unfilmed = 0, filmed = 0, over_bound = 0;
  for (u32 p = 0; p < pixels; ++p) {
    const bool w = is_water(a, p, narrow[0].water_instance);
    water += w ? 1u : 0u;
    const bool nb = luma(a, p) != luma(b, p) || a.color[p * 4] != b.color[p * 4];
    const bool wb = luma(a, p) != luma(c, p) || a.color[p * 4] != c.color[p * 4];
    const bool mb = std::memcmp(&a.color[p * 4], &m.color[p * 4], 3) != 0;
    const bool film = std::memcmp(&b.color[p * 4], &m.color[p * 4], 3) != 0;
    wet_narrow += nb ? 1u : 0u;
    wet_wide += wb ? 1u : 0u;
    if ((nb || wb) && w) ++wet_on_water;
    if (mb && luma(m, p) > luma(a, p)) ++matte_lighter;
    if (nb && luma(b, p) > luma(a, p)) {
      ++lighter;
      if (!film) ++lighter_unfilmed;
    }
    if (!film) continue;
    ++filmed;
    for (u32 ch = 0; ch < 3; ++ch) {
      const u32 on = b.color[p * 4 + ch];
      const u32 off = m.color[p * 4 + ch];
      const u32 bound = film_code_bound(narrow[0], p, ch, off);
      if (on <= off || on - off <= bound) continue;
      if (++over_bound <= 8u)
        MESSAGE("the film gains " << on - off << " codes in channel " << ch << " at ("
                                  << p % a.width << ", " << p / a.width << "), over its bound "
                                  << bound);
    }
  }
  MESSAGE("basin: " << water << " water pixels of " << pixels << "; the band at 0.3 m changes "
                    << wet_narrow << ", at 0.6 m " << wet_wide << "; its film changes " << filmed
                    << ", " << lighter << " of them lighter than dry ground");
  CHECK(water > 2000u);
  CHECK(wet_narrow > 100u);
  CHECK(wet_wide > wet_narrow + wet_narrow / 4);
  CHECK(wet_on_water == 0u);
  CHECK(matte_lighter == 0u);     // the darkening alone never lightens a pixel
  CHECK(filmed > 0u);             // the film is drawn at the waterline
  CHECK(lighter_unfilmed == 0u);  // a lighter pixel is the film's
  CHECK(over_bound == 0u);        // by no more than its reflection of the sky
}

TEST_CASE("water shore: the foam line is on the water, and only on the water") {
  if (!gpu_here()) return;
  const test::TempDir tmp("water_shore_foam");
  gfx::Device device;
  std::string error;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  Shot calm[1], foam[1];
  shoot(device, tmp, "calm", 0.0f, 0.0f, 1, calm);
  shoot(device, tmp, "foam", 0.0f, 1.0f, 1, foam);
  device.destroy();
  const CapturedFrame& a = calm[0].frame;
  const CapturedFrame& b = foam[0].frame;
  REQUIRE(a.color.size() == b.color.size());
  CHECK(a.ids == b.ids);
  const u32 pixels = a.width * a.height;
  u32 changed = 0, off_water = 0, water = 0;
  for (u32 p = 0; p < pixels; ++p) {
    const bool w = is_water(a, p, calm[0].water_instance);
    water += w ? 1u : 0u;
    const bool d = a.color[p * 4] != b.color[p * 4] || a.color[p * 4 + 1] != b.color[p * 4 + 1] ||
                   a.color[p * 4 + 2] != b.color[p * 4 + 2];
    if (!d) continue;
    ++changed;
    if (!w) ++off_water;
  }
  MESSAGE("foam: " << changed << " pixels changed of the water's " << water);
  CHECK(changed > 50u);
  CHECK(changed < water / 2);  // a line along the edge, not the whole surface
  CHECK(off_water == 0u);
}

#endif
