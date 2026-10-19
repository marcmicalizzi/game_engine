// Instances added and removed between frames (GpuScene::set_dynamic_instances,
// SceneRenderer::set_dynamic_instances; docs/subsystems/renderer.md, "Instances that come and go").
//
// The contract a streamed world relies on: a scene loaded with a prefix of instances and given a
// tail of more between frames draws **exactly** what a scene loaded with the prefix and the tail
// from the start draws — the same ids, the same depths, the same colours — through every change:
// a tail added, grown past the per-pair buffers so they are made again (twice), shrunk, and taken
// away. The ids can be the same because a tail handed over as one block follows the prefix in both
// scenes, so every (instance, cluster) pair has the same number in both. Occlusion culling is on,
// so the flags that survive a change (and the ones that do not) are exercised; they may cost a
// frame of culling, never a pixel. The CPU half — the pair cap and the placement — needs no device.
//
// **A tail handed over tile by tile** (`DynamicBlock`) puts each tile in a block of its own, which
// is wherever the layout found room: arrivals, departures that leave holes, a hole reused, a tile
// rewritten in place, a compaction asked for and one the holes bring on, and the same changes on
// consecutive frames none of which is waited for. Depth and colour are still compared word for word
// with the loaded scene; the ids are compared once each instance slot is turned into its place in
// the tail (`GpuScene::tail_indices`), which is the loaded scene's numbering. The layout's own
// arithmetic is `tile_layout_tests.cpp`'s.
#include <core/math/math.h>
#include <domain/anim/skeleton.h>
#include <domain/geometry/cluster.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/gfx/device.h>
#include <domain/gfx/shader_library.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/view_set.h>

#include <doctest/doctest.h>

#include <cmath>
#include <shaders/pair_expand.spv.h>
#include <span>
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

struct MeshSource {
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<u32> indices;
};

void add_face(MeshSource& m, Vec3 origin, Vec3 du, Vec3 dv, u32 n) {
  const Vec3 normal = normalize(cross(du, dv));
  const u32 base = m.positions.size();
  for (u32 j = 0; j <= n; ++j) {
    for (u32 i = 0; i <= n; ++i) {
      const f32 s = static_cast<f32>(i) / static_cast<f32>(n);
      const f32 t = static_cast<f32>(j) / static_cast<f32>(n);
      m.positions.push_back(origin + du * s + dv * t);
      m.normals.push_back(normal);
    }
  }
  for (u32 j = 0; j < n; ++j) {
    for (u32 i = 0; i < n; ++i) {
      const u32 a = base + j * (n + 1) + i;
      const u32 quad[6] = {a, a + 1, a + n + 2, a, a + n + 2, a + n + 1};
      for (const u32 q : quad)
        m.indices.push_back(q);
    }
  }
}

// A cube of half-size 1, each face 8 x 8 quads: 768 triangles, a handful of leaf clusters.
MeshSource box_mesh() {
  MeshSource m;
  constexpr u32 n = 8;
  const Vec3 x{2.0f, 0.0f, 0.0f};
  const Vec3 y{0.0f, 2.0f, 0.0f};
  const Vec3 z{0.0f, 0.0f, 2.0f};
  add_face(m, Vec3{1.0f, -1.0f, -1.0f}, y, z, n);
  add_face(m, Vec3{-1.0f, -1.0f, -1.0f}, z, y, n);
  add_face(m, Vec3{-1.0f, 1.0f, -1.0f}, z, x, n);
  add_face(m, Vec3{-1.0f, -1.0f, -1.0f}, x, z, n);
  add_face(m, Vec3{-1.0f, -1.0f, 1.0f}, x, y, n);
  add_face(m, Vec3{-1.0f, -1.0f, -1.0f}, y, x, n);
  return m;
}

// A flat square of side 2 facing up: the ground.
MeshSource sheet_mesh() {
  MeshSource m;
  add_face(m, Vec3{-1.0f, 0.0f, -1.0f}, Vec3{0.0f, 0.0f, 2.0f}, Vec3{2.0f, 0.0f, 0.0f}, 16);
  return m;
}

// The LOD builder's leaves as roots, so every threshold draws the same cut
// (visibility_tie_tests.cpp explains the construction): the comparison is about which instances
// exist, not about LOD.
bool leaves_as_roots(const MeshSource& source, geometry::ClusterLodMesh& out, std::string& error) {
  geometry::AttributeSource attributes;
  attributes.normals = std::span<const Vec3>(source.normals.data(), source.normals.size());
  if (!geometry::build_cluster_lod(
          std::span<const Vec3>(source.positions.data(), source.positions.size()),
          std::span<const u32>(source.indices.data(), source.indices.size()),
          geometry::ClusterLodOptions{}, out, &error, attributes)) {
    return false;
  }
  const u32 leaves = out.level_cluster_counts[0];
  out.mesh.clusters.resize(leaves);
  out.lod.resize(leaves);
  for (u32 c = 0; c < leaves; ++c) {
    geometry::ClusterLodDesc& lod = out.lod[c];
    lod.parent = lod.own;
    lod.own_error = 0.0f;
    lod.parent_error = geometry::k_lod_terminal_error;
    lod.group = c;
  }
  out.level_cluster_counts.resize(1);
  out.group_count = leaves;
  return true;
}

SceneInstance placed(u32 mesh, Vec3 at, f32 scale, f32 yaw = 0.0f) {
  SceneInstance instance;
  instance.mesh = mesh;
  instance.transform.position = at;
  instance.transform.rotation = quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, yaw);
  instance.transform.scale = Vec3{scale, scale, scale};
  return instance;
}

// Two meshes (the box, then the ground) and `instances` placed through `make_instance`, the
// placement a tail gets too. `dynamic` marks the scene as one that takes a tail.
bool make_scene(std::span<const SceneInstance> instances, bool dynamic, SceneData& out,
                std::string& error) {
  geometry::ClusterLodMesh parts[2];
  if (!leaves_as_roots(box_mesh(), parts[0], error)) return false;
  if (!leaves_as_roots(sheet_mesh(), parts[1], error)) return false;
  if (!geometry::merge_cluster_meshes(std::span<const geometry::ClusterLodMesh>(parts, 2), out.lod,
                                      out.parts, &error)) {
    return false;
  }
  for (u32 m = 0; m < 2; ++m) {
    SourceMesh source;
    source.part_material.push_back(-1);
    source.part_of_cluster.resize(out.parts[m].cluster_count, 0u);
    out.sources.push_back(std::move(source));
    out.mesh_fit.push_back(Mat4::identity());
  }
  for (const SceneInstance& source : instances) {
    gfx::InstanceDesc instance;
    std::string why;
    if (!make_instance(out, source, out.pair_count, instance, &why)) {
      error = why;
      return false;
    }
    out.pair_count += out.parts[source.mesh].cluster_count;
    out.instances.push_back(instance);
  }
  out.dynamic = dynamic;
  update_scene_bounds(out);
  // Every scene of the comparison frames the same box: the bounds are the prefix's only in the
  // dynamic one, and the lights scale with them (FrameLighting), so they are pinned.
  out.center = WorldPos{};
  out.radius = 14.0f;
  return true;
}

