// A smooth surface reflects the scene (roadmap R71; docs/subsystems/renderer.md, "Reflections on
// smooth surfaces"; surface_reflect.slang). On the tree's own fixture, content/test-scenes/
// glass-street: two walls of glass at roughness 0.06 across a street, a red box behind the eye and
// outside its view, and beside the near wall a honed panel in the fade (0.15) and a rough one past
// it (0.45). Every promise is held by a count of pixels, never by a clock.
#include "test_device.h"

#include <domain/gfx/device.h>
#include <domain/gfx/visibility_resolve.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/camera_path.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/reference.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

// The scene file's meshes, in its order.
constexpr u32 k_glass = 0;
constexpr u32 k_fade = 1;
constexpr u32 k_rough = 2;
// The sky's epoch is midnight: the street is drawn at 10:00 (the fixture's README; the cost was
// flown at 14:00), the morning sun from the east on the red box's face towards the panes, which
// stand in their own shade.
constexpr f64 k_morning_s = 10.0 * 3600.0;

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

// Puts the switches back whatever a case does.
struct RestoreReflect {
  ~RestoreReflect() {
    (void)tunables::apply_overrides("renderer.reflect.surfaces=1");
    (void)tunables::apply_overrides("renderer.reflect.shell_m=32");
  }
};

std::string fixture_file(const std::string& name) {
  return test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/glass-street/" + name,
                         "content/test-scenes/glass-street/" + name);
}

// The tiers a reflection is counted on (R71's mend, finding 6): the default raster mode, and the
// traced primaries with traced shadows, which is what the owner judges by. A device without ray
// queries has no second tier, and says so.
struct Tier {
  std::string name;  // streamed as text: a const char* member prints as its address in doctest
  RenderSettings settings;
};
Vector<Tier> tiers(RenderSettings base = {}) {
  Vector<Tier> out;
  out.push_back(Tier{"default raster", base});
  RenderSettings rt = base;
  rt.raster = RasterMode::RayTrace;
  rt.shadows = ShadowMode::RayTraced;
  out.push_back(Tier{"--raster rt --shadows rt", rt});
  return out;
}

struct Street {
  SceneDesc desc;
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  ReferenceRenderer reference;
  Camera camera;
  std::string error;
  bool traced = false;  // the device traces, so the reflection exists here

  bool build(const gfx::Device& device, const std::string& ddc, RenderSettings settings,
             const std::string& scene_file = {}) {
    const std::string path = !scene_file.empty() ? scene_file : fixture_file("scene.json");
    if (!read_scene_file(path, desc, error)) return false;
    desc.ddc = ddc;
    if (!load_scene(desc, data, error)) return false;
    CameraPath camera_path;
    if (!read_camera_path(desc.camera_path, &desc.terrain, camera_path, error)) return false;
    camera = sample_camera_path(camera_path, 0.0);
    resolve_settings(settings, device.features(), &data, resolved);
    if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
      error = "unavailable";
      return false;
    }
    traced = resolved.shadows;
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = 480;
    rd.height = 270;
    return renderer.create(device, scene, resolved, rd, &error);
  }

  // The mesh a pixel's cluster belongs to, or -1 for the sky.
  i32 mesh_at(const CapturedFrame& shot, u32 p) const {
    const u32 instance = shot.ids[u64{p} * k_id_words];
    if (instance >= data.instances.size()) return -1;
    return static_cast<i32>(data.instances[instance].mesh);
  }

  bool shoot(CapturedFrame& shot) {
    FrameDesc frame;
    frame.camera = camera;
    frame.sky_time_s = k_morning_s;
    CaptureChannels channels;
    channels.ids = true;
    // Two frames, so the counts the second reads back are the first's.
    if (!renderer.render_offscreen(frame, &error)) return false;
    return renderer.capture(frame, channels, shot, &error);
  }
};

bool reflective(i32 mesh) {
  return mesh == static_cast<i32>(k_glass) || mesh == static_cast<i32>(k_fade);
}

}  // namespace

