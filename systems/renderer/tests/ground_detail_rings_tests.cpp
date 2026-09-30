// The sand's detail across the terrain rings (docs/subsystems/renderer.md, "The sand close up"):
// seams tested, not assumed. With the rings in the scene the ground near the camera is drawn by
// three levels — the scene's grid, the middle ring and the inner ring — each cut into chunks, each
// chunk a slot of its own with its own mesh, grid and clusters; the detail must not notice. A view
// that straddles the inner ring's border and a chunk border inside both rings is drawn in the
// detail view, and every covered pixel's ripple height and grain — each a function of (x, z) alone
// — is held to the CPU's function (domain/gfx/tests/ground_detail_reference.h) at the world point
// the pixel's own depth says it shows: the same function of the same world position on either side
// of every border. The share of the ripples each pixel draws — the sand, the slope, the footprint —
// is held to be as continuous across the borders as it is inside them. The same again with the
// ergs' numbers (the second pass, every term on), whose terms also read the shading normal — which
// each level interpolates from its own lattice, so it steps slightly at a ring's border — and so
// the ripple's spacing and the share's exposure with it: the step each border adds to the
// difference from the CPU's function, which is continuous there, is measured and held. And with the
// detail on over the rings, occlusion culling and the cone test change no pixel and two runs are
// the same bytes.
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
#include <test_paths.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
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

namespace {

// What one kind of pixel measured (inside a chunk, at a ring's border, at a chunk's border): the
// worst difference of the detail view's ripple height and grain from the CPU's, of 255, and how
// many pixels are over 3 on either; the largest step between a pixel and a neighbour, of 255, of
// the ripples' drawn share and of each channel's difference from the CPU — the step a border adds,
// since the CPU's function is continuous across it; and, with `instrument`, the spacing's scale
// the GPU drew the ripple at, against the one the height function's normal gives: its largest
// difference, and the largest step of that difference between neighbours.
struct SeamKind {
  u32 count = 0;
  int ripple = 0;
  int grain = 0;
  u32 over = 0;
  u32 over_ripple = 0;
  u32 over_grain = 0;
  int share_step = 0;
  int ripple_step = 0;
  int grain_step = 0;
  u32 inferred = 0;  // pixels whose scale was read back
  f64 scale_error = 0.0;
  Vector<f64> scale_steps;  // one per pixel with a neighbour read back, the largest step to one
};

// The largest `share` of `values` from the top, after sorting them: 0.001 is the 99.9th percentile.
f64 top(Vector<f64> values, f64 share) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const u32 last = values.size() - 1;
  return values[static_cast<u32>(std::floor(static_cast<f64>(last) * (1.0 - share)))];
}