constexpr u32 k_width = 200;
constexpr u32 k_height = 150;

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  std::string error;

  bool build(const gfx::Device& device, std::span<const SceneInstance> instances, bool dynamic,
             const RenderSettings& settings, u32 frames_in_flight = 2) {
    if (!make_scene(instances, dynamic, data, error)) return false;
    resolve_settings(settings, device.features(), &data, resolved);
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = k_width;
    rd.height = k_height;
    rd.frames_in_flight = frames_in_flight;
    return renderer.create(device, scene, resolved, rd, &error);
  }
};

FrameDesc frame_at(Vec3 eye) {
  FrameDesc frame;
  frame.camera.position = absolute(WorldPos::origin(), eye);
  frame.camera.target = absolute(WorldPos::origin(), Vec3{0.0f, 0.0f, 0.0f});
  return frame;
}

const Vec3 k_eye{0.0f, 11.0f, 16.0f};

struct Difference {
  u32 ids = 0;
  u32 depth = 0;
  u32 color = 0;
};

Difference compare(const CapturedFrame& a, const CapturedFrame& b) {
  Difference d;
  const u32 pixels = a.width * a.height;
  for (u32 p = 0; p < pixels; ++p) {
    bool id = false;
    for (u32 w = 0; w < k_id_words; ++w)
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

CaptureChannels channels() {
  CaptureChannels c;
  c.color = true;
  c.ids = true;
  c.depth = true;
  return c;
}

// The picture of a scene loaded with `instances` from the start.
bool reference(const gfx::Device& device, std::span<const SceneInstance> instances,
               const RenderSettings& settings, CapturedFrame& out, std::string& error) {
  Rig rig;
  if (!rig.build(device, instances, false, settings)) {
    error = rig.error;
    return false;
  }
  return rig.renderer.capture(frame_at(k_eye), channels(), out, &error);
}

// The fixture's instance lists: the prefix (a box and the ground), and three tails.
Vector<SceneInstance> prefix() {
  Vector<SceneInstance> out;
  out.push_back(placed(0, Vec3{0.0f, 0.0f, 0.0f}, 1.0f));
  out.push_back(placed(1, Vec3{0.0f, -1.0f, 0.0f}, 12.0f));
  return out;
}
Vector<SceneInstance> tile_a() {
  Vector<SceneInstance> out;
  const Vec3 at[6] = {{-4, 0, -3}, {4, 0, -3}, {-4, 0, 3}, {4, 0, 3}, {0, 0, -5}, {0, 0.5f, 4}};
  for (u32 i = 0; i < 6; ++i)
    out.push_back(placed(0, at[i], 1.0f, 0.3f * static_cast<f32>(i)));
  return out;
}
Vector<SceneInstance> tile_b() {
  Vector<SceneInstance> out;
  out.push_back(placed(0, Vec3{-8.0f, 0.5f, 0.0f}, 0.8f, 0.7f));
  out.push_back(placed(0, Vec3{8.0f, 0.5f, 0.0f}, 0.8f, 1.1f));
  out.push_back(placed(0, Vec3{0.0f, 0.5f, -9.0f}, 1.2f));
  out.push_back(placed(1, Vec3{6.0f, 0.2f, -8.0f}, 1.5f));
  return out;
}
// Enough small boxes to take the scene past the first stride the tail grows to (4,096 pairs).
Vector<SceneInstance> tile_c() {
  Vector<SceneInstance> out;
  for (u32 i = 0; i < 32; ++i) {
    for (u32 j = 0; j < 24; ++j) {
      out.push_back(placed(
          0, Vec3{-8.0f + 0.5f * static_cast<f32>(i), -0.8f, 6.0f + 0.25f * static_cast<f32>(j)},
          0.15f));
    }
  }
  return out;
}

Vector<SceneInstance> join(std::initializer_list<const Vector<SceneInstance>*> lists) {
  Vector<SceneInstance> out;
  for (const Vector<SceneInstance>* list : lists) {
    for (const SceneInstance& i : *list)
      out.push_back(i);
  }
  return out;
}

void run_sequence(const gfx::Device& device, const RenderSettings& settings, const char* name) {
  const Vector<SceneInstance> head = prefix();
  const Vector<SceneInstance> a = tile_a();
  const Vector<SceneInstance> b = tile_b();
  const Vector<SceneInstance> c = tile_c();
  Rig dynamic;
  REQUIRE_MESSAGE(dynamic.build(device, std::span<const SceneInstance>(head.data(), head.size()),
                                true, settings),
                  dynamic.error);
  REQUIRE(dynamic.scene.dynamic());
  const u32 first_stride = dynamic.scene.pair_stride();

  struct Step {
    const char* what;
    Vector<SceneInstance> tail;
  };
  const Step steps[] = {
      {"nothing yet", {}},
      {"tile a", a},
      {"tiles a and c, past two strides", join({&a, &c})},
      {"tile b alone", b},
      {"tiles b and a, the other order", join({&b, &a})},
      {"nothing again", {}},
  };
  u32 grew = 0;
  u32 stride = first_stride;
  for (const Step& step : steps) {
    std::string error;
    REQUIRE_MESSAGE(dynamic.renderer.set_dynamic_instances(
                        std::span<const SceneInstance>(step.tail.data(), step.tail.size()), &error),
                    error);
    grew += dynamic.scene.pair_stride() != stride ? 1u : 0u;
    stride = dynamic.scene.pair_stride();
    // A few frames from elsewhere first, so the capture's occlusion history is not a fresh one.
    for (u32 f = 0; f < 3; ++f) {
      REQUIRE_MESSAGE(dynamic.renderer.render_offscreen(
                          frame_at(k_eye + Vec3{static_cast<f32>(f) - 1.0f, 0.0f, 0.0f}), &error),
                      error);
    }
    CHECK(dynamic.renderer.stats().last.pairs == dynamic.scene.pair_count());
    CHECK(dynamic.renderer.stats().last.instances == dynamic.scene.instance_count());
    CapturedFrame shot;
    REQUIRE_MESSAGE(dynamic.renderer.capture(frame_at(k_eye), channels(), shot, &error), error);

    const Vector<SceneInstance> whole = join({&head, &step.tail});
    CapturedFrame expected;
    REQUIRE_MESSAGE(reference(device, std::span<const SceneInstance>(whole.data(), whole.size()),
                              settings, expected, error),
                    error);
    const Difference d = compare(shot, expected);
    // The tail is in the picture, so the comparison is about it: pixels an instance of the tail
    // holds, by the id the capture decoded.
    u32 tail_pixels = 0;
    for (u32 p = 0; p < shot.width * shot.height; ++p) {
      const u32 instance = shot.ids[u64{p} * k_id_words];
      tail_pixels += instance != k_no_id && instance >= head.size() ? 1u : 0u;
    }
    MESSAGE(std::string(name) << ", " << std::string(step.what) << ": "
                              << dynamic.scene.instance_count() << " instances, "
                              << dynamic.scene.pair_count() << " pairs (stride "
                              << dynamic.scene.pair_stride() << "), " << shot.covered
                              << " covered pixels, " << tail_pixels << " of them the tail's; "
                              << d.ids << " id, " << d.depth << " depth and " << d.color
                              << " colour differences");
    CHECK(shot.covered > 1000);
    CHECK(shot.covered == expected.covered);
    CHECK((tail_pixels > 200) == !step.tail.empty());
    CHECK(d.ids == 0);
    CHECK(d.depth == 0);
    CHECK(d.color == 0);
  }
  CHECK(grew == 2);
  CHECK(dynamic.scene.pair_count() == dynamic.scene.static_pair_count());
  CHECK(dynamic.scene.dynamic_instance_count() == 0);
}

// ---- tiles, each a block of its own ------------------------------------------------------------

// A tile of a world: its key and its instances.
struct Tile {
  u64 key = 0;
  Vector<SceneInstance> instances;
};

Tile make_tile(u64 key, const Vector<SceneInstance>& instances) {
  Tile t;
  t.key = key;
  t.instances = instances;
  return t;
}

// Three boxes away from every other tile, so no two tiles' surfaces ever meet at one depth.
Vector<SceneInstance> tile_d() {
  Vector<SceneInstance> out;
  out.push_back(placed(0, Vec3{-3.0f, 0.0f, -6.5f}, 1.0f, 0.2f));
  out.push_back(placed(0, Vec3{3.0f, 0.3f, -6.5f}, 1.0f, 0.9f));
  out.push_back(placed(0, Vec3{-6.0f, 0.2f, -6.0f}, 0.9f));
  return out;
}
// Tile a arriving again, drawn differently: five of its boxes, one of them moved.
Vector<SceneInstance> tile_a2(bool moved) {
  Vector<SceneInstance> out = tile_a();
  out.erase_at(4);
  if (moved) out[0].transform.position = Vec3{-4.5f, 0.2f, -2.5f};
  return out;
}

// The tail of `tiles` in the order given, and its blocks.
void tail_of(std::span<const Tile* const> tiles, Vector<SceneInstance>& tail,
             Vector<DynamicBlock>& blocks) {
  tail.clear();
  blocks.clear();
  for (const Tile* t : tiles) {
    if (t == nullptr) continue;
    blocks.push_back(DynamicBlock{t->key, tail.size(), t->instances.size()});
    for (const SceneInstance& i : t->instances)
      tail.push_back(i);
  }
}

// A capture of a scene streamed tile by tile against one of a scene **loaded** with the prefix and
// the tail in the tail's order: depth and colour word for word, and the ids once each instance slot
// the streamed scene drew is turned into the index the loaded scene gives the same instance — its
// place in the tail after the prefix. A tile's instances sit wherever its block is; the surfaces,
// clusters and triangles they name must not.
Difference compare_through_tail(const CapturedFrame& streamed, const GpuScene& scene,
                                const CapturedFrame& loaded) {
  Vector<u32> tail_index;
  scene.tail_indices(tail_index);
  CapturedFrame renamed = streamed;
  const u32 pixels = streamed.width * streamed.height;
  for (u32 p = 0; p < pixels; ++p) {
    u32& id = renamed.ids[p * k_id_words];
    if (id == k_no_id || id < scene.static_instance_count()) continue;
    // A slot that is not an instance of the tail is renamed to something the other side cannot be.
    id = id < tail_index.size() && tail_index[id] != ~0u
             ? scene.static_instance_count() + tail_index[id]
             : k_no_id - 1;
  }
  return compare(renamed, loaded);
}

// Up to four tiles, in the order the tail holds them; a null one is none.
struct TileStep {
  const char* what;
  const Tile* tiles[4];
  bool compact = false;
};

// A world streamed tile by tile — arrivals, departures that leave holes, a block reused, a tile
// rewritten in place, a compaction asked for and one the holes bring on — against the loaded scene
// at every step; then the same changes on consecutive frames with frames in flight and nothing
// waited for, captured at the end.
void run_tiles(const gfx::Device& device, const RenderSettings& settings, const char* name,
               u32 frames_in_flight) {
  const Vector<SceneInstance> head = prefix();
  const Tile a = make_tile(101, tile_a());
  const Tile b = make_tile(202, tile_b());
  const Tile c = make_tile(303, tile_c());
  const Tile d = make_tile(404, tile_d());
  const Tile a2 = make_tile(101, tile_a2(false));
  const Tile a3 = make_tile(101, tile_a2(true));
  Rig dynamic;
  REQUIRE_MESSAGE(dynamic.build(device, std::span<const SceneInstance>(head.data(), head.size()),
                                true, settings, frames_in_flight),
                  dynamic.error);
  REQUIRE(dynamic.scene.table_sets() == (frames_in_flight > 2 ? frames_in_flight : 2u));
  const TileStep steps[] = {
      {"a", {&a, nullptr, nullptr, nullptr}},
      {"a and c", {&a, &c, nullptr, nullptr}},
      {"c: a leaves a hole", {&c, nullptr, nullptr, nullptr}},
      {"c and d: d takes a's hole", {&c, &d, nullptr, nullptr}},
      {"b, c and d: b goes to the end", {&b, &c, &d, nullptr}},
      {"a again, differently", {&a2, &b, &c, &d}},
      {"a moved, rewritten in place", {&a3, &b, &c, &d}},
      {"the same, compacted", {&a3, &b, &c, &d}, true},
      {"b and d: c leaves, and the holes compact", {&b, &d, nullptr, nullptr}},
      {"nothing", {nullptr, nullptr, nullptr, nullptr}},
  };
  const u64 compactions_before = dynamic.scene.compactions();
  u32 reused = 0;
  u32 rewritten = 0;
  u32 holes_seen = 0;
  Vector<SceneInstance> tail;
  Vector<DynamicBlock> blocks;
  for (const TileStep& step : steps) {
    std::string error;
    tail_of(std::span<const Tile* const>(step.tiles, 4), tail, blocks);
    const TileStats before = dynamic.renderer.stats().tiles;
    REQUIRE_MESSAGE(
        dynamic.renderer.set_dynamic_instances(
            std::span<const SceneInstance>(tail.data(), tail.size()),
            std::span<const DynamicBlock>(blocks.data(), blocks.size()), &error, step.compact),
        error);
    const TileStats& after = dynamic.renderer.stats().tiles;
    reused += static_cast<u32>(after.reused - before.reused);
    rewritten += static_cast<u32>(after.rewritten - before.rewritten);
    REQUIRE_MESSAGE(dynamic.scene.validate_tables(&error), error);
    const u32 holes = dynamic.scene.tile_layout().hole_pairs();
    holes_seen += holes > 0 ? 1u : 0u;
    for (u32 f = 0; f < 3; ++f) {
      REQUIRE_MESSAGE(dynamic.renderer.render_offscreen(
                          frame_at(k_eye + Vec3{static_cast<f32>(f) - 1.0f, 0.0f, 0.0f}), &error),
                      error);
    }
    CapturedFrame shot;
    REQUIRE_MESSAGE(dynamic.renderer.capture(frame_at(k_eye), channels(), shot, &error), error);
    const Vector<SceneInstance> whole = join({&head, &tail});
    CapturedFrame expected;
    REQUIRE_MESSAGE(reference(device, std::span<const SceneInstance>(whole.data(), whole.size()),
                              settings, expected, error),
                    error);
    const Difference diff = compare_through_tail(shot, dynamic.scene, expected);
    MESSAGE(std::string(name) << ", " << frames_in_flight << " in flight, "
                              << std::string(step.what) << ": "
                              << dynamic.scene.tile_layout().block_count() << " blocks, "
                              << dynamic.scene.tile_layout().free_count() << " free, "
                              << dynamic.scene.pair_count() << " pairs (" << holes << " holes), "
                              << shot.covered << " covered; " << diff.ids << " id, " << diff.depth
                              << " depth and " << diff.color << " colour differences");
    CHECK(shot.covered == expected.covered);
    CHECK(diff.ids == 0);
    CHECK(diff.depth == 0);
    CHECK(diff.color == 0);
  }
  CHECK(reused >= 1);
  CHECK(rewritten >= 1);
  CHECK(holes_seen >= 3);
  CHECK(dynamic.scene.compactions() - compactions_before == 2);
  CHECK(dynamic.scene.pair_count() == dynamic.scene.static_pair_count());

  // The same changes again on consecutive frames, none waited for: every frame flips to a table
  // set no frame in flight reads and brings it up to date, and the last picture is the last tail's.
  std::string error;
  for (const TileStep& step : steps) {
    tail_of(std::span<const Tile* const>(step.tiles, 4), tail, blocks);
    REQUIRE_MESSAGE(dynamic.renderer.set_dynamic_instances(
                        std::span<const SceneInstance>(tail.data(), tail.size()),
                        std::span<const DynamicBlock>(blocks.data(), blocks.size()), &error),
                    error);
    dynamic.renderer.begin_frame();
    REQUIRE_MESSAGE(dynamic.renderer.submit_frame(frame_at(k_eye), &error) != 0, error);
  }
  const Tile* last[] = {&a3, &b, &c, &d};
  tail_of(std::span<const Tile* const>(last, 4), tail, blocks);
  REQUIRE_MESSAGE(dynamic.renderer.set_dynamic_instances(
                      std::span<const SceneInstance>(tail.data(), tail.size()),
                      std::span<const DynamicBlock>(blocks.data(), blocks.size()), &error),
                  error);
  dynamic.renderer.begin_frame();
  REQUIRE_MESSAGE(dynamic.renderer.submit_frame(frame_at(k_eye), &error) != 0, error);
  CapturedFrame shot;
  REQUIRE_MESSAGE(dynamic.renderer.capture(frame_at(k_eye), channels(), shot, &error), error);
  const Vector<SceneInstance> whole = join({&head, &tail});
  CapturedFrame expected;
  REQUIRE_MESSAGE(reference(device, std::span<const SceneInstance>(whole.data(), whole.size()),
                            settings, expected, error),
                  error);
  const Difference diff = compare_through_tail(shot, dynamic.scene, expected);
  MESSAGE(std::string(name) << ", " << frames_in_flight
                            << " in flight, a change every frame, none waited for: " << diff.ids
                            << " id, " << diff.depth << " depth and " << diff.color
                            << " colour differences over " << shot.covered << " covered");
  CHECK(diff.ids == 0);
  CHECK(diff.depth == 0);
  CHECK(diff.color == 0);
}

}  // namespace

TEST_CASE("dynamic instances: a tail added and removed between frames draws the loaded scene") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE(gpu.why);
    return;
  }
  RenderSettings settings;
  settings.raster = RasterMode::Hardware;  // the vertex path where there are no mesh shaders
  settings.shadows = ShadowMode::Off;
  settings.occlusion = true;
  run_sequence(gpu.device, settings, "hw, occlusion");
  // The cascaded maps run the cull again per cascade, in runs behind the views' over the same
  // per-pair layout.
  settings.shadows = ShadowMode::Cascaded;
  run_sequence(gpu.device, settings, "hw, occlusion, cascaded shadow maps");
  // The vertex path lays its indexed draw's records out per pair too, a region per run.
  settings.raster = RasterMode::Vertex;
  settings.shadows = ShadowMode::Off;
  run_sequence(gpu.device, settings, "vertex, occlusion");
}