TEST_CASE("surface reflect: the panes reflect the red box behind the eye, and nothing else moves") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const RestoreReflect restore;
  const test::TempDir tmp("renderer_surface_reflect");
  REQUIRE(tmp.ok());
  for (const Tier& tier : tiers()) {
    CAPTURE(tier.name);
    Street street;
    if (!street.build(gpu.device, tmp.file("ddc"), tier.settings)) {
      REQUIRE_MESSAGE(street.error == "unavailable", street.error);
      MESSAGE(tier.name << ": not on this device");
      continue;
    }
    // The fixture's materials are what the case is about: the glass and the honed stone are under
    // the fade's end, the rough stone is not.
    CHECK(street.scene.has_reflective_materials());

    CapturedFrame off;
    CapturedFrame on;
    CapturedFrame again;
    REQUIRE(tunables::apply_overrides("renderer.reflect.surfaces=0"));
    REQUIRE_MESSAGE(street.shoot(off), street.error);
    CHECK(street.renderer.stats().surface_reflect_rays == 0);
    REQUIRE(tunables::apply_overrides("renderer.reflect.surfaces=1"));
    REQUIRE_MESSAGE(street.shoot(on), street.error);
    REQUIRE_MESSAGE(street.shoot(again), street.error);
    if (!street.traced) {
      // The second tier: no ray queries, so the surface draws what it drew (renderer.md).
      CHECK(on.color == off.color);
      CHECK(street.renderer.stats().surface_reflect_rays == 0);
      MESSAGE("this device does not trace: the panes keep the ambient, byte for byte");
      continue;
    }
    // With ENGINE_REFLECT_KEEP_CAPTURE set, the two pictures are left in the scratch directory.
    if (!test::detail::environment("ENGINE_REFLECT_KEEP_CAPTURE").empty()) {
      CaptureFiles files;
      std::string kept;
      write_capture(tmp.path(), "street-off", off, files, kept);
      write_capture(tmp.path(), "street-on", on, files, kept);
      tmp.keep();
    }
    const u32 pixels = on.width * on.height;
    u32 glass = 0;
    u32 red_in_glass = 0;
    u32 red_in_glass_off = 0;
    u32 changed = 0;
    u32 changed_elsewhere = 0;
    u32 changed_rough = 0;
    u32 ids_moved = 0;
    for (u32 p = 0; p < pixels; ++p) {
      for (u32 w = 0; w < k_id_words; ++w) {
        if (on.ids[u64{p} * k_id_words + w] != off.ids[u64{p} * k_id_words + w]) {
          ++ids_moved;
          break;
        }
      }
      const i32 mesh = street.mesh_at(on, p);
      const u8* a = &on.color[u64{p} * 4];
      const u8* b = &off.color[u64{p} * 4];
      const bool moved = a[0] != b[0] || a[1] != b[1] || a[2] != b[2];
      if (moved) {
        ++changed;
        if (!reflective(mesh)) ++changed_elsewhere;
        if (mesh == static_cast<i32>(k_rough)) ++changed_rough;
      }
      if (mesh == static_cast<i32>(k_glass)) {
        ++glass;
        // The box's red: red over green by six codes more than the same pixel without the ray,
        // and red over green by eight codes in the picture without it (which no pixel of glass is).
        if ((a[0] - a[1]) - (b[0] - b[1]) >= 6) ++red_in_glass;
        if (b[0] - b[1] >= 8) ++red_in_glass_off;
      }
    }
    const u32 rays = street.renderer.stats().surface_reflect_rays;
    MESSAGE(tier.name << ": glass pixels " << glass << ", red in them " << red_in_glass
                      << " (off: " << red_in_glass_off << "), changed " << changed << ", rays "
                      << rays << ", casters " << street.renderer.stats().shadow_casters);
    // The box stands outside the view and is in the panes.
    CHECK(red_in_glass > 100);
    CHECK(red_in_glass_off == 0);
    // Every pixel that changes is a surface under the threshold; the rough stone keeps its ambient.
    CHECK(changed > 0);
    CHECK(changed_elsewhere == 0);
    CHECK(changed_rough == 0);
    CHECK(ids_moved == 0);
    // The rays are counted: at least one a glass pixel (the glass's share is over the floor at
    // every angle), at most one a pixel.
    CHECK(rays >= glass);
    CHECK(rays <= pixels);
    // The same frame twice is the same bytes.
    CHECK(again.color == on.color);
  }
}

