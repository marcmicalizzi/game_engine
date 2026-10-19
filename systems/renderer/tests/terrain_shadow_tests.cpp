// The dunes behind the camera cast traced shadows into the view (docs/subsystems/renderer.md,
// "Shadows"; roadmap R84). Every terrain level is a deformed instance — the pool's terrain stage
// moves its heights to the field it draws — and until 2026-10-18 a deformed instance outside the
// frustum was no shadow caster, because the caster run had no pool blocks: so under traced shadows
// a low sun behind the camera threw the shadows of the dunes it stood among nowhere, while the
// cascaded maps, culled from the light, always held them. This is the owner's picture: the sand
// in view, a low sun behind, and the traced shadow on the sand the maps' within the filter's band.
#include <core/jobs/job_system.h>
#include <core/math/math.h>
#include <domain/gfx/device.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/terrain_time.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <memory>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

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

// 64 m of dunes at a metre, 2 m high on a 20 m wavelength, three years in (terrain_motion_tests'
// moving scene): crests a metre or two above the floors between them, every few metres.
SceneDesc dunes(const std::string& ddc) {
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.terrain.enabled = true;
  desc.terrain.size = 65;
  desc.terrain.extent = 32.0f;
  desc.terrain.seed = 5;
  desc.terrain.dune_height = 2.0f;
  desc.terrain.dune_wavelength = 20.0f;
  desc.terrain.generator = TerrainGenerator::dunes;
  desc.terrain.time_s = 94'608'000.0;
  desc.ddc = ddc;
  return desc;
}

constexpr u32 k_width = 320;
constexpr u32 k_height = 200;

// The sun 9 degrees up towards -z (azimuth from +x towards +z), behind a camera that looks along
// +z: across the bands of these dunes, whose crests then throw shadows metres long. Measured
// across the eight compass points on 2026-10-18 (RTX 5090): towards -z the maps shadow 25,227
// pixels of the view, and the rays without the off-frustum casters 3,035.
constexpr f32 k_sun_azimuth = 270.0f;
constexpr f32 k_sun_elevation = 9.0f;

struct Shot {
  CapturedFrame frame;
  u32 casters = 0;
  u32 overflow = 0;
  bool available = true;
  std::string why;
};

Shot shoot(const gfx::Device& device, const SceneDesc& desc, ShadowMode shadows, f32 azimuth,
           bool off_frustum_casters, const Camera& camera, jobs::JobSystem& pool) {
  Shot shot;
  SceneData data;
  std::string error;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  RenderSettings settings;
  settings.shadows = shadows;
  settings.occlusion = false;
  settings.lights = false;
  settings.sun_azimuth_deg = azimuth;
  settings.sun_elevation_deg = k_sun_elevation;
  settings.off_frustum_casters = off_frustum_casters;
  // A moving terrain at a standing rate: the levels, drawn through the pool's terrain stage.
  settings.time_rate_live = true;
  settings.view_mode = static_cast<u32>(gfx::ResolveMode::Shadow);
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  const RenderAvailability availability = check_availability(resolved, device.features());
  if (availability != RenderAvailability::Ok) {
    shot.available = false;
    shot.why = unavailable_reason(availability, device);
    return shot;
  }
  REQUIRE(resolved.terrain_levels);
  REQUIRE(resolved.deform_pass);
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = k_width;
  rd.height = k_height;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
  TerrainMotion motion;
  TimeLapseConfig still;
  still.rate = 0.0;
  still.wait = true;
  REQUIRE_MESSAGE(motion.start(scene, nullptr, still, &pool, &error), error);
  motion.frame(1.0 / 60.0, camera.position);
  FrameDesc frame;
  frame.camera = camera;
  CaptureChannels channels;
  channels.color = true;
  channels.ids = true;
  REQUIRE_MESSAGE(renderer.capture(frame, channels, shot.frame, &error), error);
  shot.casters = renderer.stats().shadow_casters;
  shot.overflow = renderer.stats().deform_overflow_entries;
  return shot;
}

bool sand(const Shot& s, u32 p) { return s.frame.ids[u64{p} * k_id_words] != k_no_id; }
u8 shade(const Shot& s, u32 p) { return s.frame.color[u64{p} * 4]; }

// -1 not sand or neither black nor white (a slope facing away from the sun, or a penumbra), 0 lit,
// 1 shadowed: what a shadow view says about each pixel.
Vector<i8> classes(const Shot& s) {
  Vector<i8> out(k_width * k_height, static_cast<i8>(-1));
  for (u32 p = 0; p < k_width * k_height; ++p) {
    if (!sand(s, p)) continue;
    if (shade(s, p) == 0) out[p] = 1;
    if (shade(s, p) == 255) out[p] = 0;
  }
  return out;
}

bool interior(const Vector<i8>& c, u32 p, i32 radius) {
  const i32 px = static_cast<i32>(p % k_width);
  const i32 py = static_cast<i32>(p / k_width);
  for (i32 dy = -radius; dy <= radius; ++dy) {
    for (i32 dx = -radius; dx <= radius; ++dx) {
      const i32 x = px + dx;
      const i32 y = py + dy;
      if (x < 0 || y < 0 || x >= static_cast<i32>(k_width) || y >= static_cast<i32>(k_height))
        return false;
      if (c[static_cast<u32>(y) * k_width + static_cast<u32>(x)] != c[p]) return false;
    }
  }
  return true;
}

