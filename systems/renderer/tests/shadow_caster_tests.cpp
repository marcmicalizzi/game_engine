// Shadow casters: a cluster the normal-cone test keeps out of the picture still casts its shadow.
//
// The cull pass drops a cluster whose triangles all face away from the **camera**, and the shadow
// rays trace the acceleration structures the frame builds from the cull output, so until
// 2026-09-24 such a cluster stopped casting as well — which is not a statement about the **light**
// at all. The fixture puts a caster between the sun and a ground plane with the camera on the far
// side, so the caster's lit face is the face the camera cannot see, and draws the ground's shadow
// three ways: cones off (the shadow the caster really casts), cones on with the casters dropped
// (what the renderer did), and cones on with the casters kept (what it does now). The comparison is
// the **set of shadowed ground pixels**, read through the resolve's shadow view
// (`gfx::ResolveMode::Shadow`: black where the sun's shadow ray is blocked), over the pixels that
// are ground in both pictures — cones off draws the back of a single-sided card, which covers some
// ground.
//
// Four casters, one scene each (docs/subsystems/geometry.md, "Normal cones", has the counts):
//
//   card    a single-sided square facing the sun: the whole shadow goes with the cone test.
//   slab    a closed box six hundredths thick. A ray that enters by the bottom and leaves by the
//           lit face crosses only faces the camera cannot see, but the bottom is too thin to have
//           a cluster of its own, so from this camera nothing is lost.
//   cube    a closed floating cube: **the convex case can lose too**. Lit from one side and seen
//           from the other and from above, its bottom and its lit face are both back-facing, and
//           a ray that enters by the bottom leaves by the lit face; where both are clusters with a
//           cone of their own, that part of its shadow goes.
//   ell     a closed L: a post with a foot reaching towards the sun, the concave case.
//
// The meshes are the LOD builder's leaves made into roots rather than the DAG it builds: a flat
// face simplifies with no error at all, so a built DAG draws its root — one cluster spanning every
// face, with no cone to test — and the case would not exist at any threshold. Real content has
// curvature, and its leaves have cones; the samples' numbers are in geometry.md.
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
#include <test_paths.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::renderer;

