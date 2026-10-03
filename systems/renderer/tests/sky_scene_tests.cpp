// The sky in the renderer (sky.h; docs/subsystems/renderer.md, "The sky"), end to end: a scene that
// names a sky is read, its provider found through the registry and asked to check the entry, and a
// scene that names none has none; the frame's lights are the sky's sun and a moon that is a
// directional light of the array; the sky every uncovered pixel of the resolve shows — the table,
// the sun's disc and its limb darkening, the moon's lit disc, the stars — is the CPU mirror's
// (domain/gfx/tests/sky_reference.h) at the block the frame was drawn with, through the display's
// shoulder and transfer curve, at noon-ish and on a moonlit night; the reference path tracer's
// primary misses are the same bytes; the sky through a pixel is a function of the camera's rotation
// and not of where it stands, so 50 km from the origin a move across the ground changes no byte of
// it (gfx.md, "The sky", the fifth lesson); a scene without a sky draws the stand-in's clear and
// hands the resolve no sky block; and the sky stands at the world's one clock, the ground's time
// plus the frame's offset.
//
// Compiled where the sky capability is (its "earth" provider); each GPU case skips with a message
// where there is no device, and the reference's half where the device cannot trace.
#include "sky_reference.h"

#include <domain/gfx/device.h>
#include <domain/gfx/sky.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/scene_gen/sky.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/lighting.h>
#include <systems/renderer/reference.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/sky.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::renderer;
namespace sref = engine::sky_ref;

namespace {

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

std::string scene_with(const std::string& sky) {
  return R"({"format":"engine.scene.v1","name":"sky",)"
         R"("meshes":[{"name":"marker","path":"marker.glb"}],)"
         R"("instances":[{"mesh":0,"translation":[0,0,0]}])" +
         sky + "}";
}

// The erg's calendar: a spring day at the latitude of the Sahara's north, clean air.
scene::Sky erg_sky() {
  scene::Sky entry;
  entry.latitude_deg = 31.1f;
  entry.day_of_year = 100.0f;
  entry.moon_age_days = 14.0f;
  entry.turbidity = 1.6f;
  entry.ground_albedo = 0.38f;
  return entry;
}

struct Gpu {
  gfx::Device device;
  std::string why;
  bool ok = false;
  bool trace = false;  // the reference can run: cluster acceleration structures and ray queries
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
    trace = device.features().cluster_acceleration_structure && device.features().ray_query;
  }
};

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  ReferenceRenderer reference;
  std::string error;

  bool build(const gfx::Device& device, const SceneDesc& desc, const RenderSettings& settings,
             u32 width, u32 height, bool with_reference) {
    if (!load_scene(desc, data, error)) return false;
    resolve_settings(settings, device.features(), &data, resolved);
    const RenderAvailability availability = check_availability(resolved, device.features());
    if (availability != RenderAvailability::Ok) {
      error = unavailable_reason(availability, device);
      return false;
    }
    if (with_reference && !reference_available(resolved, device, &error)) return false;
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = width;
    rd.height = height;
    if (!renderer.create(device, scene, resolved, rd, &error)) return false;
    return !with_reference || reference.create(device, scene, renderer, {}, &error);
  }
};

// The built-in heightfield (a 20 m square of low hills at the origin) under the scene's sky.
SceneDesc hills(const std::optional<scene::Sky>& sky) {
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.heightfield_grid = 65;
  desc.cache = false;
  desc.sky = sky;
  return desc;
}

// The earth provider on its own, for the test to know where the lights are before it aims.
scene_gen::SkyProvider make_earth(const scene::Sky& entry) {
  scene_gen::SkyProvider provider;
  const scene_gen::SkyProviderDesc* desc = scene_gen::GeneratorRegistry::global().find_sky("earth");
  REQUIRE(desc != nullptr);
  std::string error;
  REQUIRE_MESSAGE(desc->make(entry, provider, &error), error);
  return provider;
}

