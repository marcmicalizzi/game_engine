// The sand's detail across the terrain rings (docs/subsystems/renderer.md, "The sand close up"):
// seams tested, not assumed. With the rings in the scene the ground near the camera is drawn by
// three levels — the scene's grid, the middle ring and the inner ring — each cut into chunks, each
// chunk a slot of its own with its own mesh, grid and clusters; the detail must not notice. A view
// that straddles the inner ring's border and a chunk border inside both rings is drawn in the
// detail view, and every covered pixel's ripple height and grain — each a function of (x, z) alone
// — is held to the CPU's function (domain/gfx/tests/ground_detail_reference.h) at the world point
// the pixel's own depth says it shows: the same function of the same world position on either side
// of every border. The share of the ripples each pixel draws — the sand, the slope, the footprint —
// is held to be as continuous across the borders as it is inside them. And with the detail on over
// the rings, occlusion culling and the cone test change no pixel and two runs are the same bytes.
//
// Also, with no device: the dunes' ripple wind (`DuneField::ripple_wind`, through the ground
// provider's `wind`) is the ripple term's rule — today's direction two hours into a day — a unit
// vector, and continuous across a day's turn.
//
// Compiled only where the terrain capability is (its rings and its wind); each GPU case skips with
// a message where there is no device.
#include "ground_detail_reference.h"

#include <core/jobs/job_system.h>
#include <core/math/math.h>
#include <domain/gfx/device.h>
#include <domain/gfx/ground_detail.h>
#include <domain/terrain/dunes.h>
#include <domain/terrain/fixed.h>
#include <domain/terrain/scene_ground.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/terrain_rings.h>
#include <systems/renderer/terrain_time.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

using namespace engine;
using namespace engine::renderer;
namespace gref = engine::ground_ref;

namespace {

std::string slashes(const std::filesystem::path& p) { return p.generic_string(); }

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

// The rings shrunk to a 128 m terrain at a metre, as the rings' own tests have them: an inner ring
// 6 m either side at 25 cm and a middle one 20 m either side at 50 cm. The tunables are the
// process's, so they are put back when the case ends.
struct SmallRings {
  SmallRings() {
    set("terrain.rings.inner_half_m", "6");
    set("terrain.rings.inner_spacing_cm", "25");
    set("terrain.rings.middle_half_m", "20");
    set("terrain.rings.middle_spacing_cm", "50");
  }
  ~SmallRings() {
    for (const char* name : {"terrain.rings.inner_half_m", "terrain.rings.inner_spacing_cm",
                             "terrain.rings.middle_half_m", "terrain.rings.middle_spacing_cm"}) {
      if (tunables::Tunable* t = tunables::find(name)) t->reset();
    }
  }
  static void set(const char* name, const char* value) {
    tunables::Tunable* t = tunables::find(name);
    REQUIRE_MESSAGE(t != nullptr, name);
    std::string error;
    REQUIRE_MESSAGE(t->set_from_text(value, &error), error);
  }
};

SceneDesc ring_scene(const std::string& ddc) {
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.terrain.enabled = true;
  desc.terrain.size = 129;
  desc.terrain.extent = 64.0f;
  desc.terrain.seed = 5;
  desc.terrain.dune_height = 2.0f;
  desc.terrain.dune_wavelength = 20.0f;
  desc.terrain.generator = TerrainGenerator::dunes;
  desc.terrain.time_s = 94'608'000.0;
  desc.terrain.has_detail = true;
  desc.ddc = ddc;
  return desc;
}

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  std::unique_ptr<TerrainRingSet> rings;  // outlives the scene and the motion
  GpuScene scene;
  SceneRenderer renderer;
  TerrainMotion motion;
  std::string error;

  bool build(const gfx::Device& device, const SceneDesc& desc, const RenderSettings& settings,
             u32 width, u32 height, jobs::JobSystem* jobs, Vec3 ring_camera) {
    if (!load_scene(desc, data, error)) return false;
    resolve_settings(settings, device.features(), &data, resolved);
    if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
      error = "unavailable";
      return false;
    }
    rings = std::make_unique<TerrainRingSet>();
    if (!rings->build(data.terrain, ring_camera.x, ring_camera.z, jobs, &error)) return false;
    if (!scene.create(device, data, resolved, &error, rings.get())) return false;
    SceneRenderer::Desc rd;
    rd.width = width;
    rd.height = height;
    if (!renderer.create(device, scene, resolved, rd, &error)) return false;
    TimeLapseConfig lapse = time_lapse_config_from_tunables(0.0);
    lapse.wait = true;
    return motion.start(scene, rings.get(), lapse, jobs, &error);
  }
};

