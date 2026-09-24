// Depth ties in the visibility buffer: two surfaces at exactly the same depth must resolve the same
// way on every run, on every rasterizer, and with occlusion culling on or off.
//
// Every rasterizer folds a pixel's reversed-Z depth and its id into one 64-bit word and keeps the
// nearest with an atomic max, so at equal depth the id's bits decide. Until 2026-09-24 the id was
// the entry's index in the frame's visible list, which the cull pass filled by atomic append: a
// tie went to whichever pair's thread reached the counter last, and to whichever run of the list
// (occlusion pass 1, pass 2, the software rasterizer) the pair happened to be drawn in. The
// flythrough's occlusion check found every one of its differing pixels to be such a tie
// (docs/experiments/flythrough-desert-overlook.md). The id is now the scene's **pair**, so a tie
// goes to the larger (instance, cluster, triangle) in the scene's own order, whatever drew it
// (docs/subsystems/gfx.md, "The tie rule").
//
// The fixtures are built by hand, like the shadow casters', with the LOD builder's leaves made
// into roots, so every threshold draws the same cut and nothing about a tie depends on LOD:
//
//   coincident  `k_copies` copies of one box at one transform. Every covered pixel is a
//               `k_copies`-way tie between copies of the same triangle, and the rule says the last
//               copy wins everywhere.
//   two passes  two copies, the second with a large `bounds_padding`, which moves no vertex but
//               makes its spheres pass a frustum the first copy's do not. A first frame that looks
//               away draws only the padded copy, so the next frame's occlusion pass 1 draws it and
//               pass 2 draws the other: a tie between the two passes, which the atomic append
//               always gave to pass 2 and a single pass gave to the other copy.
//   two-sided   a curved sheet and its reverse in one mesh, cones off: the palm fronds' case,
//               coincident back-to-back faces in different clusters of one mesh.
#include <core/math/math.h>
#include <domain/geometry/cluster.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/gfx/device.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/view_set.h>

#include <doctest/doctest.h>

#include <cmath>
#include <span>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

