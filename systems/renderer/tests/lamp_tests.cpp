// Lamps (fire.h; docs/subsystems/renderer.md, "Lamps" and "Lights by reach"; roadmap R86, R87): the
// scene file's block and a prop's, refused past the frame's room with the count, burnt steady,
// every one in the light array behind the fires, a prop's lamp carried by its placed instances and
// counted past the room — and on a GPU, a lamp-lit floor held to the BSDF's CPU reference
// (brdf_reference.h): shadowed by an occluder under traced shadows as a point light is, unshadowed
// under the cascaded maps.

#include "test_device.h"

#include <core/math/world.h>
#include <domain/gfx/device.h>
#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/fire.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/lighting.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <brdf_reference.h>
#include <cmath>
#include <cstring>
#include <span>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

f32 luminance(Vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

std::string lamps_json(u32 count) {
  std::string s;
  for (u32 i = 0; i < count; ++i) {
    if (i != 0) s += ",";
    s += "{\"name\":\"l" + std::to_string(i) + "\",\"position\":[" + std::to_string(i) +
         ",2,0],\"intensity\":30}";
  }
  return s;
}

}  // namespace

TEST_CASE("lamp: the scene file's lamps and a prop's lamp are read, and past the room refused") {
  const test::TempDir tmp("renderer_lamp_scene");
  const std::string path = tmp.file("lamp.json");
  REQUIRE(
      io::write_file(
          path,
          R"({"format":"engine.scene.v1","name":"lamps","terrain":{"size":17,"extent":16,"seed":1},
      "lamps":[{"name":"a","position":[1,2,3],"color":[2,1.7,1.2],"intensity":35,"reach_m":6},
               {"name":"b","position":[0,1.5,0]}],
      "props":[{"name":"lantern","mesh":"lantern.glb",
                "lamp":{"name":"globe","position":[0,0.4,0],"color":[0.6,0.8,1]}}]})") ==
      io::Status::Ok);
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  REQUIRE(desc.lamps.size() == 2);
  CHECK(desc.fires.empty());
  const FireDesc& a = desc.lamps[0];
  CHECK(a.lamp);
  CHECK(a.name == "a");
  CHECK(a.position == WorldPos{1.0, 2.0, 3.0});
  CHECK(a.intensity == 35.0f);
  CHECK(a.reach_m == 6.0f);
  CHECK(a.flicker == 0.0f);
  CHECK(a.smoke_density == 0.0f);
  // Its chromaticity at a luminance of 1: (2, 1.7, 1.2) over its luminance.
  CHECK(luminance(a.color) == doctest::Approx(1.0).epsilon(1e-6));
  CHECK(a.color.x / a.color.z == doctest::Approx(2.0 / 1.2).epsilon(1e-5));
  // The schema's defaults: an absent colour is white, the intensity 40 cd, the reach the fire's.
  const FireDesc& b = desc.lamps[1];
  CHECK(b.color == Vec3{1.0f, 1.0f, 1.0f});
  CHECK(b.intensity == 40.0f);
  CHECK(b.reach_m == 0.0f);
  REQUIRE(desc.props.size() == 1);
  CHECK_FALSE(desc.props[0].fire.has_value());
  REQUIRE(desc.props[0].lamp.has_value());
  CHECK(desc.props[0].lamp->position == WorldPos{0.0, 0.4, 0.0});
  CHECK(luminance(desc.props[0].lamp->color) == doctest::Approx(1.0).epsilon(1e-6));

  // A bad colour, named.
  REQUIRE(
      io::write_file(
          path,
          R"({"format":"engine.scene.v1","name":"bad","terrain":{"size":17,"extent":16,"seed":1},
      "lamps":[{"name":"x","position":[0,0,0],"color":[1,-1,1]}]})") == io::Status::Ok);
  SceneDesc bad;
  CHECK_FALSE(read_scene_file(path, bad, error));
  MESSAGE(error);
  CHECK(error.find("lamps 0") != std::string::npos);
  CHECK(error.find("color") != std::string::npos);

  // The room exactly is read; one past it is refused with the count, never cut short.
  const std::string head =
      R"({"format":"engine.scene.v1","name":"many","terrain":{"size":17,"extent":16,"seed":1},"lamps":[)";
  REQUIRE(io::write_file(path, head + lamps_json(k_max_lamps) + "]}") == io::Status::Ok);
  SceneDesc full;
  REQUIRE_MESSAGE(read_scene_file(path, full, error), error);
  CHECK(full.lamps.size() == k_max_lamps);
  REQUIRE(io::write_file(path, head + lamps_json(k_max_lamps + 1) + "]}") == io::Status::Ok);
  SceneDesc over;
  CHECK_FALSE(read_scene_file(path, over, error));
  MESSAGE(error);
  CHECK(error.find(std::to_string(k_max_lamps + 1) + " lamps") != std::string::npos);
  CHECK(error.find(std::to_string(k_max_lamps)) != std::string::npos);
}