// Draws the rings in the detail view with `detail` and measures each kind of pixel (above). The
// CPU's function is evaluated at the world point each pixel's own depth says it shows, with the
// ground's normal there from the terrain's height function, which is what the normal-dependent
// terms read (the first pass has none: its ripple and grain are functions of (x, z) alone).
//
// **The instrument** (`instrument`): the spacing's scale multiplies the wavenumber, so the ripple
// a pixel shows is a function of the scale the GPU drew it at, which is the scale of the normal the
// level interpolated there; near the height function's scale, `s`, it is one number the ripple
// height can be solved for, by Newton's method on the CPU's ripple at the pixel's world point. What
// comes back is the GPU's scale less the height function's, whose step between neighbours is the
// step the level's normal takes, since the height function's is continuous: inside a chunk, the
// normal's own drift over a pixel; across a border, that and the jump. It is well conditioned only
// where a pixel's millimetres of depth do not swamp the phase, so it is read with a ripple a metre
// long and a plain sinusoid (`ripple_wavelength` 1, `ripple_asymmetry` 0.5): a kernel's reach in
// wavelengths is the defect density's alone, so a step in scale slides a crest by the same share
// of a wavelength at a metre as at the ergs' 12 cm, `(6 - 4.5 defects) Δs / s`.
void draw_seams(const gfx::Device& device, const std::string& ddc,
                const scene::TerrainDetail& detail, bool instrument, SeamKind kinds[3]) {
  SceneDesc desc = ring_scene(ddc);
  desc.terrain.detail = detail;
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
      rig.build(device, desc, settings, k_width, k_height, &pool, Vec3{3.0f, 0.0f, 3.0f}),
      rig.error);
  REQUIRE(rig.resolved.terrain_rings);
  REQUIRE(rig.scene.ground_detail());
  rig.motion.frame(1.0 / 60.0, camera.position.x, camera.position.z);
  const gfx::GroundDetailParams block = rig.scene.ground_detail_params();
  REQUIRE((!instrument || (block.flags & gfx::k_ground_spacing) != 0u));
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
  // The ground's normal at (x, z): the height function's central differences, which the levels'
  // own normals — each its lattice's central differences, interpolated over its triangles — stand
  // for at their spacings.
  const TerrainSampler sampler(desc.terrain);
  const auto ground_normal = [&](f64 x, f64 z) {
    constexpr f64 e = 0.02;
    const auto h = [&](f64 px, f64 pz) {
      return static_cast<f64>(sampler.height(static_cast<f32>(px), static_cast<f32>(pz)));
    };
    const f64 hx = (h(x + e, z) - h(x - e, z)) / (2.0 * e);
    const f64 hz = (h(x, z + e) - h(x, z - e)) / (2.0 * e);
    return brdf_ref::normalize(brdf_ref::Dvec3{-hx, 1.0, -hz});
  };
  // The spacing's scale at a normal, as ground_detail.slang takes it.
  const auto spacing = [&](brdf_ref::Dvec3 n) {
    const f64 climb =
        -(n.x * static_cast<f64>(block.wind.x) + n.z * static_cast<f64>(block.wind.y)) /
        std::max(n.y, 1e-4);
    return std::clamp(1.0 + static_cast<f64>(block.spacing_gain) * climb,
                      static_cast<f64>(block.spacing_min), static_cast<f64>(block.spacing_max));
  };
  // Every covered pixel's difference from the CPU, per channel (k_uncovered where nothing is), and
  // with the instrument the scale's (NaN where it could not be read back).
  constexpr int k_uncovered = 1 << 20;
  Vector<int> ripple_error(k_width * k_height, k_uncovered);
  Vector<int> grain_error(k_width * k_height, k_uncovered);
  Vector<f64> scale_error(k_width * k_height, std::numeric_limits<f64>::quiet_NaN());
  const brdf_ref::Dvec3 albedo{0.62, 0.47, 0.32};
  for (u32 py = 0; py < k_height; ++py) {
    for (u32 px = 0; px < k_width; ++px) {
      const u32 p = py * k_width + px;
      if (shot.ids[p * k_id_words] == k_no_id) continue;
      Vec3 world;
      if (!unproject(shot, inverse_view_proj, px, py, world)) continue;
      // The detail view's ripple is the scene's own profile, unfiltered, and its grain the coarse
      // octave: functions of (x, z), and of the normal only through the spacing's scale.
      const brdf_ref::Dvec3 point = brdf_ref::dvec3(world);
      const brdf_ref::Dvec3 normal = ground_normal(point.x, point.z);
      const gref::Shading data = gref::shade(block, point, normal, brdf_ref::Dvec3{},
                                             brdf_ref::Dvec3{}, 1.0, albedo, 0.9, true);
      ripple_error[p] = int{shot.color[p * 4]} - unorm(data.ripple * 0.5 + 0.5);
      grain_error[p] = int{shot.color[p * 4 + 2]} - unorm(data.grain * 0.5 + 0.5);
      if (!instrument) continue;
      // The ripple height the CPU draws at scale `s`, in levels of the view's red channel.
      const auto red = [&](f64 s) {
        const gref::Ripple r =
            gref::ripple(block, point.x, point.z, static_cast<f64>(block.asymmetry), s);
        return 255.0 * (r.height / (static_cast<f64>(block.amplitude) * s) * 0.5 + 0.5);
      };
      const f64 base = spacing(normal);
      const f64 got = static_cast<f64>(shot.color[p * 4]);
      f64 s = base;
      bool ok = true;
      for (u32 i = 0; i < 4 && ok; ++i) {
        constexpr f64 h = 1e-4;
        const f64 slope = (red(s + h) - red(s - h)) / (2.0 * h);
        // A level of the channel is 0.002 of scale or less, or the read is not trusted.
        if (std::abs(slope) < 500.0)
          ok = false;
        else
          s -= (red(s) - got) / slope;
        if (std::abs(s - base) > 0.05) ok = false;
      }
      if (ok && std::abs(red(s) - got) <= 0.75) scale_error[p] = s - base;
    }
  }
  enum Kind : u32 { interior = 0, level_border = 1, chunk_border = 2 };
  for (u32 py = 1; py + 1 < k_height; ++py) {
    for (u32 px = 1; px + 1 < k_width; ++px) {
      const u32 p = py * k_width + px;
      const u32 instance = shot.ids[p * k_id_words];
      if (instance == k_no_id || ripple_error[p] == k_uncovered) continue;
      Kind kind = interior;
      int share_step = 0;
      int ripple_step = 0;
      int grain_step = 0;
      f64 scale_step = -1.0;
      const u32 neighbours[4] = {p - 1, p + 1, p - k_width, p + k_width};
      for (const u32 q : neighbours) {
        const u32 other = shot.ids[q * k_id_words];
        if (other == k_no_id) continue;
        share_step =
            std::max(share_step, std::abs(int{shot.color[p * 4 + 1]} - int{shot.color[q * 4 + 1]}));
        if (ripple_error[q] != k_uncovered) {
          ripple_step = std::max(ripple_step, std::abs(ripple_error[p] - ripple_error[q]));
          grain_step = std::max(grain_step, std::abs(grain_error[p] - grain_error[q]));
        }
        if (!std::isnan(scale_error[p]) && !std::isnan(scale_error[q]))
          scale_step = std::max(scale_step, std::abs(scale_error[p] - scale_error[q]));
        if (other == instance) continue;
        if (level_of(other) != level_of(instance)) {
          kind = level_border;
        } else if (kind == interior && level_of(instance) > 0) {
          kind = chunk_border;
        }
      }
      const int ripple = std::abs(ripple_error[p]);
      const int grain = std::abs(grain_error[p]);
      SeamKind& k = kinds[kind];
      ++k.count;
      k.ripple = std::max(k.ripple, ripple);
      k.grain = std::max(k.grain, grain);
      k.over += ripple > 3 || grain > 3 ? 1u : 0u;
      k.over_ripple += ripple > 3 ? 1u : 0u;
      k.over_grain += grain > 3 ? 1u : 0u;
      k.share_step = std::max(k.share_step, share_step);
      k.ripple_step = std::max(k.ripple_step, ripple_step);
      k.grain_step = std::max(k.grain_step, grain_step);
      if (!std::isnan(scale_error[p])) {
        ++k.inferred;
        k.scale_error = std::max(k.scale_error, std::abs(scale_error[p]));
      }
      if (scale_step >= 0.0) k.scale_steps.push_back(scale_step);
    }
  }
  const char* names[3] = {"inside a chunk", "at a ring's border", "at a chunk's border"};
  for (u32 k = 0; k < 3; ++k) {
    MESSAGE(std::string(names[k])
            << ": " << kinds[k].count << " pixels, ripple height within " << kinds[k].ripple
            << " of 255 (" << kinds[k].over_ripple << " over 3) and grain within " << kinds[k].grain
            << " (" << kinds[k].over_grain
            << " over 3) of the CPU's function of each pixel's world position; between "
            << "neighbours, the share of the ripples drawn steps by at most " << kinds[k].share_step
            << ", the ripple's difference from the CPU by " << kinds[k].ripple_step
            << " and the grain's by " << kinds[k].grain_step);
    if (instrument) {
      MESSAGE(std::string(names[k])
              << ": the scale read back at " << kinds[k].inferred << " pixels, within "
              << kinds[k].scale_error << " of the height function's; its difference steps "
              << "between neighbours by at most " << top(kinds[k].scale_steps, 0.0) << " (99.9% "
              << top(kinds[k].scale_steps, 0.001) << ", 99% " << top(kinds[k].scale_steps, 0.01)
              << ", over " << kinds[k].scale_steps.size() << " pixels)");
    }
    CHECK(kinds[k].count > 0u);
  }
  rig.motion.finish();
}

}  // namespace

