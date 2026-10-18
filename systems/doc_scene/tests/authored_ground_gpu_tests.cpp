// **An authored tile set draws what its heightfield is** (docs/subsystems/tileset.md, "The pixel
// test"): the renderer handed the set `engine-content terrain-tiles` builds — read back from a
// derived-data cache through `tileset::TileSet::source()` — draws, pixel for pixel, what it draws
// handed the same heightfield computed directly in memory: the quantized points at the rings and
// each far level's box filter summed in one loop, the edge held past the data. It is the renderer's
// own "a tile set built ahead and read back" case (systems/renderer/tests/
// terrain_tiles_gpu_tests.cpp) with an authored heightfield in place of the dunes: the scene is
// still a dune scene, because the renderer draws the world's tiles only for a ground that moves
// (renderer.md, "What is refused"), and under tiles the scene's grid carries only the material and
// the UV frame; every height drawn is the source's. Offscreen, on the device the GPU lock gives.
//
// Here, beside the document's mapping, because it needs the renderer, the tile-set capability and
// the terrain capability at once, and `systems/renderer`'s own tests are not this capability's.
#include <core/jobs/job_system.h>
#include <core/math/math.h>
#include <domain/gfx/device.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/tileset.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/terrain_tiles.h>
#include <systems/renderer/terrain_time.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
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

// The renderer's test scene: a dune grid of a few samples that, under tiles, carries only the
// material and the UV frame.
SceneDesc dune_scene(const std::string& ddc) {
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.terrain.enabled = true;
  desc.terrain.size = 17;
  desc.terrain.extent = 64.0f;
  desc.terrain.seed = 5;
  desc.terrain.dune_height = 2.0f;
  desc.terrain.dune_wavelength = 20.0f;
  desc.terrain.generator = TerrainGenerator::dunes;
  desc.terrain.time_s = 94'608'000.0;
  desc.ddc = ddc;
  return desc;
}

// 8 m tiles in rings at 50 cm, 1 m and 2 m, and three far levels at 4, 8 and 16 m.
TerrainTilesDesc tiles_desc() {
  TerrainTilesDesc t;
  t.tile_size = 8.0f;
  t.ring_count = 3;
  t.radius[0] = 1.5f;
  t.radius[1] = 3.0f;
  t.radius[2] = 6.0f;
  t.cells[0] = 16;
  t.cells[1] = 8;
  t.cells[2] = 4;
  t.far_levels = 3;
  t.far_cells = 4;
  t.far_ratio = 2;
  return t;
}

TimeLapseConfig still_lapse() {
  TimeLapseConfig lapse;
  lapse.rate = 0.0;
  lapse.wait = true;
  return lapse;
}

Camera camera_between(Vec3 position, Vec3 target, f32 znear) {
  Camera camera;
  camera.position = absolute(WorldPos::origin(), position);
  camera.target = absolute(WorldPos::origin(), target);
  camera.znear = znear;
  return camera;
}

struct TileRig {
  SceneData data;
  ResolvedSettings resolved;
  std::unique_ptr<TerrainTileSet> tiles;
  GpuScene scene;
  SceneRenderer renderer;
  TerrainMotion motion;
  Vector<TerrainTile> held;
  std::string error;

  bool build(const gfx::Device& device, const SceneDesc& desc, const TerrainTilesDesc& t,
             jobs::JobSystem* jobs, WorldPos camera, const scene_gen::TileSource& source) {
    if (!load_scene(desc, data, error)) return false;
    RenderSettings settings;
    settings.shadows = ShadowMode::Off;
    settings.terrain_tiles = true;
    settings.time_rate_live = true;
    resolve_settings(settings, device.features(), &data, resolved);
    if (check_availability(resolved, device.features()) != RenderAvailability::Ok ||
        !resolved.terrain_tiles) {
      error = "the settings did not resolve to world tiles";
      return false;
    }
    tiles = std::make_unique<TerrainTileSet>();
    if (!tiles->build(data.terrain, t, source, camera, jobs, &error)) return false;
    if (!scene.create(device, data, resolved, &error, tiles.get())) return false;
    SceneRenderer::Desc rd;
    rd.width = 256;
    rd.height = 192;
    if (!renderer.create(device, scene, resolved, rd, &error)) return false;
    return motion.start(scene, tiles.get(), still_lapse(), jobs, &error);
  }
  void follow(const Camera& camera) {
    terrain_tiles_round(tiles->tiles_desc(), camera.position, held);
    tiles->set_tiles(std::span<const TerrainTile>(held.data(), held.size()));
    motion.frame(1.0 / 60.0, camera.position);
  }
};