TEST_CASE("lamp: a lamp burns steady at its colour, intensity and reach") {
  FireDesc lamp;
  lamp.lamp = true;
  lamp.intensity = 40.0f;
  lamp.color = Vec3{1.2f, 0.95f, 0.7f};
  const WorldPos at{5.0, 2.0, -1.0};
  const FrameFire a = burn_lamp(lamp, at, 1.0f);
  const FrameFire b = burn_lamp(lamp, at, 1.0f);
  // No flicker: any clock is the same light, and the light is the block's.
  CHECK(a.intensity == 40.0f);
  CHECK(b.intensity == a.intensity);
  CHECK(b.color == a.color);
  CHECK(a.color == lamp.color);
  CHECK(a.base == at);
  // No flame and no smoke for the medium to draw; its radius is the globe's.
  CHECK(a.radius_m == lamp.radius_m);
  CHECK(a.height_m == 0.0f);
  CHECK(a.smoke_density == 0.0f);
  // The reach: the fire's rule when it names none, its own when it does, scaled with a prop.
  CHECK(a.reach_m == fire_light_reach(40.0f));
  lamp.reach_m = 6.0f;
  CHECK(burn_lamp(lamp, at, 1.0f).reach_m == 6.0f);
  const FrameFire big = burn_lamp(lamp, at, 2.0f);
  CHECK(big.reach_m == 12.0f);
  CHECK(big.intensity == 160.0f);
  // With a sky, sun units: the reach is the candela's either way.
  const FrameFire sky = burn_lamp(lamp, at, 1.0f, 1.0f / 128000.0f);
  CHECK(sky.reach_m == 6.0f);
  CHECK(sky.intensity * 128000.0f == doctest::Approx(40.0).epsilon(1e-6));
}

TEST_CASE("lamp: the light array takes every lamp, in order, behind what it has") {
  FrameFire lamps[k_max_lamps + 3];
  for (u32 i = 0; i < k_max_lamps + 3; ++i) {
    lamps[i].base = WorldPos{100.0 + i, 2.0, 0.0};
    lamps[i].intensity = 10.0f + static_cast<f32>(i);
    lamps[i].color = Vec3{1.0f, 0.9f, 0.8f};
    lamps[i].reach_m = 7.0f;
  }
  lamps[1].intensity = 0.0f;  // an unlit lamp takes no light
  FrameLighting lighting;
  lighting.light_count = 1;  // a moon
  FrameFire fire;
  fire.base = WorldPos{100.0, 0.0, 0.0};
  fire.height_m = 1.0f;
  fire.intensity = 250.0f;
  fire.reach_m = 70.0f;
  CHECK(add_fire_lights(std::span<const FrameFire>(&fire, 1), WorldPos{}, lighting) == 1);
  const WorldPos eye{100.0, 2.0, 5.0};
  const u32 added = add_lamp_lights(std::span<const FrameFire>(lamps, k_max_lamps), eye, lighting);
  CHECK(added == k_max_lamps - 1);
  CHECK(lighting.light_count == 2 + k_max_lamps - 1);
  CHECK(lighting.light_count <= k_frame_lights);
  // In their order behind the moon and the fire, at their centres in the eye's frame.
  CHECK(lighting.lights[2].color_intensity.w == 10.0f);
  CHECK(lighting.lights[3].color_intensity.w == 12.0f);
  CHECK(lighting.lights[2].position_radius.x == 0.0f);
  CHECK(lighting.lights[2].position_radius.y == 0.0f);
  CHECK(lighting.lights[2].position_radius.z == -5.0f);
  CHECK(lighting.lights[2].position_radius.w == 7.0f);
  CHECK(lighting.lights[2].direction_type.w == gfx::k_light_point);
  // The array's room is the stand-in's two, every fire and every lamp (R87): a full frame of each
  // fits, and no more than `k_max_lamps` lamps are ever taken.
  FrameLighting roomy;
  roomy.light_count = k_stand_in_lights + k_max_fires;
  CHECK(add_lamp_lights(std::span<const FrameFire>(lamps, k_max_lamps + 3), eye, roomy) ==
        k_max_lamps - 1);
}