TEST_CASE("surface reflect: the eye's shell holds what no view holds") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const RestoreReflect restore;
  const test::TempDir tmp("renderer_surface_shell");
  REQUIRE(tmp.ok());
  // Without the lights' off-frustum volumes, which would hold the box as a caster where it stands
  // in the sun's sweep: what the shell adds is then alone in the difference.
  RenderSettings settings;
  settings.off_frustum_casters = false;
  for (const Tier& tier : tiers(settings)) {
    CAPTURE(tier.name);
    Street street;
    if (!street.build(gpu.device, tmp.file("ddc"), tier.settings)) {
      REQUIRE_MESSAGE(street.error == "unavailable", street.error);
      MESSAGE(tier.name << ": not on this device");
      continue;
    }
    if (!street.traced) {
      MESSAGE("skipped: this device does not trace");
      continue;
    }
    // The picture without the ray, then with it and the shell at 0 and at 32 m; and the diagnostic
    // (each traced pixel red where its ray met nothing, green where it met) at both.
    REQUIRE(tunables::apply_overrides("renderer.reflect.surfaces=0"));
    CapturedFrame off;
    REQUIRE_MESSAGE(street.shoot(off), street.error);
    struct Count {
      u32 red = 0;  // pane pixels the box's red reached: red over green by ten codes more than off
      u32 met = 0;
      u32 missed = 0;
      u32 casters = 0;
      u32 views = 0;
    };
    const auto count = [&](const char* shell) {
      Count c;
      REQUIRE(tunables::apply_overrides(shell));
      CapturedFrame shot;
      REQUIRE(tunables::apply_overrides("renderer.reflect.surfaces=1"));
      REQUIRE_MESSAGE(street.shoot(shot), street.error);
      REQUIRE_MESSAGE(street.shoot(shot), street.error);
      for (u32 p = 0; p < shot.width * shot.height; ++p) {
        if (street.mesh_at(shot, p) != static_cast<i32>(k_glass)) continue;
        const u8* a = &shot.color[u64{p} * 4];
        const u8* b = &off.color[u64{p} * 4];
        if ((a[0] - a[1]) - (b[0] - b[1]) >= 10) ++c.red;
      }
      c.casters = street.renderer.stats().shadow_casters;
      c.views = street.renderer.stats().surface_reflect_views;
      REQUIRE(tunables::apply_overrides("renderer.reflect.surfaces=3"));
      REQUIRE_MESSAGE(street.shoot(shot), street.error);
      for (u32 p = 0; p < shot.width * shot.height; ++p) {
        if (street.mesh_at(shot, p) != static_cast<i32>(k_glass)) continue;
        const u8* a = &shot.color[u64{p} * 4];
        if (a[1] == 255 && a[0] < 200) ++c.met;
        if (a[0] == 255 && a[1] < 200) ++c.missed;
      }
      return c;
    };
    const Count without = count("renderer.reflect.shell_m=0");
    const Count with = count("renderer.reflect.shell_m=32");
    MESSAGE(tier.name << ": panes reddened by the box " << without.red << " without the shell, "
                      << with.red << " with it; rays met " << without.met << " / missed "
                      << without.missed << " without, " << with.met << " / " << with.missed
                      << " with; casters " << without.casters << " / " << with.casters);
    CHECK(without.views == 0);
    CHECK(with.views == 1);
    // The box stands behind the eye, outside every view and every light's volume: without the
    // shell the rays that would meet it meet the west panes behind it (so the diagnostic's counts
    // of rays that met something do not move: the frame's cut holds the west panes, whose bounds
    // cross the frustum), with it they meet the box.
    CHECK(without.red == 0);
    CHECK(with.red > 300);
    CHECK(with.casters > without.casters);
    CHECK(with.met >= without.met);
  }
}
TEST_CASE("surface reflect: the mirror pixels agree with the reference path tracer") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const RestoreReflect restore;
  const test::TempDir tmp("renderer_surface_reference");
  REQUIRE(tmp.ok());
  RenderSettings settings;
  settings.shadows = ShadowMode::RayTraced;
  Street street;
  if (!street.build(gpu.device, tmp.file("ddc"), settings)) {
    MESSAGE("skipped: " << street.error);
    return;
  }
  std::string error;
  if (!reference_available(street.resolved, gpu.device, &error)) {
    MESSAGE("skipped: " << error);
    return;
  }
  REQUIRE_MESSAGE(street.reference.create(gpu.device, street.scene, street.renderer, {}, &error),
                  error);
  // The panes without the ray too, to say what the ray bought.
  CapturedFrame ambient_only;
  REQUIRE(tunables::apply_overrides("renderer.reflect.surfaces=0"));
  REQUIRE_MESSAGE(street.shoot(ambient_only), street.error);
  REQUIRE(tunables::apply_overrides("renderer.reflect.surfaces=1"));
  CapturedFrame shot;
  REQUIRE_MESSAGE(street.shoot(shot), street.error);
  // Two bounces: the pane, then what it reflects lit by the sun and once by the sky, which is what
  // the reflection's hit is shaded with (`shade_hit`). No jitter, as the agreement case has it.
  ReferenceSettings rs;
  rs.spp = 256;
  rs.batch = 32;
  rs.max_bounces = 2;
  rs.pixel_center = true;
  rs.sky_time_s = k_morning_s;
  ReferenceFrame reference;
  REQUIRE_MESSAGE(street.reference.render(street.camera, rs, reference, &error), error);
  REQUIRE(reference.width == shot.width);
  // The mean code value over the panes' pixels, each channel, the two pictures against each other.
  f64 sum_a[3] = {0, 0, 0};
  f64 sum_b[3] = {0, 0, 0};
  f64 abs_diff = 0.0;
  f64 abs_diff_off = 0.0;
  u32 n = 0;
  for (u32 p = 0; p < shot.width * shot.height; ++p) {
    if (street.mesh_at(shot, p) != static_cast<i32>(k_glass)) continue;
    for (u32 c = 0; c < 3; ++c) {
      const f64 a = shot.color[u64{p} * 4 + c];
      const f64 b = reference.color[u64{p} * 4 + c];
      sum_a[c] += a;
      sum_b[c] += b;
      abs_diff += std::fabs(a - b) / 3.0;
      abs_diff_off += std::fabs(static_cast<f64>(ambient_only.color[u64{p} * 4 + c]) - b) / 3.0;
    }
    ++n;
  }
  REQUIRE(n > 0);
  MESSAGE("panes: " << n << " pixels, mean codes resolve (" << sum_a[0] / n << ", " << sum_a[1] / n
                    << ", " << sum_a[2] / n << ") reference (" << sum_b[0] / n << ", "
                    << sum_b[1] / n << ", " << sum_b[2] / n << "), mean |difference| "
                    << abs_diff / n << " (without the ray " << abs_diff_off / n << ")");
  // The bounds are the measurement with room for driver noise and another card, not targets: on
  // the RTX 5090 (2026-11-26, the panes in their own shade at 10:00) the channel means were 4.8,
  // 5.9 and 6.8 codes apart, the resolve the brighter, and the mean difference 5.9 codes against
  // 10.2 without the ray. What is left is the hit's shading, not the ray: `shade_hit` lights what
  // the ray met with the sky's whole harmonics where the cache is off (as here), where the
  // reference sees the street's walls take part of that sky and adds the street's bounce; and the
  // 256 samples' noise. The ray itself is the reference's lobe at 0.06, well under a pixel wide.
  for (u32 c = 0; c < 3; ++c)
    CHECK(std::fabs(sum_a[c] - sum_b[c]) / n < 9.0);
  CHECK(abs_diff / n < 8.0);
  // And the ray brings the panes nearer the reference than the ambient did.
  CHECK(abs_diff < abs_diff_off);
}