TEST_CASE("sand detail: the same function on both sides of every ring and chunk border") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  SmallRings small;
  test::TempDir tmp{"engine_renderer_sand_rings"};
  enum Kind : u32 { interior = 0, level_border = 1, chunk_border = 2 };
  SeamKind kinds[3];
  draw_seams(gpu.device, slashes(tmp.native() / "ddc"), scene::TerrainDetail{}, false, kinds);
  for (u32 k = 0; k < 3; ++k) {
    // The tolerance: 3 of 255 on 998 pixels in 1,000 (and one more), and 16 on every one — a
    // pixel's world point comes back through a float depth, which at a grazing angle is a few
    // millimetres along the ground, and a ripple's lee and a 5 mm grain cell change fastest there
    // (13 and 11 measured inside the chunks, 1 and 4 at the borders; RTX 5090, 2026-09-29).
    CHECK(kinds[k].over * 500u <= kinds[k].count + 500u);
    CHECK(kinds[k].ripple <= 16);
    CHECK(kinds[k].grain <= 16);
  }
  // A border is no worse than the inside of a chunk: the same function on both sides.
  CHECK(kinds[level_border].ripple <= std::max(kinds[interior].ripple, 3));
  CHECK(kinds[chunk_border].ripple <= std::max(kinds[interior].ripple, 3));
  CHECK(kinds[level_border].grain <= std::max(kinds[interior].grain, 3));
  CHECK(kinds[chunk_border].grain <= std::max(kinds[interior].grain, 3));
  // The share of the ripples drawn moves no faster across a border than inside one (the footprint
  // and the slope under it are the surface's, whichever mesh draws it).
  CHECK(kinds[level_border].share_step <= kinds[interior].share_step + 4);
  CHECK(kinds[chunk_border].share_step <= kinds[interior].share_step + 4);
}