// **The heightfield computed directly**: its quantized millimetres in memory, a point the finest's
// (every r-th at a coarser spacing), a filtered lattice the trapezoid rule over each point's cell in
// one loop — nothing of the set's tiles, reader or separable sums — the edge held past the data.
struct DirectSource {
  Vector<i32> mm;
  i64 w = 0, h = 0, ox = 0, oz = 0, s = 0;
  Vector<i64> chain;
  i32 at(i64 i, i64 j) const {
    const i64 x = std::clamp<i64>(i - ox, 0, w - 1);
    const i64 z = std::clamp<i64>(j - oz, 0, h - 1);
    return mm[static_cast<u32>(z * w + x)];
  }
  static i64 floor_div(i64 a, i64 b) {
    const i64 q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
  }
  i32 filtered(i64 r, i64 I, i64 J) const {
    i64 sum = 0;
    const i64 half = r / 2;
    for (i64 b = -half; b <= half; ++b)
      for (i64 a = -half; a <= half; ++a)
        sum += (a == -half || a == half ? 1 : 2) * (b == -half || b == half ? 1 : 2) *
               static_cast<i64>(at(I * r + a, J * r + b));
    return static_cast<i32>(floor_div(2 * sum + 4 * r * r, 8 * r * r));
  }
  static bool heights(const void* state, f64, i64 spacing, i64 filter, i32 i0, i32 j0, u32 nx,
                      u32 nz, u32 begin, u32 end, std::span<f32> out) noexcept {
    const auto& self = *static_cast<const DirectSource*>(state);
    if (filter > 0 && (filter != spacing ||
                       std::find(self.chain.begin(), self.chain.end(), spacing) == self.chain.end()))
      return false;
    if (filter == 0 && spacing % self.s != 0) return false;
    const i64 r = spacing / self.s;
    const u32 per_side = (nx + scene_gen::k_height_block - 1) / scene_gen::k_height_block;
    for (u32 b = begin; b < end; ++b) {
      const u32 bx = (b % per_side) * scene_gen::k_height_block;
      const u32 bz = (b / per_side) * scene_gen::k_height_block;
      for (u32 z = bz; z < std::min(nz, bz + scene_gen::k_height_block); ++z) {
        for (u32 x = bx; x < std::min(nx, bx + scene_gen::k_height_block); ++x) {
          const i64 I = i64{i0} + x;
          const i64 J = i64{j0} + z;
          const i32 v = filter > 0 ? self.filtered(r, I, J) : self.at(I * r, J * r);
          out[z * nx + x] = static_cast<f32>(static_cast<f64>(v) / 1000.0);
        }
      }
    }
    return true;
  }
};
constexpr scene_gen::TileSourceOps k_direct_ops{.heights = &DirectSource::heights};

}  // namespace

