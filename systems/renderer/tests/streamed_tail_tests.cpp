// A world whose ground is drawn from its tiles takes a streamed tail (docs/subsystems/renderer.md,
// "Instances that come and go"; scatter.md, "Where it enters the renderer"): until 2026-10-12 the
// reader refused the two together, because a scene whose instances came and went had neither the
// deformed-vertex pool the tiles are drawn through nor the ray tracing chain. Here a small world of
// dune tiles (the tile set of `terrain_tiles_gpu_tests.cpp`) takes a test layer — two boxes on
// every tile round the camera, a block per tile keyed by it — and the cases hold what the change
// promises: the placements come and go with the camera and the ids show them; the ground drawn
// through the pool is the picture it was, with the layer and without it, and with the scene loaded
// to take a tail or not; the ray path's visibility is the rasterizer's word for word over the tail
// with shadow casters on; the boxes cast traced shadows; and a block that leaves traces nothing.
#include <core/jobs/job_system.h>
#include <core/math/math.h>
#include <domain/gfx/device.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/terrain_tiles.h>
#include <systems/renderer/terrain_time.h>
#include <systems/renderer/tile_layout.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

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
  bool traces() const {
    return device.features().cluster_acceleration_structure && device.features().ray_query;
  }
};

void put_u32(std::vector<u8>& out, u32 v) {
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>(v >> (8 * i)));
}
void put_f32(std::vector<u8>& out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  put_u32(out, bits);
}