TEST_CASE("lamp: a prop's lamp stands where each placed instance stands, counted past the room") {
  SceneData scene;
  scene.parts.resize(3);
  FireDesc lamp;
  lamp.lamp = true;
  lamp.position = WorldPos{0.0, 0.5, 0.25};  // from the prop's origin
  lamp.intensity = 20.0f;
  SceneProp prop{"lantern", 2u};
  prop.lamp = lamp;
  scene.props.push_back(prop);
  FireDesc fixed = lamp;
  fixed.position = WorldPos{-30.0, 2.0, 0.0};
  scene.lamps.push_back(fixed);
  FireSet set;
  set.reset(scene);
  CHECK(set.count() == 0);  // lamps are not fires
  CHECK(set.lamp_count() == 1);
  SceneInstance tail[2];
  tail[0].mesh = 2;
  tail[0].origin = WorldPos{1000.0, 5.0, 2000.0};
  tail[0].transform.rotation = quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, 0.5f * k_pi);
  tail[0].transform.scale = Vec3{2.0f, 2.0f, 2.0f};
  tail[1].mesh = 1;  // not a lamp's
  set.place(std::span<const SceneInstance>(tail, 2));
  CHECK(set.lamp_count() == 2);
  CHECK(set.lamps_refused() == 0);
  Vector<FrameFire> out;
  set.burn_lamps(1.0f, out);
  REQUIRE(out.size() == 2);
  CHECK(out[0].base == fixed.position);
  // The offset turned a quarter about +y (z to x) and doubled; the intensity four times.
  CHECK(out[1].base.x == doctest::Approx(1000.5));
  CHECK(out[1].base.y == doctest::Approx(6.0));
  CHECK(out[1].base.z == doctest::Approx(2000.0));
  CHECK(out[1].intensity == doctest::Approx(80.0));
  Vector<FrameFire> fires;
  set.burn(0.0, 1.0f, fires);
  CHECK(fires.empty());  // the medium is handed no lamp

  // A tail of more lanterns than the room: the first in its order, the rest counted.
  Vector<SceneInstance> many;
  many.resize(k_max_lamps + 5);
  for (SceneInstance& instance : many)
    instance.mesh = 2;
  set.place(std::span<const SceneInstance>(many.data(), many.size()));
  CHECK(set.lamp_count() == k_max_lamps);
  CHECK(set.lamps_refused() == 6);  // the scene's own takes one of the room
  set.place({});
  CHECK(set.lamp_count() == 1);
  CHECK(set.lamps_refused() == 0);
}

// ---- on a GPU
// ------------------------------------------------------------------------------------