namespace {

// A device, or nothing, exactly as the renderer's other cases open one.
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

// ---- the fixture's geometry ------------------------------------------------------------------
//
// Everything is authored in a frame whose +x points at the sun along the ground and whose +y is up;
// each instance's world transform turns that frame about y onto the renderer's fixed sun
// (`frame_lighting`), so in the fixture's own frame the sun is `(sin e, cos e, 0)`-ish and a point
// at height h throws its shadow `h * 0.753` towards -x. The camera stands on the -x side, above.

struct MeshSource {
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<u32> indices;
};

// One face: the parallelogram `origin + s * du + t * dv`, split into `nu` by `nv` quads and wound
// counter-clockwise seen from `du x dv`, which is therefore its outward normal.
void add_face(MeshSource& m, Vec3 origin, Vec3 du, Vec3 dv) {
  const u32 nu = std::max(1u, static_cast<u32>(std::lround(length(du) * 8.0f)));
  const u32 nv = std::max(1u, static_cast<u32>(std::lround(length(dv) * 8.0f)));
  const Vec3 normal = normalize(cross(du, dv));
  const u32 base = m.positions.size();
  for (u32 j = 0; j <= nv; ++j) {
    for (u32 i = 0; i <= nu; ++i) {
      const f32 s = static_cast<f32>(i) / static_cast<f32>(nu);
      const f32 t = static_cast<f32>(j) / static_cast<f32>(nv);
      m.positions.push_back(origin + du * s + dv * t);
      m.normals.push_back(normal);
    }
  }
  for (u32 j = 0; j < nv; ++j) {
    for (u32 i = 0; i < nu; ++i) {
      const u32 a = base + j * (nu + 1) + i;
      const u32 b = a + 1;
      const u32 c = a + nu + 2;
      const u32 d = a + nu + 1;
      const u32 quad[6] = {a, b, c, a, c, d};
      for (const u32 q : quad)
        m.indices.push_back(q);
    }
  }
}

// A closed prism: the counter-clockwise polygon `profile` in the xy plane, extruded from z0 to z1.
// The caps are handed in as rectangles covering the profile exactly, which is all an L needs.
struct Rect {
  f32 x0, y0, x1, y1;
};
void add_prism(MeshSource& m, std::span<const Vec2> profile, std::span<const Rect> caps, f32 z0,
               f32 z1) {
  for (u32 i = 0; i < profile.size(); ++i) {
    const Vec2 a = profile[i];
    const Vec2 b = profile[(i + 1) % profile.size()];
    add_face(m, Vec3{a.x, a.y, z0}, Vec3{b.x - a.x, b.y - a.y, 0.0f}, Vec3{0.0f, 0.0f, z1 - z0});
  }
  for (const Rect& r : caps) {
    const f32 dx = r.x1 - r.x0;
    const f32 dy = r.y1 - r.y0;
    add_face(m, Vec3{r.x0, r.y0, z1}, Vec3{dx, 0.0f, 0.0f}, Vec3{0.0f, dy, 0.0f});  // +z
    add_face(m, Vec3{r.x0, r.y0, z0}, Vec3{0.0f, dy, 0.0f}, Vec3{dx, 0.0f, 0.0f});  // -z
  }
}

void add_box(MeshSource& m, f32 x0, f32 y0, f32 z0, f32 x1, f32 y1, f32 z1) {
  const Vec2 profile[4] = {Vec2{x0, y0}, Vec2{x1, y0}, Vec2{x1, y1}, Vec2{x0, y1}};
  const Rect cap{x0, y0, x1, y1};
  add_prism(m, profile, std::span<const Rect>(&cap, 1), z0, z1);
}

enum class Caster : u8 { card, slab, cube, ell };
constexpr Caster k_casters[4] = {Caster::card, Caster::slab, Caster::cube, Caster::ell};

const char* caster_name(Caster c) {
  switch (c) {
    case Caster::card: return "card";
    case Caster::slab: return "slab";
    case Caster::cube: return "cube";
    case Caster::ell: return "ell";
  }
  return "?";
}

MeshSource caster_mesh(Caster c) {
  MeshSource m;
  switch (c) {
    case Caster::card:  // one sided, facing the sun: +x
      add_face(m, Vec3{0.0f, 0.5f, -0.5f}, Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f});
      break;
    case Caster::slab: add_box(m, -0.03f, 0.5f, -0.5f, 0.03f, 1.5f, 0.5f); break;
    case Caster::cube: add_box(m, -0.5f, 0.5f, -0.5f, 0.5f, 1.5f, 0.5f); break;
    case Caster::ell: {
      // A post, and a foot reaching from its base towards the sun; counter-clockwise from +z.
      const Vec2 profile[6] = {Vec2{-0.15f, 0.4f}, Vec2{1.05f, 0.4f}, Vec2{1.05f, 0.7f},
                               Vec2{0.15f, 0.7f},  Vec2{0.15f, 1.6f}, Vec2{-0.15f, 1.6f}};
      const Rect caps[2] = {Rect{-0.15f, 0.4f, 0.15f, 1.6f}, Rect{0.15f, 0.4f, 1.05f, 0.7f}};
      add_prism(m, profile, caps, -0.5f, 0.5f);
      break;
    }
  }
  return m;
}

MeshSource ground_mesh() {
  MeshSource m;
  add_face(m, Vec3{-4.0f, 0.0f, -3.0f}, Vec3{0.0f, 0.0f, 6.0f}, Vec3{7.0f, 0.0f, 0.0f});  // +y
  return m;
}

// The LOD builder's own leaves — the clusters real content is drawn from up close, with the cones
// it fits them — made into a "DAG" in which every leaf is a root: own error 0, no parent. That is
// the cut every threshold selects, so the pictures below are the leaves and nothing else. The
// levels above them stay in the streams, unreferenced.
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

// The fixture frame's rotation: +x onto the sun's direction along the ground, +y up.
Mat4 fixture_frame() {
  const Vec3 sun = normalize(Vec3{0.4f, 0.8f, 0.45f});  // frame_lighting's, which is fixed
  const Vec3 h = normalize(Vec3{sun.x, 0.0f, sun.z});
  Mat4 m = Mat4::identity();
  m.c[0] = Vec4{h.x, 0.0f, h.z, 0.0f};
  m.c[1] = Vec4{0.0f, 1.0f, 0.0f, 0.0f};
  m.c[2] = Vec4{-h.z, 0.0f, h.x, 0.0f};
  return m;
}