// A unit cube of 6 x 8 x 8 quads as a GLB: a few leaf clusters, what the layer places.
bool write_box(const std::string& path) {
  std::vector<Vec3> positions;
  std::vector<Vec3> normals;
  std::vector<u32> indices;
  const Vec3 face[6] = {Vec3{1, 0, 0},  Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                        Vec3{0, -1, 0}, Vec3{0, 0, 1},  Vec3{0, 0, -1}};
  const Vec3 tangent[6] = {Vec3{0, 1, 0}, Vec3{0, 0, 1}, Vec3{0, 0, 1},
                           Vec3{1, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}};
  constexpr u32 n = 8;
  for (u32 f = 0; f < 6; ++f) {
    const Vec3 nrm = face[f];
    const Vec3 t = tangent[f];
    const Vec3 b = cross(nrm, t);
    const u32 base = static_cast<u32>(positions.size());
    for (u32 j = 0; j <= n; ++j) {
      for (u32 i = 0; i <= n; ++i) {
        const f32 s = static_cast<f32>(i) / static_cast<f32>(n) - 0.5f;
        const f32 r = static_cast<f32>(j) / static_cast<f32>(n) - 0.5f;
        positions.push_back(nrm * 0.5f + t * s + b * r);
        normals.push_back(nrm);
      }
    }
    for (u32 j = 0; j < n; ++j) {
      for (u32 i = 0; i < n; ++i) {
        const u32 a = base + j * (n + 1) + i;
        const u32 quad[6] = {a, a + 1, a + n + 2, a, a + n + 2, a + n + 1};
        for (const u32 q : quad)
          indices.push_back(q);
      }
    }
  }
  std::vector<u8> bin;
  for (const Vec3& p : positions) {
    put_f32(bin, p.x);
    put_f32(bin, p.y);
    put_f32(bin, p.z);
  }
  const u32 normal_offset = static_cast<u32>(bin.size());
  for (const Vec3& v : normals) {
    put_f32(bin, v.x);
    put_f32(bin, v.y);
    put_f32(bin, v.z);
  }
  const u32 index_offset = static_cast<u32>(bin.size());
  for (const u32 i : indices)
    put_u32(bin, i);
  const u32 index_bytes = static_cast<u32>(bin.size()) - index_offset;
  const auto num = [](u32 v) { return std::to_string(v); };
  std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,"
      "\"NORMAL\":1},\"indices\":2,\"material\":0}]}],\"materials\":[{\"pbrMetallicRoughness\":"
      "{\"baseColorFactor\":[0.8,0.3,0.2,1],\"metallicFactor\":0,\"roughnessFactor\":0.7}}],"
      "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":" +
      num(static_cast<u32>(positions.size())) +
      ",\"type\":\"VEC3\",\"min\":[-0.5,-0.5,-0.5],\"max\":[0.5,0.5,0.5]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":" +
      num(static_cast<u32>(normals.size())) +
      ",\"type\":\"VEC3\"},{\"bufferView\":2,\"componentType\":5125,\"count\":" +
      num(static_cast<u32>(indices.size())) +
      ",\"type\":\"SCALAR\"}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
      num(normal_offset) + "},{\"buffer\":0,\"byteOffset\":" + num(normal_offset) +
      ",\"byteLength\":" + num(index_offset - normal_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + num(index_offset) +
      ",\"byteLength\":" + num(index_bytes) +
      "}],\"buffers\":[{\"byteLength\":" + num(static_cast<u32>(bin.size())) + "}]}";
  while (json.size() % 4 != 0)
    json += ' ';
  std::vector<u8> glb;
  put_u32(glb, 0x46546c67u);
  put_u32(glb, 2u);
  put_u32(glb, static_cast<u32>(12 + 8 + json.size() + 8 + bin.size()));
  put_u32(glb, static_cast<u32>(json.size()));
  put_u32(glb, 0x4e4f534au);
  glb.insert(glb.end(), json.begin(), json.end());
  put_u32(glb, static_cast<u32>(bin.size()));
  put_u32(glb, 0x004e4942u);
  glb.insert(glb.end(), bin.begin(), bin.end());
  std::ofstream file(path, std::ios::binary);
  if (!file.is_open()) return false;
  file.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
  return file.good();
}

// The dunes of `terrain_tiles_gpu_tests.cpp` (20 m bands, 2 m high), the terrain only as the load's
// instance, and the box as a second mesh no instance of the load names: the tail's.
SceneDesc dunes_and_box(const std::filesystem::path& dir) {
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.meshes.push_back((dir / "box.glb").generic_string());
  SceneInstance terrain;
  terrain.mesh = 0;
  desc.instances.push_back(terrain);
  desc.terrain.enabled = true;
  desc.terrain.size = 17;
  desc.terrain.extent = 64.0f;
  desc.terrain.seed = 5;
  desc.terrain.dune_height = 2.0f;
  desc.terrain.dune_wavelength = 20.0f;
  desc.terrain.generator = TerrainGenerator::dunes;
  desc.terrain.time_s = 94'608'000.0;
  desc.ddc = (dir / "ddc").generic_string();
  return desc;
}

TerrainTilesDesc small_tiles() {
  TerrainTilesDesc t;
  t.tile_size = 8.0f;
  t.ring_count = 3;
  t.radius[0] = 1.5f;
  t.radius[1] = 3.0f;
  t.radius[2] = 6.0f;
  t.cells[0] = 16;
  t.cells[1] = 8;
  t.cells[2] = 4;
  return t;
}

constexpr u32 k_width = 256;
constexpr u32 k_height = 160;
constexpr f64 k_tile = 8.0;  // the tiles' edge, metres

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  std::unique_ptr<TerrainSampler> ground;
  std::unique_ptr<TerrainTileSet> tiles;
  GpuScene scene;
  SceneRenderer renderer;
  TerrainMotion motion;
  Vector<TerrainTile> held;
  std::string error;

  bool build(const gfx::Device& device, const SceneDesc& desc, RenderSettings settings,
             bool dynamic, jobs::JobSystem* jobs) {
    if (!load_scene(desc, data, error)) return false;
    data.dynamic = dynamic;  // what the reader sets for a world with streamed placements
    settings.terrain_tiles = true;
    settings.time_rate_live = true;
    resolve_settings(settings, device.features(), &data, resolved);
    if (!resolved.terrain_tiles) {
      error = "the settings did not resolve to world tiles";
      return false;
    }
    ground = std::make_unique<TerrainSampler>(data.terrain);
    tiles = std::make_unique<TerrainTileSet>();
    if (!tiles->build(data.terrain, small_tiles(), ground->provider().tiles(), WorldPos::origin(),
                      jobs, &error)) {
      return false;
    }
    if (!scene.create(device, data, resolved, &error, tiles.get())) return false;
    SceneRenderer::Desc rd;
    rd.width = k_width;
    rd.height = k_height;
    if (!renderer.create(device, scene, resolved, rd, &error)) return false;
    TimeLapseConfig still;
    still.rate = 0.0;
    still.wait = true;
    return motion.start(scene, tiles.get(), still, jobs, &error);
  }

  void follow(const Camera& camera) {
    terrain_tiles_round(tiles->tiles_desc(), camera.position, held);
    tiles->set_tiles(std::span<const TerrainTile>(held.data(), held.size()));
    motion.frame(1.0 / 60.0, camera.position);
  }

  // The test layer round `centre`: two boxes on every tile within `reach` tiles, standing on the
  // sand, a block per tile keyed by it — a tile's instances a function of the tile alone.
  bool place_round(WorldPos centre, i32 reach) {
    Vector<SceneInstance> tail;
    Vector<DynamicBlock> blocks;
    const scene_gen::Ground surface = ground->provider().view();
    const i32 cx = static_cast<i32>(std::floor(centre.x / k_tile));
    const i32 cz = static_cast<i32>(std::floor(centre.z / k_tile));
    for (i32 z = cz - reach; z <= cz + reach; ++z) {
      for (i32 x = cx - reach; x <= cx + reach; ++x) {
        DynamicBlock block;
        block.key = (static_cast<u64>(static_cast<u32>(x)) << 32) | static_cast<u32>(z);
        block.first = tail.size();
        for (u32 k = 0; k < 2; ++k) {
          const f64 px = (static_cast<f64>(x) + 0.3 + 0.4 * k) * k_tile;
          const f64 pz = (static_cast<f64>(z) + 0.35 + 0.3 * k) * k_tile;
          SceneInstance box;
          box.mesh = 1;
          box.transform.rotation =
              quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, 0.4f + 0.9f * static_cast<f32>(k));
          box.transform.scale = Vec3{1.2f, 1.2f, 1.2f};
          box.origin = WorldPos{px, surface.surface(WorldPos{px, 0.0, pz}) + 0.3, pz};
          tail.push_back(box);
        }
        block.count = tail.size() - block.first;
        blocks.push_back(block);
      }
    }
    return renderer.set_dynamic_instances(
        std::span<const SceneInstance>(tail.data(), tail.size()),
        std::span<const DynamicBlock>(blocks.data(), blocks.size()), &error);
  }

  bool place_none() { return renderer.set_dynamic_instances({}, {}, &error); }

  bool shoot(const Camera& camera, CapturedFrame& out) {
    follow(camera);
    FrameDesc frame;
    frame.camera = camera;
    CaptureChannels channels;
    channels.color = true;
    channels.ids = true;
    channels.depth = true;
    return renderer.capture(frame, channels, out, &error);
  }
};