struct Agreement {
  u32 compared = 0;  // interior sand pixels the reference calls lit or shadowed
  u32 shadowed = 0;  // of those, shadowed
  u32 differ = 0;    // of those, the other picture says otherwise
};

// `shot` against the classes of `reference`, away from every edge of them by `radius` pixels.
Agreement agree(const Shot& shot, const Vector<i8>& reference, i32 radius) {
  Agreement a;
  for (u32 p = 0; p < k_width * k_height; ++p) {
    if (reference[p] < 0 || !sand(shot, p) || !interior(reference, p, radius)) continue;
    ++a.compared;
    a.shadowed += reference[p] == 1 ? 1u : 0u;
    const u8 v = shade(shot, p);
    a.differ += (reference[p] == 1 ? v != 0 : v != 255) ? 1u : 0u;
  }
  return a;
}

u32 id_differences(const Shot& a, const Shot& b) {
  u32 out = 0;
  for (u32 w = 0; w < a.frame.ids.size() && w < b.frame.ids.size(); ++w)
    out += a.frame.ids[w] != b.frame.ids[w] ? 1u : 0u;
  return out;
}

// The filter's band, in this picture's pixels: the cascades over 64 m of sand are a few
// centimetres a texel, and a pixel on the near sand is several, so two pixels covers the 4 x 4
// footprint and the two paths' disagreement about which side of an edge a centre falls.
constexpr i32 k_band = 2;

// Two metres over the sand, 8 m from the middle towards the sun, looking away from it and down:
// the dunes between the eye and the sun are outside the frustum and inside the volume swept from
// it towards the sun.
Camera facing_away(const TerrainDesc& terrain, f32 azimuth) {
  const f32 a = azimuth * 3.14159265f / 180.0f;
  const Vec3 towards{std::cos(a), 0.0f, std::sin(a)};
  const Vec3 at = towards * 8.0f;
  const Vec3 look = at - towards * 16.0f;
  Camera camera;
  camera.position =
      absolute(WorldPos::origin(), Vec3{at.x, terrain_height(terrain, at.x, at.z) + 2.0f, at.z});
  camera.target = absolute(WorldPos::origin(),
                           Vec3{look.x, terrain_height(terrain, look.x, look.z) - 1.0f, look.z});
  camera.znear = 0.05f;
  return camera;
}

u32 black(const Shot& s) {
  u32 n = 0;
  for (u32 p = 0; p < k_width * k_height; ++p)
    n += sand(s, p) && shade(s, p) == 0 ? 1u : 0u;
  return n;
}

}  // namespace

TEST_CASE("terrain shadows: the dunes behind the camera cast their traced shadows into the view") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  if (!gpu.device.features().cluster_acceleration_structure || !gpu.device.features().ray_query) {
    MESSAGE(gpu.device.adapter().name << " cannot trace shadows; skipped");
    return;
  }
  test::TempDir tmp{"engine_renderer_terrain_shadows"};
  const SceneDesc desc = dunes((tmp.native() / "ddc").generic_string());
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
  SceneData probe;
  std::string error;
  REQUIRE_MESSAGE(load_scene(desc, probe, error), error);
  const f32 azimuth = k_sun_azimuth;
  const Camera camera = facing_away(probe.terrain, azimuth);
  const Shot mapped = shoot(gpu.device, desc, ShadowMode::Cascaded, azimuth, true, camera, pool);
  REQUIRE(mapped.available);
  const Shot traced = shoot(gpu.device, desc, ShadowMode::RayTraced, azimuth, true, camera, pool);
  REQUIRE_MESSAGE(traced.available, traced.why);
  const Shot before = shoot(gpu.device, desc, ShadowMode::RayTraced, azimuth, false, camera, pool);

  // The same sand under every pixel: shadows change only the shade.
  CHECK(id_differences(mapped, traced) == 0);
  CHECK(id_differences(traced, before) == 0);
  const Vector<i8> maps = classes(mapped);
  const Agreement now = agree(traced, maps, k_band);
  const Agreement old = agree(before, maps, k_band);
  // The pixels the dunes behind the camera shadow: black in the maps and in the rays, lit without
  // the off-frustum casters.
  u32 from_behind = 0;
  for (u32 p = 0; p < k_width * k_height; ++p) {
    if (!sand(traced, p)) continue;
    const bool behind = shade(mapped, p) == 0 && shade(traced, p) == 0 && shade(before, p) == 255;
    from_behind += behind ? 1u : 0u;
  }
  MESSAGE("the sun " << k_sun_elevation << " degrees up behind the camera: " << now.compared
                     << " interior sand pixels in the maps' view, " << now.shadowed
                     << " shadowed; the rays differ on " << now.differ << " (" << traced.casters
                     << " casters, " << traced.overflow
                     << " pool entries refused), without the off-frustum casters on " << old.differ
                     << " (" << before.casters << " casters); " << from_behind
                     << " pixels shadowed by the dunes behind the camera; black sand: the maps "
                     << black(mapped) << ", the rays " << black(traced) << ", without "
                     << black(before));
  // The witness: the dunes behind cast a real share of the shadow in view.
  CHECK(now.shadowed > 1000);
  CHECK(from_behind > 500);
  CHECK(traced.casters > before.casters);
  CHECK(traced.overflow == 0);
  // The traced shadow is the maps' away from the band: what the old cull lost is back.
  CHECK(old.differ > 500);
  CHECK(now.differ * 200 <= now.compared);
}
