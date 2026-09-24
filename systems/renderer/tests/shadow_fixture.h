#pragma once

// The shadow casters' fixture, shared by the ray-traced shadows' tests (shadow_caster_tests.cpp)
// and the cascaded shadow maps' (shadow_map_tests.cpp): a ground plane and one caster standing
// between it and the renderer's fixed sun, with the camera on the caster's dark side.
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

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace engine::renderer::shadow_fixture {

// A device, or nothing, exactly as the renderer's other cases open one.
struct Gpu {
  gfx::Device device;
  std::string why;
  bool ok = false;
  explicit Gpu(const gfx::DeviceOptions& options = {}) {
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
inline void add_face(MeshSource& m, Vec3 origin, Vec3 du, Vec3 dv) {
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
inline void add_prism(MeshSource& m, std::span<const Vec2> profile, std::span<const Rect> caps,
                      f32 z0, f32 z1) {
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

inline void add_box(MeshSource& m, f32 x0, f32 y0, f32 z0, f32 x1, f32 y1, f32 z1) {
  const Vec2 profile[4] = {Vec2{x0, y0}, Vec2{x1, y0}, Vec2{x1, y1}, Vec2{x0, y1}};
  const Rect cap{x0, y0, x1, y1};
  add_prism(m, profile, std::span<const Rect>(&cap, 1), z0, z1);
}

enum class Caster : u8 { card, slab, cube, ell };
inline constexpr Caster k_casters[4] = {Caster::card, Caster::slab, Caster::cube, Caster::ell};

inline const char* caster_name(Caster c) {
  switch (c) {
    case Caster::card: return "card";
    case Caster::slab: return "slab";
    case Caster::cube: return "cube";
    case Caster::ell: return "ell";
  }
  return "?";
}

inline MeshSource caster_mesh(Caster c) {
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

inline MeshSource ground_mesh() {
  MeshSource m;
  add_face(m, Vec3{-4.0f, 0.0f, -3.0f}, Vec3{0.0f, 0.0f, 6.0f}, Vec3{7.0f, 0.0f, 0.0f});  // +y
  return m;
}

// The LOD builder's own leaves — the clusters real content is drawn from up close, with the cones
// it fits them — made into a "DAG" in which every leaf is a root: own error 0, no parent. That is
// the cut every threshold selects, so the pictures below are the leaves and nothing else. The
// levels above them stay in the streams, unreferenced.
inline bool leaves_as_roots(const MeshSource& source, geometry::ClusterLodMesh& out,
                            std::string& error) {
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
inline Mat4 fixture_frame() {
  const Vec3 sun = normalize(Vec3{0.4f, 0.8f, 0.45f});  // frame_lighting's, which is fixed
  const Vec3 h = normalize(Vec3{sun.x, 0.0f, sun.z});
  Mat4 m = Mat4::identity();
  m.c[0] = Vec4{h.x, 0.0f, h.z, 0.0f};
  m.c[1] = Vec4{0.0f, 1.0f, 0.0f, 0.0f};
  m.c[2] = Vec4{-h.z, 0.0f, h.x, 0.0f};
  return m;
}

inline Vec3 to_world(const Mat4& frame, Vec3 local) {
  const Vec4 w = frame * Vec4{local, 1.0f};
  return Vec3{w.x, w.y, w.z};
}

// The camera, in the fixture's frame: on the dark side of every caster, above, a little off axis.
inline const Vec3 k_eye_local{-6.0f, 3.0f, 1.2f};
inline const Vec3 k_target_local{-0.6f, 0.5f, 0.0f};

// Ground (instance 0) and one caster (instance 1), by hand for the reason the file header gives.
inline bool make_scene(Caster caster, SceneData& out, std::string& error) {
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
inline u32 cone_culled(const SceneData& data, u32& clusters) {
  const geometry::ClusterMeshPart& part = data.parts[1];
  clusters = part.cluster_count;
  u32 culled = 0;
  for (u32 c = 0; c < part.cluster_count; ++c) {
    const geometry::ClusterDesc& desc = data.lod.mesh.clusters[part.first_cluster + c];
    culled += geometry::cluster_backfacing(desc, k_eye_local) ? 1u : 0u;
  }
  return culled;
}

inline constexpr u32 k_width = 320;
inline constexpr u32 k_height = 240;
inline constexpr u32 k_ground = 0;  // the ground's instance

struct Shot {
  CapturedFrame frame;
  u32 casters = 0;       // Stats::shadow_casters
  u32 visible = 0;       // Stats::visible_pairs()
  u32 shadow_pairs = 0;  // Stats::shadow_pairs
  bool csm = false;      // the frame drew cascaded maps
  bool available = true;
  std::string why;
};

// What a picture of the fixture is drawn with.
struct ShotOptions {
  RasterMode raster = RasterMode::Hardware;
  ShadowMode shadows = ShadowMode::RayTraced;
  bool cone = true;
  bool shadow_casters = true;
  bool shadow_frustum = true;
  u32 view_mode = static_cast<u32>(gfx::ResolveMode::Shadow);
  // The camera, in the fixture's frame.
  Vec3 eye = k_eye_local;
  Vec3 target = k_target_local;
};

// One picture of the scene, with its ids, under the options given.
inline Shot render(const gfx::Device& device, Caster caster, const ShotOptions& options) {
  Shot shot;
  SceneData data;
  std::string error;
  REQUIRE_MESSAGE(make_scene(caster, data, error), error);
  RenderSettings settings;
  settings.raster = options.raster;
  settings.shadows = options.shadows;
  settings.cone = options.cone;
  settings.shadow_casters = options.shadow_casters;
  settings.shadow_frustum = options.shadow_frustum;
  settings.lights = false;  // the sun alone: its shadow is what the shadow view shows
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  const RenderAvailability availability = check_availability(resolved, device.features());
  if (availability != RenderAvailability::Ok) {
    shot.available = false;
    shot.why = unavailable_reason(availability, device);
    return shot;
  }
  shot.csm = resolved.csm;
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  SceneRenderer renderer;
  SceneRenderer::Desc desc;
  desc.width = k_width;
  desc.height = k_height;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, desc, &error), error);
  const Mat4 frame_rotation = fixture_frame();
  FrameDesc frame;
  frame.camera.position = to_world(frame_rotation, options.eye);
  frame.camera.target = to_world(frame_rotation, options.target);
  frame.view_mode = options.view_mode;
  CaptureChannels channels;
  channels.color = true;
  channels.ids = true;
  REQUIRE_MESSAGE(renderer.capture(frame, channels, shot.frame, &error), error);
  shot.casters = renderer.stats().shadow_casters;
  shot.visible = renderer.stats().visible_pairs();
  shot.shadow_pairs = renderer.stats().shadow_pairs;
  return shot;
}

// The ray-traced shadows' test calls, which name the settings the way they always did.
inline Shot render(const gfx::Device& device, Caster caster, RasterMode raster, bool cone,
                   bool shadow_casters) {
  ShotOptions options;
  options.raster = raster;
  options.cone = cone;
  options.shadow_casters = shadow_casters;
  return render(device, caster, options);
}

inline bool is_ground(const Shot& s, u32 p) { return s.frame.ids[u64{p} * k_id_words] == k_ground; }
inline bool shadowed(const Shot& s, u32 p) { return s.frame.color[u64{p} * 4] == 0; }

struct Compared {
  u32 ground = 0;    // pixels that are ground in both pictures
  u32 shadow_a = 0;  // of those, shadowed in the first
  u32 shadow_b = 0;  // and in the second
  u32 differ = 0;    // shadowed in one and not the other
};

inline u32 id_differences(const Shot& a, const Shot& b) {
  u32 out = 0;
  for (u32 w = 0; w < a.frame.ids.size() && w < b.frame.ids.size(); ++w)
    out += a.frame.ids[w] != b.frame.ids[w] ? 1u : 0u;
  return out + (a.frame.ids.size() != b.frame.ids.size() ? 1u : 0u);
}

inline Compared compare_ground(const Shot& a, const Shot& b) {
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

// ---- a ground for the Khronos samples to stand on ---------------------------------------------

inline void put_u32(std::vector<u8>& out, u32 v) {
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((v >> (8 * i)) & 0xffu));
}

inline void put_f32(std::vector<u8>& out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  put_u32(out, bits);
}

// A unit square on y = 0 facing +y, as a GLB: the ground a sample stands on.
inline bool write_ground_glb(const std::string& path) {
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

}  // namespace engine::renderer::shadow_fixture