Camera camera_over(f64 x, f64 z) {
  Camera camera;
  camera.position = WorldPos{x - 6.0, 14.0, z + 16.0};
  camera.target = WorldPos{x, 0.0, z};
  return camera;
}

struct Census {
  u32 tail_pixels = 0;
  u32 tail_instances = 0;  // distinct
  u32 strays = 0;          // a tail slot the layout does not hold
};

Census census(const CapturedFrame& shot, const GpuScene& scene) {
  Census c;
  Vector<u32> tail_index;
  scene.tail_indices(tail_index);
  Vector<u8> seen(tail_index.size(), u8{0});
  for (u32 p = 0; p < shot.width * shot.height; ++p) {
    const u32 id = shot.ids[u64{p} * k_id_words];
    if (id == k_no_id || id < scene.static_instance_count()) continue;
    ++c.tail_pixels;
    if (id >= tail_index.size() || tail_index[id] == ~0u) {
      ++c.strays;
      continue;
    }
    if (seen[id] == 0) ++c.tail_instances;
    seen[id] = 1;
  }
  return c;
}

struct Difference {
  u32 ids = 0;
  u32 depth = 0;
  u32 color = 0;
  u32 compared = 0;
};

// Every pixel, or with `scene` only those neither side draws a tail instance in.
Difference compare(const CapturedFrame& a, const CapturedFrame& b, const GpuScene* a_scene,
                   const GpuScene* b_scene, bool ids_too = true) {
  Difference d;
  for (u32 p = 0; p < a.width * a.height; ++p) {
    const u32 ia = a.ids[u64{p} * k_id_words];
    const u32 ib = b.ids[u64{p} * k_id_words];
    if (a_scene != nullptr && ia != k_no_id && ia >= a_scene->static_instance_count()) continue;
    if (b_scene != nullptr && ib != k_no_id && ib >= b_scene->static_instance_count()) continue;
    ++d.compared;
    bool id = false;
    for (u32 w = 0; w < k_id_words && ids_too; ++w)
      id = id || a.ids[u64{p} * k_id_words + w] != b.ids[u64{p} * k_id_words + w];
    d.ids += id ? 1u : 0u;
    d.depth += a.depth[p] != b.depth[p] ? 1u : 0u;
    bool colour = false;
    for (u32 c = 0; c < 4; ++c)
      colour = colour || a.color[u64{p} * 4 + c] != b.color[u64{p} * 4 + c];
    d.color += colour ? 1u : 0u;
  }
  return d;
}

}  // namespace