// A device, or nothing, exactly as the renderer's other cases open one.
struct Gpu {
  gfx::Device device;
  std::string why;
  bool ok = false;
  explicit Gpu(const gfx::DeviceOptions& options = gfx::DeviceOptions{}) {
    std::string error;
    if (!device.create(options, &error)) {
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

// The parallelogram `origin + s * du + t * dv` in `n` by `n` quads, wound counter-clockwise seen
// from `du x dv`, which is its normal.
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

// A cube of half-size 1 about the origin, each face in 12 x 12 quads: 1,728 triangles, about
// fifteen leaf clusters, so a tie is decided between clusters of many workgroups.
MeshSource box_mesh() {
  MeshSource m;
  constexpr u32 n = 12;
  const Vec3 x{2.0f, 0.0f, 0.0f};
  const Vec3 y{0.0f, 2.0f, 0.0f};
  const Vec3 z{0.0f, 0.0f, 2.0f};
  add_face(m, Vec3{1.0f, -1.0f, -1.0f}, y, z, n);   // +x
  add_face(m, Vec3{-1.0f, -1.0f, -1.0f}, z, y, n);  // -x
  add_face(m, Vec3{-1.0f, 1.0f, -1.0f}, z, x, n);   // +y
  add_face(m, Vec3{-1.0f, -1.0f, -1.0f}, x, z, n);  // -y
  add_face(m, Vec3{-1.0f, -1.0f, 1.0f}, x, y, n);   // +z
  add_face(m, Vec3{-1.0f, -1.0f, -1.0f}, y, x, n);  // -z
  return m;
}

// A quarter cylinder of radius 1.5 about +y, facing outwards, and the same surface again facing
// inwards: every point of it is covered twice at exactly one depth, by two triangles with the same
// three corners in the opposite order. The halves share no vertex (their normals differ), so the
// builder is free to put them in different clusters, which is where a tie is decided by more than
// the triangle index.
MeshSource two_sided_mesh() {
  MeshSource m;
  constexpr u32 around = 24;
  constexpr u32 up = 16;
  for (u32 side = 0; side < 2; ++side) {
    const u32 base = m.positions.size();
    for (u32 j = 0; j <= up; ++j) {
      for (u32 i = 0; i <= around; ++i) {
        const f32 a = -0.785398f + 1.570796f * static_cast<f32>(i) / static_cast<f32>(around);
        const Vec3 out{std::sin(a), 0.0f, std::cos(a)};
        m.positions.push_back(
            Vec3{1.5f * out.x, -1.0f + 2.0f * j / static_cast<f32>(up), 1.5f * out.z});
        m.normals.push_back(side == 0 ? out : out * -1.0f);
      }
    }
    for (u32 j = 0; j < up; ++j) {
      for (u32 i = 0; i < around; ++i) {
        const u32 a = base + j * (around + 1) + i;
        const u32 b = a + 1;
        const u32 c = a + around + 2;
        const u32 d = a + around + 1;
        // Counter-clockwise seen from outside for the front, from inside for the back.
        const u32 front[6] = {a, b, c, a, c, d};
        const u32 back[6] = {a, c, b, a, d, c};
        for (u32 k = 0; k < 6; ++k)
          m.indices.push_back(side == 0 ? front[k] : back[k]);
      }
    }
  }
  return m;
}

// The LOD builder's leaves as a DAG in which every leaf is a root (own error 0, no parent), so
// every threshold selects exactly the leaves; shadow_caster_tests.cpp explains the construction.
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

// One mesh and one instance per entry of `paddings`, every one at the identity transform: the
// copies are the same triangles on the same 16-bit grid under the same matrix, so they rasterize
// to the same depth bits, and only the id tells them apart.
bool make_scene(const MeshSource& mesh, std::span<const f32> paddings, SceneData& out,
                std::string& error) {
  geometry::ClusterLodMesh part;
  if (!leaves_as_roots(mesh, part, error)) return false;
  if (!geometry::merge_cluster_meshes(std::span<const geometry::ClusterLodMesh>(&part, 1), out.lod,
                                      out.parts, &error)) {
    return false;
  }
  SourceMesh source;
  source.part_material.push_back(-1);
  source.part_of_cluster.resize(out.parts[0].cluster_count, 0u);
  out.sources.push_back(std::move(source));
  for (const f32 padding : paddings) {
    gfx::InstanceDesc instance{};
    gfx::set_instance_transform(instance, Mat4::identity());
    instance.mesh = 0;
    instance.first_pair = out.pair_count;
    instance.bounds_padding = padding;  // culling only: a rigid instance's vertices never move
    out.pair_count += out.parts[0].cluster_count;
    out.instances.push_back(instance);
    out.instance_joints.push_back(0);
  }
  update_scene_bounds(out);
  return true;
}

constexpr u32 k_width = 160;
constexpr u32 k_height = 120;
constexpr u32 k_copies = 24;
constexpr u32 k_repeats = 6;

// The scene, the settings, the GPU scene and the renderer, in the order they build in.
struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  std::string error;
  std::string unavailable;  // why these settings cannot run on this device, when they cannot

  bool build(const gfx::Device& device, const MeshSource& mesh, std::span<const f32> paddings,
             const RenderSettings& settings) {
    if (!make_scene(mesh, paddings, data, error)) return false;
    resolve_settings(settings, device.features(), &data, resolved);
    const RenderAvailability availability = check_availability(resolved, device.features());
    if (availability != RenderAvailability::Ok) {
      unavailable = unavailable_reason(availability, device);
      return false;
    }
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = k_width;
    rd.height = k_height;
    return renderer.create(device, scene, resolved, rd, &error);
  }
};

FrameDesc frame_at(Vec3 eye, Vec3 target) {
  FrameDesc frame;
  frame.camera.position = eye;
  frame.camera.target = target;
  return frame;
}

const Vec3 k_eye{3.2f, 2.4f, 4.0f};

CaptureChannels all_channels() {
  CaptureChannels channels;
  channels.color = true;
  channels.ids = true;
  channels.depth = true;
  return channels;
}

// How many pixels of `a` and `b` differ in each channel. Two captures of one picture differ in
// none of them; the ids are the resolved (instance, cluster, triangle), which is the word's id
// bits named in the scene's terms, and with the depth that is the whole word.
struct Difference {
  u32 ids = 0;
  u32 depth = 0;
  u32 color = 0;
  u32 total() const noexcept { return ids + depth + color; }
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

// Covered pixels whose surface is not instance `winner`'s.
u32 not_won_by(const CapturedFrame& shot, u32 winner) {
  u32 lost = 0;
  for (u32 p = 0; p < shot.width * shot.height; ++p) {
    const u32 instance = shot.ids[u64{p} * k_id_words];
    if (instance != k_no_id && instance != winner) ++lost;
  }
  return lost;
}

// One rasterizer's captures of one frame: the first, and how many of the rest differ from it.
struct Drawn {
  std::string name;
  CapturedFrame first;
  u32 unstable = 0;  // later captures that differ from the first in any pixel
  u32 lost = 0;      // the first capture's covered pixels the rule's winner does not hold
  bool ran = false;
};

void draw_repeatedly(const gfx::Device& device, const char* name, const MeshSource& mesh,
                     std::span<const f32> paddings, const RenderSettings& settings, u32 winner,
                     Drawn& out) {
  out.name = name;
  Rig rig;
  if (!rig.build(device, mesh, paddings, settings)) {
    REQUIRE_MESSAGE(!rig.unavailable.empty(), rig.error);
    MESSAGE(std::string(name) << ": unavailable here, " << rig.unavailable);
    return;
  }
  const FrameDesc frame = frame_at(k_eye, Vec3{0.0f, 0.0f, 0.0f});
  std::string error;
  REQUIRE_MESSAGE(rig.renderer.capture(frame, all_channels(), out.first, &error), error);
  REQUIRE(out.first.covered > 1000);
  for (u32 r = 1; r < k_repeats; ++r) {
    CapturedFrame again;
    REQUIRE_MESSAGE(rig.renderer.capture(frame, all_channels(), again, &error), error);
    out.unstable += compare(out.first, again).total() > 0 ? 1u : 0u;
  }
  out.lost = not_won_by(out.first, winner);
  out.ran = true;
  MESSAGE(std::string(name) << ": " << out.first.covered << " covered pixels, " << out.unstable
                            << " of " << (k_repeats - 1) << " repeats differ from the first, "
                            << out.lost << " covered pixels not won by instance " << winner);
}

}  // namespace

TEST_CASE(
    "visibility ties: coincident copies resolve to the last one, every run, every rasterizer") {
  const MeshSource box = box_mesh();
  const Vector<f32> copies(k_copies, 0.0f);
  const std::span<const f32> paddings(copies.data(), copies.size());
  const u32 winner = k_copies - 1;
  RenderSettings base;
  base.shadows = ShadowMode::Off;
  base.occlusion = false;

  Drawn mesh_path, indexed, software, ray;
  Drawn capacity;
  bool mesh_shaders = false;
  bool ray_queries = false;
  {
    Gpu gpu;
    if (!gpu.ok) {
      MESSAGE("renderer unavailable here: " << gpu.why);
      return;
    }
    mesh_shaders = gpu.device.features().mesh_shader;
    ray_queries = gpu.device.features().ray_query;
    RenderSettings s = base;
    if (mesh_shaders) {
      s.raster = RasterMode::Hardware;
      draw_repeatedly(gpu.device, "mesh", box, paddings, s, winner, mesh_path);
    }
    s.raster = RasterMode::Vertex;
    draw_repeatedly(gpu.device, "vertex (indexed where the device can)", box, paddings, s, winner,
                    indexed);
    s.raster = RasterMode::Software;
    draw_repeatedly(gpu.device, "software", box, paddings, s, winner, software);
    if (ray_queries) {
      s.raster = RasterMode::RayTrace;
      draw_repeatedly(gpu.device, "ray", box, paddings, s, winner, ray);
    }
  }
  {
    // The capacity draw is the vertex path on a device without geometryShader. One live device at
    // a time (docs/subsystems/gfx.md, "One live device per process"), so it gets a scope of its
    // own.
    gfx::DeviceOptions options;
    options.overrides.absent.push_back("VK_EXT_mesh_shader");
    options.overrides.absent.push_back("geometryShader");
    Gpu gpu(options);
    REQUIRE_MESSAGE(gpu.ok, gpu.why);
    RenderSettings s = base;
    s.raster = RasterMode::Vertex;
    draw_repeatedly(gpu.device, "vertex capacity draw", box, paddings, s, winner, capacity);
  }

  // Every run of every rasterizer, and of the ray path, draws the same picture, and the rule's
  // winner holds every pixel.
  for (const Drawn* d : {&mesh_path, &indexed, &capacity, &software, &ray}) {
    if (!d->ran) continue;
    CAPTURE(d->name);
    CHECK(d->unstable == 0);
    CHECK(d->lost == 0);
  }
  // The hardware rasterizers draw the same words: the same coverage, the same depth bits, and at a
  // tie the same winner.
  REQUIRE(indexed.ran);
  REQUIRE(capacity.ran);
  const Difference capacity_vs_indexed = compare(indexed.first, capacity.first);
  CHECK(capacity_vs_indexed.total() == 0);
  if (mesh_path.ran) {
    const Difference mesh_vs_indexed = compare(mesh_path.first, indexed.first);
    CHECK(mesh_vs_indexed.total() == 0);
  }
  // The software rasterizer's edge rule and depth arithmetic are its own (visibility_tests.cpp
  // bounds how far), and the ray path's depth comes from the hit distance rather than from
  // interpolation, so those two are compared by winner only, above. The ray path gets there by
  // looking again at the tied distance (ray_visibility.slang): its traversal alone commits
  // whichever copy the acceleration structure meets first.
}

TEST_CASE("visibility ties: a tie between the two occlusion passes resolves as one pass does") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const MeshSource box = box_mesh();
  // Instance 0 is plain; instance 1's spheres are 40 units larger, which puts them in front of a
  // camera that faces away from the box.
  const f32 paddings[2] = {0.0f, 40.0f};
  RenderSettings on;
  on.shadows = ShadowMode::Off;
  on.raster = gpu.device.features().mesh_shader ? RasterMode::Hardware : RasterMode::Vertex;
  RenderSettings off = on;
  off.occlusion = false;
  Rig with;
  Rig without;
  REQUIRE_MESSAGE(with.build(gpu.device, box, paddings, on), (with.error + with.unavailable));
  REQUIRE_MESSAGE(without.build(gpu.device, box, paddings, off),
                  (without.error + without.unavailable));
  REQUIRE(with.resolved.occlusion);
  REQUIRE_FALSE(without.resolved.occlusion);

  // Frame 0 looks away: the plain copy is behind the camera and culled, the padded one's spheres
  // reach in front of it and pass, so its clusters the cone test keeps are drawn — onto no pixel —
  // and are what frame 1's pass 1 draws.
  const u32 clusters = with.data.parts[0].cluster_count;
  const FrameDesc away = frame_at(k_eye, k_eye * 2.0f);
  const FrameDesc toward = frame_at(k_eye, Vec3{0.0f, 0.0f, 0.0f});
  CaptureChannels channels = all_channels();
  std::string error;
  CapturedFrame a0, b0, a, b;
  REQUIRE_MESSAGE(with.renderer.capture(away, channels, a0, &error), error);
  REQUIRE_MESSAGE(without.renderer.capture(away, channels, b0, &error), error);
  CHECK(a0.covered == 0);
  const u32 padded_drawn = with.renderer.stats().visible_pairs();
  CHECK(padded_drawn > 0);
  CHECK(padded_drawn < clusters);  // the back faces' clusters are cone-culled
  REQUIRE_MESSAGE(with.renderer.capture(toward, channels, a, &error), error);
  REQUIRE_MESSAGE(without.renderer.capture(toward, channels, b, &error), error);
  const Stats& s = with.renderer.stats();
  MESSAGE("frame 0 drew " << padded_drawn << " of the padded copy's " << clusters
                          << " clusters; frame 1's pass 1 drew " << s.visible_hw
                          << " pairs and pass 2 " << s.visible_pass2 << "; " << a.covered
                          << " covered pixels");
  // The construction: pass 1 drew the padded copy's front clusters and pass 2 the plain copy's
  // same clusters, so every covered pixel is a tie between the two passes.
  REQUIRE(s.visible_hw == padded_drawn);
  REQUIRE(s.visible_pass2 == padded_drawn);
  REQUIRE(a.covered > 1000);
  const Difference d = compare(a, b);
  MESSAGE("occlusion on against off: " << d.ids << " id, " << d.depth << " depth and " << d.color
                                       << " colour pixels differ; " << not_won_by(a, 1)
                                       << " covered pixels not won by the padded copy with it, "
                                       << not_won_by(b, 1) << " without");
  CHECK(d.total() == 0);
  CHECK(not_won_by(a, 1) == 0);
  CHECK(not_won_by(b, 1) == 0);
}

TEST_CASE("visibility ties: back-to-back faces resolve the same way every run and with occlusion") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const MeshSource sheet = two_sided_mesh();
  const f32 paddings[1] = {0.0f};
  RenderSettings on;
  on.shadows = ShadowMode::Off;
  on.cone = false;  // a curled frond's clusters have no usable cone either
  on.raster = gpu.device.features().mesh_shader ? RasterMode::Hardware : RasterMode::Vertex;
  RenderSettings off = on;
  off.occlusion = false;
  Rig with;
  Rig without;
  REQUIRE_MESSAGE(with.build(gpu.device, sheet, paddings, on), (with.error + with.unavailable));
  REQUIRE_MESSAGE(without.build(gpu.device, sheet, paddings, off),
                  (without.error + without.unavailable));
  const FrameDesc frame = frame_at(Vec3{2.5f, 1.0f, 4.5f}, Vec3{0.0f, 0.0f, 0.0f});
  std::string error;
  CapturedFrame first;
  REQUIRE_MESSAGE(without.renderer.capture(frame, all_channels(), first, &error), error);
  REQUIRE(first.covered > 1000);
  u32 unstable = 0;
  for (u32 r = 0; r < k_repeats; ++r) {
    CapturedFrame again;
    REQUIRE_MESSAGE(without.renderer.capture(frame, all_channels(), again, &error), error);
    unstable += compare(first, again).total() > 0 ? 1u : 0u;
    REQUIRE_MESSAGE(with.renderer.capture(frame, all_channels(), again, &error), error);
    unstable += compare(first, again).total() > 0 ? 1u : 0u;
  }
  MESSAGE(first.covered << " covered pixels; " << unstable << " of " << 2 * k_repeats
                        << " captures with and without occlusion culling differ from the first");
  CHECK(unstable == 0);
}