Vec3 to_world(const Mat4& frame, Vec3 local) {
  const Vec4 w = frame * Vec4{local, 1.0f};
  return Vec3{w.x, w.y, w.z};
}

// The camera, in the fixture's frame: on the dark side of every caster, above, a little off axis.
const Vec3 k_eye_local{-6.0f, 3.0f, 1.2f};
const Vec3 k_target_local{-0.6f, 0.5f, 0.0f};

// Ground (instance 0) and one caster (instance 1), by hand for the reason the file header gives.
bool make_scene(Caster caster, SceneData& out, std::string& error) {
  geometry::ClusterLodMesh parts[2];
  if (!leaves_as_roots(ground_mesh(), parts[0], error)) return false;
  if (!leaves_as_roots(caster_mesh(caster), parts[1], error)) return false;
  if (!geometry::merge_cluster_meshes(std::span<const geometry::ClusterLodMesh>(parts, 2), out.lod,
                                      out.parts, &error)) {
    return false;
  }
  const Mat4 frame = fixture_frame();
  for (u32 i = 0; i < 2; ++i) {
    SourceMesh source;
    source.part_material.push_back(-1);  // the default material: one plain grey for everything
    source.part_of_cluster.resize(out.parts[i].cluster_count, 0u);
    out.sources.push_back(std::move(source));
    gfx::InstanceDesc instance{};
    gfx::set_instance_transform(instance, frame);
    instance.mesh = i;
    instance.first_pair = out.pair_count;
    out.pair_count += out.parts[i].cluster_count;
    out.instances.push_back(instance);
    out.instance_joints.push_back(0);
  }
  update_scene_bounds(out);
  return true;
}

// How many of the caster's clusters the cone test drops from the fixture's camera, on the CPU with
// the shader's own arithmetic (`geometry::cluster_backfacing`), in the caster's own frame.
u32 cone_culled(const SceneData& data, u32& clusters) {
  const geometry::ClusterMeshPart& part = data.parts[1];
  clusters = part.cluster_count;
  u32 culled = 0;
  for (u32 c = 0; c < part.cluster_count; ++c) {
    const geometry::ClusterDesc& desc = data.lod.mesh.clusters[part.first_cluster + c];
    culled += geometry::cluster_backfacing(desc, k_eye_local) ? 1u : 0u;
  }
  return culled;
}

constexpr u32 k_width = 320;
constexpr u32 k_height = 240;
constexpr u32 k_ground = 0;  // the ground's instance

struct Shot {
  CapturedFrame frame;
  u32 casters = 0;  // Stats::shadow_casters
  u32 visible = 0;  // Stats::visible_pairs()
  bool available = true;
  std::string why;
};

// One picture of the scene in the shadow view, with its ids, under the settings given.
Shot render(const gfx::Device& device, Caster caster, RasterMode raster, bool cone,
            bool shadow_casters) {
  Shot shot;
  SceneData data;
  std::string error;
  REQUIRE_MESSAGE(make_scene(caster, data, error), error);
  RenderSettings settings;
  settings.raster = raster;
  settings.shadows = ShadowMode::RayTraced;
  settings.cone = cone;
  settings.shadow_casters = shadow_casters;
  settings.lights = false;  // the sun alone: its shadow is what the shadow view shows
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  const RenderAvailability availability = check_availability(resolved, device.features());
  if (availability != RenderAvailability::Ok) {
    shot.available = false;
    shot.why = unavailable_reason(availability, device);
    return shot;
  }
  CHECK(resolved.casters == (cone && shadow_casters));
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  SceneRenderer renderer;
  SceneRenderer::Desc desc;
  desc.width = k_width;
  desc.height = k_height;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, desc, &error), error);
  const Mat4 frame_rotation = fixture_frame();
  FrameDesc frame;
  frame.camera.position = to_world(frame_rotation, k_eye_local);
  frame.camera.target = to_world(frame_rotation, k_target_local);
  frame.view_mode = static_cast<u32>(gfx::ResolveMode::Shadow);
  CaptureChannels channels;
  channels.color = true;
  channels.ids = true;
  REQUIRE_MESSAGE(renderer.capture(frame, channels, shot.frame, &error), error);
  shot.casters = renderer.stats().shadow_casters;
  shot.visible = renderer.stats().visible_pairs();
  return shot;
}

bool is_ground(const Shot& s, u32 p) { return s.frame.ids[u64{p} * k_id_words] == k_ground; }
bool shadowed(const Shot& s, u32 p) { return s.frame.color[u64{p} * 4] == 0; }