TEST_CASE(
    "dynamic instances: tiles arriving and leaving in blocks of their own draw the loaded "
    "scene") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE(gpu.why);
    return;
  }
  RenderSettings settings;
  settings.raster = RasterMode::Hardware;  // the vertex path where there are no mesh shaders
  settings.shadows = ShadowMode::Off;
  settings.occlusion = true;
  run_tiles(gpu.device, settings, "hw, occlusion", 2);
  // Three frames in flight keep three table sets, and a change reaches the third a frame later.
  run_tiles(gpu.device, settings, "hw, occlusion", 3);
  settings.shadows = ShadowMode::Cascaded;
  run_tiles(gpu.device, settings, "hw, occlusion, cascaded shadow maps", 2);
  settings.raster = RasterMode::Vertex;
  settings.shadows = ShadowMode::Off;
  run_tiles(gpu.device, settings, "vertex, occlusion", 2);
}

TEST_CASE("dynamic instances: the pair expansion's push constants are the shader's") {
  gfx::ShaderLibrary library;
  std::string error;
  REQUIRE_MESSAGE(library.create(nullptr, &error), error);
  library.add_embedded("pair_expand", shaders::k_pair_expand_spirv,
                       shaders::k_pair_expand_spirv_size);
  const gfx::Shader* shader = library.get("pair_expand", &error);
  REQUIRE_MESSAGE(shader != nullptr, error);
  CHECK(shader->reflection.push_constant_bytes == sizeof(PairExpandParams));
  const gfx::ShaderEntryPoint* entry = shader->reflection.entry("pair_expand_main");
  REQUIRE(entry != nullptr);
  CHECK(entry->local_size[0] == k_pair_expand_workgroup);
}