TEST_CASE("streamed tail on tile ground: placements come and go, and the ground is unchanged") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  test::TempDir tmp{"engine_renderer_streamed_tail"};
  REQUIRE(write_box((tmp.native() / "box.glb").string()));
  const SceneDesc desc = dunes_and_box(tmp.native());
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  settings.occlusion = false;

  // The ground as it was drawn before the change: a scene that takes no tail.
  Rig fixed;
  REQUIRE_MESSAGE(fixed.build(gpu.device, desc, settings, false, &pool), fixed.error);
  Rig streamed;
  REQUIRE_MESSAGE(streamed.build(gpu.device, desc, settings, true, &pool), streamed.error);
  REQUIRE(streamed.scene.dynamic());
  REQUIRE(streamed.resolved.deform_pass);  // the tiles are drawn through the pool

  const Camera a = camera_over(0.0, 0.0);
  const Camera b = camera_over(48.0, 8.0);
  CapturedFrame ground_fixed;
  CapturedFrame ground_streamed;
  REQUIRE_MESSAGE(fixed.shoot(a, ground_fixed), fixed.error);
  REQUIRE_MESSAGE(streamed.shoot(a, ground_streamed), streamed.error);
  // Loaded to take a tail and holding none, the pool-drawn ground is the picture to the bit.
  const Difference bare = compare(ground_fixed, ground_streamed, nullptr, nullptr);
  CHECK(bare.ids == 0);
  CHECK(bare.depth == 0);
  CHECK(bare.color == 0);

  // The layer round a: boxes on the tiles round it, in the ids, none a stray.
  REQUIRE_MESSAGE(streamed.place_round(a.target, 2), streamed.error);
  CapturedFrame at_a;
  REQUIRE_MESSAGE(streamed.shoot(a, at_a), streamed.error);
  const Census ca = census(at_a, streamed.scene);
  MESSAGE("at a: " << ca.tail_instances << " boxes over " << ca.tail_pixels << " pixels");
  CHECK(ca.tail_instances >= 10);
  CHECK(ca.strays == 0);
  // Where no box is drawn, the ground is the ground without the layer (no shadows here).
  const Difference under = compare(ground_fixed, at_a, nullptr, &streamed.scene);
  CHECK(under.compared > k_width * k_height / 2);
  CHECK(under.ids == 0);
  CHECK(under.depth == 0);
  CHECK(under.color == 0);

  // The camera moves on and the layer with it: a's tiles leave, b's arrive. The scene that takes
  // no tail flies the same way, so its tiles' slots — which the ids name — are the same ones.
  REQUIRE_MESSAGE(streamed.place_round(b.target, 2), streamed.error);
  CapturedFrame at_b;
  REQUIRE_MESSAGE(streamed.shoot(b, at_b), streamed.error);
  CapturedFrame fixed_b;
  REQUIRE_MESSAGE(fixed.shoot(b, fixed_b), fixed.error);
  const Census cb = census(at_b, streamed.scene);
  CHECK(cb.tail_instances >= 10);
  CHECK(cb.strays == 0);
  const Difference under_b = compare(fixed_b, at_b, nullptr, &streamed.scene);
  CHECK(under_b.ids == 0);
  CHECK(under_b.depth == 0);
  CHECK(under_b.color == 0);
  // And back: the same boxes, the same picture, wherever their blocks went.
  REQUIRE_MESSAGE(streamed.place_round(a.target, 2), streamed.error);
  CapturedFrame again;
  REQUIRE_MESSAGE(streamed.shoot(a, again), streamed.error);
  CapturedFrame fixed_again;
  REQUIRE_MESSAGE(fixed.shoot(a, fixed_again), fixed.error);
  const Difference back = compare(at_a, again, nullptr, nullptr, false);
  CHECK(back.depth == 0);
  CHECK(back.color == 0);
  CHECK(census(again, streamed.scene).tail_instances == ca.tail_instances);
  // And none: the ground alone again, word for word.
  REQUIRE_MESSAGE(streamed.place_none(), streamed.error);
  CapturedFrame gone;
  REQUIRE_MESSAGE(streamed.shoot(a, gone), streamed.error);
  const Difference left = compare(fixed_again, gone, nullptr, nullptr);
  CHECK(left.ids == 0);
  CHECK(left.depth == 0);
  CHECK(left.color == 0);
  std::string why;
  CHECK_MESSAGE(streamed.scene.validate_tables(&why), why);
}