namespace {

constexpr u32 k_width = 256;
constexpr u32 k_height = 192;

// The floor and the slab over it: a 12 m square at y = 0 and a 0.6 m one at y = 0.75, both facing
// up, grey and fully rough, written as a glTF with its buffer beside it.
constexpr f32 k_slab_y = 0.75f;
constexpr f32 k_slab_x0 = 1.0f, k_slab_x1 = 1.6f, k_slab_z = 0.3f;
constexpr f32 k_albedo = 0.5f;

void put_f32(std::string& bytes, f32 v) {
  char b[4];
  std::memcpy(b, &v, 4);
  bytes.append(b, 4);
}

void put_u32(std::string& bytes, u32 v) {
  char b[4];
  std::memcpy(b, &v, 4);
  bytes.append(b, 4);
}

// The globe: a closed cube 0.2 m across round the lamp's centre, faces outward. It stands in the
// lamp's every shadow ray unless the ray stops at the globe's radius (`FireDesc::radius_m`).
constexpr f32 k_globe_half = 0.1f;
constexpr f32 k_globe_radius = 0.18f;  // past the cube's corners, 0.173 m out
const Vec3 k_lamp_at{0.0f, 1.5f, 0.0f};

struct Quad {
  Vec3 centre, u, v, normal;  // half-axes; counter-clockwise seen from `normal`
};

bool write_room(const test::TempDir& dir, std::string& gltf_path) {
  // The floor and the slab, each facing up, then the globe's six faces.
  Vector<Quad> quads;
  quads.push_back({Vec3{0.0f, 0.0f, 0.0f}, Vec3{6.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 6.0f},
                   Vec3{0.0f, 1.0f, 0.0f}});
  quads.push_back({Vec3{0.5f * (k_slab_x0 + k_slab_x1), k_slab_y, 0.0f},
                   Vec3{0.5f * (k_slab_x1 - k_slab_x0), 0.0f, 0.0f}, Vec3{0.0f, 0.0f, k_slab_z},
                   Vec3{0.0f, 1.0f, 0.0f}});
  for (u32 axis = 0; axis < 3; ++axis) {
    for (const f32 sign : {-1.0f, 1.0f}) {
      Vec3 n{}, u{}, v{};
      (&n.x)[axis] = sign;
      (&u.x)[(axis + 1) % 3] = k_globe_half;
      (&v.x)[(axis + 2) % 3] = k_globe_half;
      quads.push_back({k_lamp_at + n * k_globe_half, u, v, n});
    }
  }
  const u32 vertices = static_cast<u32>(quads.size()) * 4;
  const u32 indices = static_cast<u32>(quads.size()) * 6;
  std::string bytes;
  for (const Quad& q : quads) {
    const bool flip = dot(cross(q.u, q.v), q.normal) < 0.0f;
    const Vec3 u = flip ? q.v : q.u;
    const Vec3 v = flip ? q.u : q.v;
    const Vec3 corners[4] = {q.centre - u - v, q.centre + u - v, q.centre + u + v,
                             q.centre - u + v};
    for (const Vec3& p : corners) {
      put_f32(bytes, p.x);
      put_f32(bytes, p.y);
      put_f32(bytes, p.z);
    }
  }
  for (const Quad& q : quads) {
    for (u32 i = 0; i < 4; ++i) {
      put_f32(bytes, q.normal.x);
      put_f32(bytes, q.normal.y);
      put_f32(bytes, q.normal.z);
    }
  }
  for (u32 i = 0; i < vertices; ++i) {
    put_f32(bytes, 0.0f);
    put_f32(bytes, 0.0f);
  }
  for (u32 q = 0; q < quads.size(); ++q) {
    const u32 b = q * 4;
    for (const u32 i : {b, b + 1, b + 2, b, b + 2, b + 3})
      put_u32(bytes, i);
  }
  const std::string bin = dir.file("room.bin");
  if (io::write_file(bin, bytes) != io::Status::Ok) return false;
  const u32 vec3_bytes = vertices * 12;
  const u32 uv_bytes = vertices * 8;
  const auto n = [](u32 v) { return std::to_string(v); };
  const std::string json =
      R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0, "name": "room"}],
  "meshes": [{"name": "room", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1,
    "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}],
  "materials": [{"name": "grey", "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 1],
    "metallicFactor": 0, "roughnessFactor": 1}}],
  "buffers": [{"byteLength": )" +
      n(static_cast<u32>(bytes.size())) + R"(, "uri": "room.bin"}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": )" +
      n(vec3_bytes) + R"(}, {"buffer": 0, "byteOffset": )" + n(vec3_bytes) + R"(, "byteLength": )" +
      n(vec3_bytes) + R"(}, {"buffer": 0, "byteOffset": )" + n(2 * vec3_bytes) +
      R"(, "byteLength": )" + n(uv_bytes) + R"(}, {"buffer": 0, "byteOffset": )" +
      n(2 * vec3_bytes + uv_bytes) + R"(, "byteLength": )" + n(indices * 4) + R"(}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": )" +
      n(vertices) + R"(, "type": "VEC3", "min": [-6, 0, -6], "max": [6, 1.6, 6]},
    {"bufferView": 1, "componentType": 5126, "count": )" +
      n(vertices) + R"(, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5126, "count": )" +
      n(vertices) + R"(, "type": "VEC2"},
    {"bufferView": 3, "componentType": 5125, "count": )" +
      n(indices) + R"(, "type": "SCALAR"}]})";
  gltf_path = dir.file("room.gltf");
  return io::write_file(gltf_path, json) == io::Status::Ok;
}