TEST_CASE("dynamic instances: a scene read whole refuses a tail, and a dynamic one is traced") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE(gpu.why);
    return;
  }
  const Vector<SceneInstance> head = prefix();
  const Vector<SceneInstance> a = tile_a();
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  Rig whole;
  REQUIRE_MESSAGE(whole.build(gpu.device, std::span<const SceneInstance>(head.data(), head.size()),
                              false, settings),
                  whole.error);
  std::string error;
  CHECK_FALSE(whole.renderer.set_dynamic_instances(
      std::span<const SceneInstance>(a.data(), a.size()), &error));
  CHECK(error.find("not loaded to take instances") != std::string::npos);
  CHECK(whole.scene.instance_count() == head.size());

  // Since 2026-10-12 a dynamic scene is traced (its tail's blocks write their own top-level
  // records; streamed_tail_tests.cpp holds the pictures): the ray path and traced shadows stay.
  // What is resolved away, with a warning, is a deformed tail (`--deform`) and cluster templates.
  SceneData data;
  REQUIRE(make_scene(std::span<const SceneInstance>(head.data(), head.size()), true, data, error));
  gfx::DeviceFeatures traced = gpu.device.features();
  traced.cluster_acceleration_structure = true;
  traced.ray_query = true;
  RenderSettings rt;
  rt.raster = RasterMode::RayTrace;
  rt.shadows = ShadowMode::RayTraced;
  rt.deform = true;
  rt.rt_templates = true;
  ResolvedSettings resolved;
  resolve_settings(rt, traced, &data, resolved);
  CHECK(resolved.ray_path);
  CHECK(resolved.rt_chain);
  CHECK(resolved.shadows);
  CHECK_FALSE(resolved.settings.deform);
  CHECK_FALSE(resolved.settings.rt_templates);
}