struct Compared {
  u32 ground = 0;    // pixels that are ground in both pictures
  u32 shadow_a = 0;  // of those, shadowed in the first
  u32 shadow_b = 0;  // and in the second
  u32 differ = 0;    // shadowed in one and not the other
};

u32 id_differences(const Shot& a, const Shot& b) {
  u32 out = 0;
  for (u32 w = 0; w < a.frame.ids.size() && w < b.frame.ids.size(); ++w)
    out += a.frame.ids[w] != b.frame.ids[w] ? 1u : 0u;
  return out + (a.frame.ids.size() != b.frame.ids.size() ? 1u : 0u);
}

Compared compare_ground(const Shot& a, const Shot& b) {
  Compared out;
  for (u32 p = 0; p < k_width * k_height; ++p) {
    if (!is_ground(a, p) || !is_ground(b, p)) continue;
    ++out.ground;
    const bool sa = shadowed(a, p);
    const bool sb = shadowed(b, p);
    out.shadow_a += sa ? 1u : 0u;
    out.shadow_b += sb ? 1u : 0u;
    out.differ += sa != sb ? 1u : 0u;
  }
  return out;
}

}  // namespace

TEST_CASE("renderer: a cluster the cone test drops from the picture still casts its shadow") {
  // The witness, on the CPU and so on every machine: from the fixture's camera the cone test drops
  // clusters of every caster, and all of the card's. Without that the comparison below could not
  // fail, and a test that cannot fail says nothing.
  for (const Caster caster : k_casters) {
    SceneData data;
    std::string error;
    REQUIRE_MESSAGE(make_scene(caster, data, error), error);
    u32 clusters = 0;
    const u32 culled = cone_culled(data, clusters);
    MESSAGE(std::string(caster_name(caster))
            << ": " << culled << " of " << clusters << " clusters face away from the camera");
    CHECK(culled > 0);
    if (caster == Caster::card) CHECK(culled == clusters);
  }

  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  // Both raster paths that trace shadows: the mesh shaders and the vertex path. They count their
  // drawn pairs in different words of their argument blocks, and the casters in neither, so both
  // are asked. `hw` on a device without mesh shaders is the vertex path, which has its own turn.
  const RasterMode modes[2] = {RasterMode::Hardware, RasterMode::Vertex};
  for (const RasterMode mode : modes) {
    if (mode == RasterMode::Hardware && !gpu.device.features().mesh_shader) {
      MESSAGE("hw path unavailable here: " << gpu.device.adapter().name
                                           << " has no VK_EXT_mesh_shader");
      continue;
    }
    for (const Caster caster : k_casters) {
      const Shot off = render(gpu.device, caster, mode, false, true);
      if (!off.available) {
        MESSAGE("ray-traced shadows unavailable here: " << off.why);
        return;
      }
      const Shot dropped = render(gpu.device, caster, mode, true, false);
      const Shot kept = render(gpu.device, caster, mode, true, true);
      const Compared lost = compare_ground(off, dropped);
      const Compared fixed = compare_ground(off, kept);
      MESSAGE(std::string(raster_name(mode))
              << " " << std::string(caster_name(caster)) << ": ground " << lost.ground
              << " px; shadowed with cones off " << lost.shadow_a << ", cones on without casters "
              << lost.shadow_b << " (" << lost.differ << " differ), with " << kept.casters
              << " casters " << fixed.shadow_b << " (" << fixed.differ << " differ); visible pairs "
              << off.visible << " / " << dropped.visible << " / " << kept.visible);
      CHECK(lost.shadow_a > 200);  // the caster throws a real shadow onto the ground in view
      // Dropping the casters loses the card's whole shadow and part of the closed cube's. The slab
      // and the L keep theirs from this camera: every ray that reaches them crosses a cluster the
      // camera can see, because their back faces share clusters with faces that it can.
      if (caster == Caster::card) CHECK(lost.shadow_b == 0);
      if (caster == Caster::card || caster == Caster::cube) {
        CHECK(lost.differ > 0);
        CHECK(lost.shadow_b < lost.shadow_a);
      }
      // And the casters restore every pixel of it, with the picture's own cut: the kept frame draws
      // exactly the pairs the dropped one does, and builds the rest only into the shadows.
      CHECK(fixed.differ == 0);
      CHECK(kept.casters > 0);
      CHECK(kept.visible == dropped.visible);
      CHECK(kept.visible + kept.casters == off.visible);
      // The ids of the two cones-on pictures are the same words: a caster is never drawn.
      CHECK(id_differences(kept, dropped) == 0);
    }
  }
}