namespace {

// **A street whose panes are smooth by their map** (R71's mend, finding 1): the fixture's scene
// and cubes copied into the case's scratch directory, the honed stone's slot given the rough
// stone's cube (so the panes are the scene's only candidate), and the glass written again as an
// imported curtain wall is authored: a roughness factor of 1 and a metallic-roughness map, with an
// optional normal map and occlusion map, each a 64 x 64 PNG the case writes. No binary asset is
// committed: the glTF is text and the images are made here.
struct PaneMaps {
  // The map's G (roughness) at texel (x, y); B (metallic) is 0, R (the occlusion map's, when it
  // is one) is `occlusion`.
  u8 (*roughness)(u32 x, u32 y) = nullptr;
  u8 occlusion = 255;
  bool occlusion_map = false;  // the same image as the occlusion map (its R)
  bool normal_map = false;     // a steep, slow tilt along u (`steep_normal`)
};

// Tilts the normal by up to 70 degrees about the v axis, once over the image: slow enough that the
// normal map's variance chain widens no roughness past the fade, steep enough that the mirror of
// the view dips under the pane's plane over part of it at the camera path's pose.
void steep_normal(u32 x, u8* rgba) {
  const f32 u = (static_cast<f32>(x) + 0.5f) / 64.0f;
  const f32 theta = 1.2217305f * std::sin(6.2831853f * u);
  rgba[0] = static_cast<u8>(std::lround((std::sin(theta) * 0.5f + 0.5f) * 255.0f));
  rgba[1] = 128;
  rgba[2] = static_cast<u8>(std::lround((std::cos(theta) * 0.5f + 0.5f) * 255.0f));
  rgba[3] = 255;
}

bool write_png64(const std::string& path, const Vector<u8>& rgba) {
  return image::write_png(path, 64, 64, 4, std::span<const u8>(rgba.data(), rgba.size())) ==
         io::Status::Ok;
}

// Writes the street into `dir` and returns its scene file, or "" with `error` set.
std::string write_textured_street(const std::string& dir, const PaneMaps& maps,
                                  std::string& error) {
  const auto copy = [&](const char* name, const char* as) {
    std::string bytes;
    if (io::read_file(fixture_file(name), bytes) != io::Status::Ok) return false;
    return io::write_file(dir + "/" + as, bytes) == io::Status::Ok;
  };
  if (io::make_directories(dir) != io::Status::Ok || !copy("scene.json", "scene.json") ||
      !copy("rough.gltf", "fade.gltf") || !copy("rough.gltf", "rough.gltf") ||
      !copy("red.gltf", "red.gltf") || !copy("pavement.gltf", "pavement.gltf") ||
      !copy("camera-path.json", "camera-path.json")) {
    error = "the fixture could not be copied into " + dir;
    return {};
  }
  std::string glass;
  if (io::read_file(fixture_file("glass.gltf"), glass) != io::Status::Ok) {
    error = "no glass.gltf";
    return {};
  }
  const size_t from = glass.find("\"materials\"");
  const size_t to = glass.find("\"buffers\"");
  if (from == std::string::npos || to == std::string::npos || to < from) {
    error = "glass.gltf has no materials block before its buffers";
    return {};
  }
  std::string material =
      "\"materials\": [{\"name\": \"curtain wall by its map\", \"pbrMetallicRoughness\": "
      "{\"baseColorFactor\": [0.04, 0.05, 0.06, 1], \"metallicFactor\": 1, \"roughnessFactor\": 1, "
      "\"metallicRoughnessTexture\": {\"index\": 0}}";
  if (maps.occlusion_map) material += ", \"occlusionTexture\": {\"index\": 0}";
  if (maps.normal_map) material += ", \"normalTexture\": {\"index\": 1}";
  material += "}],\n  \"textures\": [{\"source\": 0}";
  if (maps.normal_map) material += ", {\"source\": 1}";
  material += "],\n  \"images\": [{\"uri\": \"mr.png\"}";
  if (maps.normal_map) material += ", {\"uri\": \"normal.png\"}";
  material += "],\n  ";
  glass = glass.substr(0, from) + material + glass.substr(to);
  if (io::write_file(dir + "/glass.gltf", glass) != io::Status::Ok) {
    error = "glass.gltf not written";
    return {};
  }
  Vector<u8> mr(64 * 64 * 4);
  Vector<u8> normal(64 * 64 * 4);
  for (u32 y = 0; y < 64; ++y) {
    for (u32 x = 0; x < 64; ++x) {
      u8* t = &mr[(y * 64 + x) * 4];
      t[0] = maps.occlusion;
      t[1] = maps.roughness(x, y);
      t[2] = 0;
      t[3] = 255;
      steep_normal(x, &normal[(y * 64 + x) * 4]);
    }
  }
  if (!write_png64(dir + "/mr.png", mr) ||
      (maps.normal_map && !write_png64(dir + "/normal.png", normal))) {
    error = "a map's PNG not written";
    return {};
  }
  return dir + "/scene.json";
}

u8 smooth_map(u32, u32) { return 15; }                                 // 0.0588: curtain-wall glass
u8 rough_map(u32, u32) { return 204; }                                 // 0.8: a rough wall
u8 mullion_map(u32 x, u32) { return (x / 8u) % 2u == 0u ? 15 : 204; }  // panes between mullions
// 0.10 to 0.30 along u: through the fade's end, and the floor on the reflected share just before
// it.
u8 ramp_map(u32 x, u32) {
  return static_cast<u8>(
      std::lround((0.10f + 0.20f * (static_cast<f32>(x) + 0.5f) / 64.0f) * 255.0f));
}

// The street from `scene` on each tier: `body(street, tier)` once a tier that builds.
template <typename Body>
void each_tier(const Gpu& gpu, const test::TempDir& tmp, const std::string& scene, Body&& body) {
  for (const Tier& tier : tiers()) {
    CAPTURE(tier.name);
    Street street;
    if (!street.build(gpu.device, tmp.file("ddc"), tier.settings, scene)) {
      REQUIRE_MESSAGE(street.error == "unavailable", street.error);
      MESSAGE(tier.name << ": not on this device");
      continue;
    }
    body(street, tier);
  }
}

// A pixel of the diagnostic (`renderer.reflect.surfaces=3`): a traced pixel's share is driven past
// any exposure, red where its ray met nothing and green where it met.
bool diagnosed(const u8* a) { return (a[0] == 255 && a[1] < 200) || (a[1] == 255 && a[0] < 200); }

}  // namespace