TEST_CASE("dynamic instances: the pair cap and the placement need no device") {
  SceneData data;
  std::string error;
  const Vector<SceneInstance> head = prefix();
  REQUIRE(make_scene(std::span<const SceneInstance>(head.data(), head.size()), true, data, error));
  const u32 box = data.parts[0].cluster_count;
  const Vector<SceneInstance> a = tile_a();
  u32 end = 0;
  REQUIRE(pairs_after(data, data.pair_count, std::span<const SceneInstance>(a.data(), a.size()),
                      k_max_pairs, end, &error));
  CHECK(end == data.pair_count + 6 * box);
  // A budget of the caller's own is the same check.
  CHECK_FALSE(pairs_after(data, data.pair_count, std::span<const SceneInstance>(a.data(), a.size()),
                          data.pair_count + box, end, &error));
  CHECK(error.find("past") != std::string::npos);
  // Past the visibility id's 2^24: the prefix sum is checked before anything is allocated.
  CHECK_FALSE(pairs_after(data, k_max_pairs - box + 1, std::span<const SceneInstance>(a.data(), 1),
                          k_max_pairs, end, &error));
  // A mesh the scene has not, and a skinned instance, are refused by the placement.
  SceneInstance stray = placed(7, Vec3{}, 1.0f);
  CHECK_FALSE(pairs_after(data, 0, std::span<const SceneInstance>(&stray, 1), k_max_pairs, end));
  gfx::InstanceDesc desc;
  CHECK_FALSE(make_instance(data, stray, 0, desc, &error));
  SceneInstance skinned = placed(0, Vec3{}, 1.0f);
  skinned.joints = 4;
  CHECK_FALSE(make_instance(data, skinned, 0, desc, &error));
  // The placement is the load's: the fit, then the transform, at the pair given.
  data.mesh_fit[0] = scaling(Vec3{2.0f, 2.0f, 2.0f});
  REQUIRE(make_instance(data, placed(0, Vec3{1.0f, 0.0f, 0.0f}, 3.0f), 40, desc, &error));
  CHECK(desc.first_pair == 40);
  CHECK(desc.scale_max == doctest::Approx(6.0f));
  CHECK(desc.mesh == 0);
}

// ---- a skinned loaded instance beside the tail (renderer.md, "The pool and the chain over the
// tail"; roadmap R84) ---------------------------------------------------------------------------
//
// Until 2026-10-18 `GpuScene::create` refused a scene whose instances come and go if any of its
// instances was skinned or morphed — the test game's two actors and its prop tail could not be
// one scene, so its animation was off. The deform table is laid out once, for the load's instances,
// whose slots never move; the tail is placed rigid; the pool's per-frame blocks follow whatever the
// cull emits. So a scene with a bending two-bone bar among its loaded instances takes a tail of
// boxes, draws the bar through the pool and the boxes rigid, builds the chain over both, and is the
// picture of the same instances loaded whole — while a skinned instance handed over in the tail is
// refused alone, by name.
namespace {

struct Bar {
  anim::Skeleton skeleton;
  MeshSource mesh;
  Vector<geometry::SkinBinding> bindings;  // parallel to the mesh's positions
};

// The two-bone bar of renderer_tests.cpp: a square column 2 high in 32 slices, bound root to tip
// by height with a ramp between the quarter and three-quarter marks.
void build_bar(Bar& bar) {
  bar.skeleton.resize(2);
  bar.skeleton.names[0] = "root";
  bar.skeleton.names[1] = "tip";
  bar.skeleton.parents[0] = anim::k_no_joint;
  bar.skeleton.parents[1] = 0;
  bar.skeleton.local_bind[0] = Transform3::identity();
  bar.skeleton.local_bind[1] = Transform3::identity();
  bar.skeleton.local_bind[1].position = Vec3{0.0f, 1.0f, 0.0f};
  anim::compute_inverse_bind(bar.skeleton);
  constexpr u32 k_rings = 33;
  const f32 h = 0.25f;
  const Vec3 ring[4] = {Vec3{-h, 0, -h}, Vec3{h, 0, -h}, Vec3{h, 0, h}, Vec3{-h, 0, h}};
  for (u32 r = 0; r < k_rings; ++r) {
    const f32 t = static_cast<f32>(r) / static_cast<f32>(k_rings - 1);
    const f32 tip = t < 0.25f ? 0.0f : (t > 0.75f ? 1.0f : (t - 0.25f) * 2.0f);
    const u32 joints[4] = {0, 1, 0, 0};
    const f32 weights[4] = {1.0f - tip, tip, 0.0f, 0.0f};
    const geometry::SkinBinding binding = geometry::make_skin_binding(joints, weights);
    for (const Vec3& c : ring) {
      bar.mesh.positions.push_back(Vec3{c.x, 2.0f * t, c.z});
      bar.mesh.normals.push_back(normalize(Vec3{c.x, 0.0f, c.z}));
      bar.bindings.push_back(binding);
    }
  }
  for (u32 r = 0; r + 1 < k_rings; ++r) {
    for (u32 c = 0; c < 4; ++c) {
      const u32 a = r * 4 + c;
      const u32 b = r * 4 + (c + 1) % 4;
      const u32 tri[6] = {a, a + 4, b, b, a + 4, b + 4};  // counter-clockwise seen from outside
      for (const u32 i : tri)
        bar.mesh.indices.push_back(i);
    }
  }
  const u32 top = (k_rings - 1) * 4;
  const u32 caps[12] = {0, 2, 1, 0, 3, 2, top, top + 1, top + 2, top, top + 2, top + 3};
  for (const u32 i : caps)
    bar.mesh.indices.push_back(i);
}

// The bar's skinning matrices for a bend of the tip joint by `degrees` about +z.
void bend(const Bar& bar, f32 degrees, Vector<anim::JointMatrix>& out) {
  anim::Pose pose;
  anim::rest_pose(bar.skeleton, pose);
  pose.rotation[1] = quat_from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, radians(degrees));
  Vector<Mat4> model(bar.skeleton.joint_count(), Mat4::identity());
  anim::local_to_model(bar.skeleton, pose, std::span<Mat4>(model.data(), model.size()));
  out.resize(bar.skeleton.joint_count());
  anim::skinning_matrices(
      std::span<const Mat4>(model.data(), model.size()),
      std::span<const Mat4>(bar.skeleton.inverse_bind.data(), bar.skeleton.inverse_bind.size()),
      std::span<anim::JointMatrix>(out.data(), out.size()));
}