TEST_CASE("authored ground: a built tile set draws its heightfield, pixel for pixel") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  test::TempDir tmp{"engine_doc_scene_authored_gpu"};
  // A seeded ridge 481 points a side at 50 cm — 240 m, its corner 120 m west and north of the
  // origin — scaled to a tenth, so a 4 m ridge on a 60 cm slope with a few centimetres of roughness.
  const tileset::Heightfield field =
      tileset::synth_heightfield(tileset::SynthKind::ridge, 481, 500, 11, -120000, -120000);
  std::string error;
  REQUIRE_MESSAGE(tileset::write_heightfield(tmp.file("ridge.json"), field, error), error);
  tileset::BuildOptions options;
  options.cells = 32;
  options.ring_spacing_mm = 2000;  // the outermost ring's: 8 m in 4 cells
  options.far_ratio = 2;
  options.far_levels = 3;  // 4, 8 and 16 m
  options.vertical_scale = 0.1;
  tileset::BuildResult built;
  REQUIRE_MESSAGE(tileset::ensure_tileset(tmp.file("ridge.json"), options, "", tmp.file("ddc"),
                                          false, built, error),
                  error);
  tileset::TileSet set;
  REQUIRE_MESSAGE(set.open(built.manifest, error), error);
  const scene_gen::TileSource authored_source = set.source();

  DirectSource direct;
  REQUIRE_MESSAGE(tileset::quantize_heights(field, options, direct.mm, error), error);
  direct.w = field.width;
  direct.h = field.height;
  direct.s = field.spacing_mm;
  direct.ox = field.origin_x_mm / field.spacing_mm;
  direct.oz = field.origin_z_mm / field.spacing_mm;
  REQUIRE(tileset::chain_spacings(options, direct.chain, &error));
  const scene_gen::TileSource direct_source{&k_direct_ops, &direct};

  const SceneDesc desc = dune_scene(tmp.file("ddc"));
  const TerrainTilesDesc t = tiles_desc();
  // The chain the set holds is every far level the renderer asks.
  for (u32 k = 1; k <= t.far_levels; ++k)
    REQUIRE(set.has_filter(terrain_far_level(t, k).spacing));
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
  const Vec3 centre{3.0f, 0.0f, -2.0f};
  TileRig reference;
  REQUIRE_MESSAGE(reference.build(gpu.device, desc, t, &pool, absolute(WorldPos::origin(), centre),
                                  direct_source),
                  reference.error);
  TileRig authored;
  REQUIRE_MESSAGE(authored.build(gpu.device, desc, t, &pool, absolute(WorldPos::origin(), centre),
                                 authored_source),
                  authored.error);
  const f32 ground = set.height_mm(3000, -2000);
  // Looking down at the rings, grazing across the ridge, and high over the far levels and past the
  // data's edge 120 m out, where both hold the edge.
  const Camera cameras[3] = {
      camera_between(Vec3{0.0f, ground + 26.0f, 7.0f}, Vec3{4.0f, ground, -4.0f}, 0.5f),
      camera_between(Vec3{3.0f, ground + 2.5f, -2.0f}, Vec3{33.0f, ground - 4.0f, -20.0f}, 0.1f),
      camera_between(Vec3{-17.0f, ground + 90.0f, 28.0f}, Vec3{103.0f, ground, -92.0f}, 0.5f)};
  CaptureChannels channels;
  channels.depth = true;
  u32 compared = 0;
  u32 differ = 0;
  for (const Camera& camera : cameras) {
    reference.follow(camera);
    authored.follow(camera);
    FrameDesc frame;
    frame.camera = camera;
    CapturedFrame a;
    CapturedFrame b;
    REQUIRE_MESSAGE(reference.renderer.capture(frame, channels, a, &reference.error),
                    reference.error);
    REQUIRE_MESSAGE(authored.renderer.capture(frame, channels, b, &authored.error), authored.error);
    u32 view_differ = 0;
    for (u32 p = 0; p < a.width * a.height; ++p) {
      if (!(a.depth[p] > 0.0f) && !(b.depth[p] > 0.0f)) continue;
      ++compared;
      bool same = a.depth[p] == b.depth[p];
      for (u32 k = 0; k < 4 && same; ++k)
        same = a.color[u64{p} * 4 + k] == b.color[u64{p} * 4 + k];
      view_differ += same ? 0u : 1u;
    }
    differ += view_differ;
    for (u32 k = 1; k < reference.motion.level_count(); ++k) {
      CAPTURE(k);
      CHECK(reference.motion.level_stats(k).last_hash == authored.motion.level_stats(k).last_hash);
    }
    MESSAGE("a view: " << view_differ << " pixels differ");
  }
  MESSAGE("an authored tile set: " << built.tiles << " tiles in " << built.levels << " levels, "
                                   << set.reads() << " read back, " << compared
                                   << " pixels compared, " << differ << " differ");
  CHECK(set.reads() > 0);
  CHECK(set.failures() == 0);
  CHECK(compared > 40'000);
  CHECK(differ == 0);
}