TEST_CASE("surface reflect: a pane smooth by its map, under a factor of 1, reflects the box") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const RestoreReflect restore;
  const test::TempDir tmp("renderer_surface_map");
  REQUIRE(tmp.ok());
  // The map at 0.06 alone, and again as the occlusion map at 0 (finding 5): the occlusion darkens
  // what the ambient brings, not what the ray saw, so the box is in the panes either way.
  for (const bool occluded : {false, true}) {
    CAPTURE(occluded);
    PaneMaps maps;
    maps.roughness = smooth_map;
    maps.occlusion_map = occluded;
    maps.occlusion = occluded ? 0 : 255;
    std::string error;
    const std::string scene =
        write_textured_street(tmp.file(occluded ? "occluded" : "smooth"), maps, error);
    REQUIRE_MESSAGE(!scene.empty(), error);
    each_tier(gpu, tmp, scene, [&](Street& street, const Tier& tier) {
      CHECK(street.scene.has_reflective_materials());
      CapturedFrame off;
      CapturedFrame on;
      REQUIRE(tunables::apply_overrides("renderer.reflect.surfaces=0"));
      REQUIRE_MESSAGE(street.shoot(off), street.error);
      REQUIRE(tunables::apply_overrides("renderer.reflect.surfaces=1"));
      REQUIRE_MESSAGE(street.shoot(on), street.error);
      if (!street.traced) {
        CHECK(on.color == off.color);
        CHECK(street.renderer.stats().surface_reflect_rays == 0);
        return;
      }
      u32 glass = 0;
      u32 red_in_glass = 0;
      for (u32 p = 0; p < on.width * on.height; ++p) {
        if (street.mesh_at(on, p) != static_cast<i32>(k_glass)) continue;
        ++glass;
        const u8* a = &on.color[u64{p} * 4];
        const u8* b = &off.color[u64{p} * 4];
        if ((a[0] - a[1]) - (b[0] - b[1]) >= 6) ++red_in_glass;
      }
      const u32 rays = street.renderer.stats().surface_reflect_rays;
      MESSAGE(tier.name << std::string(occluded ? ", occluded" : "") << ": glass pixels " << glass
                        << ", red in them " << red_in_glass << ", rays " << rays);
      CHECK(red_in_glass > 100);
      CHECK(rays >= glass);
    });
  }
}