// A display byte of a radiance at the frame's exposure: the resolve's shoulder, its transfer curve
// and the UNORM target's rounding.
int shown(f64 radiance, f64 exposure, f64 shoulder) {
  const f64 exposed = sref::tone(radiance * exposure, shoulder);
  const f64 v = std::pow(std::max(exposed, 0.0), 1.0 / 2.2);
  return static_cast<int>(std::lround(std::min(v, 1.0) * 255.0));
}

// The angle between two unit vectors, robust near zero.
f64 angle(sref::Dvec3 a, sref::Dvec3 b) {
  const sref::Dvec3 c = brdf_ref::cross(a, b);
  return std::atan2(std::sqrt(brdf_ref::dot(c, c)), brdf_ref::dot(a, b));
}

// One frame's uncovered pixels against the mirror (and, where given, the reference's bytes).
struct SkyCompare {
  u32 pixels = 0;  // uncovered pixels compared
  u32 limb = 0;    // left out: within a pixel and a half of a disc's edge
  int worst = 0;   // the largest difference of any channel, of 255
  u32 over = 0;    // pixels over the tolerance
  f64 mean = 0.0;  // mean absolute difference over channels
  u32 disc = 0;    // pixels inside the sun's or the moon's disc
  u32 stars = 0;   // pixels the stars brighten by at least a byte
  int reference_worst = 0;
  u32 reference_over = 0;
  u32 reference_pixels = 0;
};

constexpr int k_tolerance = 2;  // of 255, on every channel

SkyCompare compare_sky(const gfx::SkyParams& params, const CapturedFrame& shot,
                       const sref::Stars& stars, const ReferenceFrame* reference) {
  const sref::Sky sky = sref::from_params(params);
  sref::Tables tables;
  tables.transmittance = sref::build_transmittance(sky);
  tables.multiscatter = sref::build_multiscatter(sky, tables);
  tables.sky_view = sref::build_sky_view(sky, tables);
  const sref::Frame frame = sref::frame(sky, tables);
  const f64 pixel = static_cast<f64>(params.views[0].pixel.x);
  const Mat4& clip_to_ray = params.views[0].clip_to_ray;
  const sref::Dvec3 sun = brdf_ref::dvec3(Vec3{params.sun.x, params.sun.y, params.sun.z});
  const sref::Dvec3 moon = brdf_ref::dvec3(Vec3{params.moon.x, params.moon.y, params.moon.z});
  SkyCompare out;
  f64 sum = 0.0;
  const auto covered = [&](i64 x, i64 y) {
    if (x < 0 || y < 0 || x >= static_cast<i64>(shot.width) || y >= static_cast<i64>(shot.height))
      return false;
    return shot.depth[static_cast<u32>(y) * shot.width + static_cast<u32>(x)] > 0.0f;
  };
  for (u32 y = 0; y < shot.height; ++y) {
    for (u32 x = 0; x < shot.width; ++x) {
      if (covered(x, y)) continue;
      // The resolve's direction: the pixel centre's ndc through the view's clip-to-ray matrix.
      const f32 nx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(shot.width) * 2.0f - 1.0f;
      const f32 ny = 1.0f - (static_cast<f32>(y) + 0.5f) / static_cast<f32>(shot.height) * 2.0f;
      const sref::Dvec3 dir =
          sref::pixel_direction(clip_to_ray, static_cast<f64>(nx), static_cast<f64>(ny));
      // A pixel whose centre is within a pixel and a half of a disc's edge is left out: float and
      // double can put that direction on either side of the limb, and a disc is thousands of times
      // brighter than the sky beside it.
      const f64 to_sun = angle(dir, sun);
      const f64 to_moon = angle(dir, moon);
      if (std::fabs(to_sun - static_cast<f64>(params.sun.w)) < 1.5 * pixel ||
          std::fabs(to_moon - static_cast<f64>(params.moon.w)) < 1.5 * pixel) {
        ++out.limb;
        continue;
      }
      if (to_sun < static_cast<f64>(params.sun.w) || to_moon < static_cast<f64>(params.moon.w))
        ++out.disc;
      const sref::Dvec3 bg = sref::background(sky, tables, &stars, dir, pixel);
      const sref::Dvec3 plain = sref::background(sky, tables, nullptr, dir, pixel);
      const int want[3] = {shown(bg.x, frame.exposure, sky.shoulder),
                           shown(bg.y, frame.exposure, sky.shoulder),
                           shown(bg.z, frame.exposure, sky.shoulder)};
      if (shown(bg.y, frame.exposure, sky.shoulder) > shown(plain.y, frame.exposure, sky.shoulder))
        ++out.stars;
      const u8* got = shot.color.data() + 4 * (y * shot.width + x);
      int worst = 0;
      for (u32 c = 0; c < 3; ++c) {
        const int e = std::abs(static_cast<int>(got[c]) - want[c]);
        worst = std::max(worst, e);
        sum += static_cast<f64>(e);
      }
      out.worst = std::max(out.worst, worst);
      if (worst > k_tolerance) ++out.over;
      ++out.pixels;
      // The reference's primary miss is the same function, from its own camera: away from the
      // geometry's edge, where a pixel covered in one and not the other is antialiasing's.
      bool edge = false;
      for (i64 dy = -1; dy <= 1; ++dy)
        for (i64 dx = -1; dx <= 1; ++dx)
          edge = edge || covered(x + dx, y + dy);
      if (reference != nullptr && !edge) {
        const u8* ref = reference->color.data() + 4 * (y * shot.width + x);
        int r = 0;
        for (u32 c = 0; c < 3; ++c)
          r = std::max(r, std::abs(static_cast<int>(ref[c]) - static_cast<int>(got[c])));
        out.reference_worst = std::max(out.reference_worst, r);
        if (r > k_tolerance) ++out.reference_over;
        ++out.reference_pixels;
      }
    }
  }
  out.mean = out.pixels > 0 ? sum / (3.0 * out.pixels) : 0.0;
  return out;
}