struct LampRig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  std::string error;

  bool build(const gfx::Device& device, const std::string& room, ShadowMode shadows,
             const FireDesc* lamp, std::span<const FireDesc> more = {}) {
    SceneDesc desc;
    desc.meshes.push_back(room);
    desc.cache = false;
    if (!load_scene(desc, data, error)) return false;
    if (lamp != nullptr) data.lamps.push_back(*lamp);
    for (const FireDesc& light : more)
      (light.lamp ? data.lamps : data.fires).push_back(light);
    RenderSettings settings;
    settings.shadows = shadows;
    settings.lights = false;  // the stand-in's two lights out: the lamp is the only point light
    settings.dither = false;
    resolve_settings(settings, device.features(), &data, resolved);
    if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
      error = "unavailable";
      return false;
    }
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = k_width;
    rd.height = k_height;
    rd.color_format = gfx::Format::R16G16B16A16Sfloat;
    rd.display = gfx::DisplayEncoding::Linear;
    return renderer.create(device, scene, resolved, rd, &error);
  }
};

void lamp_on_the_floor(ShadowMode shadows, const char* name) {
  gfx::Device device;
  std::string error;
  if (!device.create(renderer_test::device_options(), &error) ||
      !device.features().buffer_int64_atomics) {
    MESSAGE("skipped: no device with 64-bit buffer atomics: " << error);
    return;
  }
  const test::TempDir dir("renderer_lamp_floor");
  std::string room;
  REQUIRE(write_room(dir, room));
  const bool traced = shadows == ShadowMode::RayTraced;

  FireDesc lamp;
  lamp.lamp = true;
  lamp.name = "globe";
  lamp.position = WorldPos{static_cast<f64>(k_lamp_at.x), static_cast<f64>(k_lamp_at.y),
                           static_cast<f64>(k_lamp_at.z)};
  lamp.intensity = 4.0f;  // the stand-in's units: its sun is 1
  lamp.color = Vec3{1.25f, 0.95f, 0.7f};
  lamp.color = lamp.color * (1.0f / luminance(lamp.color));
  lamp.radius_m = k_globe_radius;  // the cube round it is its globe
  const Vec3 eye{1.5f, 7.0f, 3.0f};
  const Vec3 target{1.5f, 0.0f, 0.0f};
  const f32 fov_y = 60.0f * k_pi / 180.0f;
  FrameDesc frame;
  frame.camera.position =
      WorldPos{static_cast<f64>(eye.x), static_cast<f64>(eye.y), static_cast<f64>(eye.z)};
  frame.camera.target =
      WorldPos{static_cast<f64>(target.x), static_cast<f64>(target.y), static_cast<f64>(target.z)};
  frame.camera.fov_y = fov_y;
  frame.camera.znear = 0.05f;

  LampRig lit;
  if (!lit.build(device, room, shadows, &lamp)) {
    REQUIRE_MESSAGE(lit.error == "unavailable", lit.error);
    MESSAGE("skipped: " << std::string(name) << " is not available here");
    return;
  }
  LampRig dark;
  REQUIRE_MESSAGE(dark.build(device, room, shadows, nullptr), dark.error);
  // An unlit lamp is no light at all: the same bytes as none.
  FireDesc off = lamp;
  off.intensity = 0.0f;
  LampRig unlit;
  REQUIRE_MESSAGE(unlit.build(device, room, shadows, &off), unlit.error);
  CaptureChannels channels;
  channels.color = false;
  channels.light = true;
  CapturedFrame a, b, c;
  REQUIRE_MESSAGE(lit.renderer.capture(frame, channels, a, &lit.error), lit.error);
  REQUIRE_MESSAGE(dark.renderer.capture(frame, channels, b, &dark.error), dark.error);
  REQUIRE_MESSAGE(unlit.renderer.capture(frame, channels, c, &unlit.error), unlit.error);
  CHECK(lit.renderer.stats().lamp_lights == 1);
  CHECK(dark.renderer.stats().lamp_lights == 0);
  CHECK(unlit.renderer.stats().lamp_lights == 0);
  CHECK(lit.renderer.stats().fire_lights == 0);
  REQUIRE(a.light.size() == size_t{k_width} * k_height * 3);
  REQUIRE(b.light.size() == a.light.size());
  CHECK(c.light == b.light);
  // The same lamp with no radius under traced shadows: its globe stands in every shadow ray, and
  // the floor is as dark as with no lamp. Under the maps, which shadow no lamp, it is the lamp.
  FireDesc bare = lamp;
  bare.radius_m = 0.0f;
  LampRig enclosed;
  REQUIRE_MESSAGE(enclosed.build(device, room, shadows, &bare), enclosed.error);
  CapturedFrame g;
  REQUIRE_MESSAGE(enclosed.renderer.capture(frame, channels, g, &enclosed.error), enclosed.error);
  if (traced) {
    // Dark but for what R115's faint-light rule adds untested (renderer.md, "A light too faint to
    // test"): at most 2^-12 of a pixel's other light, which a half's rounding may double.
    f64 most = 0.0;
    for (u32 i = 0; i < g.light.size(); ++i) {
      const f64 rest = static_cast<f64>(b.light[i]);
      most = std::max(most, std::fabs(static_cast<f64>(g.light[i]) - rest) / (rest + 1e-4));
    }
    MESSAGE("a globe with no radius moved the floor by at most " << most << " of its light");
    CHECK(most <= 1.0 / 512.0);
  } else {
    CHECK(g.light == a.light);
  }

  // Each pixel's ray onto the floor, as look_at and the projection lay it out (row 0 the top).
  const Vec3 f = normalize(target - eye);
  const Vec3 s = normalize(cross(f, Vec3{0.0f, 1.0f, 0.0f}));
  const Vec3 u = cross(s, f);
  const f64 t = std::tan(0.5 * static_cast<f64>(fov_y));
  const f64 aspect = static_cast<f64>(k_width) / k_height;
  const brdf_ref::Dvec3 lamp_at = brdf_ref::dvec3(k_lamp_at);
  u32 checked = 0, shadowed = 0, outside = 0;
  f64 worst = 0.0;
  for (u32 py = 2; py < k_height; py += 3) {
    for (u32 px = 2; px < k_width; px += 3) {
      const f64 xn = (2.0 * (px + 0.5) / k_width - 1.0) * t * aspect;
      const f64 yn = (1.0 - 2.0 * (py + 0.5) / k_height) * t;
      const brdf_ref::Dvec3 d = brdf_ref::normalize(brdf_ref::dvec3(f) + brdf_ref::dvec3(s) * xn +
                                                    brdf_ref::dvec3(u) * yn);
      const brdf_ref::Dvec3 e = brdf_ref::dvec3(eye);
      // The globe seen from above, and a margin about it: not the floor (the ray's slab test
      // against its box grown by 5 cm).
      {
        f64 t0 = 0.0, t1 = 1e30;
        const f64 lo[3] = {lamp_at.x - 0.15, lamp_at.y - 0.15, lamp_at.z - 0.15};
        const f64 hi[3] = {lamp_at.x + 0.15, lamp_at.y + 0.15, lamp_at.z + 0.15};
        const f64 o[3] = {e.x, e.y, e.z}, dd[3] = {d.x, d.y, d.z};
        for (u32 k = 0; k < 3; ++k) {
          const f64 a0 = (lo[k] - o[k]) / dd[k], a1 = (hi[k] - o[k]) / dd[k];
          t0 = std::max(t0, std::min(a0, a1));
          t1 = std::min(t1, std::max(a0, a1));
        }
        if (t0 <= t1) continue;
      }
      // The slab seen from above, and a margin about it: not the floor.
      const f64 ts = (static_cast<f64>(k_slab_y) - e.y) / d.y;
      const f64 sx = e.x + d.x * ts, sz = e.z + d.z * ts;
      if (sx > static_cast<f64>(k_slab_x0) - 0.15 && sx < static_cast<f64>(k_slab_x1) + 0.15 &&
          std::fabs(sz) < static_cast<f64>(k_slab_z) + 0.15)
        continue;
      const f64 tf = -e.y / d.y;
      const brdf_ref::Dvec3 p{e.x + d.x * tf, 0.0, e.z + d.z * tf};
      if (std::fabs(p.x) > 5.5 || std::fabs(p.z) > 5.5) continue;
      // The slab's shadow from the lamp: the floor points whose midpoint with the lamp is over it.
      const f64 x0 = static_cast<f64>(k_slab_x0), x1 = static_cast<f64>(k_slab_x1);
      const f64 z1 = static_cast<f64>(k_slab_z);
      const f64 mx = 0.5 * (p.x + static_cast<f64>(lamp_at.x));
      const f64 mz = 0.5 * (p.z + static_cast<f64>(lamp_at.z));
      const f64 edge =
          std::min(std::min(std::fabs(mx - x0), std::fabs(mx - x1)), std::fabs(std::fabs(mz) - z1));
      if (edge < 0.08) continue;  // the pixel's footprint may straddle the shadow's edge
      const bool in_shadow = mx > x0 && mx < x1 && std::fabs(mz) < z1;
      brdf_ref::Surface surface;
      surface.position = p;
      surface.normal = brdf_ref::Dvec3{0.0, 1.0, 0.0};
      surface.view = brdf_ref::normalize(e - p);
      surface.albedo = brdf_ref::splat(static_cast<f64>(k_albedo));
      surface.roughness = 1.0;
      const brdf_ref::Dvec3 to_light = lamp_at - p;
      const f64 d2 = brdf_ref::dot(to_light, to_light);
      const f64 reach = static_cast<f64>(fire_light_reach(lamp.intensity));
      const brdf_ref::Dvec3 radiance =
          brdf_ref::dvec3(lamp.color) *
          (static_cast<f64>(lamp.intensity) * brdf_ref::distance_attenuation(d2, reach));
      brdf_ref::Dvec3 expected = brdf_ref::direct(surface, brdf_ref::normalize(to_light), radiance);
      if (traced && in_shadow) expected = brdf_ref::Dvec3{};
      const u32 i = (py * k_width + px) * 3;
      for (u32 ch = 0; ch < 3; ++ch) {
        const f64 got = static_cast<f64>(a.light[i + ch]) - static_cast<f64>(b.light[i + ch]);
        const f64 want = ch == 0 ? expected.x : (ch == 1 ? expected.y : expected.z);
        const f64 err = std::fabs(got - want) / (std::fabs(want) * 0.02 + 2e-3);
        worst = std::max(worst, err);
        if (err > 1.0) {
          MESSAGE("pixel " << px << "," << py << " (" << p.x << ", " << p.z << ") channel " << ch
                           << ": " << got << " against the reference's " << want);
        }
        CHECK(err <= 1.0);
      }
      ++checked;
      (in_shadow ? shadowed : outside) += 1;
    }
  }
  MESSAGE(std::string(name) << ": " << checked << " floor pixels held to the reference ("
                            << shadowed << " in the slab's shadow from the lamp, " << outside
                            << " outside it), worst at " << worst << " of the allowance");
  CHECK(checked > 1000);
  CHECK(shadowed > 30);
  // The same frame again is the same bytes: a lamp is steady.
  CapturedFrame again;
  REQUIRE_MESSAGE(lit.renderer.capture(frame, channels, again, &lit.error), lit.error);
  CHECK(again.light == a.light);
}