TEST_CASE(
    "streamed tail on tile ground: traced like the prefix, and a block that leaves traces "
    "nothing") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  if (!gpu.traces()) {
    MESSAGE(gpu.device.adapter().name << " has no cluster acceleration structures; skipped");
    return;
  }
  test::TempDir tmp{"engine_renderer_streamed_tail_rt"};
  REQUIRE(write_box((tmp.native() / "box.glb").string()));
  const SceneDesc desc = dunes_and_box(tmp.native());
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
  // Traced shadows with the casters, on the rasterizer and on the ray path.
  RenderSettings raster;
  raster.raster = RasterMode::Hardware;
  raster.shadows = ShadowMode::RayTraced;
  raster.shadow_casters = true;
  raster.occlusion = false;
  RenderSettings traced = raster;
  traced.raster = RasterMode::RayTrace;
  Rig r;
  REQUIRE_MESSAGE(r.build(gpu.device, desc, raster, true, &pool), r.error);
  Rig t;
  REQUIRE_MESSAGE(t.build(gpu.device, desc, traced, true, &pool), t.error);
  Rig bare;  // the ground alone, traced, never given a tail
  REQUIRE_MESSAGE(bare.build(gpu.device, desc, raster, true, &pool), bare.error);
  REQUIRE(r.resolved.rt_chain);
  REQUIRE(r.resolved.shadows);
  REQUIRE(t.resolved.ray_path);
  CHECK(r.resolved.casters);

  const Camera a = camera_over(0.0, 0.0);
  const Camera b = camera_over(40.0, -16.0);
  CapturedFrame ground;
  REQUIRE_MESSAGE(bare.shoot(a, ground), bare.error);
  {
    // The ground alone on the ray path is the rasterizer's, before any tail.
    CapturedFrame traced_ground;
    REQUIRE_MESSAGE(t.shoot(a, traced_ground), t.error);
    const Difference g = compare(ground, traced_ground, nullptr, nullptr, true);
    MESSAGE("the ground alone, traced against rasterized: " << g.ids << " ids, " << g.depth
                                                            << " depths");
  }

  const Camera cameras[2] = {a, b};
  for (u32 step = 0; step < 2; ++step) {
    const Camera& camera = cameras[step];
    REQUIRE_MESSAGE(r.place_round(camera.target, 2), r.error);
    REQUIRE_MESSAGE(t.place_round(camera.target, 2), t.error);
    CapturedFrame rs;
    CapturedFrame ts;
    REQUIRE_MESSAGE(r.shoot(camera, rs), r.error);
    REQUIRE_MESSAGE(t.shoot(camera, ts), t.error);
    // The two scenes laid their tails out the same way, so the ids are comparable as they stand.
    const Census c = census(rs, r.scene);
    MESSAGE("step " << step << ": " << c.tail_instances << " boxes over " << c.tail_pixels
                    << " pixels");
    CHECK(c.tail_instances >= 10);
    CHECK(c.strays == 0);
    // **The ray path's cut over the tail is the rasterizer's.** A ray and a rasterized edge
    // disagree about the pixels a silhouette decides, as they do for any mesh (renderer_tests'
    // posed bar, gfx's ray query test: coverage is compared rather than every word, and the ray
    // path's depth is its hit's, not the rasterizer's interpolation), so the tail is held to: no
    // pixel where both hit the tail names another instance or cluster, a coverage difference under
    // 1% of the tail's pixels, a triangle difference under 2%.
    u32 tail_pixels = 0;
    u32 coverage_differ = 0;
    u32 word_differ = 0;
    u32 triangle_differ = 0;
    const u32 prefix = r.scene.static_instance_count();
    for (u32 p = 0; p < k_width * k_height; ++p) {
      const u32* ri = &rs.ids[u64{p} * k_id_words];
      const u32* ti = &ts.ids[u64{p} * k_id_words];
      const bool r_tail = ri[0] != k_no_id && ri[0] >= prefix;
      const bool t_tail = ti[0] != k_no_id && ti[0] >= prefix;
      if (!r_tail && !t_tail) continue;
      ++tail_pixels;
      if (r_tail != t_tail) {
        ++coverage_differ;
      } else if (ri[0] != ti[0] || ri[1] != ti[1]) {
        ++word_differ;
      } else if (ri[2] != ti[2]) {
        ++triangle_differ;
      }
    }
    MESSAGE("step " << step << ": the tail traced against rasterized over " << tail_pixels
                    << " pixels: " << coverage_differ << " coverage, " << word_differ
                    << " instance or cluster, " << triangle_differ << " triangle differences");
    CHECK(word_differ == 0);
    CHECK(static_cast<f64>(coverage_differ) < 0.01 * static_cast<f64>(tail_pixels));
    CHECK(static_cast<f64>(triangle_differ) < 0.02 * static_cast<f64>(tail_pixels));
    // The ground alone, traced and shadowed, from the same camera: the scene that never had a tail
    // follows the same cameras, so its tiles' slots are the same ones.
    CapturedFrame ground_here;
    REQUIRE_MESSAGE(bare.shoot(camera, ground_here), bare.error);
    if (step == 0) {
      // The boxes cast traced shadows on the sand: where the ground shows, some of it is darker
      // than the ground alone.
      u32 darker = 0;
      for (u32 p = 0; p < k_width * k_height; ++p) {
        const u32 ia = rs.ids[u64{p} * k_id_words];
        if (ia == k_no_id || ia >= prefix) continue;
        const u32 lum_with =
            rs.color[u64{p} * 4] + rs.color[u64{p} * 4 + 1] + rs.color[u64{p} * 4 + 2];
        const u32 lum_without = ground_here.color[u64{p} * 4] + ground_here.color[u64{p} * 4 + 1] +
                                ground_here.color[u64{p} * 4 + 2];
        darker += lum_with + 30 < lum_without ? 1u : 0u;
      }
      MESSAGE("shadowed ground pixels: " << darker);
      CHECK(darker > 20);
    }
  }
  // The tail grew the stride past the prefix's, so the chain and the pool were laid out again
  // with the device idle at least once (`GpuScene::relayout`), and drew the same after it.
  MESSAGE("relayouts: " << r.renderer.stats().tiles.relayouts << ", stride "
                        << r.scene.pair_stride() << ", limit " << r.scene.pair_limit());
  CHECK(r.renderer.stats().tiles.relayouts >= 1);
  // Every block leaves: the traced picture is the ground alone's, shadows and all.
  REQUIRE_MESSAGE(r.place_none(), r.error);
  CapturedFrame after;
  REQUIRE_MESSAGE(r.shoot(b, after), r.error);
  CapturedFrame ground_final;
  REQUIRE_MESSAGE(bare.shoot(b, ground_final), bare.error);
  const Difference d = compare(ground_final, after, nullptr, nullptr);
  CHECK(d.ids == 0);
  CHECK(d.depth == 0);
  CHECK(d.color == 0);
  std::string why;
  CHECK_MESSAGE(r.scene.validate_tables(&why), why);
  CHECK_MESSAGE(t.scene.validate_tables(&why), why);
}