bool unproject(const CapturedFrame& shot, const Mat4& inverse_view_proj, u32 px, u32 py,
               Vec3& out) {
  const f32 depth = shot.depth[py * shot.width + px];
  if (!(depth > 0.0f)) return false;
  const f32 x = (static_cast<f32>(px) + 0.5f) / static_cast<f32>(shot.width) * 2.0f - 1.0f;
  const f32 y = 1.0f - (static_cast<f32>(py) + 0.5f) / static_cast<f32>(shot.height) * 2.0f;
  const Vec4 p = inverse_view_proj * Vec4{x, y, depth, 1.0f};
  if (!(std::abs(p.w) > 0.0f)) return false;
  out = Vec3{p.x / p.w, p.y / p.w, p.z / p.w};
  return true;
}

int unorm(double v) { return static_cast<int>(std::lround(brdf_ref::clamp01(v) * 255.0)); }

}  // namespace

TEST_CASE("sand detail: the dunes' ripple wind is the ripple term's rule and never steps") {
  scene::Terrain entry;
  entry.seed = 7;
  entry.dune_height = 8.0f;
  entry.dune_wavelength = 110.0f;
  entry.generator = scene::TerrainGenerator::Dunes;
  entry.time = 94'608'000.0;
  const terrain::DuneField field(terrain::field_desc_of(entry));
  const i64 day0 = entry.time > 0.0 ? static_cast<i64>(entry.time / 86'400.0) : 0;
  // Two hours and more into a day: the day's own direction.
  for (i64 day = day0; day < day0 + 5; ++day) {
    const i64 t = (day * 86'400 + 3 * 3600) * terrain::k_us_per_second;
    f64 x = 0.0;
    f64 z = 0.0;
    field.ripple_wind(t, x, z);
    const terrain::WindDay& today = field.wind().day(terrain::day_of(t));
    const f64 turn = std::atan2(static_cast<f64>(terrain::fx::sin_q15(today.turn)),
                                static_cast<f64>(terrain::fx::cos_q15(today.turn)));
    CHECK(std::abs(x - std::cos(turn)) < 1e-9);
    CHECK(std::abs(z - std::sin(turn)) < 1e-9);
  }
  // Across four day boundaries, a game minute a step: a unit vector every step, turning by no
  // more than the day's change spread over its two hours of realignment allows.
  f64 px = 0.0;
  f64 pz = 0.0;
  field.ripple_wind(static_cast<i64>(day0 * 86'400 - 600) * terrain::k_us_per_second, px, pz);
  f64 largest_step = 0.0;
  for (i64 s = -600; s < 4 * 86'400; s += 60) {
    const i64 t = (day0 * 86'400 + s) * terrain::k_us_per_second;
    f64 x = 0.0;
    f64 z = 0.0;
    field.ripple_wind(t, x, z);
    CHECK(std::abs(x * x + z * z - 1.0) < 1e-9);
    largest_step = std::max(largest_step, std::acos(std::clamp(x * px + z * pz, -1.0, 1.0)));
    px = x;
    pz = z;
  }
  // A half turn over two hours is the most a day can ask: pi / 120 a minute.
  MESSAGE("the ripple wind turns at most " << largest_step * 180.0 / 3.14159265358979
                                           << " degrees in a game minute across four days");
  CHECK(largest_step <= 3.14159265358979 / 120.0 + 1e-9);
  // And the provider hands the same direction to a renderer through its table.
  const TerrainDesc desc = [] {
    TerrainDesc d;
    d.enabled = true;
    d.seed = 7;
    d.dune_height = 8.0f;
    d.dune_wavelength = 110.0f;
    d.generator = TerrainGenerator::dunes;
    d.time_s = 94'608'000.0;
    return d;
  }();
  const TerrainSampler sampler(desc);
  REQUIRE(sampler.ok());
  const Vec2 w = sampler.wind(desc.time_s + 5000.0);
  f64 x = 0.0;
  f64 z = 0.0;
  field.ripple_wind(static_cast<i64>(std::floor((desc.time_s + 5000.0) * 1e6 + 0.5)), x, z);
  CHECK(std::abs(static_cast<f64>(w.x) - x) < 1e-6);
  CHECK(std::abs(static_cast<f64>(w.y) - z) < 1e-6);
}

TEST_CASE("sand detail: the same function on both sides of every ring and chunk border") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  SmallRings small;
  test::TempDir tmp{"engine_renderer_sand_rings"};
  const SceneDesc desc = ring_scene(slashes(tmp.native() / "ddc"));
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  settings.terrain_rings = true;
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
  constexpr u32 k_width = 256;
  constexpr u32 k_height = 192;
  // Five metres over the sand looking down and ahead: the rings are centred on the camera at
  // (3, 3), so the inner ring runs from -3 to 9 on both axes and its border crosses the view at
  // z = -3; its chunks and the middle ring's are world-aligned (32 and 64 m), so a chunk border
  // runs across both rings at x = 0 and at z = 0.
  const auto camera_for = [&](const TerrainDesc& terrain) {
    Camera camera;
    camera.position = Vec3{3.0f, terrain_height(terrain, 3.0f, 3.0f) + 5.0f, 3.0f};
    camera.target = Vec3{2.0f, terrain_height(terrain, 2.0f, -3.0f), -3.0f};
    camera.znear = 0.05f;
    return camera;
  };
  Rig rig;
  const Camera camera = camera_for(desc.terrain);
  REQUIRE_MESSAGE(
      rig.build(gpu.device, desc, settings, k_width, k_height, &pool, Vec3{3.0f, 0.0f, 3.0f}),
      rig.error);
  REQUIRE(rig.resolved.terrain_rings);
  REQUIRE(rig.scene.ground_detail());
  rig.motion.frame(1.0 / 60.0, camera.position.x, camera.position.z);
  const gfx::GroundDetailParams block = rig.scene.ground_detail_params();
  FrameDesc frame;
  frame.camera = camera;
  frame.frame_index = 0;
  frame.view_mode = static_cast<u32>(gfx::ResolveMode::GroundDetail);
  CaptureChannels channels;
  channels.ids = true;
  channels.depth = true;
  CapturedFrame shot;
  REQUIRE_MESSAGE(rig.renderer.render_offscreen(frame, &rig.error), rig.error);
  REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot, &rig.error), rig.error);
  const Mat4 inverse_view_proj = inverse(rig.renderer.views()[0].view_proj);
  const u32 scene_instances = rig.data.instances.size();
  const u32 middle_slots = rig.scene.terrain_slots(1);
  // Which level drew a pixel, and which instance: the scene's grid, a middle ring's slot, an inner
  // ring's slot — each slot one chunk.
  const auto level_of = [&](u32 instance) -> u32 {
    if (instance < scene_instances) return 0u;
    return instance - scene_instances < middle_slots ? 1u : 2u;
  };
  enum Kind : u32 { interior = 0, level_border = 1, chunk_border = 2 };
  u32 count[3] = {};
  int worst_ripple[3] = {};
  int worst_grain[3] = {};
  u32 over[3] = {};
  int worst_share_step[3] = {};
  for (u32 py = 1; py + 1 < k_height; ++py) {
    for (u32 px = 1; px + 1 < k_width; ++px) {
      const u32 p = py * k_width + px;
      const u32 instance = shot.ids[p * k_id_words];
      if (instance == k_no_id) continue;
      Vec3 world;
      if (!unproject(shot, inverse_view_proj, px, py, world)) continue;
      Kind kind = interior;
      int share_step = 0;
      const u32 neighbours[4] = {p - 1, p + 1, p - k_width, p + k_width};
      for (const u32 q : neighbours) {
        const u32 other = shot.ids[q * k_id_words];
        if (other == k_no_id) continue;
        share_step =
            std::max(share_step, std::abs(int{shot.color[p * 4 + 1]} - int{shot.color[q * 4 + 1]}));
        if (other == instance) continue;
        if (level_of(other) != level_of(instance)) {
          kind = level_border;
        } else if (kind == interior && level_of(instance) > 0) {
          kind = chunk_border;
        }
      }
      // The detail view's ripple is the scene's own profile, unfiltered: a function of (x, z).
      const f64 wx = static_cast<f64>(world.x);
      const f64 wz = static_cast<f64>(world.z);
      const gref::Ripple r = gref::ripple(block, wx, wz, static_cast<f64>(block.asymmetry));
      const gref::Noise2 g =
          gref::value_noise2(wx, wz, static_cast<f64>(block.grain_size), block.seed, 16u);
      const int ripple = std::abs(int{shot.color[p * 4]} -
                                  unorm(r.height / static_cast<f64>(block.amplitude) * 0.5 + 0.5));
      const int grain = std::abs(int{shot.color[p * 4 + 2]} - unorm(g.a * 0.5 + 0.5));
      ++count[kind];
      worst_ripple[kind] = std::max(worst_ripple[kind], ripple);
      worst_grain[kind] = std::max(worst_grain[kind], grain);
      over[kind] += ripple > 3 || grain > 3 ? 1u : 0u;
      worst_share_step[kind] = std::max(worst_share_step[kind], share_step);
    }
  }
  const char* names[3] = {"inside a chunk", "at a ring's border", "at a chunk's border"};
  for (u32 k = 0; k < 3; ++k) {
    MESSAGE(std::string(names[k]) << ": " << count[k] << " pixels, ripple height within "
                                  << worst_ripple[k] << " of 255 and grain within "
                                  << worst_grain[k] << " of the CPU's function of each pixel's "
                                  << "world position (" << over[k] << " over 3); the share of "
                                  << "the ripples drawn steps by at most " << worst_share_step[k]
                                  << " between neighbours");
    CHECK(count[k] > 0u);
    // The tolerance: 3 of 255 on 998 pixels in 1,000 (and one more), and 16 on every one — a
    // pixel's world point comes back through a float depth, which at a grazing angle is a few
    // millimetres along the ground, and a ripple's lee and a 5 mm grain cell change fastest there
    // (13 and 11 measured inside the chunks, 1 and 4 at the borders; RTX 5090, 2026-09-29).
    CHECK(over[k] * 500u <= count[k] + 500u);
    CHECK(worst_ripple[k] <= 16);
    CHECK(worst_grain[k] <= 16);
  }
  // A border is no worse than the inside of a chunk: the same function on both sides.
  CHECK(worst_ripple[level_border] <= std::max(worst_ripple[interior], 3));
  CHECK(worst_ripple[chunk_border] <= std::max(worst_ripple[interior], 3));
  CHECK(worst_grain[level_border] <= std::max(worst_grain[interior], 3));
  CHECK(worst_grain[chunk_border] <= std::max(worst_grain[interior], 3));
  // The share of the ripples drawn moves no faster across a border than inside one (the footprint
  // and the slope under it are the surface's, whichever mesh draws it).
  CHECK(worst_share_step[level_border] <= worst_share_step[interior] + 4);
  CHECK(worst_share_step[chunk_border] <= worst_share_step[interior] + 4);
  rig.motion.finish();
}

TEST_CASE("sand detail: over the rings, culling changes no pixel and two runs are the same bytes") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  SmallRings small;
  test::TempDir tmp{"engine_renderer_sand_rings_culling"};
  const SceneDesc desc = ring_scene(slashes(tmp.native() / "ddc"));
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
  constexpr u32 k_width = 200;
  constexpr u32 k_height = 120;
  RenderSettings base;
  base.shadows = ShadowMode::Off;
  base.terrain_rings = true;
  RenderSettings variants[4] = {base, base, base, base};
  variants[2].occlusion = false;
  variants[3].cone = false;
  CapturedFrame shots[4];
  for (u32 v = 0; v < 4; ++v) {
    Rig rig;
    // A walker's eyes over the sand, looking along it across the inner ring's border.
    Camera camera;
    camera.position = Vec3{-2.0f, terrain_height(desc.terrain, -2.0f, 2.0f) + 1.65f, 2.0f};
    camera.target = Vec3{6.0f, terrain_height(desc.terrain, 6.0f, -9.0f), -9.0f};
    camera.znear = 0.05f;
    REQUIRE_MESSAGE(
        rig.build(gpu.device, desc, variants[v], k_width, k_height, &pool, camera.position),
        rig.error);
    rig.motion.frame(1.0 / 60.0, camera.position.x, camera.position.z);
    FrameDesc frame;
    frame.camera = camera;
    frame.frame_index = 0;
    REQUIRE_MESSAGE(rig.renderer.render_offscreen(frame, &rig.error), rig.error);
    CaptureChannels channels;
    channels.depth = true;  // which is what counts the covered pixels
    REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shots[v], &rig.error), rig.error);
    rig.motion.finish();
  }
  const auto differing = [&](u32 a, u32 b) {
    u32 n = 0;
    for (u32 p = 0; p < k_width * k_height; ++p)
      n += std::memcmp(&shots[a].color[p * 4], &shots[b].color[p * 4], 3) != 0 ? 1u : 0u;
    return n;
  };
  MESSAGE("over the rings, detail on: two runs differ on "
          << differing(0, 1) << " pixels, occlusion off on " << differing(0, 2) << ", cones off on "
          << differing(0, 3) << ", of " << shots[0].covered << " covered");
  CHECK(shots[0].covered > k_width * k_height / 3);
  CHECK(differing(0, 1) == 0u);
  CHECK(differing(0, 2) == 0u);
  CHECK(differing(0, 3) == 0u);
}