// **Lights by reach** (gfx/light_grid.h; renderer.md, "Lights by reach"; R87): a floor under 63
// lamps of short reach and six fires, the slab and the globe's cube among them, drawn with the
// grid and without it (`renderer.lights.grid`) on one renderer: the same bytes in the linear light
// the resolve wrote, because a light's windowed inverse square is exactly zero past its reach and
// the walk keeps the array's order. The grid has to have culled for the comparison to mean
// anything: its set bits are far fewer than its cells times its lights.
void lights_by_reach(ShadowMode shadows, const char* name) {
  gfx::Device device;
  std::string error;
  if (!device.create(renderer_test::device_options(), &error) ||
      !device.features().buffer_int64_atomics) {
    MESSAGE("skipped: no device with 64-bit buffer atomics: " << error);
    return;
  }
  const test::TempDir dir("renderer_lights_by_reach");
  std::string room;
  REQUIRE(write_room(dir, room));
  Vector<FireDesc> lights;
  for (u32 j = 0; j < 7; ++j) {
    for (u32 i = 0; i < 9; ++i) {
      FireDesc lamp;
      lamp.lamp = true;
      lamp.name = "lamp";
      lamp.position = WorldPos{-5.0 + 1.25 * i, 0.3 + 0.15 * ((i + j) % 5), -4.5 + 1.5 * j};
      lamp.intensity = 0.4f + 0.05f * static_cast<f32>((i * 7 + j) % 9);
      lamp.color = Vec3{0.6f + 0.05f * static_cast<f32>(i % 5), 1.0f,
                        0.5f + 0.08f * static_cast<f32>(j % 6)};
      lamp.color = lamp.color * (1.0f / luminance(lamp.color));
      lamp.reach_m = 0.9f + 0.25f * static_cast<f32>((i + 2 * j) % 6);
      lights.push_back(lamp);
    }
  }
  for (u32 k = 0; k < 6; ++k) {  // more than the four the array took before R87
    FireDesc fire;
    fire.name = "fire";
    fire.position = WorldPos{-4.0 + 1.6 * k, 0.0, 4.0 - 1.2 * (k % 3)};
    fire.radius_m = 0.2f;
    fire.height_m = 0.5f;
    fire.intensity = 0.02f;  // reaches 0.63 m (fire_light_reach)
    fire.seed = 11 + k;
    lights.push_back(fire);
  }
  LampRig rig;
  if (!rig.build(device, room, shadows, nullptr, std::span<const FireDesc>(lights))) {
    REQUIRE_MESSAGE(rig.error == "unavailable", rig.error);
    MESSAGE("skipped: " << std::string(name) << " is not available here");
    return;
  }
  FrameDesc frame;
  frame.camera.position = WorldPos{0.0, 9.0, 7.5};
  frame.camera.target = WorldPos{0.0, 0.0, 0.0};
  frame.camera.fov_y = 70.0f * k_pi / 180.0f;
  frame.camera.znear = 0.05f;
  frame.world_time_s = 3.25;  // the fires flicker by the clock: one clock for both frames
  CaptureChannels channels;
  channels.color = false;  // the rig's target is linear: its picture is the light channel
  channels.light = true;
  struct Restore {
    ~Restore() { (void)tunables::apply_overrides("renderer.lights.grid=true"); }
  } restore;
  REQUIRE(tunables::apply_overrides("renderer.lights.grid=true"));
  CapturedFrame culled, whole;
  REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, culled, &rig.error), rig.error);
  const auto on = rig.renderer.stats();
  REQUIRE(tunables::apply_overrides("renderer.lights.grid=false"));
  REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, whole, &rig.error), rig.error);
  const auto off = rig.renderer.stats();
  CHECK(on.lamp_lights == 63);
  CHECK(on.fire_lights == 6);
  CHECK(on.light_grid_cells > 100);
  CHECK(off.light_grid_cells == 0);
  CHECK(off.light_grid_refs == 0);
  // It culled: on average a cell holds a small share of the 69 lights.
  const f64 per_cell = static_cast<f64>(on.light_grid_refs) / on.light_grid_cells;
  MESSAGE(std::string(name) << ": " << on.light_grid_cells << " cells, " << on.light_grid_refs
                            << " set bits, " << per_cell << " lights a cell of 69");
  CHECK(per_cell < 8.0);
  REQUIRE(culled.light.size() == size_t{k_width} * k_height * 3);
  REQUIRE(whole.light.size() == culled.light.size());
  u32 differ = 0;
  for (u32 i = 0; i < culled.light.size(); ++i)
    differ += culled.light[i] != whole.light[i];
  MESSAGE(std::string(name) << ": " << differ << " light values differ with the grid off");
  CHECK(differ == 0);
  CHECK(culled.light == whole.light);
  // And the floor is lit: the lamps reach it (the frame is not the moonless dark of none).
  u32 lit = 0;
  for (u32 i = 0; i < culled.light.size(); ++i)
    lit += culled.light[i] > 1e-3f;
  CHECK(lit > culled.light.size() / 8);
}

}  // namespace

TEST_CASE("lamp: a lamp-lit floor is the model's, shadowed by a slab under traced shadows") {
  lamp_on_the_floor(ShadowMode::RayTraced, "--shadows rt");
}

TEST_CASE("lamp: a lamp-lit floor is the model's, unshadowed under the cascaded maps") {
  lamp_on_the_floor(ShadowMode::Cascaded, "--shadows csm");
}

TEST_CASE("lamp: lights culled by reach draw the bytes of every light, under traced shadows") {
  lights_by_reach(ShadowMode::RayTraced, "--shadows rt");
}

TEST_CASE("lamp: lights culled by reach draw the bytes of every light, under the cascaded maps") {
  lights_by_reach(ShadowMode::Cascaded, "--shadows csm");
}