TEST_CASE(
    "surface reflect: a rough map under a factor of 1 leaves the scene on the plain resolve") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const RestoreReflect restore;
  const test::TempDir tmp("renderer_surface_rough_map");
  REQUIRE(tmp.ok());
  PaneMaps maps;
  maps.roughness = rough_map;
  std::string error;
  const std::string scene = write_textured_street(tmp.file("rough"), maps, error);
  REQUIRE_MESSAGE(!scene.empty(), error);
  each_tier(gpu, tmp, scene, [&](Street& street, const Tier& tier) {
    // The variant's flag: no material may reflect, so the resolve's family is made with
    // `k_scene_reflect` unset (the plain resolve) and no frame traces or carries the shell.
    CHECK_FALSE(street.scene.has_reflective_materials());
    CapturedFrame shot;
    REQUIRE_MESSAGE(street.shoot(shot), street.error);
    REQUIRE_MESSAGE(street.shoot(shot), street.error);
    MESSAGE(tier.name << ": rays " << street.renderer.stats().surface_reflect_rays);
    CHECK_FALSE(street.renderer.stats().surface_reflect);
    CHECK(street.renderer.stats().surface_reflect_rays == 0);
    CHECK(street.renderer.stats().surface_reflect_views == 0);
  });
}

TEST_CASE("surface reflect: a map of panes between rough mullions traces the panes' pixels only") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const RestoreReflect restore;
  const test::TempDir tmp("renderer_surface_mullions");
  REQUIRE(tmp.ok());
  PaneMaps maps;
  maps.roughness = mullion_map;
  std::string error;
  const std::string scene = write_textured_street(tmp.file("mullions"), maps, error);
  REQUIRE_MESSAGE(!scene.empty(), error);
  each_tier(gpu, tmp, scene, [&](Street& street, const Tier& tier) {
    // The map's least at its 32 x 32 level is the panes' 0.06, so the material flags.
    CHECK(street.scene.has_reflective_materials());
    if (!street.traced) return;
    REQUIRE(tunables::apply_overrides("renderer.reflect.surfaces=3"));
    CapturedFrame shot;
    REQUIRE_MESSAGE(street.shoot(shot), street.error);
    u32 glass = 0;
    u32 traced = 0;
    u32 traced_elsewhere = 0;
    for (u32 p = 0; p < shot.width * shot.height; ++p) {
      const bool is_glass = street.mesh_at(shot, p) == static_cast<i32>(k_glass);
      const bool lit = diagnosed(&shot.color[u64{p} * 4]);
      glass += is_glass ? 1u : 0u;
      traced += is_glass && lit ? 1u : 0u;
      traced_elsewhere += !is_glass && lit ? 1u : 0u;
    }
    const u32 rays = street.renderer.stats().surface_reflect_rays;
    MESSAGE(tier.name << ": glass pixels " << glass << ", traced " << traced << ", rays " << rays);
    // Per pixel: the panes' pixels trace and the mullions' do not (half the map each; the
    // filtered edge between them is the fade's), and the rays counted are the traced pixels.
    CHECK(traced > glass / 4);
    CHECK(traced < glass * 3 / 4);
    CHECK(traced_elsewhere == 0);
    CHECK(rays == traced);
  });
}