// Where the bar stands unless a case moves it: on the ground beside the box.
const Vec3 k_bar_at{-3.0f, -1.0f, 1.0f};

// The bar's placement standing at `at`: what the load places and what a move hands over.
SceneInstance bar_placement(Vec3 at) {
  SceneInstance standing = placed(0, at, 1.5f, 0.4f);
  standing.bounds_padding = 1.0f;  // a bend of the tip moves no vertex further than this
  return standing;
}

// Three meshes — the bar (skinned, and first, so the merged mesh carries a skin), the box and the
// ground — and the instances: the bar standing at `bar_at`, skinned with two joints, then `others`
// (meshes 1 and 2), placed as the tail is.
bool make_skinned_scene(const Bar& bar, std::span<const SceneInstance> others, bool dynamic,
                        Vec3 bar_at, SceneData& out, std::string& error) {
  geometry::ClusterLodMesh parts[3];
  geometry::AttributeSource attributes;
  attributes.normals = std::span<const Vec3>(bar.mesh.normals.data(), bar.mesh.normals.size());
  attributes.skin =
      std::span<const geometry::SkinBinding>(bar.bindings.data(), bar.bindings.size());
  attributes.joint_count = bar.skeleton.joint_count();
  if (!geometry::build_cluster_lod(
          std::span<const Vec3>(bar.mesh.positions.data(), bar.mesh.positions.size()),
          std::span<const u32>(bar.mesh.indices.data(), bar.mesh.indices.size()),
          geometry::ClusterLodOptions{}, parts[0], &error, attributes)) {
    return false;
  }
  if (!leaves_as_roots(box_mesh(), parts[1], error)) return false;
  if (!leaves_as_roots(sheet_mesh(), parts[2], error)) return false;
  if (!geometry::merge_cluster_meshes(std::span<const geometry::ClusterLodMesh>(parts, 3), out.lod,
                                      out.parts, &error)) {
    return false;
  }
  for (u32 m = 0; m < 3; ++m) {
    SourceMesh source;
    source.part_material.push_back(-1);
    source.part_of_cluster.resize(out.parts[m].cluster_count, 0u);
    out.sources.push_back(std::move(source));
    out.mesh_fit.push_back(Mat4::identity());
  }
  const auto add = [&](const SceneInstance& source, u32 joints) {
    gfx::InstanceDesc instance;
    std::string why;
    if (!make_instance(out, source, out.pair_count, instance, &why)) {
      error = why;
      return false;
    }
    out.pair_count += out.parts[source.mesh].cluster_count;
    out.instances.push_back(instance);
    out.instance_joints.push_back(joints);
    out.skinned_instances += joints > 0 ? 1u : 0u;
    return true;
  };
  if (!add(bar_placement(bar_at), 2)) return false;
  for (const SceneInstance& source : others) {
    if (!add(source, 0)) return false;
  }
  out.max_joints = 2;
  out.dynamic = dynamic;
  update_scene_bounds(out);
  out.center = WorldPos{};
  out.radius = 14.0f;
  return true;
}

// A list of the box-and-ground scene's instances on this scene's mesh numbers.
Vector<SceneInstance> renumbered(const Vector<SceneInstance>& list) {
  Vector<SceneInstance> out = list;
  for (SceneInstance& i : out)
    i.mesh += 1;
  return out;
}

struct SkinnedRig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  std::string error;
  bool build(const gfx::Device& device, const Bar& bar, std::span<const SceneInstance> others,
             bool dynamic, const RenderSettings& settings, Vec3 bar_at = k_bar_at) {
    if (!make_skinned_scene(bar, others, dynamic, bar_at, data, error)) return false;
    resolve_settings(settings, device.features(), &data, resolved);
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = k_width;
    rd.height = k_height;
    return renderer.create(device, scene, resolved, rd, &error);
  }
};

}  // namespace

