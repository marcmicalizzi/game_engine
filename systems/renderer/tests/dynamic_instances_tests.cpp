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
  out.center = Vec3{0.0f, 0.0f, 0.0f};
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
  frame.camera.position = eye;
  frame.camera.target = Vec3{0.0f, 0.0f, 0.0f};
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

TEST_CASE("dynamic instances: a scene read whole refuses a tail, and a dynamic one is not traced") {
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

  // The ray path and traced shadows are resolved away for a dynamic scene, with a warning.
  SceneData data;
  REQUIRE(make_scene(std::span<const SceneInstance>(head.data(), head.size()), true, data, error));
  gfx::DeviceFeatures traced = gpu.device.features();
  traced.cluster_acceleration_structure = true;
  traced.ray_query = true;
  RenderSettings rt;
  rt.raster = RasterMode::RayTrace;
  rt.shadows = ShadowMode::RayTraced;
  ResolvedSettings resolved;
  resolve_settings(rt, traced, &data, resolved);
  CHECK_FALSE(resolved.ray_path);
  CHECK_FALSE(resolved.rt_chain);
  CHECK_FALSE(resolved.shadows);
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