TEST_CASE("renderer: the ray path traces the casters' shadows and not the casters") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  // The card: every cluster of it is a caster, so it is the case where a primary ray that saw the
  // casters would draw something the raster path does not.
  const Shot dropped = render(gpu.device, Caster::card, RasterMode::RayTrace, true, false);
  if (!dropped.available) {
    MESSAGE("rt path unavailable here: " << dropped.why);
    return;
  }
  const Shot kept = render(gpu.device, Caster::card, RasterMode::RayTrace, true, true);
  const Shot off = render(gpu.device, Caster::card, RasterMode::RayTrace, false, true);
  const u32 differing_ids = id_differences(kept, dropped);
  const Compared lost = compare_ground(off, dropped);
  const Compared fixed = compare_ground(off, kept);
  MESSAGE("rt path, card: " << differing_ids << " id words differ with the casters in the "
                            << "structures; shadowed ground " << lost.shadow_a << " cones off, "
                            << lost.shadow_b << " without casters, " << fixed.shadow_b << " with "
                            << kept.casters);
  // The primary rays cull non-opaque geometry, and a caster is the only non-opaque geometry.
  CHECK(differing_ids == 0);
  CHECK(kept.casters > 0);
  CHECK(lost.differ > 0);
  CHECK(fixed.differ == 0);
}

TEST_CASE("renderer: the casters are on only where they mean something") {
  gfx::DeviceFeatures features;
  features.mesh_shader = true;
  features.buffer_int64_atomics = true;
  features.acceleration_structure = true;
  features.cluster_acceleration_structure = true;
  features.ray_query = true;
  ResolvedSettings resolved;
  RenderSettings settings;
  settings.shadows = ShadowMode::RayTraced;
  resolve_settings(settings, features, nullptr, resolved);
  CHECK(resolved.casters);  // the default: shadows traced, cones on
  settings.cone = false;
  resolve_settings(settings, features, nullptr, resolved);
  CHECK_FALSE(resolved.casters);  // nothing is cone-culled
  settings.cone = true;
  settings.shadow_casters = false;
  resolve_settings(settings, features, nullptr, resolved);
  CHECK_FALSE(resolved.casters);  // asked not to
  settings.shadow_casters = true;
  settings.shadows = ShadowMode::Off;
  settings.raster = RasterMode::RayTrace;
  resolve_settings(settings, features, nullptr, resolved);
  CHECK_FALSE(resolved.casters);  // the ray path without shadows has no shadow ray to serve
  settings.shadows = ShadowMode::RayTraced;
  settings.rt_templates = true;
  resolve_settings(settings, features, nullptr, resolved);
  CHECK_FALSE(resolved.casters);  // a template's flags are opaque: a caster could not be hidden
  settings.rt_templates = false;
  features.ray_query = false;
  settings.shadows = ShadowMode::Auto;
  settings.raster = RasterMode::Hardware;
  resolve_settings(settings, features, nullptr, resolved);
  CHECK_FALSE(resolved.casters);  // no shadows at all
}

// ---- the Khronos samples: what the casters restore, and what they cost ------------------------
//
// The measurement the decision rests on (docs/subsystems/geometry.md, "Normal cones"), kept as code
// so it can be re-taken rather than re-argued. Skipped by default: it needs the samples
// (`tools/fetch-samples.ps1`), it renders a few hundred frames, and it asserts only what the
// fixture above already pins. Run it, in a release build and under the GPU lock, with
//
//     engine_renderer_tests -tc='renderer: shadow casters on the Khronos samples' -ns -s
//
// Every sample is drawn alone and standing on a ground plane (the realistic case for a cast
// shadow), from eight yaws of the orbit engine-view uses, three ways: cones off, cones on with the
// casters dropped, cones on with them kept. The pixel counts compare the two cones-on frames, whose
// ids are the same words, so any pixel that differs is a shadow and nothing else.
namespace {

void put_u32(std::vector<u8>& out, u32 v) {
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((v >> (8 * i)) & 0xffu));
}

void put_f32(std::vector<u8>& out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  put_u32(out, bits);
}