TEST_CASE("dynamic instances: a skinned loaded instance beside the tail draws through the pool") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE(gpu.why);
    return;
  }
  Bar bar;
  build_bar(bar);
  Vector<anim::JointMatrix> matrices;
  bend(bar, 40.0f, matrices);
  const InstanceJoints runs[1] = {{0, 2}};  // the bar is instance 0
  const auto posed = [&]() {
    FrameDesc frame = frame_at(k_eye);
    frame.joints = std::span<const anim::JointMatrix>(matrices.data(), matrices.size());
    frame.instance_joints = std::span<const InstanceJoints>(runs, 1);
    return frame;
  };
  const Vector<SceneInstance> head = renumbered(prefix());
  const Vector<SceneInstance> a = renumbered(tile_a());
  const Vector<SceneInstance> whole = join({&head, &a});
  // No shadows, the cascaded maps, and where the device traces, traced shadows with the casters,
  // so the chain is built over the bar's pool blocks and the tail's rigid boxes.
  const bool traces =
      gpu.device.features().cluster_acceleration_structure && gpu.device.features().ray_query;
  const ShadowMode modes[3] = {ShadowMode::Off, ShadowMode::Cascaded, ShadowMode::RayTraced};
  for (const ShadowMode mode : modes) {
    if (mode == ShadowMode::RayTraced && !traces) continue;
    CAPTURE(static_cast<u32>(mode));
    RenderSettings settings;
    settings.shadows = mode;
    settings.occlusion = false;

    SkinnedRig dynamic;
    REQUIRE_MESSAGE(
        dynamic.build(gpu.device, bar, std::span<const SceneInstance>(head.data(), head.size()),
                      true, settings),
        dynamic.error);
    REQUIRE(dynamic.scene.dynamic());
    REQUIRE(dynamic.scene.skinned());
    REQUIRE(dynamic.resolved.deform_pass);
    CHECK(dynamic.resolved.rt_chain == (mode == ShadowMode::RayTraced));
    SkinnedRig loaded_head;
    REQUIRE_MESSAGE(
        loaded_head.build(gpu.device, bar, std::span<const SceneInstance>(head.data(), head.size()),
                          false, settings),
        loaded_head.error);
    SkinnedRig loaded_whole;
    REQUIRE_MESSAGE(loaded_whole.build(gpu.device, bar,
                                       std::span<const SceneInstance>(whole.data(), whole.size()),
                                       false, settings),
                    loaded_whole.error);

    std::string error;
    CapturedFrame bare;
    CapturedFrame bare_expected;
    REQUIRE_MESSAGE(dynamic.renderer.capture(posed(), channels(), bare, &error), error);
    REQUIRE_MESSAGE(loaded_head.renderer.capture(posed(), channels(), bare_expected, &error),
                    error);
    const u32 bar_entries = dynamic.renderer.stats().deform_entries;
    const Difference d0 = compare(bare, bare_expected);
    // The bar is in the picture, bent: its pixels.
    u32 bar_pixels = 0;
    for (u32 p = 0; p < bare.width * bare.height; ++p)
      bar_pixels += bare.ids[u64{p} * k_id_words] == 0 ? 1u : 0u;
    MESSAGE("no tail: the bar over " << bar_pixels << " pixels from " << bar_entries
                                     << " pool entries; " << d0.ids << " id, " << d0.depth
                                     << " depth and " << d0.color << " colour differences");
    CHECK(bar_pixels > 100);
    CHECK(bar_entries > 0);
    CHECK(d0.ids == 0);
    CHECK(d0.depth == 0);
    CHECK(d0.color == 0);

    // The tail: six boxes, which grow the stride, so the pool's slot table and the chain are laid
    // out again with a skinned instance among the loaded ones.
    REQUIRE_MESSAGE(dynamic.renderer.set_dynamic_instances(
                        std::span<const SceneInstance>(a.data(), a.size()), &error),
                    error);
    CapturedFrame shot;
    CapturedFrame expected;
    REQUIRE_MESSAGE(dynamic.renderer.capture(posed(), channels(), shot, &error), error);
    REQUIRE_MESSAGE(loaded_whole.renderer.capture(posed(), channels(), expected, &error), error);
    const Difference d = compare(shot, expected);
    u32 tail_pixels = 0;
    for (u32 p = 0; p < shot.width * shot.height; ++p) {
      const u32 instance = shot.ids[u64{p} * k_id_words];
      tail_pixels += instance != k_no_id && instance > head.size() ? 1u : 0u;
    }
    MESSAGE("with the tail: " << dynamic.scene.instance_count() << " instances, stride "
                              << dynamic.scene.pair_stride() << ", " << tail_pixels
                              << " tail pixels, " << dynamic.renderer.stats().deform_entries
                              << " pool entries; " << d.ids << " id, " << d.depth << " depth and "
                              << d.color << " colour differences against the scene loaded whole");
    CHECK(tail_pixels > 200);
    CHECK(d.ids == 0);
    CHECK(d.depth == 0);
    // Traced, this is also the case that found the chain's capacity ceiling frozen at the load's
    // prefix: the tail's frames wanted 70 structures of a chain held at 21, dropped ten instances a
    // frame, and shadowed 2,619 pixels otherwise than the scene loaded whole (scene_renderer.cpp,
    // `set_dynamic_instances`).
    CHECK(d.color == 0);
    if (mode == ShadowMode::RayTraced) {
      CHECK(dynamic.renderer.stats().rt.capacity >= dynamic.renderer.stats().rt.wanted);
    }
    // The tail's boxes are rigid: the pool holds the bar's entries and nothing more.
    CHECK(dynamic.renderer.stats().deform_entries == bar_entries);
    CHECK(loaded_whole.renderer.stats().deform_entries == bar_entries);

    // A skinned instance in the tail is refused alone, by name, and the scene is as it was.
    Vector<SceneInstance> skinned_tail = a;
    SceneInstance actor = placed(0, Vec3{5.0f, -1.0f, 4.0f}, 1.0f);
    actor.joints = 2;
    skinned_tail.push_back(actor);
    CHECK_FALSE(dynamic.renderer.set_dynamic_instances(
        std::span<const SceneInstance>(skinned_tail.data(), skinned_tail.size()), &error));
    MESSAGE("refused: " << error);
    CHECK(error.find("tail instance 6") != std::string::npos);
    CHECK(error.find("is skinned") != std::string::npos);
    CHECK(dynamic.scene.dynamic_instance_count() == a.size());
    CapturedFrame after;
    REQUIRE_MESSAGE(dynamic.renderer.capture(posed(), channels(), after, &error), error);
    const Difference kept = compare(shot, after);
    CHECK(kept.ids == 0);
    CHECK(kept.depth == 0);
    CHECK(kept.color == 0);
  }
}