sref::Stars mirror_stars(const scene_gen::SkyProvider& provider) {
  sref::Stars out;
  const std::span<const scene_gen::Star> stars = provider.stars();
  for (const scene_gen::Star& s : stars) {
    out.direction.push_back(brdf_ref::dvec3(s.direction));
    out.irradiance.push_back(brdf_ref::dvec3(star_irradiance(s)));
  }
  Vector<u32> cells;
  Vector<u32> index;
  build_star_cells(stars, cells, index);
  out.cells.assign(cells.begin(), cells.end());
  out.index.assign(index.begin(), index.end());
  return out;
}

Camera aimed(Vec3 eye, Vec3 towards, f32 fov_deg) {
  Camera camera;
  camera.position = eye;
  camera.target = eye + towards * 100.0f;
  camera.fov_y = fov_deg * 3.14159265f / 180.0f;
  camera.znear = 0.05f;
  return camera;
}

}  // namespace

TEST_CASE("sky scene: a scene's sky is read and checked, and a scene without one has none") {
  const test::TempDir tmp("renderer_sky_scene");
  std::string error;
  const std::string plain = tmp.file("plain.json");
  REQUIRE(write_text(plain, scene_with("")));
  SceneDesc none;
  REQUIRE_MESSAGE(read_scene_file(plain, none, error), error);
  CHECK_FALSE(none.sky.has_value());

  // An empty entry is the default provider with the schema's defaults.
  const std::string empty = tmp.file("empty.json");
  REQUIRE(write_text(empty, scene_with(R"(,"sky":{})")));
  SceneDesc desc;
  REQUIRE_MESSAGE(read_scene_file(empty, desc, error), error);
  REQUIRE(desc.sky.has_value());
  CHECK(scene_gen::sky_provider_name(*desc.sky) == "earth");
  CHECK(desc.sky->latitude_deg == 30.0f);
  CHECK(desc.sky->moon);
  CHECK(desc.sky->stars);
  CHECK(desc.sky->exposure == scene::SkyExposure::Auto);

  const std::string numbers = tmp.file("numbers.json");
  REQUIRE(write_text(numbers, scene_with(R"(,"sky":{"latitude_deg":-33.9,"day_of_year":355,)"
                                         R"("turbidity":4,"moon":false,"exposure":"Fixed",)"
                                         R"("exposure_ev100":12})")));
  REQUIRE_MESSAGE(read_scene_file(numbers, desc, error), error);
  REQUIRE(desc.sky.has_value());
  CHECK(desc.sky->latitude_deg == doctest::Approx(-33.9));
  CHECK(desc.sky->day_of_year == 355.0f);
  CHECK(desc.sky->turbidity == 4.0f);
  CHECK_FALSE(desc.sky->moon);
  CHECK(desc.sky->exposure == scene::SkyExposure::Fixed);
  CHECK(desc.sky->exposure_ev100 == 12.0f);

  // A provider this executable does not carry is refused with the registry's sentence, and an
  // entry the provider cannot draw with the provider's.
  const std::string mars = tmp.file("mars.json");
  REQUIRE(write_text(mars, scene_with(R"(,"sky":{"provider":"mars"})")));
  CHECK_FALSE(read_scene_file(mars, desc, error));
  CHECK(error.find("names the sky provider \"mars\", which this build does not have") !=
        std::string::npos);
  const std::string murky = tmp.file("murky.json");
  REQUIRE(write_text(murky, scene_with(R"(,"sky":{"turbidity":0.5})")));
  CHECK_FALSE(read_scene_file(murky, desc, error));
  CHECK(error.find("sky.turbidity must be within [1, 10]") != std::string::npos);
}