// A unit square on y = 0 facing +y, as a GLB: the ground a sample stands on.
bool write_ground_glb(const std::string& path) {
  std::vector<u8> bin;
  const f32 corners[4][2] = {{-0.5f, -0.5f}, {-0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, -0.5f}};
  for (const auto& c : corners) {
    put_f32(bin, c[0]);
    put_f32(bin, 0.0f);
    put_f32(bin, c[1]);
  }
  for (u32 i = 0; i < 4; ++i) {
    put_f32(bin, 0.0f);
    put_f32(bin, 1.0f);
    put_f32(bin, 0.0f);
  }
  const u32 index_offset = static_cast<u32>(bin.size());
  const u32 indices[6] = {0, 1, 2, 0, 2, 3};  // counter-clockwise seen from +y
  for (const u32 i : indices)
    put_u32(bin, i);
  const std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,"
      "\"NORMAL\":1},\"indices\":2,\"material\":0}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.6,0.6,0.6,1],"
      "\"metallicFactor\":0,\"roughnessFactor\":0.9}}],\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\","
      "\"min\":[-0.5,0,-0.5],\"max\":[0.5,0,0.5]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5125,\"count\":6,\"type\":\"SCALAR\"}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":" +
      std::to_string(index_offset) +
      ",\"byteLength\":24}],\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]}";
  std::string padded = json;
  while (padded.size() % 4 != 0)
    padded += ' ';
  std::vector<u8> glb;
  put_u32(glb, 0x46546c67u);  // "glTF"
  put_u32(glb, 2u);
  put_u32(glb, static_cast<u32>(12 + 8 + padded.size() + 8 + bin.size()));
  put_u32(glb, static_cast<u32>(padded.size()));
  put_u32(glb, 0x4e4f534au);  // "JSON"
  glb.insert(glb.end(), padded.begin(), padded.end());
  put_u32(glb, static_cast<u32>(bin.size()));
  put_u32(glb, 0x004e4942u);  // "BIN\0"
  glb.insert(glb.end(), bin.begin(), bin.end());
  std::ofstream f(path, std::ios::binary);
  if (!f.is_open()) return false;
  f.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
  return f.good();
}

// One configuration of the three, alive for a whole sample so every yaw is drawn by the same GPU
// scene.
struct SampleRig {
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  bool open(const gfx::Device& device, const SceneData& data, bool cone, bool casters, u32 width,
            u32 height, std::string& error) {
    RenderSettings settings;
    settings.raster = RasterMode::Hardware;
    settings.shadows = ShadowMode::RayTraced;
    settings.cone = cone;
    settings.shadow_casters = casters;
    resolve_settings(settings, device.features(), &data, resolved);
    if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
      error = "unavailable";
      return false;
    }
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc desc;
    desc.width = width;
    desc.height = height;
    return renderer.create(device, scene, resolved, desc, &error);
  }
};

}  // namespace