TEST_CASE("sand detail: the second pass on both sides of every ring and chunk border") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  // The ergs' own block, read from their scene, which is also the copy the GPU cases draw.
  const std::string path =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/desert-erg/scene.json",
                      "content/test-scenes/desert-erg/scene.json");
  if (!test::path_exists(path)) {
    MESSAGE("the desert-erg scene is not here; skipped");
    return;
  }
  SceneDesc erg;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, erg, error), error);
  REQUIRE(erg.terrain.has_detail);
  const gfx::GroundDetailDesc from_file = terrain_detail_desc(erg.terrain);
  const gfx::GroundDetailDesc copy = gref::erg_numbers();
  CHECK(std::memcmp(&from_file, &copy, sizeof(copy)) == 0);
  SmallRings small;
  test::TempDir tmp{"engine_renderer_sand_rings_erg"};
  enum Kind : u32 { interior = 0, level_border = 1, chunk_border = 2 };
  const std::string ddc = slashes(tmp.native() / "ddc");
  SeamKind kinds[3];
  draw_seams(gpu.device, ddc, erg.terrain.detail, false, kinds);
  // What is a function of position alone holds as the first pass's does. The grain: within 23 of
  // 255 inside a chunk and 9 and 1 at the ring's and a chunk's borders (RTX 5090, 2026-09-30) —
  // more than the first pass's 11 inside, since the gradient grain's coarse octave changes faster
  // than the value noise did, and the view shows it scaled to three of its root-mean-squares, so
  // the millimetres of a float depth show more — and on 52 pixels of 46,867 over 3.
  for (u32 k = 0; k < 3; ++k) {
    CHECK(kinds[k].over_grain * 500u <= kinds[k].count + 500u);
    CHECK(kinds[k].grain <= 32);
  }
  CHECK(kinds[level_border].grain <= std::max(kinds[interior].grain, 3));
  CHECK(kinds[chunk_border].grain <= std::max(kinds[interior].grain, 3));
  // The share of the ripples drawn — which now carries the exposure, a function of the level's
  // normal — steps across a border no more than inside one (19 at the ring's border, 7 at a
  // chunk's, 19 inside).
  CHECK(kinds[level_border].share_step <= kinds[interior].share_step + 4);
  CHECK(kinds[chunk_border].share_step <= kinds[interior].share_step + 4);
  // The ripple is not held to the CPU here: with the spacing its wavelength is the level's
  // normal's, which is the height function's only to the level's own spacing — up to 0.05 of
  // scale apart inside a chunk, near a brink, which is a fifth of a wavelength of phase and any
  // height at all (236 of 255 inside a chunk). What a border adds to that is the instrument's.
  //
  // The instrument: the ergs' numbers with a metre-long plain sinusoid and no defects, so each
  // pixel's scale reads back (above).
  scene::TerrainDetail metre = erg.terrain.detail;
  metre.ripple_wavelength = 1.0f;
  metre.ripple_asymmetry = 0.5f;
  metre.ripple_defects = 0.0f;
  SeamKind steps[3];
  draw_seams(gpu.device, ddc, metre, true, steps);
  // The step the level's normal takes at a border, as the spacing reads it: across the inner
  // ring's border the scale's difference from the height function's steps by at most 0.009
  // between neighbours at the 125 pixels read back there (0.005 at the 99th percentile) — the
  // normal by at most 0.004 radians, which slides a crest by `(6 - 4.5 defects) Δs / s`, 0.04 of
  // a wavelength at a scale of 1, 5 mm on the ergs' 12 cm ripples; across a chunk's border by at
  // most 0.028 (0.017 at the 99th percentile, 549 pixels), where a chunk's cut is simplified on its
  // own; and between neighbours inside a chunk by 0.020 at the 99th percentile over 25,743 pixels,
  // near the brinks, where the levels' interpolated normal cannot follow the height function's (RTX
  // 5090, 2026-09-30). So at the 99th percentile both borders step the normal by less than the
  // inside of a chunk moves it between one pixel and the next, and the ring's border does at its
  // worst too.
  const f64 inside = top(steps[interior].scale_steps, 0.01);
  CHECK(steps[level_border].scale_steps.size() >= 60u);
  CHECK(steps[chunk_border].scale_steps.size() >= 60u);
  CHECK(top(steps[level_border].scale_steps, 0.0) <= 0.015);
  CHECK(top(steps[level_border].scale_steps, 0.01) <= inside);
  CHECK(top(steps[chunk_border].scale_steps, 0.01) <= inside);
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