TEST_CASE("sky scene: the frame's lights are the sky's sun and a directional moon") {
  SceneData scene;
  FrameSky sky;
  sky.active = true;
  sky.moon = true;
  sky.state.sun = normalize(Vec3{0.3f, -0.5f, 0.2f});
  sky.state.moon = normalize(Vec3{-0.2f, 0.7f, 0.4f});
  sky.state.moon_illuminance = 1.6e-6f;
  FrameLighting lighting;
  frame_lighting(scene, sky, lighting);
  // The sun's direction, at 1: its light at the eye is the sky's to give, through the air.
  CHECK(length(lighting.sun.xyz() - sky.state.sun) < 1e-6f);
  CHECK(lighting.sun.w == 1.0f);
  // The moon: the array's light 0, directional, at its illuminance outside the air.
  REQUIRE(lighting.light_count == 1);
  const gfx::ResolveLight& moon = lighting.lights[0];
  CHECK(moon.direction_type.w == gfx::k_light_directional);
  CHECK(length(moon.position_radius.xyz() - sky.state.moon) < 1e-6f);
  CHECK(moon.color_intensity.w == 1.6e-6f);
  // No moon, no light; and never the stand-in's orbiting point lights.
  sky.moon = false;
  frame_lighting(scene, sky, lighting);
  CHECK(lighting.light_count == 0);
}