// **A loaded instance moved in a dynamic scene** (roadmap S88's remainder; renderer.md, "Instances
// that come and go"): the test game's actors are skinned instances its scene file names, and its
// props make the scene dynamic. A move there rides the next frame's flip to a table set no frame in
// flight reads, as a tail block's change does, and waits for nothing. Here the bar walks along a
// line, a step a frame for ten frames, while tiles of boxes arrive, leave and come back in blocks
// of their own; each frame it is moved twice — half a step, a frame submitted and not waited for,
// then the rest — and the frame captured after the second is compared word for word with the
// scene loaded whole with the bar where it now stands and the tail of that frame.
TEST_CASE(
    "dynamic instances: a loaded skinned instance moved every frame among tiles that come and go "
    "draws the loaded scene") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE(gpu.why);
    return;
  }
  Bar bar;
  build_bar(bar);
  Vector<anim::JointMatrix> matrices;
  bend(bar, 40.0f, matrices);
  const InstanceJoints runs[1] = {{0, 2}};  // the bar is instance 0
  const auto posed = [&]() {
    FrameDesc frame = frame_at(k_eye);
    frame.joints = std::span<const anim::JointMatrix>(matrices.data(), matrices.size());
    frame.instance_joints = std::span<const InstanceJoints>(runs, 1);
    return frame;
  };
  const Vector<SceneInstance> head = renumbered(prefix());
  const Tile ta = make_tile(101, renumbered(tile_a()));
  const Tile tb = make_tile(202, renumbered(tile_b()));
  const Tile td = make_tile(404, renumbered(tile_d()));
  struct Step {
    const Tile* tiles[3];
  };
  const Step steps[10] = {
      {{&ta, nullptr, nullptr}}, {{&ta, &tb, nullptr}},         {{&tb, nullptr, nullptr}},
      {{&tb, &td, nullptr}},     {{&td, nullptr, nullptr}},     {{&td, &ta, nullptr}},
      {{&td, &ta, nullptr}},     {{nullptr, nullptr, nullptr}}, {{&tb, nullptr, nullptr}},
      {{&ta, &tb, &td}},
  };
  // Along +x in front of the box, 0.6 m a frame.
  const auto bar_at = [](f32 step) { return Vec3{-3.0f + 0.6f * step, -1.0f, 1.8f}; };
  const bool traces =
      gpu.device.features().cluster_acceleration_structure && gpu.device.features().ray_query;
  const ShadowMode modes[3] = {ShadowMode::Off, ShadowMode::Cascaded, ShadowMode::RayTraced};
  for (const ShadowMode mode : modes) {
    if (mode == ShadowMode::RayTraced && !traces) continue;
    CAPTURE(static_cast<u32>(mode));
    RenderSettings settings;
    settings.shadows = mode;
    settings.occlusion = false;
    SkinnedRig dynamic;
    REQUIRE_MESSAGE(
        dynamic.build(gpu.device, bar, std::span<const SceneInstance>(head.data(), head.size()),
                      true, settings, bar_at(0.0f)),
        dynamic.error);
    REQUIRE(dynamic.scene.dynamic());
    REQUIRE(dynamic.resolved.deform_pass);
    std::string error;
    Vector<SceneInstance> tail;
    Vector<DynamicBlock> blocks;
    f32 first_centre = 0.0f;
    f32 last_centre = 0.0f;
    for (u32 k = 0; k < 10; ++k) {
      CAPTURE(k);
      tail_of(std::span<const Tile* const>(steps[k].tiles, 3), tail, blocks);
      REQUIRE_MESSAGE(dynamic.renderer.set_dynamic_instances(
                          std::span<const SceneInstance>(tail.data(), tail.size()),
                          std::span<const DynamicBlock>(blocks.data(), blocks.size()), &error),
                      error);
      const u32 index = 0;
      const f32 at = static_cast<f32>(k) + 1.0f;
      const SceneInstance half = bar_placement(bar_at(at - 0.5f));
      REQUIRE_MESSAGE(
          dynamic.renderer.move_instances(std::span<const u32>(&index, 1),
                                          std::span<const SceneInstance>(&half, 1), &error),
          error);
      dynamic.renderer.begin_frame();
      REQUIRE_MESSAGE(dynamic.renderer.submit_frame(posed(), &error) != 0, error);
      // With that frame in flight: the rest of the step.
      const SceneInstance full = bar_placement(bar_at(at));
      REQUIRE_MESSAGE(
          dynamic.renderer.move_instances(std::span<const u32>(&index, 1),
                                          std::span<const SceneInstance>(&full, 1), &error),
          error);
      REQUIRE_MESSAGE(dynamic.scene.validate_tables(&error), error);
      CapturedFrame shot;
      REQUIRE_MESSAGE(dynamic.renderer.capture(posed(), channels(), shot, &error), error);

      const Vector<SceneInstance> whole = join({&head, &tail});
      SkinnedRig loaded;
      REQUIRE_MESSAGE(
          loaded.build(gpu.device, bar, std::span<const SceneInstance>(whole.data(), whole.size()),
                       false, settings, bar_at(at)),
          loaded.error);
      CapturedFrame expected;
      REQUIRE_MESSAGE(loaded.renderer.capture(posed(), channels(), expected, &error), error);
      const Difference d = compare_through_tail(shot, dynamic.scene, expected);
      u32 bar_pixels = 0;
      u32 tail_pixels = 0;
      f32 centre = 0.0f;
      for (u32 p = 0; p < shot.width * shot.height; ++p) {
        const u32 instance = shot.ids[u64{p} * k_id_words];
        if (instance == 0) {
          ++bar_pixels;
          centre += static_cast<f32>(p % shot.width);
        }
        tail_pixels +=
            instance != k_no_id && instance >= dynamic.scene.static_instance_count() ? 1u : 0u;
      }
      centre = bar_pixels > 0 ? centre / static_cast<f32>(bar_pixels) : -1.0f;
      MESSAGE("shadows " << static_cast<u32>(mode) << ", frame " << k << ": the bar at x "
                         << bar_at(at).x << " over " << bar_pixels << " pixels (centre column "
                         << centre << "), " << tail.size() << " tail instances in " << blocks.size()
                         << " blocks over " << tail_pixels << " pixels; " << d.ids << " id, "
                         << d.depth << " depth and " << d.color
                         << " colour differences against the scene loaded whole");
      CHECK(bar_pixels > 50);  // 70 at x = 0.6, behind tile a's box at (0, 0.5, 4)
      CHECK((tail_pixels > 100) == !tail.empty());
      CHECK(shot.covered == expected.covered);
      CHECK(d.ids == 0);
      CHECK(d.depth == 0);
      CHECK(d.color == 0);
      if (k == 0) first_centre = centre;
      last_centre = centre;
    }
    // The bar walked: 5.4 m to the right, which the boxes in front of it hide only in part.
    MESSAGE("the bar's pixels moved from column " << first_centre << " to " << last_centre);
    CHECK(last_centre - first_centre > 40.0f);
    // A tail slot is the tail's to place, and a placement of another mesh is refused; neither
    // moves anything.
    const u32 slot = dynamic.scene.static_instance_count();
    const SceneInstance box = placed(1, Vec3{}, 1.0f);
    CHECK_FALSE(dynamic.renderer.move_instances(std::span<const u32>(&slot, 1),
                                                std::span<const SceneInstance>(&box, 1), &error));
    MESSAGE("refused: " << error);
    CHECK(error.find("dynamic tail") != std::string::npos);
    const u32 index = 0;
    CHECK_FALSE(dynamic.renderer.move_instances(std::span<const u32>(&index, 1),
                                                std::span<const SceneInstance>(&box, 1), &error));
    CHECK(error.find("a mesh it is not") != std::string::npos);
  }
}