TEST_CASE(
    "surface reflect: no step where a mirror dips under its plane or the share meets its floor") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const RestoreReflect restore;
  const test::TempDir tmp("renderer_surface_edges");
  REQUIRE(tmp.ok());
  // A steep normal map on a smooth pane (finding 4, the first edge): a mirror direction under the
  // geometric plane is brought up to graze it, so every pixel of the pane traces — before the mend
  // the pixels whose mirror dipped dropped to the ambient, a speckle at grazing angles.
  {
    PaneMaps maps;
    maps.roughness = smooth_map;
    maps.normal_map = true;
    std::string error;
    const std::string scene = write_textured_street(tmp.file("steep"), maps, error);
    REQUIRE_MESSAGE(!scene.empty(), error);
    each_tier(gpu, tmp, scene, [&](Street& street, const Tier& tier) {
      if (!street.traced) return;
      REQUIRE(tunables::apply_overrides("renderer.reflect.surfaces=3"));
      CapturedFrame shot;
      REQUIRE_MESSAGE(street.shoot(shot), street.error);
      u32 glass = 0;
      u32 traced = 0;
      for (u32 p = 0; p < shot.width * shot.height; ++p) {
        if (street.mesh_at(shot, p) != static_cast<i32>(k_glass)) continue;
        ++glass;
        traced += diagnosed(&shot.color[u64{p} * 4]) ? 1u : 0u;
      }
      MESSAGE(tier.name << ", steep normals: glass pixels " << glass << ", traced " << traced
                        << ", rays " << street.renderer.stats().surface_reflect_rays);
      CHECK(glass > 0);
      CHECK(traced == glass);
      CHECK(street.renderer.stats().surface_reflect_rays == glass);
    });
  }
  // A ramp of roughness through the fade's end (the second edge): the floor on the reflected share
  // fades the ray's weight in over the floor instead of switching it, so a pair of neighbouring
  // pixels of the pane, one traced and one not, differ by at most 2 codes a channel.
  {
    PaneMaps maps;
    maps.roughness = ramp_map;
    std::string error;
    const std::string scene = write_textured_street(tmp.file("ramp"), maps, error);
    REQUIRE_MESSAGE(!scene.empty(), error);
    each_tier(gpu, tmp, scene, [&](Street& street, const Tier& tier) {
      if (!street.traced) return;
      REQUIRE(tunables::apply_overrides("renderer.reflect.surfaces=3"));
      CapturedFrame diagnostic;
      REQUIRE_MESSAGE(street.shoot(diagnostic), street.error);
      REQUIRE(tunables::apply_overrides("renderer.reflect.surfaces=1"));
      CapturedFrame shot;
      REQUIRE_MESSAGE(street.shoot(shot), street.error);
      const u32 w = shot.width;
      const u32 n = w * shot.height;
      u32 pairs = 0;
      i32 worst = 0;
      for (u32 p = 0; p < n; ++p) {
        if (street.mesh_at(shot, p) != static_cast<i32>(k_glass)) continue;
        const bool a = diagnosed(&diagnostic.color[u64{p} * 4]);
        for (const u32 q : {p + 1, p + w}) {
          if (q >= n || (q == p + 1 && q % w == 0)) continue;
          if (street.mesh_at(shot, q) != static_cast<i32>(k_glass)) continue;
          if (diagnosed(&diagnostic.color[u64{q} * 4]) == a) continue;
          ++pairs;
          for (u32 c = 0; c < 3; ++c) {
            const i32 d =
                std::abs(i32{shot.color[u64{p} * 4 + c]} - i32{shot.color[u64{q} * 4 + c]});
            worst = d > worst ? d : worst;
          }
        }
      }
      MESSAGE(tier.name << ", ramp: pairs across the threshold " << pairs << ", worst step "
                        << worst << " codes");
      CHECK(pairs > 0);
      CHECK(worst <= 2);
    });
  }
}