TEST_CASE("sky scene: the resolve draws the mirror's sky, and the reference the resolve's") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const scene::Sky entry = erg_sky();
  const scene_gen::SkyProvider provider = make_earth(entry);
  const sref::Stars stars = mirror_stars(provider);
  REQUIRE(stars.direction.size() > 8000);

  // An afternoon, and the first night hour after it with the moon well up and the sun well down.
  const f64 afternoon = 15.5 * 3600.0;
  f64 night = 0.0;
  scene_gen::SkyState state;
  for (f64 t = 19.0 * 3600.0; t < 30.0 * 3600.0; t += 900.0) {
    provider.state(t, state);
    if (state.sun.y < -0.35f && state.moon.y > 0.4f) {
      night = t;
      break;
    }
  }
  REQUIRE_MESSAGE(night > 0.0, "the moon is never well up at night on the erg's calendar");

  RenderSettings settings;
  settings.shadows = gpu.trace ? ShadowMode::RayTraced : ShadowMode::Off;
  constexpr u32 k_width = 192;
  constexpr u32 k_height = 108;
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, hills(entry), settings, k_width, k_height, gpu.trace),
                  rig.error);
  REQUIRE(rig.renderer.sky().active());
  if (!gpu.trace) MESSAGE("the reference's half skipped: the device cannot trace");

  struct Shot {
    const char* name;
    f64 time_s;
    bool at_sun;  // aim at the sun, or at the moon
    f32 fov_deg;
    f32 pitch;  // radians added to the aim, so the frame is not all disc
  };
  const Shot shots[] = {
      {"the afternoon sun", afternoon, true, 12.0f, 0.03f},
      {"the afternoon's horizon", afternoon, true, 60.0f, -1.0f},
      {"the moonlit night", night, false, 12.0f, 0.02f},
      {"the night's horizon", night, false, 60.0f, -1.0f},
  };
  const Vec3 eye{0.0f, 3.0f, 0.0f};
  for (const Shot& s : shots) {
    const std::string name = s.name;
    CAPTURE(name);
    provider.state(s.time_s, state);
    const Vec3 light = s.at_sun ? state.sun : state.moon;
    Vec3 towards = light;
    if (s.pitch < 0.0f) {
      // Towards the light's azimuth, ten degrees up: the hills, the planet's ground past them, and
      // the sky over the horizon.
      towards = normalize(Vec3{light.x, 0.0f, light.z});
      towards = normalize(towards + Vec3{0.0f, 0.18f, 0.0f});
    } else {
      towards = normalize(light + Vec3{0.0f, s.pitch, 0.0f});
    }
    const Camera camera = aimed(eye, towards, s.fov_deg);
    FrameDesc frame;
    frame.camera = camera;
    frame.sun_time_s = s.time_s;
    CaptureChannels channels;
    channels.depth = true;
    CapturedFrame shot;
    REQUIRE_MESSAGE(rig.renderer.render_offscreen(frame, &rig.error), rig.error);
    REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot, &rig.error), rig.error);
    // The block the frame was drawn with, rebuilt from the same sky, views, camera and request.
    const FrameSky& frame_sky = rig.renderer.frame_sky();
    REQUIRE(frame_sky.active);
    CHECK(rig.renderer.sky_params_address() != 0);
    // With no terrain the ground's clock is at zero: the sky stands at the frame's offset alone.
    CHECK(frame_sky.time_s == s.time_s);
    CHECK(frame_sky.moon_key == (s.time_s == night));
    gfx::SkyParams params;
    rig.renderer.sky().fill(frame_sky, rig.renderer.views(), camera, ExposureRequest{}, params);
    ReferenceFrame reference;
    if (gpu.trace) {
      ReferenceSettings rs;
      rs.spp = 1;
      rs.batch = 1;
      rs.max_bounces = 1;
      rs.pixel_center = true;
      rs.sun_time_s = s.time_s;
      REQUIRE_MESSAGE(rig.reference.render(camera, rs, reference, &rig.error), rig.error);
    }
    const SkyCompare c = compare_sky(params, shot, stars, gpu.trace ? &reference : nullptr);
    MESSAGE(name << ": " << c.pixels << " sky pixels against the mirror, worst " << c.worst
                 << " of 255, mean " << c.mean << ", " << c.over << " over " << k_tolerance << "; "
                 << c.limb << " at a disc's limb left out, " << c.disc << " inside a disc, "
                 << c.stars << " lit by a star; the reference: " << c.reference_pixels
                 << " pixels, worst " << c.reference_worst << ", " << c.reference_over << " over");
    CHECK(c.pixels > k_width * k_height / 3);
    CHECK(c.over == 0);
    CHECK(c.reference_over == 0);
    if (s.pitch > 0.0f) CHECK(c.disc > 0);                        // the disc is drawn, not lost
    if (s.time_s == night && s.pitch > 0.0f) CHECK(c.stars > 0);  // and the stars round the moon
  }
}