TEST_CASE("renderer: shadow casters on the Khronos samples" * doctest::skip()) {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  if (!gpu.device.features().cluster_acceleration_structure || !gpu.device.features().ray_query) {
    MESSAGE("ray-traced shadows unavailable here");
    return;
  }
  test::TempDir dir("shadow-casters");
  const std::string ground = dir.file("ground.glb");
  REQUIRE(write_ground_glb(ground));
  const char* k_samples[] = {"FlightHelmet/FlightHelmet.gltf",
                             "SciFiHelmet/SciFiHelmet.gltf",
                             "BoomBox/BoomBox.glb",
                             "Lantern/Lantern.glb",
                             "Corset/Corset.glb",
                             "Avocado/Avocado.glb",
                             "Suzanne/Suzanne.gltf",
                             "Fox/Fox.glb",
                             "RiggedFigure/RiggedFigure.glb"};
  constexpr u32 k_w = 1280;
  constexpr u32 k_h = 720;
  constexpr u32 k_yaws = 8;
  constexpr u32 k_timed_frames = 240;
  for (const char* relative : k_samples) {
    const std::string path =
        test::data_path(std::string(ENGINE_SOURCE_DIR "/content/samples/") + relative,
                        std::string("content/samples/") + relative);
    if (!test::path_exists(path)) {
      MESSAGE("skipped, not fetched (tools/fetch-samples.ps1): " << path);
      continue;
    }
    const std::string sample(relative);
    // The sample alone (its shadows fall on itself), standing on a ground six radii wide (the
    // realistic case for a cast shadow), and — for the FlightHelmet — the 3x3 grid engine-view's
    // `--grid-instances 3` draws, where a helmet's casters could shadow its neighbours. The camera
    // frames the sample's own bounds, not the ground's.
    for (u32 variant = 0; variant < 3; ++variant) {
      if (variant == 2 && sample.find("FlightHelmet") == std::string::npos) continue;
      SceneDesc desc;
      desc.meshes.push_back(path);
      desc.ddc = dir.path() + "/ddc";
      std::string error;
      SceneData probe;
      REQUIRE_MESSAGE(load_scene(desc, probe, error), error);
      if (variant == 1) {
        f32 lowest = 1e30f;
        for (const Vec3& v : probe.lod.mesh.vertices)
          lowest = std::min(lowest, v.y);
        desc.meshes.push_back(ground);
        SceneInstance model;
        model.mesh = 0;
        desc.instances.push_back(model);
        SceneInstance floor;
        floor.mesh = 1;
        floor.transform.position = Vec3{probe.center.x, lowest, probe.center.z};
        floor.transform.scale = Vec3{6.0f * probe.radius, 1.0f, 6.0f * probe.radius};
        desc.instances.push_back(floor);
      }
      if (variant == 2) desc.grid_instances = 3;
      SceneData data;
      REQUIRE_MESSAGE(load_scene(desc, data, error), error);
      const Vec3 focus = variant == 1 ? probe.center : data.center;
      const f32 reach = variant == 1 ? probe.radius : data.radius;
      const std::string where =
          sample + (variant == 0 ? " alone" : (variant == 1 ? " on a ground" : " 3x3 grid"));

      SampleRig rigs[3];  // cones off; cones on, casters dropped; cones on, casters kept
      const bool cones[3] = {false, true, true};
      const bool keep[3] = {true, false, true};
      for (u32 k = 0; k < 3; ++k)
        REQUIRE_MESSAGE(rigs[k].open(gpu.device, data, cones[k], keep[k], k_w, k_h, error), error);

      // Eight yaws at engine-view's pitch, and eight more low over the horizon, which is where a
      // camera looks back towards the sun under an object.
      const f32 pitches[2] = {k_orbit_pitch, 0.12f};
      u32 worst_view = 0;
      u32 worst_casters = 0;
      u64 total_sun = 0;
      u64 total_shaded = 0;
      u64 total_restore = 0;
      u64 total_ids = 0;
      u64 total_casters = 0;
      u64 total_pairs_on = 0;
      for (u32 view = 0; view < 2 * k_yaws; ++view) {
        const u32 y = view % k_yaws;
        const f32 pitch = pitches[view / k_yaws];
        const f32 yaw = k_two_pi * static_cast<f32>(y) / static_cast<f32>(k_yaws);
        FrameDesc frame;
        frame.camera = orbit_camera_at(focus, reach, 0.0f, yaw, pitch);
        CaptureChannels channels;
        channels.ids = true;
        CapturedFrame shadow[3];
        CapturedFrame shaded[3];
        u32 pairs[3] = {};
        u32 casters = 0;
        for (u32 k = 0; k < 3; ++k) {
          frame.view_mode = static_cast<u32>(gfx::ResolveMode::Shadow);
          REQUIRE_MESSAGE(rigs[k].renderer.capture(frame, channels, shadow[k], &error), error);
          pairs[k] = rigs[k].renderer.stats().visible_pairs();
          if (k == 2) casters = rigs[k].renderer.stats().shadow_casters;
          frame.view_mode = static_cast<u32>(gfx::ResolveMode::Shaded);
          REQUIRE_MESSAGE(rigs[k].renderer.capture(frame, channels, shaded[k], &error), error);
        }
        // Dropped against kept. Their ids are the same words except where two surfaces tie in
        // depth and the atomic's order picks one (a coincident pair of the FlightHelmet's, for
        // one), so a pixel whose ids agree and whose colour does not is a shadow and nothing else.
        u32 ids_differ = 0;
        u32 sun_differ = 0;
        u32 shaded_differ = 0;
        u32 sun_shadowed = 0;
        u32 restore_differ = 0;  // kept against cones off, where both show the same triangle
        const u32 pixels = k_w * k_h;
        for (u32 p = 0; p < pixels; ++p) {
          bool same_ids = true;
          bool same_as_off = true;
          for (u32 c = 0; c < k_id_words; ++c) {
            const u32 w = p * k_id_words + c;
            same_ids = same_ids && shadow[1].ids[w] == shadow[2].ids[w];
            same_as_off = same_as_off && shadow[0].ids[w] == shadow[2].ids[w];
          }
          ids_differ += same_ids ? 0u : 1u;
          sun_shadowed += shadow[2].color[u64{p} * 4] == 0 ? 1u : 0u;
          if (!same_ids) continue;
          sun_differ += shadow[1].color[u64{p} * 4] != shadow[2].color[u64{p} * 4] ? 1u : 0u;
          bool shade_same = true;
          for (u32 c = 0; c < 3; ++c)
            shade_same =
                shade_same && shaded[1].color[u64{p} * 4 + c] == shaded[2].color[u64{p} * 4 + c];
          shaded_differ += shade_same ? 0u : 1u;
          if (same_as_off && shadow[2].ids[p * k_id_words] != k_no_id) {
            restore_differ += shadow[0].color[u64{p} * 4] != shadow[2].color[u64{p} * 4] ? 1u : 0u;
          }
        }
        MESSAGE(where << " yaw " << y * 45 << " pitch " << pitch << ": pairs " << pairs[0]
                      << " off / " << pairs[1] << " on, " << casters << " casters; covered "
                      << shadow[2].covered << ", sun-shadowed " << sun_shadowed
                      << "; casters change the sun's shadow on " << sun_differ
                      << " px and the shaded picture on " << shaded_differ << " px; ids differ "
                      << ids_differ << "; kept vs cones off differ on " << restore_differ);
        total_sun += sun_differ;
        total_shaded += shaded_differ;
        total_restore += restore_differ;
        total_ids += ids_differ;
        total_casters += casters;
        total_pairs_on += pairs[1];
        if (casters > worst_casters) {
          worst_casters = casters;
          worst_view = view;
        }
      }
      MESSAGE(where << ", all 16 views: casters " << total_casters << " beside " << total_pairs_on
                    << " drawn pairs; sun " << total_sun << " px, shaded " << total_shaded
                    << " px, kept vs off " << total_restore << ", tied ids " << total_ids);

      // The cost, at the view with the most casters: the same frames three ways, timed. Each
      // configuration is warmed up first and the three are timed twice, forwards and backwards,
      // because a GPU that is still raising its clocks makes whichever runs first look slowest.
      // The FlightHelmet alone is also timed at the finest cut (`lod_px` 0), which is the most
      // clusters — and so the most casters — a frame of it can have.
      FrameDesc timed;
      timed.camera = orbit_camera_at(
          focus, reach, 0.0f,
          k_two_pi * static_cast<f32>(worst_view % k_yaws) / static_cast<f32>(k_yaws),
          pitches[worst_view / k_yaws]);
      const bool finest = variant == 0 && sample.find("FlightHelmet") != std::string::npos;
      for (u32 cut = 0; cut < (finest ? 2u : 1u); ++cut) {
        timed.lod_px = cut == 0 ? -1.0f : 0.0f;
        for (u32 pass = 0; pass < 6; ++pass) {
          const u32 k = pass < 3 ? pass : 5 - pass;
          SceneRenderer& r = rigs[k].renderer;
          for (u32 f = 0; f < 60; ++f) {
            timed.frame_index = f;
            REQUIRE_MESSAGE(r.render_offscreen(timed, &error), error);
          }
          r.reset_stats();
          for (u32 f = 0; f < k_timed_frames; ++f) {
            timed.frame_index = f;
            REQUIRE_MESSAGE(r.render_offscreen(timed, &error), error);
          }
          const Stats& s = r.stats();
          const std::string name = k == 0 ? "cones off" : (k == 1 ? "dropped" : "kept");
          MESSAGE(where << std::string(cut == 0 ? "" : " finest cut") << " view " << worst_view
                        << " " << name << ": " << s.visible_pairs() << " pairs + "
                        << s.shadow_casters << " casters; ms cull " << s.cull_ms() << " hw "
                        << s.hw_ms() << " rt " << s.rt_ms() << " (clas " << s.clas_ms()
                        << ") resolve " << s.resolve_ms() << " total " << s.total_ms() << " over "
                        << s.timed_frames << " frames");
        }
      }
    }
  }
}