namespace {

// A camera looking along `towards`, its target 100 m out with the offset rounded to 1/64 m: every
// eye this test uses and every eye plus that offset is a float exactly, so `target - position` is
// the same bits for every eye and the cameras below differ in where they are and in nothing else.
Camera placed(Vec3 eye, Vec3 towards, f32 fov_deg) {
  const auto q = [](f32 v) { return std::round(v * 100.0f * 64.0f) / 64.0f; };
  Camera camera;
  camera.position = eye;
  camera.target = eye + Vec3{q(towards.x), q(towards.y), q(towards.z)};
  camera.fov_y = fov_deg * 3.14159265f / 180.0f;
  camera.znear = 0.05f;
  return camera;
}

struct SkyShot {
  CapturedFrame shot;
  gfx::SkyParams params;
};

SkyShot sky_shot(Rig& rig, const Camera& camera, f64 time_s) {
  FrameDesc frame;
  frame.camera = camera;
  frame.sun_time_s = time_s;
  CaptureChannels channels;
  channels.depth = true;
  SkyShot out;
  REQUIRE_MESSAGE(rig.renderer.render_offscreen(frame, &rig.error), rig.error);
  REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, out.shot, &rig.error), rig.error);
  rig.renderer.sky().fill(rig.renderer.frame_sky(), rig.renderer.views(), camera, ExposureRequest{},
                          out.params);
  return out;
}

// Two pictures' pixels that neither covers: how many, how many differ, and by how much at most.
struct SkyDiff {
  u32 pixels = 0;
  u32 differ = 0;
  int worst = 0;
};

SkyDiff diff_sky(const CapturedFrame& a, const CapturedFrame& b) {
  SkyDiff out;
  for (u32 i = 0; i < a.width * a.height; ++i) {
    if (a.depth[i] > 0.0f || b.depth[i] > 0.0f) continue;
    ++out.pixels;
    int worst = 0;
    for (u32 c = 0; c < 3; ++c) {
      worst = std::max(worst, std::abs(static_cast<int>(a.color[4 * i + c]) -
                                       static_cast<int>(b.color[4 * i + c])));
    }
    if (worst > 0) ++out.differ;
    out.worst = std::max(out.worst, worst);
  }
  return out;
}

}  // namespace

TEST_CASE("sky scene: the sky does not move with the camera 50 km from the origin") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const scene::Sky entry = erg_sky();
  const scene_gen::SkyProvider provider = make_earth(entry);
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  constexpr u32 k_width = 192;
  constexpr u32 k_height = 108;
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, hills(entry), settings, k_width, k_height, false),
                  rig.error);
  REQUIRE(rig.renderer.sky().active());

  // The afternoon, looking half way up to the sun: its disc near the top of the frame, the horizon
  // and the planet's ground below it near the bottom — the two sharpest edges the sky has, where a
  // direction that is wrong by a fraction of a degree moves bytes first.
  const f64 afternoon = 15.5 * 3600.0;
  scene_gen::SkyState state;
  provider.state(afternoon, state);
  const Vec3 level = normalize(Vec3{state.sun.x, 0.0f, state.sun.z});
  const Vec3 towards = normalize(normalize(state.sun) + level);
  constexpr f32 k_fov = 60.0f;

  // 50 km out along x, 20 m up: a float there holds 4 mm. A millimetre (2^-10 m) along z, a metre
  // along x, and a metre up, each with the same rotation to the bit.
  const Vec3 far{50000.0f, 20.0f, 0.0f};
  const SkyShot base = sky_shot(rig, placed(far, towards, k_fov), afternoon);
  struct Move {
    const char* name;
    Vec3 by;
    int tolerance;  // of 255
  };
  // A move across the ground changes nothing the sky depends on: identical bytes. A move up
  // changes the eye's altitude, which every table of the sky is a function of (20 m to 21 m above
  // the planet's ground), so a pixel may move by a level there and no further.
  const Move moves[] = {
      {"a millimetre across", Vec3{0.0f, 0.0f, 0.0009765625f}, 0},
      {"a metre across", Vec3{1.0f, 0.0f, 0.0f}, 0},
      {"a metre up", Vec3{0.0f, 1.0f, 0.0f}, 1},
  };
  for (const Move& m : moves) {
    const std::string name = m.name;
    CAPTURE(name);
    const SkyShot moved = sky_shot(rig, placed(far + m.by, towards, k_fov), afternoon);
    const SkyDiff d = diff_sky(base.shot, moved.shot);
    MESSAGE("50 km out, " << name << ": " << d.pixels << " sky pixels, " << d.differ
                          << " differ, worst " << d.worst << " of 255");
    CHECK(d.pixels > k_width * k_height / 2);
    CHECK(d.worst <= m.tolerance);
  }

  // At the origin the picture is the one the old direction drew: a point at depth 0.5 through the
  // inverse of the whole view-projection, less the eye, in float as the shader computed it. There
  // the eye's coordinates are small and that difference was nearly exact, so the two directions
  // agree to a small fraction of a pixel and the mirror's bytes along them to a level. The GPU's
  // picture is held to the mirror along the new direction by the case above, at 2 levels.
  const Camera home = placed(Vec3{0.0f, 20.0f, 0.0f}, towards, k_fov);
  const SkyShot origin = sky_shot(rig, home, afternoon);
  const SkyCompare against_mirror =
      compare_sky(origin.params, origin.shot, mirror_stars(provider), nullptr);
  CHECK(against_mirror.over == 0);
  const sref::Sky sky = sref::from_params(origin.params);
  sref::Tables tables;
  tables.transmittance = sref::build_transmittance(sky);
  tables.multiscatter = sref::build_multiscatter(sky, tables);
  tables.sky_view = sref::build_sky_view(sky, tables);
  const sref::Frame frame = sref::frame(sky, tables);
  const View& view = rig.renderer.views()[0];
  const Mat4 old_inverse = inverse(view.view_proj);
  const f64 pixel = static_cast<f64>(origin.params.views[0].pixel.x);
  const sref::Dvec3 sun = brdf_ref::dvec3(state.sun);
  f64 widest = 0.0;  // radians between the old direction and the new
  int worst = 0;     // of 255, between the mirror's bytes along the two
  u32 compared = 0;
  for (u32 y = 0; y < k_height; ++y) {
    for (u32 x = 0; x < k_width; ++x) {
      if (origin.shot.depth[y * k_width + x] > 0.0f) continue;
      const f32 nx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(k_width) * 2.0f - 1.0f;
      const f32 ny = 1.0f - (static_cast<f32>(y) + 0.5f) / static_cast<f32>(k_height) * 2.0f;
      const Vec4 ahead = old_inverse * Vec4{nx, ny, 0.5f, 1.0f};
      const Vec3 old_f =
          normalize(Vec3{ahead.x / ahead.w, ahead.y / ahead.w, ahead.z / ahead.w} - home.position);
      const sref::Dvec3 was = brdf_ref::dvec3(old_f);
      const sref::Dvec3 now =
          sref::pixel_direction(view.clip_to_ray, static_cast<f64>(nx), static_cast<f64>(ny));
      widest = std::max(widest, angle(was, now));
      // A disc's limb is a step of thousands: a direction a hair to either side of it is a
      // different byte however close the two are, so the limb's pixels are left out as above.
      if (std::fabs(angle(now, sun) - static_cast<f64>(origin.params.sun.w)) < 1.5 * pixel)
        continue;
      const sref::Dvec3 a = sref::background(sky, tables, nullptr, was, pixel);
      const sref::Dvec3 b = sref::background(sky, tables, nullptr, now, pixel);
      const f64 ea[3] = {a.x, a.y, a.z};
      const f64 eb[3] = {b.x, b.y, b.z};
      for (u32 c = 0; c < 3; ++c) {
        worst = std::max(worst, std::abs(shown(ea[c], frame.exposure, sky.shoulder) -
                                         shown(eb[c], frame.exposure, sky.shoulder)));
      }
      ++compared;
    }
  }
  MESSAGE("at the origin: " << compared << " sky pixels, the old direction and the new "
                            << widest / pixel << " of a pixel apart at most, the mirror's bytes "
                            << worst << " of 255 at most");
  CHECK(compared > k_width * k_height / 2);
  CHECK(widest < 0.05 * pixel);
  CHECK(worst <= 1);
}

TEST_CASE("sky scene: a scene without a sky draws the stand-in and hands the resolve no sky") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, hills(std::nullopt), settings, 96, 54, false), rig.error);
  CHECK_FALSE(rig.renderer.sky().active());
  FrameDesc frame;
  frame.camera = aimed(Vec3{0.0f, 3.0f, 0.0f}, normalize(Vec3{1.0f, 0.3f, 0.0f}), 60.0f);
  frame.sun_time_s = 20.0 * 3600.0;  // a day that moves: still the stand-in's sky
  CaptureChannels channels;
  channels.depth = true;
  CapturedFrame shot;
  REQUIRE_MESSAGE(rig.renderer.render_offscreen(frame, &rig.error), rig.error);
  REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot, &rig.error), rig.error);
  CHECK_FALSE(rig.renderer.frame_sky().active);
  CHECK(rig.renderer.sky_params_address() == 0);
  CHECK_FALSE(rig.renderer.stats().sky.active);
  // Every uncovered pixel is the clear: the stand-in's sky colour, written as it always was.
  u32 uncovered = 0;
  u32 other = 0;
  for (u32 i = 0; i < shot.width * shot.height; ++i) {
    if (shot.depth[i] > 0.0f) continue;
    ++uncovered;
    const u8* p = shot.color.data() + 4 * i;
    const f32 want[3] = {k_sky.x, k_sky.y, k_sky.z};
    for (u32 c = 0; c < 3; ++c) {
      if (std::abs(static_cast<f32>(p[c]) - want[c] * 255.0f) > 1.0f) {
        ++other;
        break;
      }
    }
  }
  CHECK(uncovered > 0);
  CHECK(other == 0);
}

#if defined(ENGINE_SKY_TESTS_TERRAIN)
TEST_CASE("sky scene: the sky stands at the ground's time plus the frame's offset") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const test::TempDir tmp("renderer_sky_clock");
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.terrain.enabled = true;
  desc.terrain.size = 65;
  desc.terrain.extent = 64.0f;
  desc.terrain.seed = 5;
  desc.terrain.dune_height = 2.0f;
  desc.terrain.dune_wavelength = 20.0f;
  desc.terrain.generator = TerrainGenerator::dunes;
  desc.terrain.time_s = 94'608'000.0 + 5.25 * 3600.0;  // three years in, at a quarter past five
  desc.ddc = tmp.file("ddc");
  const scene::Sky entry = erg_sky();
  desc.sky = entry;
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, desc, settings, 64, 36, false), rig.error);
  CHECK(rig.scene.ground_time_s() == desc.terrain.time_s);
  const scene_gen::SkyProvider provider = make_earth(entry);
  for (const f64 offset : {0.0, 7.0 * 3600.0, 30.0 * 86'400.0}) {
    FrameDesc frame;
    frame.camera = aimed(Vec3{0.0f, 20.0f, 0.0f}, normalize(Vec3{1.0f, 0.1f, 0.0f}), 60.0f);
    frame.sun_time_s = offset;
    REQUIRE_MESSAGE(rig.renderer.render_offscreen(frame, &rig.error), rig.error);
    const FrameSky& sky = rig.renderer.frame_sky();
    const f64 t = desc.terrain.time_s + offset;
    CHECK(sky.time_s == t);
    // The provider's answer at that time, and the calendar it reads: the scene's day, counted on.
    scene_gen::SkyState want;
    provider.state(t, want);
    CHECK(length(sky.state.sun - want.sun) < 1e-6f);
    CHECK(length(sky.state.moon - want.moon) < 1e-6f);
    CHECK(sky.state.day_of_year ==
          doctest::Approx(static_cast<f64>(entry.day_of_year) + t / 86'400.0).epsilon(1e-9));
    CHECK(rig.renderer.stats().sky.time_s == t);
  }
}
#endif
