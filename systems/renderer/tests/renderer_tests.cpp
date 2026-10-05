// The renderer, headless. Every case here builds a scene from a cube GLB written at test time,
// renders it into an offscreen color target, and reads the result back: no window, no surface,
// no swapchain, and no file in the tree. On a machine with no Vulkan device, or one that cannot
// write the visibility buffer, the cases record a skip, exactly as engine-view exits 3. A device
// without mesh shaders is **not** a skip: every case about the picture runs there through the
// vertex-shader baseline tier, which is what `RenderSettings{}` resolves to, and only a case about
// the mesh path or the ray path itself skips, naming the feature that is missing.
//
// The renderer must work without a window, because engine-host has none. That is a build-time
// property, not a runtime one: `foundation/window` is not a dependency of this module, so its
// header is not on the include path of anything that links it, and the check below fails the
// build the day someone adds one.
#if __has_include(<foundation/window/window.h>)
#error "systems/renderer must not depend on foundation/window: engine-host renders without one"
#endif

#include <core/math/math.h>
#include <domain/anim/skeleton.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/stress_mesh.h>
#include <domain/gfx/device.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/reference.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/view_set.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::renderer;

namespace {

void put_u32(std::vector<u8>& out, u32 v) {
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((v >> (8 * i)) & 0xffu));
}

void put_u16(std::vector<u8>& out, u16 v) {
  out.push_back(static_cast<u8>(v & 0xffu));
  out.push_back(static_cast<u8>(v >> 8));
}

void put_f32(std::vector<u8>& out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  put_u32(out, bits);
}

std::string n(u32 v) { return std::to_string(v); }

// A unit cube as a GLB: 24 vertices with normals and UVs and one primitive of twelve triangles
// with one plain material. The same shape apps/engine_view/tests writes, minus the second
// material and the embedded texture, because nothing here is about materials. `morph` adds one
// morph target, named "push", that moves the +x face's four corners out by `morph` along x.
bool write_cube_glb(const std::string& path, f32 morph = 0.0f) {
  const Vec3 normals[6] = {Vec3{1, 0, 0},  Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                           Vec3{0, -1, 0}, Vec3{0, 0, 1},  Vec3{0, 0, -1}};
  const Vec3 tangents[6] = {Vec3{0, 1, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1},
                            Vec3{0, 0, 1}, Vec3{1, 0, 0}, Vec3{1, 0, 0}};
  std::vector<u8> bin;
  for (u32 f = 0; f < 6; ++f) {  // corners wound counter-clockwise seen from outside
    const Vec3 nrm = normals[f];
    const Vec3 t = tangents[f];
    const Vec3 b = cross(nrm, t);
    const Vec3 corners[4] = {nrm * 0.5f - t * 0.5f - b * 0.5f, nrm * 0.5f + t * 0.5f - b * 0.5f,
                             nrm * 0.5f + t * 0.5f + b * 0.5f, nrm * 0.5f - t * 0.5f + b * 0.5f};
    for (const Vec3& c : corners) {
      put_f32(bin, c.x);
      put_f32(bin, c.y);
      put_f32(bin, c.z);
    }
  }
  const u32 normal_offset = static_cast<u32>(bin.size());
  for (u32 f = 0; f < 6; ++f) {
    for (u32 c = 0; c < 4; ++c) {
      put_f32(bin, normals[f].x);
      put_f32(bin, normals[f].y);
      put_f32(bin, normals[f].z);
    }
  }
  const u32 uv_offset = static_cast<u32>(bin.size());
  for (u32 f = 0; f < 6; ++f) {
    const f32 uvs[8] = {0, 0, 1, 0, 1, 1, 0, 1};
    for (const f32 v : uvs)
      put_f32(bin, v);
  }
  const u32 index_offset = static_cast<u32>(bin.size());
  for (u32 f = 0; f < 6; ++f) {
    const u16 base = static_cast<u16>(f * 4);
    const u16 tris[6] = {base, static_cast<u16>(base + 1), static_cast<u16>(base + 2),
                         base, static_cast<u16>(base + 2), static_cast<u16>(base + 3)};
    for (const u16 i : tris)
      put_u16(bin, i);
  }
  const u32 index_bytes = static_cast<u32>(bin.size()) - index_offset;
  while (bin.size() % 4 != 0)
    bin.push_back(0);
  const u32 target_offset = static_cast<u32>(bin.size());
  if (morph != 0.0f) {
    for (u32 f = 0; f < 6; ++f) {
      for (u32 c = 0; c < 4; ++c) {
        put_f32(bin, f == 0 ? morph : 0.0f);
        put_f32(bin, 0.0f);
        put_f32(bin, 0.0f);
      }
    }
  }
  const bool morphed = morph != 0.0f;

  std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
      "\"TEXCOORD_0\":2},\"indices\":3,\"material\":0" +
      std::string(morphed ? ",\"targets\":[{\"POSITION\":4}]}],\"weights\":[0],"
                            "\"extras\":{\"targetNames\":[\"push\"]}}],"
                          : "}]}],") +
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.8,0.5,0.2,1],"
      "\"metallicFactor\":0,\"roughnessFactor\":0.6}}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\","
      "\"min\":[-0.5,-0.5,-0.5],\"max\":[0.5,0.5,0.5]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":24,\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5123,\"count\":36,\"type\":\"SCALAR\"}" +
      std::string(morphed ? ",{\"bufferView\":4,\"componentType\":5126,\"count\":24,"
                            "\"type\":\"VEC3\"}"
                          : "") +
      "],"
      "\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
      n(normal_offset) + "},{\"buffer\":0,\"byteOffset\":" + n(normal_offset) +
      ",\"byteLength\":" + n(uv_offset - normal_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + n(uv_offset) +
      ",\"byteLength\":" + n(index_offset - uv_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + n(index_offset) + ",\"byteLength\":" + n(index_bytes) +
      "}" +
      (morphed ? ",{\"buffer\":0,\"byteOffset\":" + n(target_offset) +
                     ",\"byteLength\":" + n(static_cast<u32>(bin.size()) - target_offset) + "}"
               : std::string()) +
      "],\"buffers\":[{\"byteLength\":" + n(static_cast<u32>(bin.size())) + "}]}";
  while (json.size() % 4 != 0)
    json += ' ';

  std::vector<u8> glb;
  put_u32(glb, 0x46546c67u);  // "glTF"
  put_u32(glb, 2u);
  put_u32(glb, static_cast<u32>(12 + 8 + json.size() + 8 + bin.size()));
  put_u32(glb, static_cast<u32>(json.size()));
  put_u32(glb, 0x4e4f534au);  // "JSON"
  glb.insert(glb.end(), json.begin(), json.end());
  put_u32(glb, static_cast<u32>(bin.size()));
  put_u32(glb, 0x004e4942u);  // "BIN\0"
  glb.insert(glb.end(), bin.begin(), bin.end());
  std::ofstream f(path, std::ios::binary);
  if (!f.is_open()) return false;
  f.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
  return f.good();
}

std::string slashes(const std::filesystem::path& p) {
  std::string s = p.string();
  for (char& c : s) {
    if (c == '\\') c = '/';
  }
  return s;
}

// A device, or nothing: every case skips the same way engine-view exits 3.
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

// The scene, the settings, the GPU scene, and the renderer, in the one order they build in.
struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  std::string error;

  bool build(const gfx::Device& device, const SceneDesc& desc, const RenderSettings& settings,
             u32 width, u32 height) {
    if (!load_scene(desc, data, error)) return false;
    return finish(device, settings, width, height);
  }

  // The same from a `SceneData` the caller filled in itself, which is how the skinning cases get a
  // procedural mesh onto the GPU without inventing a skinned glTF to write at test time.
  bool finish(const gfx::Device& device, const RenderSettings& settings, u32 width, u32 height) {
    resolve_settings(settings, device.features(), &data, resolved);
    const RenderAvailability availability = check_availability(resolved, device.features());
    if (availability != RenderAvailability::Ok) {
      error = unavailable_reason(availability, device);
      return false;
    }
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = width;
    rd.height = height;
    // The layout the settings asked for, the way both hosts fill it in.
    rd.views.layout = resolved.settings.views;
    rd.views.surround.side_yaw = resolved.settings.side_yaw;
    rd.views.panini_d = resolved.settings.panini_d;
    rd.views.peripheral_lod = resolved.settings.peripheral_lod;
    return renderer.create(device, scene, resolved, rd, &error);
  }
};

const u32* pixel_id(const CapturedFrame& frame, u32 x, u32 y) {
  return &frame.ids[(u64{y} * frame.width + x) * k_id_words];
}

// The world point a view's centre ray reaches `distance` from the eye: normalized device
// coordinates (0, 0) at the reversed-Z depth a point that far away has, back through the view's
// own projection. Used to put something in front of each view of a surround and nowhere else.
Vec3 view_centre_point(const View& view, const Camera& camera, f32 distance) {
  const Mat4 inverse_vp = inverse(view.view_proj);
  const Vec4 clip{0.0f, 0.0f, camera.znear / distance, 1.0f};
  const Vec4 world = inverse_vp * clip;
  // The view-projection is the frame's, whose origin is the eye (ADR-0053): back to the world.
  return Vec3{world.x, world.y, world.z} * (1.0f / world.w) +
         relative(camera.position, WorldPos::origin());
}

// ---- the skinning fixture ---------------------------------------------------------------------
//
// A two-bone bar: a square column from y = 0 to y = 2, sliced into rings, bound root-to-tip by
// height. It is built here rather than imported, because a skinned glTF written at test time
// would be testing the importer as well, and what these cases are about is the *renderer's* half
// of the contract — the binding stream reaching the GPU, the joint matrices reaching a
// `DeformDesc`, and the cull pass staying conservative while the bar bends.
//
// The bar is the same shape `domain/gfx`'s skinning test uses, for the same reason: every vertex
// moves under a bend of the tip joint, so a missing weight, a transposed matrix or a stale pose
// shows up as a picture and not as a rounding difference.
struct SkinnedBar {
  anim::Skeleton skeleton;
  Vector<Vec3> positions;                  // the source mesh, before clustering
  Vector<geometry::SkinBinding> bindings;  // parallel to `positions`
  Vector<u32> indices;
  static constexpr u32 k_rings = 33;  // 32 slices: ~260 triangles, several leaf clusters
  static constexpr f32 k_height = 2.0f;
  static constexpr f32 k_half = 0.25f;
};

void build_skinned_bar(SkinnedBar& bar) {
  // The skeleton: a root at the origin and a tip halfway up, so a rotation of the tip bends the
  // top half of the bar and leaves the bottom half where it was.
  bar.skeleton.resize(2);
  bar.skeleton.names[0] = "root";
  bar.skeleton.names[1] = "tip";
  bar.skeleton.parents[0] = anim::k_no_joint;
  bar.skeleton.parents[1] = 0;
  bar.skeleton.local_bind[0] = Transform3::identity();
  bar.skeleton.local_bind[1] = Transform3::identity();
  bar.skeleton.local_bind[1].position = Vec3{0.0f, 0.5f * SkinnedBar::k_height, 0.0f};
  anim::compute_inverse_bind(bar.skeleton);

  const f32 h = SkinnedBar::k_half;
  const Vec3 ring[4] = {Vec3{-h, 0, -h}, Vec3{h, 0, -h}, Vec3{h, 0, h}, Vec3{-h, 0, h}};
  for (u32 r = 0; r < SkinnedBar::k_rings; ++r) {
    const f32 t = static_cast<f32>(r) / static_cast<f32>(SkinnedBar::k_rings - 1);
    const f32 y = t * SkinnedBar::k_height;
    // Weight by height: all root at the bottom, all tip at the top, a linear ramp between the
    // quarter and three-quarter marks so the middle rings mix the two matrices.
    const f32 tip = t < 0.25f ? 0.0f : (t > 0.75f ? 1.0f : (t - 0.25f) * 2.0f);
    const u32 joints[4] = {0, 1, 0, 0};
    const f32 weights[4] = {1.0f - tip, tip, 0.0f, 0.0f};
    const geometry::SkinBinding binding = geometry::make_skin_binding(joints, weights);
    for (const Vec3& c : ring) {
      bar.positions.push_back(Vec3{c.x, y, c.z});
      bar.bindings.push_back(binding);
    }
  }
  for (u32 r = 0; r + 1 < SkinnedBar::k_rings; ++r) {
    for (u32 c = 0; c < 4; ++c) {
      const u32 a = r * 4 + c;
      const u32 b = r * 4 + (c + 1) % 4;
      const u32 a2 = a + 4;
      const u32 b2 = b + 4;
      const u32 tri[6] = {a, a2, b, b, a2, b2};  // counter-clockwise seen from outside
      for (const u32 i : tri)
        bar.indices.push_back(i);
    }
  }
  const u32 top = (SkinnedBar::k_rings - 1) * 4;
  const u32 caps[12] = {0, 2, 1, 0, 3, 2, top, top + 1, top + 2, top, top + 2, top + 3};
  for (const u32 i : caps)
    bar.indices.push_back(i);
}

// The bar as a `SceneData` of `instances` copies, `joints` of them skinned. Everything the loader
// would have filled in, filled in by hand: one part, one source with one material, the instance
// table, and the bounds.
bool make_bar_scene(const SkinnedBar& bar, u32 instances, u32 skinned_joints, f32 bounds_padding,
                    SceneData& out, std::string& error) {
  geometry::AttributeSource attributes;
  attributes.skin =
      std::span<const geometry::SkinBinding>(bar.bindings.data(), bar.bindings.size());
  attributes.joint_count = bar.skeleton.joint_count();
  if (!geometry::build_cluster_lod(
          std::span<const Vec3>(bar.positions.data(), bar.positions.size()),
          std::span<const u32>(bar.indices.data(), bar.indices.size()),
          geometry::ClusterLodOptions{}, out.lod, &error, attributes)) {
    return false;
  }
  geometry::ClusterMeshPart part;
  part.cluster_count = out.lod.mesh.clusters.size();
  part.leaf_cluster_count = out.lod.level_cluster_counts[0];
  part.quant_origin = out.lod.mesh.quant_origin;
  part.quant_scale = out.lod.mesh.quant_scale;
  out.parts.push_back(part);
  SourceMesh source;
  source.part_material.push_back(-1);  // one part, no material: the default one is appended
  source.part_of_cluster.resize(part.cluster_count, 0u);
  out.sources.push_back(std::move(source));
  for (u32 i = 0; i < instances; ++i) {
    gfx::InstanceDesc instance{};
    Mat4 world = Mat4::identity();
    world.c[3] = Vec4{static_cast<f32>(i) * 1.5f, 0.0f, 0.0f, 1.0f};
    gfx::set_instance_transform(instance, world);
    instance.first_pair = out.pair_count;
    instance.bounds_padding = skinned_joints > 0 ? bounds_padding : 0.0f;
    out.pair_count += part.cluster_count;
    out.instances.push_back(instance);
    out.instance_joints.push_back(skinned_joints);
    out.skinned_instances += skinned_joints > 0 ? 1u : 0u;
  }
  out.max_joints = skinned_joints * instances;
  update_scene_bounds(out);
  return true;
}

// The bar's skinning matrices for a bend of the tip joint by `degrees` about +Z.
void bar_matrices(const SkinnedBar& bar, f32 degrees, Vector<anim::JointMatrix>& out) {
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

// Where the CPU reference says the bar's vertices land on screen, as a pixel rectangle. This is
// the prediction the picture is checked against: `anim::skin_positions` is the definition of what
// the pass computes, so a bar the GPU drew somewhere else is a bug in the renderer's half.
struct PixelRect {
  f32 x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
};

PixelRect predicted_rect(const SkinnedBar& bar, std::span<const anim::JointMatrix> matrices,
                         const Mat4& world, const Mat4& view_proj, u32 width, u32 height) {
  Vector<Vec3> skinned(bar.positions.size());
  anim::skin_positions(
      std::span<const Vec3>(bar.positions.data(), bar.positions.size()),
      std::span<const geometry::SkinBinding>(bar.bindings.data(), bar.bindings.size()), matrices,
      std::span<Vec3>(skinned.data(), skinned.size()));
  PixelRect rect;
  for (const Vec3& p : skinned) {
    const Vec4 clip = view_proj * (world * Vec4{p, 1.0f});
    if (clip.w <= 1.0e-6f) continue;
    const f32 x = (clip.x / clip.w * 0.5f + 0.5f) * static_cast<f32>(width);
    const f32 y = (0.5f - clip.y / clip.w * 0.5f) * static_cast<f32>(height);
    rect.x0 = std::min(rect.x0, x);
    rect.y0 = std::min(rect.y0, y);
    rect.x1 = std::max(rect.x1, x);
    rect.y1 = std::max(rect.y1, y);
  }
  return rect;
}

// The rectangle of pixels a capture actually covered, and how many there were.
PixelRect covered_rect(const CapturedFrame& shot, u32& covered) {
  PixelRect rect;
  covered = 0;
  for (u32 y = 0; y < shot.height; ++y) {
    for (u32 x = 0; x < shot.width; ++x) {
      if (pixel_id(shot, x, y)[0] == k_no_id) continue;
      ++covered;
      rect.x0 = std::min(rect.x0, static_cast<f32>(x));
      rect.y0 = std::min(rect.y0, static_cast<f32>(y));
      rect.x1 = std::max(rect.x1, static_cast<f32>(x) + 1.0f);
      rect.y1 = std::max(rect.y1, static_cast<f32>(y) + 1.0f);
    }
  }
  return rect;
}

// One frame of a skinned scene: the whole contract, filled in the way engine-view fills it.
//
// The camera sits on +z rather than the +x these cases' neighbours use, because the bar bends
// about +z: from +x that swing is straight along the view axis and moves nothing on screen, which
// is a fine way to write a test that cannot fail. From +z it is a sideways swing.
FrameDesc skinned_frame(const SceneData& data, std::span<const anim::JointMatrix> matrices,
                        std::span<const InstanceJoints> runs, f32 distance) {
  FrameDesc frame;
  frame.camera = orbit_camera_at(data.center, data.radius, distance, radians(90.0f), k_orbit_pitch);
  frame.joints = matrices;
  frame.instance_joints = runs;
  return frame;
}

}  // namespace

TEST_CASE("renderer: draws a cube offscreen with no window and reads every channel back") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_renderer");
  const std::filesystem::path dir = tmp.native();
  const std::string mesh = slashes(dir / "cube.glb");
  REQUIRE(write_cube_glb(mesh));

  SceneDesc desc;
  desc.meshes.push_back(mesh);
  desc.ddc = slashes(dir / "ddc");  // never the repository's cache
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, desc, RenderSettings{}, 160, 120), rig.error);
  CHECK(rig.data.cluster_count() >= 1);
  CHECK(rig.data.lod.leaf_triangle_count == 12);
  CHECK(rig.scene.instance_count() == 1);
  CHECK(rig.scene.pair_count() == rig.data.cluster_count());

  FrameDesc frame;
  frame.camera = orbit_camera_at(rig.data.center, rig.data.radius, 40.0f, 0.0f, k_orbit_pitch);
  CaptureChannels channels;
  channels.ids = true;
  channels.depth = true;
  channels.normals = true;
  CapturedFrame shot;
  std::string error;
  REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot, &error), error);
  CHECK(shot.width == 160);
  CHECK(shot.height == 120);
  CHECK(shot.color.size() == u64{160} * 120 * 4);
  CHECK(shot.normals.size() == u64{160} * 120 * 3);
  CHECK(shot.ids.size() == u64{160} * 120 * k_id_words);
  CHECK(shot.depth.size() == u64{160} * 120);
  CHECK(shot.covered > 0);
  CHECK(shot.depth_max > 0.0f);
  CHECK(shot.depth_min <= shot.depth_max);

  // The center pixel names the one instance placed there, a cluster of that scene, and a
  // triangle within it. A pixel the cube does not cover names nothing at all.
  const u32* center = pixel_id(shot, 80, 60);
  CHECK(center[0] == 0);
  CHECK(center[1] < rig.data.cluster_count());
  CHECK(center[2] < 128u);
  const u32* corner = pixel_id(shot, 0, 0);
  CHECK(corner[0] == k_no_id);
  CHECK(corner[1] == k_no_id);
  CHECK(corner[2] == k_no_id);

  // Every statistic the JSON summary and a benchmark report is filled in. The GPU zones are
  // read a frame late, so the first frame may have none; the visible counts are not.
  const Stats& stats = rig.renderer.stats();
  CHECK(stats.frames == 2);  // the color frame and the normals frame
  CHECK(stats.visible_pairs() > 0);
  CHECK(stats.visible_min <= stats.visible_pairs());
  CHECK(stats.visible_max >= stats.visible_pairs());
  CHECK(stats.cpu_ms_per_frame() > 0.0);
  CHECK(stats.cull_ms() >= 0.0);
  CHECK(stats.hw_ms() >= 0.0);
  CHECK(stats.sw_ms() >= 0.0);
  CHECK(stats.hiz_ms() >= 0.0);
  CHECK(stats.resolve_ms() >= 0.0);
  CHECK(stats.rt_ms() >= 0.0);
  CHECK(stats.clas_ms() >= 0.0);
  CHECK(stats.deform_ms() >= 0.0);
  CHECK(stats.trace_ms() >= 0.0);
  CHECK(stats.total_ms() >= 0.0);

  // Device memory, which is what says whether a frame was timed against a card another process
  // was holding. create() samples it, so it is filled without the caller asking.
  CHECK(stats.gpu_memory.device_local_total_mib > 0);
  if (stats.gpu_memory.valid) {  // VK_EXT_memory_budget; a device without it reports zeros
    CHECK(stats.gpu_memory.budget_mib > 0);
    CHECK(stats.gpu_memory.budget_mib <= stats.gpu_memory.device_local_total_mib);
    CHECK(stats.gpu_memory.used_mib <= stats.gpu_memory.device_local_total_mib);
  }
  // A reset does not leave the summary without a figure, and a second sample still agrees.
  rig.renderer.sample_gpu_memory();
  CHECK(stats.gpu_memory.device_local_total_mib > 0);

  // The channels land on disk in the documented shapes.
  CaptureFiles files;
  REQUIRE_MESSAGE(write_capture(slashes(dir), "shot", shot, files, error), error);
  CHECK(std::filesystem::exists(files.color));
  CHECK(std::filesystem::exists(files.normals));
  CHECK(std::filesystem::exists(files.depth));
  CHECK(std::filesystem::exists(files.ids));
  CHECK(std::filesystem::exists(files.ids_header));
  CHECK(std::filesystem::file_size(files.ids) == u64{160} * 120 * k_id_words * 4);
}

TEST_CASE("renderer: two instances give two ids") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_renderer_ids");
  const std::filesystem::path dir = tmp.native();
  const std::string mesh = slashes(dir / "cube.glb");
  REQUIRE(write_cube_glb(mesh));

  SceneDesc desc;
  desc.meshes.push_back(mesh);
  desc.ddc = slashes(dir / "ddc");
  SceneInstance left;
  left.transform.position = Vec3{-1.0f, 0.0f, 0.0f};
  SceneInstance right;
  right.transform.position = Vec3{1.0f, 0.0f, 0.0f};
  desc.instances.push_back(left);
  desc.instances.push_back(right);
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, desc, RenderSettings{}, 200, 120), rig.error);
  CHECK(rig.scene.instance_count() == 2);
  CHECK(rig.scene.pair_count() == rig.data.cluster_count() * 2);

  FrameDesc frame;
  frame.camera =
      orbit_camera_at(rig.data.center, rig.data.radius, 40.0f, radians(90.0f), k_orbit_pitch);
  CaptureChannels channels;
  channels.color = false;
  channels.ids = true;
  CapturedFrame shot;
  std::string error;
  REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot, &error), error);

  bool saw[2] = {false, false};
  u32 other = 0;
  for (u32 p = 0; p < shot.width * shot.height; ++p) {
    const u32 instance = shot.ids[u64{p} * k_id_words];
    if (instance == k_no_id) continue;
    if (instance < 2) {
      saw[instance] = true;
    } else {
      ++other;
    }
  }
  CHECK(saw[0]);
  CHECK(saw[1]);
  CHECK(other == 0);
}

TEST_CASE("renderer: settings resolve the same way for every host") {
  gfx::DeviceFeatures features;  // a device with nothing optional: the baseline tier
  RenderSettings settings;
  ResolvedSettings resolved;
  resolve_settings(settings, features, nullptr, resolved);
  CHECK(resolved.settings.raster == RasterMode::Vertex);  // no mesh shaders: the baseline tier
  CHECK(resolved.vertex_path);
  CHECK_FALSE(resolved.shadows);
  CHECK_FALSE(resolved.rt_chain);
  CHECK(resolved.occlusion);
  CHECK(check_availability(resolved, features) == RenderAvailability::NoVisibilityBuffer);

  features.buffer_int64_atomics = true;
  settings.raster = RasterMode::RayTrace;
  resolve_settings(settings, features, nullptr, resolved);
  CHECK(resolved.ray_path);
  CHECK(check_availability(resolved, features) == RenderAvailability::NoAccelerationStructures);

  // The TITAN Xp as its driver reports itself (docs/ci/self-hosted-runners.md, "The first run on
  // the Titan Xp"): acceleration structures and ray-tracing pipelines through the compute
  // fallback, and no ray query, no mesh shaders and no cluster structures. The default request is
  // the baseline tier with occlusion culling and the sun's cascaded shadow maps; an *explicit* ray
  // request — the ray path or `--shadows rt` — is refused rather than quietly dropped.
  gfx::DeviceFeatures pascal;
  pascal.buffer_int64_atomics = true;
  pascal.shader_int64 = true;
  pascal.acceleration_structure = true;
  pascal.ray_tracing_pipeline = true;
  resolve_settings(RenderSettings{}, pascal, nullptr, resolved);
  CHECK(resolved.settings.raster == RasterMode::Vertex);
  CHECK(resolved.vertex_path);
  CHECK_FALSE(resolved.shadows);
  CHECK(resolved.csm);
  CHECK_FALSE(resolved.rt_chain);
  CHECK(resolved.occlusion);
  CHECK(check_availability(resolved, pascal) == RenderAvailability::Ok);
  RenderSettings asked;
  asked.shadows = ShadowMode::RayTraced;
  resolve_settings(asked, pascal, nullptr, resolved);
  CHECK(check_availability(resolved, pascal) == RenderAvailability::NoAccelerationStructures);
  asked = RenderSettings{};
  asked.raster = RasterMode::RayTrace;
  resolve_settings(asked, pascal, nullptr, resolved);
  CHECK(check_availability(resolved, pascal) == RenderAvailability::NoAccelerationStructures);
  asked = RenderSettings{};
  asked.raster = RasterMode::Direct;  // mesh shaders to colour: the baseline tier here too
  resolve_settings(asked, pascal, nullptr, resolved);
  CHECK(resolved.vertex_path);
  CHECK_FALSE(resolved.direct);

  features.cluster_acceleration_structure = true;
  features.ray_query = true;
  features.mesh_shader = true;
  settings.raster = RasterMode::Hardware;
  settings.cull = false;
  settings.rt_templates = true;
  resolve_settings(settings, features, nullptr, resolved);
  CHECK(resolved.shadows);  // auto: the device can build the structures
  CHECK(resolved.rt_chain);
  CHECK(resolved.settings.cull);    // forced on: the structures come from the cull output
  CHECK_FALSE(resolved.occlusion);  // off with shadows: one visible list to build from
  CHECK(resolved.settings.rt_templates);
  CHECK(check_availability(resolved, features) == RenderAvailability::Ok);

  // A path that cannot shadow says so and leaves everything else alone. That is a settings
  // conflict on a device that can trace, not a device that cannot, so it is a warning and the
  // frame still runs.
  settings.raster = RasterMode::Software;
  settings.shadows = ShadowMode::RayTraced;
  settings.rt_templates = true;
  resolve_settings(settings, features, nullptr, resolved);
  CHECK_FALSE(resolved.shadows);
  CHECK_FALSE(resolved.rt_chain);
  CHECK_FALSE(resolved.settings.rt_templates);
  CHECK(check_availability(resolved, features) == RenderAvailability::Ok);
}

TEST_CASE("renderer: the direct path has no id or depth channel") {
  Gpu gpu;
  if (!gpu.ok || !gpu.device.features().mesh_shader) {
    MESSAGE("renderer unavailable here: " << (gpu.ok ? "no mesh shaders" : gpu.why));
    return;
  }
  const test::TempDir tmp("engine_renderer_direct");
  const std::filesystem::path dir = tmp.native();
  const std::string mesh = slashes(dir / "cube.glb");
  REQUIRE(write_cube_glb(mesh));
  SceneDesc desc;
  desc.meshes.push_back(mesh);
  desc.ddc = slashes(dir / "ddc");
  RenderSettings settings;
  settings.raster = RasterMode::Direct;
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, desc, settings, 96, 96), rig.error);

  FrameDesc frame;
  frame.camera = orbit_camera_at(rig.data.center, rig.data.radius, 40.0f, 0.0f, k_orbit_pitch);
  CaptureChannels channels;
  channels.ids = true;
  CapturedFrame shot;
  std::string error;
  CHECK_FALSE(rig.renderer.capture(frame, channels, shot, &error));
  CHECK(error.find("visibility buffer") != std::string::npos);
  // Color alone still works there.
  channels.ids = false;
  error.clear();
  REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot, &error), error);
  CHECK(shot.color.size() == u64{96} * 96 * 4);
}

// ---- the baseline tier on a device that really lacks the rest ---------------------------------
//
// The first GPU run on the Pascal server (docs/ci/self-hosted-runners.md, "The first run on the
// Titan Xp") failed 13 of this module's cases in one place: `SceneRenderer` built its mesh-shader
// pipelines whatever path the settings had resolved to, so on a device without mesh shaders no
// renderer could be created at all while `resolve_settings` said "vertex". Nothing here had seen
// it, because every GPU this suite had run on has mesh shaders. This case is that device on
// whatever GPU is present: the TITAN Xp's extension set through `DeviceOptions::overrides`, which
// creates a device that really lacks what they remove (a mesh pipeline on it fails exactly as on
// the hardware). It holds the baseline tier to ADR-0024's promise — the default request draws,
// through the vertex path, the picture the mesh path draws, to the byte — and it holds an explicit
// ray request to a refusal in the words of the device's own verdict.
TEST_CASE("renderer: a TITAN-Xp-like device draws the default request through the vertex path") {
  const test::TempDir tmp("engine_renderer_pascal");
  const std::filesystem::path dir = tmp.native();
  const std::string mesh = slashes(dir / "cube.glb");
  REQUIRE(write_cube_glb(mesh));
  SceneDesc desc;
  desc.meshes.push_back(mesh);
  desc.ddc = slashes(dir / "ddc");
  // Two rows, one behind the other, so occlusion culling has something to do on both devices.
  for (i32 z = 0; z < 2; ++z) {
    for (i32 x = -1; x <= 1; ++x) {
      SceneInstance instance;
      instance.transform.position =
          Vec3{static_cast<f32>(x) * 1.4f, 0.0f, static_cast<f32>(z) * -2.2f};
      desc.instances.push_back(instance);
    }
  }
  constexpr u32 k_width = 200;
  constexpr u32 k_height = 120;
  CaptureChannels channels;
  channels.ids = true;
  channels.depth = true;
  std::string error;

  // **One live device at a time.** `Device::create` loads volk's process-wide dispatch table, so
  // a second live device with a different extension set replaces the first one's entry points —
  // with this profile alive, the full device's `vkCmdDrawMeshTasksIndirectEXT` is null, which is
  // how the first draft of this case crashed (docs/subsystems/gfx.md, "One live device per
  // process"). So the device as it is draws first and is gone before the profile exists.
  //
  // It draws with shadows off, so both frames resolve through the same pipeline with occlusion
  // culling on: the mesh path wherever there are mesh shaders, and the vertex path — which makes
  // this comparison weaker but still true — on a device that has none.
  FrameDesc frame;
  CapturedFrame b;
  u32 full_pairs = 0;
  RasterMode full_raster = RasterMode::Hardware;
  {
    Gpu gpu;
    if (!gpu.ok) {
      MESSAGE("renderer unavailable here: " << gpu.why);
      return;
    }
    RenderSettings plain;
    plain.shadows = ShadowMode::Off;
    Rig full;
    REQUIRE_MESSAGE(full.build(gpu.device, desc, plain, k_width, k_height), full.error);
    frame.camera = orbit_camera_at(full.data.center, full.data.radius, 14.0f, 0.3f, k_orbit_pitch);
    REQUIRE_MESSAGE(full.renderer.capture(frame, channels, b, &error), error);
    full_pairs = full.renderer.stats().visible_pairs();
    full_raster = full.resolved.settings.raster;
  }

  gfx::DeviceOptions options;
  for (const char* row :
       {"VK_EXT_mesh_shader", "VK_KHR_ray_query", "VK_NV_cluster_acceleration_structure",
        "VK_EXT_memory_decompression", "VK_KHR_fragment_shading_rate"}) {
    options.overrides.absent.push_back(row);
  }
  gfx::Device pascal;
  REQUIRE_MESSAGE(pascal.create(options, &error), error);  // the GPU that just drew above
  CHECK(pascal.verdict().usable);
  CHECK(pascal.verdict().tier == "raster");  // acceleration structures without ray queries
  CHECK_FALSE(pascal.features().mesh_shader);
  CHECK_FALSE(pascal.features().ray_query);

  // The default request here shadows with cascaded maps — `auto` without ray queries — so the
  // byte comparison with the full device's shadowless frame asks for no shadows on both sides; the
  // maps are checked below it.
  RenderSettings shadowless;
  shadowless.shadows = ShadowMode::Off;
  Rig baseline;  // declared after `pascal`, so it is destroyed first
  REQUIRE_MESSAGE(baseline.build(pascal, desc, shadowless, k_width, k_height), baseline.error);
  CHECK(baseline.resolved.settings.raster == RasterMode::Vertex);
  CHECK(baseline.resolved.vertex_path);
  CHECK(baseline.resolved.occlusion);
  CHECK_FALSE(baseline.resolved.shadows);
  CHECK_FALSE(baseline.resolved.rt_chain);
  CapturedFrame a;
  REQUIRE_MESSAGE(baseline.renderer.capture(frame, channels, a, &error), error);
  REQUIRE(a.covered > 1000);
  u64 id_differences = 0;
  u64 colour_differences = 0;
  u64 depth_differences = 0;
  for (u32 p = 0; p < k_width * k_height; ++p) {
    for (u32 w = 0; w < k_id_words; ++w)
      if (a.ids[p * k_id_words + w] != b.ids[p * k_id_words + w]) ++id_differences;
    for (u32 c = 0; c < 4; ++c)
      if (a.color[p * 4 + c] != b.color[p * 4 + c]) ++colour_differences;
    if (a.depth[p] != b.depth[p]) ++depth_differences;
  }
  // `std::string` around every `const char*`: doctest prints a bare `char*` as a pointer.
  MESSAGE("baseline tier (" << std::string(raster_name(baseline.resolved.settings.raster))
                            << ") against " << std::string(raster_name(full_raster)) << ": "
                            << a.covered << " covered pixels, " << id_differences << " id words, "
                            << colour_differences << " colour bytes and " << depth_differences
                            << " depths differ");
  CHECK(a.covered == b.covered);
  CHECK(id_differences == 0);
  CHECK(colour_differences == 0);
  CHECK(depth_differences == 0);
  CHECK(baseline.renderer.stats().visible_pairs() == full_pairs);
  // The TITAN Xp has geometryShader and fullDrawIndexUint32, so its culled cut is the indexed
  // draw: exactly the cut's triangles, none of them through the fallback on a scene this small
  // (gfx::VertexDrawHeader).
  CHECK(baseline.resolved.vertex_indexed);
  CHECK(baseline.renderer.stats().triangles_hw > 0);
  CHECK(baseline.renderer.stats().vertex_fallback == 0);

  // An explicit ray request is refused, and the refusal is the verdict's own sentence, naming
  // the missing ray query: `--shadows rt`, `--raster rt`, and the reference path tracer alike.
  for (const bool ray_path : {false, true}) {
    RenderSettings asked;
    if (ray_path) {
      asked.raster = RasterMode::RayTrace;
    } else {
      asked.shadows = ShadowMode::RayTraced;
    }
    ResolvedSettings resolved;
    resolve_settings(asked, pascal.features(), &baseline.data, resolved);
    const RenderAvailability availability = check_availability(resolved, pascal.features());
    REQUIRE(availability == RenderAvailability::NoAccelerationStructures);
    const std::string reason = unavailable_reason(availability, pascal);
    MESSAGE(std::string(ray_path ? "--raster rt" : "--shadows rt") << " refused: " << reason);
    CHECK(reason.find(std::string(pascal.adapter().name)) == 0);
    CHECK(reason.find("VK_KHR_ray_query") != std::string::npos);
    bool from_verdict = false;
    for (const std::string& line : pascal.verdict().degraded)
      from_verdict = from_verdict || reason.find(line) != std::string::npos;
    CHECK(from_verdict);
  }
  std::string why;
  CHECK_FALSE(reference_available(baseline.resolved, pascal, &why));
  CHECK(why.find("VK_KHR_ray_query") != std::string::npos);

  // **The default request shadows with cascaded maps** here (docs/plan/04-renderer.md §4.4, the
  // 2026-09-23 direction note): `auto` without ray queries, drawn by the vertex path's indexed draw
  // from the light, with two-pass occlusion culling still on for the picture. The picture's ids
  // are the shadowless frame's word for word — the maps change the lighting and nothing drawn.
  Rig mapped;
  REQUIRE_MESSAGE(mapped.build(pascal, desc, RenderSettings{}, k_width, k_height), mapped.error);
  CHECK(mapped.resolved.csm);
  CHECK_FALSE(mapped.resolved.shadows);
  CHECK(mapped.resolved.occlusion);
  CHECK(mapped.resolved.vertex_indexed);
  CHECK(mapped.resolved.shadow_cascades == k_default_shadow_cascades);
  CapturedFrame m;
  REQUIRE_MESSAGE(mapped.renderer.capture(frame, channels, m, &error), error);
  CHECK(mapped.renderer.stats().shadow_pairs > 0);
  CHECK(mapped.renderer.stats().shadow_fallback == 0);
  CHECK(mapped.renderer.shadow_cascades().count >= 1);
  u64 mapped_ids = 0;
  for (u32 w = 0; w < m.ids.size() && w < a.ids.size(); ++w)
    mapped_ids += m.ids[w] != a.ids[w] ? 1u : 0u;
  CHECK(mapped_ids == 0);
  MESSAGE("the default request on the profile: csm, "
          << mapped.renderer.shadow_cascades().count << " cascades drawn, "
          << mapped.renderer.stats().shadow_pairs << " pairs in the maps");
}

// The baseline tier on a device without geometryShader. The vertex path's culled draw names each
// triangle by SV_PrimitiveID, which a vertex pipeline only has with that feature, so without it
// `resolve_settings` leaves `vertex_indexed` off and the path draws every culled cluster's
// capacity instead (cluster_vertex.slang, docs/subsystems/gfx.md "Baseline tier"); the same holds
// without fullDrawIndexUint32, and removing one of the two is enough to make the point. The two
// draws must be one picture to the byte — that is what lets the renderer choose between them by a
// device feature alone — and the capacity draw reports no indexed triangles.
TEST_CASE("renderer: without geometryShader the vertex path capacity-draws the same picture") {
  const test::TempDir tmp("engine_renderer_capacity");
  const std::filesystem::path dir = tmp.native();
  const std::string mesh = slashes(dir / "cube.glb");
  REQUIRE(write_cube_glb(mesh));
  SceneDesc desc;
  desc.meshes.push_back(mesh);
  desc.ddc = slashes(dir / "ddc");
  for (i32 z = 0; z < 2; ++z) {
    for (i32 x = -1; x <= 1; ++x) {
      SceneInstance instance;
      instance.transform.position =
          Vec3{static_cast<f32>(x) * 1.4f, 0.0f, static_cast<f32>(z) * -2.2f};
      desc.instances.push_back(instance);
    }
  }
  constexpr u32 k_width = 200;
  constexpr u32 k_height = 120;
  CaptureChannels channels;
  channels.ids = true;
  channels.depth = true;
  RenderSettings settings;  // the default request: occlusion on, so both hardware runs draw
  settings.shadows = ShadowMode::Off;
  std::string error;

  // One live device at a time (the case above says why): each profile draws in its own scope.
  struct Drawn {
    CapturedFrame frame;
    bool indexed = false;
    u32 triangles = 0;
    u32 pairs = 0;
  };
  FrameDesc frame;
  bool have_camera = false;
  auto draw_with = [&](bool geometry_shader, Drawn& out) {
    gfx::DeviceOptions options;
    options.overrides.absent.push_back("VK_EXT_mesh_shader");
    if (!geometry_shader) options.overrides.absent.push_back("geometryShader");
    gfx::Device device;
    if (!device.create(options, &error)) return false;
    bool ok = false;
    {
      Rig rig;
      ok = rig.build(device, desc, settings, k_width, k_height);
      if (!ok) error = rig.error;
      if (ok && !have_camera) {
        frame.camera =
            orbit_camera_at(rig.data.center, rig.data.radius, 14.0f, 0.3f, k_orbit_pitch);
        have_camera = true;
      }
      if (ok) ok = rig.renderer.capture(frame, channels, out.frame, &error);
      if (ok) {
        out.indexed = rig.resolved.vertex_indexed;
        out.triangles = rig.renderer.stats().triangles_hw;
        out.pairs = rig.renderer.stats().visible_pairs();
      }
    }
    device.destroy();
    return ok;
  };
  {
    Gpu probe;  // skips, as every renderer case does, where there is no device at all
    if (!probe.ok) {
      MESSAGE("renderer unavailable here: " << probe.why);
      return;
    }
    const gfx::DeviceFeatures& f = probe.device.features();
    if (!f.geometry_shader || !f.full_draw_index_uint32) {
      MESSAGE("skipped: " << std::string(probe.device.adapter().name) << " has no "
                          << (f.geometry_shader ? "fullDrawIndexUint32" : "geometryShader"));
      return;
    }
  }
  Drawn indexed;
  Drawn capacity;
  REQUIRE_MESSAGE(draw_with(true, indexed), error);
  REQUIRE_MESSAGE(draw_with(false, capacity), error);
  CHECK(indexed.indexed);
  CHECK_FALSE(capacity.indexed);
  CHECK(indexed.triangles > 0);
  CHECK(capacity.triangles == 0);
  CHECK(indexed.pairs == capacity.pairs);
  REQUIRE(indexed.frame.covered > 1000);
  u64 id_differences = 0;
  u64 colour_differences = 0;
  u64 depth_differences = 0;
  for (u32 p = 0; p < k_width * k_height; ++p) {
    for (u32 w = 0; w < k_id_words; ++w) {
      if (indexed.frame.ids[p * k_id_words + w] != capacity.frame.ids[p * k_id_words + w])
        ++id_differences;
    }
    for (u32 c = 0; c < 4; ++c) {
      if (indexed.frame.color[p * 4 + c] != capacity.frame.color[p * 4 + c]) ++colour_differences;
    }
    if (indexed.frame.depth[p] != capacity.frame.depth[p]) ++depth_differences;
  }
  MESSAGE("indexed against capacity: "
          << indexed.frame.covered << " covered pixels, " << indexed.triangles
          << " triangles drawn indexed, " << id_differences << " id words, " << colour_differences
          << " colour bytes and " << depth_differences << " depths differ");
  CHECK(indexed.frame.covered == capacity.frame.covered);
  CHECK(id_differences == 0);
  CHECK(colour_differences == 0);
  CHECK(depth_differences == 0);
}

// ---- multi-view (docs/plan/04-renderer.md §4.6, experiment E9) ----------------------------------

TEST_CASE("view set: the layouts put the views where the projections say they are") {
  // A single view is one rectangle over the whole target with the camera's own field of view.
  ViewSet single;
  std::string error;
  REQUIRE_MESSAGE(single.build(ViewSetDesc{}, 1200, 400, &error), error);
  CHECK(single.size() == 1);
  CHECK(single[0].rect.width == 1200);
  CHECK(single[0].rect.height == 400);
  CHECK(single[0].source_width == 1200);
  CHECK(single[0].symmetric);
  CHECK(single.source_pixels() == u64{1200} * 400);
  CHECK_FALSE(single.resample());

  // A flat surround is three thirds of one wide rectilinear frustum. The tangents prove it: the
  // union of the three rectangles at unit distance is the single view's, and the centre one is a
  // third of it, centred.
  const f32 fov_y = radians(55.0f);
  const f32 wide = std::tan(fov_y * 0.5f) * 1200.0f / 400.0f;  // the single view's half-width
  ViewSetDesc flat;
  flat.layout = ViewLayout::Surround3;
  ViewSet surround;
  REQUIRE_MESSAGE(surround.build(flat, 1200, 400, &error), error);
  REQUIRE(surround.size() == 3);
  for (u32 v = 0; v < 3; ++v) {
    CHECK(surround[v].rect.x == v * 400);
    CHECK(surround[v].rect.width == 400);
    CHECK(surround[v].rect.height == 400);
    CHECK(surround[v].yaw == 0.0f);  // flat: the turn is in the projection, not the camera
  }
  CHECK(surround[0].left == doctest::Approx(-wide).epsilon(1e-5));
  CHECK(surround[2].right == doctest::Approx(wide).epsilon(1e-5));
  CHECK(surround[1].left == doctest::Approx(-wide / 3.0f).epsilon(1e-5));
  CHECK(surround[1].right == doctest::Approx(wide / 3.0f).epsilon(1e-5));
  // The rectangles meet: one view's right edge is the next one's left.
  CHECK(surround[0].right == doctest::Approx(surround[1].left).epsilon(1e-5));
  CHECK(surround[1].right == doctest::Approx(surround[2].left).epsilon(1e-5));
  // The centre monitor is centred whatever the arrangement, so it takes the symmetric projection
  // and is bit-for-bit the single view of one monitor.
  CHECK(surround[1].symmetric);
  CHECK(surround[1].aspect == 1.0f);
  CHECK_FALSE(surround[0].symmetric);

  // Turning the side monitors inward turns their cameras and narrows their frusta in tangent
  // terms, because a turned monitor is nearer its own view axis than a flat one is.
  ViewSetDesc angled = flat;
  angled.surround.side_yaw = radians(30.0f);
  angled.peripheral_lod = 4.0f;
  ViewSet turned;
  REQUIRE_MESSAGE(turned.build(angled, 1200, 400, &error), error);
  CHECK(turned[0].yaw == doctest::Approx(-radians(30.0f)));
  CHECK(turned[1].yaw == 0.0f);
  CHECK(turned[2].yaw == doctest::Approx(radians(30.0f)));
  CHECK(turned[2].right < surround[2].right);
  // The attention region is the centre monitor: only the side views take the coarser threshold.
  CHECK(turned[0].quality.lod_scale == 4.0f);
  CHECK(turned[1].quality.lod_scale == 1.0f);
  CHECK(turned[2].quality.lod_scale == 4.0f);

  // Panini: one view, the picture's rectangle, and a wider rectilinear source. At d = 0 the
  // projection *is* rectilinear, so the source is the picture and nothing is oversampled.
  ViewSetDesc panini;
  panini.layout = ViewLayout::Panini;
  panini.panini_d = 0.0f;
  ViewSet flat_panini;
  REQUIRE_MESSAGE(flat_panini.build(panini, 1200, 400, &error), error);
  CHECK(flat_panini.size() == 1);
  CHECK(flat_panini.oversample() == doctest::Approx(1.0f).epsilon(1e-5));
  CHECK(flat_panini[0].source_width == 1200);
  CHECK(flat_panini.panini_half_width() == doctest::Approx(flat_panini.source_half_width()));
  CHECK(flat_panini.resample());

  panini.panini_d = 1.0f;
  ViewSet curved;
  REQUIRE_MESSAGE(curved.build(panini, 1200, 400, &error), error);
  // The factor the arithmetic gives: (d + cos t) / ((d + 1) cos t) at the half field of view,
  // which is what the wide rectilinear source has to be so that it never magnifies.
  const f32 half_fov_x = std::atan(wide);
  const f32 expected = (1.0f + std::cos(half_fov_x)) / (2.0f * std::cos(half_fov_x));
  CHECK(curved.oversample() == doctest::Approx(expected).epsilon(1e-4));
  CHECK(curved[0].source_width > 1200);
  CHECK(curved[0].rect.width == 1200);
  // Too wide a field of view at too large a d asks for more source than the renderer will make.
  ViewSetDesc huge = panini;
  huge.panini_d = 3.0f;
  ViewSet refused;
  CHECK_FALSE(refused.build(huge, 11520, 1080, &error));
  CHECK(error.find("Panini") != std::string::npos);
}

TEST_CASE("view set: an off-axis projection through a centred rectangle is the centred one") {
  const f32 fov_y = radians(55.0f);
  const f32 znear = 0.25f;
  const f32 aspect = 16.0f / 9.0f;
  const f32 top = znear * std::tan(fov_y * 0.5f);
  const f32 right = top * aspect;
  const Mat4 centred = perspective_reversed_z(fov_y, aspect, znear);
  const Mat4 off_axis = off_center_reversed_z(-right, right, -top, top, znear);
  for (u32 r = 0; r < 4; ++r) {
    for (u32 c = 0; c < 4; ++c) {
      CHECK(off_axis.at(r, c) == doctest::Approx(centred.at(r, c)).epsilon(1e-5));
    }
  }
  // A rectangle shifted to one side puts the axis off centre and nothing else: a point on the
  // rectangle's own centre line lands at normalized device x of zero.
  const Mat4 shifted = off_center_reversed_z(right, 3.0f * right, -top, top, znear);
  const Vec4 clip = shifted * Vec4{2.0f * right, 0.0f, -znear, 1.0f};
  CHECK(clip.x / clip.w == doctest::Approx(0.0f).epsilon(1e-5));
}

TEST_CASE("view set: the Panini inverse map agrees with the projection it inverts") {
  ViewSetDesc desc;
  desc.layout = ViewLayout::Panini;
  desc.panini_d = 1.0f;
  ViewSet set;
  std::string error;
  REQUIRE_MESSAGE(set.build(desc, 800, 400, &error), error);
  const View& view = set[0];
  // The picture's centre reads the source's centre, whatever d is.
  u32 sx = 0;
  u32 sy = 0;
  REQUIRE(panini_source_pixel(set, view, view.rect.width / 2, view.rect.height / 2, sx, sy));
  CHECK(sx == view.source_width / 2);
  CHECK(sy == view.source_height / 2);
  // Forward and back: an azimuth's Panini abscissa maps to the rectilinear abscissa of the same
  // azimuth, which is what makes the resample a change of projection and not a distortion.
  for (u32 i = 1; i < 8; ++i) {
    const f32 theta = static_cast<f32>(i) * 0.15f;
    const f32 x_panini = gfx::panini_abscissa(desc.panini_d, theta);
    const f32 px_f =
        (x_panini / set.panini_half_width() * 0.5f + 0.5f) * static_cast<f32>(view.rect.width);
    if (!(px_f >= 0.0f) || px_f >= static_cast<f32>(view.rect.width)) continue;
    REQUIRE(panini_source_pixel(set, view, static_cast<u32>(px_f), view.rect.height / 2, sx, sy));
    const f32 expected = (std::tan(theta) / set.source_half_width() * 0.5f + 0.5f) *
                         static_cast<f32>(view.source_width);
    CHECK(static_cast<f32>(sx) == doctest::Approx(expected).epsilon(5e-3));
  }
}

TEST_CASE("renderer: a surround's centre view is the single view of one monitor, to the pixel") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_renderer_surround_centre");
  const std::filesystem::path dir = tmp.native();
  const std::string mesh = slashes(dir / "cube.glb");
  REQUIRE(write_cube_glb(mesh));
  SceneDesc desc;
  desc.meshes.push_back(mesh);
  desc.ddc = slashes(dir / "ddc");
  // A row of cubes wide enough to reach past the centre monitor: at the camera below, the centre
  // monitor covers |x| < 6.2 and the side ones take over from there, so the two outermost cubes
  // are the side views' and the three inner ones the centre's.
  for (i32 i = -2; i <= 2; ++i) {
    SceneInstance instance;
    instance.transform.position = Vec3{static_cast<f32>(i) * 4.0f, 0.0f, 0.0f};
    desc.instances.push_back(instance);
  }

  constexpr u32 k_monitor = 128;
  RenderSettings surround_settings;
  surround_settings.views = ViewLayout::Surround3;
  Rig surround;
  REQUIRE_MESSAGE(surround.build(gpu.device, desc, surround_settings, k_monitor * 3, k_monitor),
                  surround.error);
  REQUIRE(surround.renderer.views().size() == 3);
  // Three views over one scene: one set of scene buffers, one visible list, three runs each, laid
  // out run-major so that every view's first run is one contiguous range at the front.
  CHECK(surround.scene.view_count() == 3);
  CHECK(surround.scene.visible_base(0, 0) == 0);
  CHECK(surround.scene.visible_base(1, 0) == surround.scene.pair_count());
  CHECK(surround.scene.visible_base(0, 1) == surround.scene.pair_count() * 3);

  Rig single;
  REQUIRE_MESSAGE(single.build(gpu.device, desc, RenderSettings{}, k_monitor, k_monitor),
                  single.error);

  // An explicit camera rather than an orbit: the orbit's distance scales with the scene's radius,
  // which is exactly the thing that decides whether the row reaches the side monitors.
  FrameDesc frame;
  frame.camera.position = absolute(WorldPos::origin(), Vec3{0.0f, 0.0f, 12.0f});
  frame.camera.target = absolute(WorldPos::origin(), Vec3{0.0f, 0.0f, 0.0f});
  frame.camera.znear = 0.05f;
  CaptureChannels channels;
  channels.ids = true;
  CapturedFrame wide;
  CapturedFrame narrow;
  std::string error;
  REQUIRE_MESSAGE(surround.renderer.capture(frame, channels, wide, &error), error);
  REQUIRE_MESSAGE(single.renderer.capture(frame, channels, narrow, &error), error);

  // The centre monitor's frustum is the single view's, bit for bit, so its cull produces the same
  // set of pairs and its resolve produces the same picture. Anything less than "the same" here
  // would mean a view's cut depends on how many views there are beside it.
  CHECK(surround.renderer.stats().views[1].visible_pairs() ==
        single.renderer.stats().visible_pairs());
  u64 same_ids = 0;
  u64 same_color = 0;
  for (u32 y = 0; y < k_monitor; ++y) {
    for (u32 x = 0; x < k_monitor; ++x) {
      const u32* a = pixel_id(wide, k_monitor + x, y);
      const u32* b = pixel_id(narrow, x, y);
      same_ids += (a[0] == b[0] && a[1] == b[1] && a[2] == b[2]) ? 1 : 0;
      const u32 pa = (y * wide.width + k_monitor + x) * 4;
      const u32 pb = (y * narrow.width + x) * 4;
      same_color += std::memcmp(&wide.color[pa], &narrow.color[pb], 4) == 0 ? 1 : 0;
    }
  }
  CHECK(same_ids == u64{k_monitor} * k_monitor);
  CHECK(same_color == u64{k_monitor} * k_monitor);

  // The side views drew as well, and the per-view counts add up to the frame's totals.
  const Stats& stats = surround.renderer.stats();
  CHECK(stats.view_count == 3);
  CHECK(stats.views[0].visible_pairs() > 0);
  CHECK(stats.views[2].visible_pairs() > 0);
  CHECK(stats.views[0].visible_pairs() + stats.views[1].visible_pairs() +
            stats.views[2].visible_pairs() ==
        stats.visible_pairs());
  CHECK(stats.views[0].width == k_monitor);
  CHECK(stats.views[2].x == k_monitor * 2);
}

TEST_CASE(
    "renderer: each view of a turned surround names what is in front of it, through one TLAS") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_renderer_surround_views");
  const std::filesystem::path dir = tmp.native();
  const std::string mesh = slashes(dir / "cube.glb");
  REQUIRE(write_cube_glb(mesh));

  // Where the three views of a 35-degree surround look, from a camera on +z looking at the
  // origin. The layout is laid out first so the scene can be built around it: one cube in front
  // of each view's own centre and nothing anywhere else.
  constexpr u32 k_monitor = 128;
  constexpr f32 k_distance = 6.0f;
  ViewSetDesc layout;
  layout.layout = ViewLayout::Surround3;
  layout.surround.side_yaw = radians(35.0f);
  ViewSet views;
  std::string error;
  REQUIRE_MESSAGE(views.build(layout, k_monitor * 3, k_monitor, &error), error);
  Camera camera;
  camera.position = absolute(WorldPos::origin(), Vec3{0.0f, 0.0f, k_distance});
  camera.target = absolute(WorldPos::origin(), Vec3{0.0f, 0.0f, 0.0f});
  camera.znear = 0.05f;
  views.update(camera);

  SceneDesc desc;
  desc.meshes.push_back(mesh);
  desc.ddc = slashes(dir / "ddc");
  for (u32 v = 0; v < 3; ++v) {
    SceneInstance instance;
    instance.transform.position = view_centre_point(views[v], camera, k_distance);
    desc.instances.push_back(instance);
  }

  // Both ways of producing visibility, against the same layout: the rasterizers, which write each
  // view's own region of the visibility buffer, and the ray path, which traces every view against
  // the one top-level structure built from the union of the views' cuts. The raster request is
  // `hw`, which is the vertex path on a device without mesh shaders — a surround is a question
  // about views and not about which rasterizer, so it runs there too rather than skipping.
  for (const RasterMode mode : {RasterMode::Hardware, RasterMode::RayTrace}) {
    if (mode == RasterMode::RayTrace && !(gpu.device.features().cluster_acceleration_structure &&
                                          gpu.device.features().ray_query)) {
      MESSAGE("ray path skipped here: " << unavailable_reason(
                  RenderAvailability::NoAccelerationStructures, gpu.device));
      continue;
    }
    RenderSettings settings;
    settings.raster = mode;
    settings.views = ViewLayout::Surround3;
    settings.side_yaw = radians(35.0f);
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, desc, settings, k_monitor * 3, k_monitor), rig.error);
    REQUIRE(rig.scene.instance_count() == 3);
    FrameDesc frame;
    frame.camera = camera;
    CaptureChannels channels;
    channels.color = false;
    channels.ids = true;
    CapturedFrame shot;
    REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot, &error), error);
    for (u32 v = 0; v < 3; ++v) {
      const ViewRect& rect = rig.renderer.views()[v].rect;
      const u32* centre = pixel_id(shot, rect.x + rect.width / 2, rect.y + rect.height / 2);
      INFO("raster " << std::string(raster_name(rig.resolved.settings.raster)) << ", view " << v
                     << " names instance " << centre[0]);
      CHECK(centre[0] == v);
      CHECK(centre[1] < rig.data.cluster_count() * 3);
    }
    // Every view culled something of its own.
    const Stats& stats = rig.renderer.stats();
    for (u32 v = 0; v < 3; ++v)
      CHECK(stats.views[v].visible_pairs() > 0);
  }
}

// Two-pass occlusion culling brings two pieces of machinery into a frame that a frame without it
// does not have — the Hi-Z pyramid, and the per-32x32-tile coverage mask the last build of it
// leaves behind for the resolve to skip empty tiles by — and **neither may change one pixel**.
// The mask is the sharper of the two: it decides, before the visibility word is read at all,
// that a region of the picture is sky, and a mask that were wrong by one tile would put a
// 32x32 hole in a surface. So the test is the same scene rendered with occlusion on and off,
// compared byte for byte in colour *and* in the ids under it, at both layouts — because a
// surround gives each view its own mask at its own offset and pitch, which is the part of the
// wiring a single view cannot exercise. Shadows are off because they turn occlusion off with
// them (one visible list to build the acceleration structures from), and this GPU would
// otherwise have taken them.
TEST_CASE("renderer: occlusion culling's Hi-Z and coverage mask change no pixel") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_renderer_occlusion");
  const std::filesystem::path dir = tmp.native();
  const std::string mesh = slashes(dir / "cube.glb");
  REQUIRE(write_cube_glb(mesh));
  SceneDesc desc;
  desc.meshes.push_back(mesh);
  desc.ddc = slashes(dir / "ddc");
  // A wall of cubes at two depths, so something is genuinely behind something else and the
  // picture is a mixture of covered tiles, empty tiles, and tiles that straddle a silhouette.
  for (i32 z = 0; z < 2; ++z) {
    for (i32 x = -3; x <= 3; ++x) {
      for (i32 y = -1; y <= 1; ++y) {
        SceneInstance instance;
        instance.transform.position = Vec3{static_cast<f32>(x) * 1.4f, static_cast<f32>(y) * 1.4f,
                                           static_cast<f32>(z) * -2.2f};
        desc.instances.push_back(instance);
      }
    }
  }

  // Not a multiple of the 32-pixel tile on either axis, and a surround's thirds are not either.
  constexpr u32 k_width = 302;
  constexpr u32 k_height = 154;
  for (const ViewLayout layout : {ViewLayout::Single, ViewLayout::Surround3}) {
    RenderSettings on;
    on.shadows = ShadowMode::Off;
    on.views = layout;
    RenderSettings off = on;
    off.occlusion = false;
    Rig with;
    Rig without;
    REQUIRE_MESSAGE(with.build(gpu.device, desc, on, k_width, k_height), with.error);
    REQUIRE_MESSAGE(without.build(gpu.device, desc, off, k_width, k_height), without.error);
    REQUIRE(with.resolved.occlusion);
    REQUIRE_FALSE(without.resolved.occlusion);

    FrameDesc frame;
    frame.camera = orbit_camera_at(with.data.center, with.data.radius, 12.0f, 0.4f, k_orbit_pitch);
    CaptureChannels channels;
    channels.ids = true;
    CapturedFrame a;
    CapturedFrame b;
    std::string error;
    REQUIRE_MESSAGE(with.renderer.capture(frame, channels, a, &error), error);
    REQUIRE_MESSAGE(without.renderer.capture(frame, channels, b, &error), error);
    REQUIRE(a.covered > k_width * k_height / 16);  // the scene is actually in the picture
    REQUIRE(a.covered < u64{k_width} * k_height);  // and so is some sky, which is what is skipped
    u64 colour_mismatch = 0;
    u64 surface_mismatch = 0;
    for (u32 p = 0; p < k_width * k_height; ++p) {
      for (u32 c = 0; c < 4; ++c)
        if (a.color[p * 4 + c] != b.color[p * 4 + c]) ++colour_mismatch;
      // The triangle may be the other one of a shared edge where the depths tie, but the
      // instance and the cluster a pixel names cannot move.
      for (u32 w = 0; w < 2; ++w)
        if (a.ids[p * k_id_words + w] != b.ids[p * k_id_words + w]) ++surface_mismatch;
    }
    MESSAGE(std::string(view_layout_name(layout))
            << " " << k_width << "x" << k_height << ": " << a.covered << " covered px, "
            << colour_mismatch << " colour differences, " << surface_mismatch
            << " surface differences");
    CHECK(colour_mismatch == 0);
    CHECK(surface_mismatch == 0);
  }
}

TEST_CASE("renderer: a Panini view at d = 0 is the rectilinear picture") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_renderer_panini");
  const std::filesystem::path dir = tmp.native();
  const std::string mesh = slashes(dir / "cube.glb");
  REQUIRE(write_cube_glb(mesh));
  SceneDesc desc;
  desc.meshes.push_back(mesh);
  desc.ddc = slashes(dir / "ddc");
  for (i32 i = -3; i <= 3; ++i) {
    SceneInstance instance;
    instance.transform.position = Vec3{static_cast<f32>(i) * 1.5f, 0.0f, 0.0f};
    desc.instances.push_back(instance);
  }

  constexpr u32 k_width = 384;
  constexpr u32 k_height = 128;
  RenderSettings panini;
  panini.views = ViewLayout::Panini;
  panini.panini_d = 0.0f;
  Rig resampled;
  Rig plain;
  REQUIRE_MESSAGE(resampled.build(gpu.device, desc, panini, k_width, k_height), resampled.error);
  REQUIRE_MESSAGE(plain.build(gpu.device, desc, RenderSettings{}, k_width, k_height), plain.error);
  // d = 0 needs no oversampling: the Panini projection at d = 0 *is* the rectilinear one.
  CHECK(resampled.renderer.views()[0].source_width == k_width);

  FrameDesc frame;
  frame.camera = orbit_camera_at(plain.data.center, plain.data.radius, 40.0f, 0.0f, k_orbit_pitch);
  CaptureChannels channels;
  CapturedFrame a;
  CapturedFrame b;
  std::string error;
  REQUIRE_MESSAGE(resampled.renderer.capture(frame, channels, a, &error), error);
  REQUIRE_MESSAGE(plain.renderer.capture(frame, channels, b, &error), error);

  // The resample is a round trip through atan and tan, so it is the identity in exact arithmetic
  // and need not be in floating point: a pixel whose mapped source lands a hair over a boundary
  // reads its neighbour. **Measured here: not one of the 49,152 pixels differs at all** on the
  // RTX 5090 at this size, and the allowance below is for another driver's transcendentals, not
  // for a difference this one has. The tolerance is on how many pixels differ and by how much,
  // rather than on none differing, because a silhouette pixel that swaps is a whole surface.
  u64 differing = 0;
  u32 worst = 0;
  for (u32 p = 0; p < k_width * k_height; ++p) {
    u32 here = 0;
    for (u32 c = 0; c < 3; ++c) {
      const i32 delta = static_cast<i32>(a.color[p * 4 + c]) - static_cast<i32>(b.color[p * 4 + c]);
      const u32 magnitude = static_cast<u32>(delta < 0 ? -delta : delta);
      here = magnitude > here ? magnitude : here;
    }
    if (here > 1) ++differing;
    worst = here > worst ? here : worst;
  }
  const f64 fraction = static_cast<f64>(differing) / static_cast<f64>(u64{k_width} * k_height);
  MESSAGE("panini d=0 against rectilinear: " << differing << " of " << k_width * k_height
                                             << " pixels differ by more than 1, worst channel "
                                             << worst);
  CHECK(fraction < 0.005);  // docs/experiments/e9-multi-view.md has the measurement
  CHECK(worst <= 128);      // a silhouette pixel taking its neighbour, not a shading difference
}

// ---- skinned instances ------------------------------------------------------------------------
//
// Four cases over the two-bone bar, in the order the property they check was built:
//
//   1. a skinned instance at the **bind pose** draws exactly what the rigid one draws;
//   2. a **bend** moves the picture where `anim::skin_positions` says it does;
//   3. **culling stays conservative** at every phase of that bend;
//   4. the mesh, vertex and ray paths **agree** on a posed frame.
//
// Everything they need is built here (`build_skinned_bar`, `make_bar_scene`): no glTF, no clip
// library, and no dependency on `systems/animation` — the renderer's side of the contract is a
// span of `anim::JointMatrix` and a run per instance, and that is exactly what these hand it.

TEST_CASE("renderer: a skinned instance at the bind pose draws the unskinned picture") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  SkinnedBar bar;
  build_skinned_bar(bar);
  constexpr u32 k_width = 200;
  constexpr u32 k_height = 200;

  Rig rigid;
  REQUIRE_MESSAGE(make_bar_scene(bar, 1, 0, 0.0f, rigid.data, rigid.error), rigid.error);
  REQUIRE_MESSAGE(rigid.finish(gpu.device, RenderSettings{}, k_width, k_height), rigid.error);
  CHECK_FALSE(rigid.data.skinned());
  CHECK_FALSE(rigid.scene.skinned());

  Rig skinned;
  REQUIRE_MESSAGE(make_bar_scene(bar, 1, 2, 0.0f, skinned.data, skinned.error), skinned.error);
  REQUIRE_MESSAGE(skinned.finish(gpu.device, RenderSettings{}, k_width, k_height), skinned.error);
  CHECK(skinned.data.skinned());
  CHECK(skinned.scene.skinned());
  CHECK(skinned.scene.skinned_instances() == 1);
  // The pool pass runs even though nobody asked for `--deform`: skinning is one of its kinds.
  CHECK(skinned.resolved.deform_pass);
  CHECK(skinned.scene.deform_pool_bytes() > 0);

  Vector<anim::JointMatrix> matrices;
  bar_matrices(bar, 0.0f, matrices);  // the bind pose: every skinning matrix is the identity
  const InstanceJoints runs[1] = {{0, 2}};

  CaptureChannels channels;
  channels.ids = true;
  channels.depth = true;
  CapturedFrame rigid_shot;
  CapturedFrame skinned_shot;
  std::string error;
  FrameDesc plain;
  plain.camera =
      orbit_camera_at(rigid.data.center, rigid.data.radius, 22.0f, radians(90.0f), k_orbit_pitch);
  REQUIRE_MESSAGE(rigid.renderer.capture(plain, channels, rigid_shot, &error), error);
  const FrameDesc posed = skinned_frame(
      skinned.data, std::span<const anim::JointMatrix>(matrices.data(), matrices.size()),
      std::span<const InstanceJoints>(runs, 1), 22.0f);
  REQUIRE_MESSAGE(skinned.renderer.capture(posed, channels, skinned_shot, &error), error);

  // Identical coverage and identical ids. The depths are not bit-identical and cannot be: the
  // rigid path dequantizes inside the rasterizer while the skinned one reads the float the pool
  // pass stored, and the two compilations of the same expression round the last bit differently
  // (domain/gfx's deform test measures the same thing and states the same tolerance).
  u32 rigid_covered = 0;
  u32 skinned_covered = 0;
  covered_rect(rigid_shot, rigid_covered);
  covered_rect(skinned_shot, skinned_covered);
  CHECK(rigid_covered > 1000);
  CHECK(skinned_covered == rigid_covered);
  u64 id_differences = 0;
  f32 worst_depth = 0.0f;
  for (u32 p = 0; p < k_width * k_height; ++p) {
    for (u32 c = 0; c < k_id_words; ++c) {
      if (rigid_shot.ids[p * k_id_words + c] != skinned_shot.ids[p * k_id_words + c])
        ++id_differences;
    }
    const f32 delta = rigid_shot.depth[p] - skinned_shot.depth[p];
    worst_depth = std::max(worst_depth, delta < 0.0f ? -delta : delta);
  }
  MESSAGE("bind pose against rigid: " << rigid_covered << " covered pixels, " << id_differences
                                      << " id words differ, worst depth " << worst_depth);
  CHECK(id_differences == 0);
  CHECK(worst_depth <= 2.0e-7f);
}

// ---- morph channels, end to end ----------------------------------------------------------------
//
// The GPU chain itself is `domain/gfx`'s to check against a CPU reference; what is this module's
// is the wiring — the stream uploaded once per scene, the two halves of the weights array, the
// budgeted static cache, the normal pool reaching the resolve, and the bound the cull pass
// inflates by. Three properties cover it, and the first is the one that matters most: **a morphed
// mesh at weight zero draws the picture the same mesh with no channels draws**, because if it does
// not, every existing picture of every other scene is in question too.
namespace {

// The morph sphere of `geometry::stress_mesh.h` as a `SceneData`, the way `make_bar_scene` does
// it for the skinned bar: one part, one source with one material, one instance.
bool make_morph_scene(const geometry::MorphFixtureMesh& source, bool with_channels, SceneData& out,
                      std::string& error) {
  geometry::AttributeSource attributes;
  attributes.normals = std::span<const Vec3>(source.normals.data(), source.normals.size());
  attributes.uvs = std::span<const Vec2>(source.uvs.data(), source.uvs.size());
  if (with_channels) {
    attributes.morph =
        std::span<const geometry::MorphChannelSource>(source.morph.data(), source.morph.size());
  }
  if (!geometry::build_cluster_lod(
          std::span<const Vec3>(source.positions.data(), source.positions.size()),
          std::span<const u32>(source.indices.data(), source.indices.size()),
          geometry::ClusterLodOptions{}, out.lod, &error, attributes)) {
    return false;
  }
  geometry::ClusterMeshPart part;
  part.cluster_count = out.lod.mesh.clusters.size();
  part.leaf_cluster_count = out.lod.level_cluster_counts[0];
  part.quant_origin = out.lod.mesh.quant_origin;
  part.quant_scale = out.lod.mesh.quant_scale;
  part.morph_channel_count = out.lod.mesh.morph_channels.size();
  out.parts.push_back(part);
  SourceMesh mesh_source;
  mesh_source.part_material.push_back(-1);
  mesh_source.part_of_cluster.resize(part.cluster_count, 0u);
  out.sources.push_back(std::move(mesh_source));
  gfx::InstanceDesc instance{};
  gfx::set_instance_transform(instance, Mat4::identity());
  instance.first_pair = 0;
  out.pair_count = part.cluster_count;
  out.instances.push_back(instance);
  out.instance_joints.push_back(0u);
  update_scene_bounds(out);
  return true;
}

geometry::MorphFixtureOptions morph_rig() {
  geometry::MorphFixtureOptions options;
  options.segments = 48;
  options.rings = 24;
  options.channels = 4;
  options.falloff = 0.4f;
  options.amplitude = 0.25f;  // large, so a weight of 1 is unmistakable in the picture
  return options;
}

}  // namespace

TEST_CASE("renderer: a morphed mesh at weight zero draws the unmorphed picture, and moves at one") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  geometry::MorphFixtureMesh source;
  geometry::build_morph_sphere(morph_rig(), source);
  constexpr u32 k_width = 220;
  constexpr u32 k_height = 220;

  Rig plain;
  REQUIRE_MESSAGE(make_morph_scene(source, false, plain.data, plain.error), plain.error);
  REQUIRE_MESSAGE(plain.finish(gpu.device, RenderSettings{}, k_width, k_height), plain.error);
  CHECK_FALSE(plain.data.morphed());
  CHECK_FALSE(plain.resolved.deform_pass);  // nothing deforms, so no pool at all

  RenderSettings rest;
  Rig zero;
  REQUIRE_MESSAGE(make_morph_scene(source, true, zero.data, zero.error), zero.error);
  REQUIRE_MESSAGE(zero.finish(gpu.device, rest, k_width, k_height), zero.error);
  CHECK(zero.data.morphed());
  CHECK(zero.scene.morphed());
  CHECK(zero.scene.morph_channel_count() == 4);
  // A morphed mesh is deformed whatever the flags say, exactly as a skinned one is.
  CHECK(zero.resolved.deform_pass);
  CHECK(zero.scene.static_cached_instances() == 1);
  CHECK(zero.scene.static_cache_bytes() ==
        u64{zero.data.lod.mesh.vertices.size()} * sizeof(gfx::DeformCacheVertex));

  CaptureChannels channels;
  channels.ids = true;
  std::string error;
  FrameDesc frame;
  frame.camera =
      orbit_camera_at(plain.data.center, plain.data.radius, 6.0f, radians(90.0f), k_orbit_pitch);
  CapturedFrame unmorphed;
  CapturedFrame at_zero;
  REQUIRE_MESSAGE(plain.renderer.capture(frame, channels, unmorphed, &error), error);
  REQUIRE_MESSAGE(zero.renderer.capture(frame, channels, at_zero, &error), error);

  // Property one: at weight zero the chain writes the rest position into the pool and the normal
  // pool holds the rest normal, so the two pictures name the same surface everywhere.
  u32 plain_covered = 0;
  u32 zero_covered = 0;
  covered_rect(unmorphed, plain_covered);
  covered_rect(at_zero, zero_covered);
  CHECK(plain_covered > 2000);
  CHECK(zero_covered == plain_covered);
  u64 id_differences = 0;
  for (u32 p = 0; p < k_width * k_height * k_id_words; ++p) {
    if (unmorphed.ids[p] != at_zero.ids[p]) ++id_differences;
  }
  MESSAGE("morph at weight 0 against no channels: " << plain_covered << " covered pixels, "
                                                    << id_differences << " id words differ");
  CHECK(id_differences == 0);

  // Property two: a static weight moves the surface, and the cache is what holds it. The scene is
  // rebuilt rather than re-weighted because the weights are the *scene's* — which is the whole
  // difference between the static stage and the pose stage.
  RenderSettings played;
  played.morph_static_weights.resize(4, 0.0f);
  played.morph_static_weights[0] = 1.0f;
  played.morph_static_weights[2] = 1.0f;
  Rig shaped;
  REQUIRE_MESSAGE(make_morph_scene(source, true, shaped.data, shaped.error), shaped.error);
  REQUIRE_MESSAGE(shaped.finish(gpu.device, played, k_width, k_height), shaped.error);
  // The cull pass has to be told the surface left its rest bounds, or a channel that pushes a
  // cluster outward is culled while it is on screen.
  CHECK(shaped.data.instances[0].bounds_padding == 0.0f);  // the caller's; the scene adds its own
  CHECK(shaped.scene.static_cache_bytes() > 0);
  CapturedFrame at_one;
  REQUIRE_MESSAGE(shaped.renderer.capture(frame, channels, at_one, &error), error);
  u32 shaped_covered = 0;
  covered_rect(at_one, shaped_covered);
  u64 moved = 0;
  for (u32 p = 0; p < k_width * k_height * k_id_words; ++p) {
    if (at_zero.ids[p] != at_one.ids[p]) ++moved;
  }
  MESSAGE("two channels at weight 1: " << shaped_covered << " covered pixels, " << moved
                                       << " id words differ from the rest shape");
  CHECK(moved > 200);

  // Property three: the **pose** stage moves it too, from a frame's span, with the scene's static
  // weights left where they were. One frame at zero and one at one, same scene.
  Vector<f32> pose(4, 0.0f);
  FrameDesc posed = frame;
  posed.morph_weights = {pose.data(), pose.size()};
  CapturedFrame pose_zero;
  REQUIRE_MESSAGE(zero.renderer.capture(posed, channels, pose_zero, &error), error);
  pose[1] = 1.0f;
  CapturedFrame pose_one;
  REQUIRE_MESSAGE(zero.renderer.capture(posed, channels, pose_one, &error), error);
  u64 pose_moved = 0;
  for (u32 p = 0; p < k_width * k_height * k_id_words; ++p) {
    if (pose_zero.ids[p] != pose_one.ids[p]) ++pose_moved;
  }
  MESSAGE("one pose channel at weight 1: " << pose_moved << " id words differ");
  CHECK(pose_moved > 100);
}

// ---- the deformed-vertex pool's per-frame suballocation -----------------------------------------
//
// Two properties, and they are the two halves of what the budget replaced E25's per-instance pool
// for: with room, the pool holds **the cut** and nothing else, and every visible pair gets a block;
// without room, the pairs that did not fit draw their **rest pose**, are counted, and nothing
// crashes or reads a wild address. The second half is why `deform_pool_kib` is in kibibytes — a
// budget a bar's own cut cannot fit has to be expressible, or the overflow rule is untested.
// Regression, found by the Khronos morph samples: a **single** morphed glTF — the ordinary
// `engine-view --mesh face.glb` — could not be drawn at all. `load_scene` builds a one-mesh scene
// without `merge_cluster_meshes`, which is what fills a part's morph channel run, so the part said
// "no channels" while the stream and `resolve_settings` said otherwise; the GPU scene then gave the
// instance no deform record, sized the deformed-vertex pool for zero vertices, and the create
// failed in `vmaCreateBuffer`. The case above never saw it, because it builds its `SceneData` by
// hand and fills the part in itself. This one goes through the loader, the way both hosts do.
TEST_CASE("renderer: a single morphed glTF loads, draws, and moves through load_scene") {
  const test::TempDir tmp("engine_renderer_morph_load");
  const std::filesystem::path dir = tmp.native();
  const std::string mesh = slashes(dir / "push.glb");
  REQUIRE(write_cube_glb(mesh, 0.25f));

  SceneDesc desc;
  desc.meshes.push_back(mesh);
  desc.ddc = slashes(dir / "ddc");
  SceneData data;
  std::string error;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  REQUIRE(data.parts.size() == 1);
  CHECK(data.lod.mesh.morph_channels.size() == 1);
  CHECK(data.parts[0].first_morph_channel == 0);
  CHECK(data.parts[0].morph_channel_count == 1);
  CHECK(data.morphed());

  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  Rig rest;
  REQUIRE_MESSAGE(rest.build(gpu.device, desc, RenderSettings{}, 160, 120), rest.error);
  CHECK(rest.scene.morphed());
  CHECK(rest.scene.morph_channel_count() == 1);
  RenderSettings pushed_settings;
  pushed_settings.morph_static_weights = {1.0f};
  Rig pushed;
  REQUIRE_MESSAGE(pushed.build(gpu.device, desc, pushed_settings, 160, 120), pushed.error);
  CHECK(pushed.scene.static_cached_instances() == 1);

  // The same camera for both — the rest scene's — so the only difference is the face.
  FrameDesc frame;
  frame.camera = orbit_camera_at(rest.data.center, rest.data.radius, 40.0f, 0.0f, k_orbit_pitch);
  CaptureChannels channels;
  channels.ids = true;
  CapturedFrame a;
  CapturedFrame b;
  REQUIRE_MESSAGE(rest.renderer.capture(frame, channels, a, &error), error);
  REQUIRE_MESSAGE(pushed.renderer.capture(frame, channels, b, &error), error);
  CHECK(a.covered > 0);
  u32 differing = 0;
  for (u32 p = 0; p < 160u * 120u; ++p) {
    if (a.ids[p * k_id_words] != b.ids[p * k_id_words]) ++differing;
  }
  MESSAGE("the +x face pushed out a quarter: " << a.covered << " covered pixels at rest, "
                                               << b.covered << " pushed, " << differing
                                               << " pixels change coverage");
  CHECK(b.covered > a.covered);
  CHECK(differing > 0);
}

TEST_CASE("renderer: the pool holds the frame's cut, and a budget too small falls back to rest") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  SkinnedBar bar;
  build_skinned_bar(bar);
  constexpr u32 k_width = 240;
  constexpr u32 k_height = 240;
  constexpr u32 k_instances = 8;

  CaptureChannels channels;
  channels.ids = true;
  Vector<anim::JointMatrix> matrices;
  bar_matrices(bar, 45.0f, matrices);
  Vector<anim::JointMatrix> population;  // one pose per instance, as a crowd hands over
  Vector<InstanceJoints> runs;
  for (u32 i = 0; i < k_instances; ++i) {
    runs.push_back(InstanceJoints{static_cast<u32>(population.size()), 2});
    for (const anim::JointMatrix& m : matrices)
      population.push_back(m);
  }
  const std::span<const anim::JointMatrix> joints(population.data(), population.size());
  const std::span<const InstanceJoints> joint_runs(runs.data(), runs.size());

  // With the default budget: nothing overflows, every visible pair has a block, and the pool's
  // occupancy is what the cut needs rather than what the instances' meshes are. Where the default
  // request draws the sun's cascaded shadow maps (a device without ray queries), every cascade's
  // cut of these deformed bars is in the pool too, one block per entry like the picture's, so the
  // entries are the visible pairs plus the maps' pairs; `shadow_pairs` is 0 everywhere else.
  Rig roomy;
  REQUIRE_MESSAGE(make_bar_scene(bar, k_instances, 2, 0.6f, roomy.data, roomy.error), roomy.error);
  REQUIRE_MESSAGE(roomy.finish(gpu.device, RenderSettings{}, k_width, k_height), roomy.error);
  CapturedFrame roomy_shot;
  std::string error;
  const FrameDesc posed = skinned_frame(roomy.data, joints, joint_runs, 22.0f);
  REQUIRE_MESSAGE(roomy.renderer.capture(posed, channels, roomy_shot, &error), error);
  const Stats& wide = roomy.renderer.stats();
  MESSAGE("roomy pool: " << roomy.scene.deform_pool_bytes() << " B budgeted of "
                         << roomy.scene.deform_whole_mesh_bytes() << " B whole-mesh, "
                         << wide.deform_vertices << " vertices used by " << wide.deform_entries
                         << " of " << wide.visible_pairs() << " visible pairs and "
                         << wide.shadow_pairs << " in the shadow maps");
  CHECK(wide.deform_overflow_entries == 0);
  CHECK(wide.deform_entries == wide.visible_pairs() + wide.shadow_pairs);
  CHECK(wide.deform_vertices > 0);
  CHECK(wide.deform_vertices <= roomy.scene.deform_pool_vertices());
  u32 roomy_covered = 0;
  covered_rect(roomy_shot, roomy_covered);
  CHECK(roomy_covered > 1000);

  // One kibibyte is 85 vertices, against the 272 this cut needs: two or three of its clusters fit
  // and the rest do not. The refused pairs take the rest pose, so the picture is still a bar —
  // straighter in places — and every pair is accounted for on one side or the other.
  RenderSettings tight;
  tight.deform_pool_kib = 1;
  Rig cramped;
  REQUIRE_MESSAGE(make_bar_scene(bar, k_instances, 2, 0.6f, cramped.data, cramped.error),
                  cramped.error);
  REQUIRE_MESSAGE(cramped.finish(gpu.device, tight, k_width, k_height), cramped.error);
  CHECK(cramped.scene.deform_pool_vertices() < wide.deform_vertices);
  CapturedFrame cramped_shot;
  REQUIRE_MESSAGE(cramped.renderer.capture(posed, channels, cramped_shot, &error), error);
  const Stats& narrow = cramped.renderer.stats();
  MESSAGE("cramped pool: " << cramped.scene.deform_pool_bytes() << " B, " << narrow.deform_entries
                           << " placed, " << narrow.deform_overflow_entries << " refused of "
                           << narrow.visible_pairs() << " visible pairs and " << narrow.shadow_pairs
                           << " in the shadow maps");
  CHECK(narrow.deform_overflow_entries > 0);
  CHECK(narrow.deform_entries + narrow.deform_overflow_entries ==
        narrow.visible_pairs() + narrow.shadow_pairs);
  CHECK(narrow.deform_vertices <= cramped.scene.deform_pool_vertices());
  // The frame is still a picture of the same scene: covered, and covered by the same instances.
  u32 cramped_covered = 0;
  covered_rect(cramped_shot, cramped_covered);
  CHECK(cramped_covered > 1000);
  u32 instance_out_of_range = 0;
  for (u32 p = 0; p < k_width * k_height; ++p) {
    const u32 instance = cramped_shot.ids[p * k_id_words];
    if (instance != k_no_id && instance >= k_instances) ++instance_out_of_range;
  }
  CHECK(instance_out_of_range == 0);
}

TEST_CASE("renderer: a bend puts the bar where anim::skin_positions says it goes") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  SkinnedBar bar;
  build_skinned_bar(bar);
  constexpr u32 k_width = 200;
  constexpr u32 k_height = 200;
  constexpr f32 k_bend = 60.0f;

  // The padding the instance is culled with: the exact largest displacement the bend produces,
  // which is what `animation::clip_displacement_bound` bounds from above for a whole clip.
  Vector<anim::JointMatrix> bent;
  bar_matrices(bar, k_bend, bent);
  Vector<Vec3> moved(bar.positions.size());
  anim::skin_positions(
      std::span<const Vec3>(bar.positions.data(), bar.positions.size()),
      std::span<const geometry::SkinBinding>(bar.bindings.data(), bar.bindings.size()),
      std::span<const anim::JointMatrix>(bent.data(), bent.size()),
      std::span<Vec3>(moved.data(), moved.size()));
  f32 displacement = 0.0f;
  for (u32 v = 0; v < moved.size(); ++v)
    displacement = std::max(displacement, length(moved[v] - bar.positions[v]));
  CHECK(displacement > 0.4f);  // the bend is a real one, not a rounding difference

  Rig rig;
  REQUIRE_MESSAGE(make_bar_scene(bar, 1, 2, displacement, rig.data, rig.error), rig.error);
  REQUIRE_MESSAGE(rig.finish(gpu.device, RenderSettings{}, k_width, k_height), rig.error);
  const InstanceJoints runs[1] = {{0, 2}};

  CaptureChannels channels;
  channels.ids = true;
  CapturedFrame rest_shot;
  CapturedFrame bent_shot;
  std::string error;
  Vector<anim::JointMatrix> rest;
  bar_matrices(bar, 0.0f, rest);
  REQUIRE_MESSAGE(
      rig.renderer.capture(
          skinned_frame(rig.data, std::span<const anim::JointMatrix>(rest.data(), rest.size()),
                        std::span<const InstanceJoints>(runs, 1), 22.0f),
          channels, rest_shot, &error),
      error);
  REQUIRE_MESSAGE(
      rig.renderer.capture(
          skinned_frame(rig.data, std::span<const anim::JointMatrix>(bent.data(), bent.size()),
                        std::span<const InstanceJoints>(runs, 1), 22.0f),
          channels, bent_shot, &error),
      error);

  const Mat4& view_proj = rig.renderer.views()[0].view_proj;
  // The view-projection is the frame's, whose origin is the eye (ADR-0053), so the instance goes
  // into the same space.
  const Camera bent_camera =
      skinned_frame(rig.data, std::span<const anim::JointMatrix>(bent.data(), bent.size()),
                    std::span<const InstanceJoints>(runs, 1), 22.0f)
          .camera;
  const Mat4 world = gfx::instance_matrix(rig.data.instances[0], frame_eye(bent_camera));
  const PixelRect predicted =
      predicted_rect(bar, std::span<const anim::JointMatrix>(bent.data(), bent.size()), world,
                     view_proj, k_width, k_height);
  u32 rest_covered = 0;
  u32 bent_covered = 0;
  const PixelRect rest_drawn = covered_rect(rest_shot, rest_covered);
  const PixelRect bent_drawn = covered_rect(bent_shot, bent_covered);

  // The bend is visible: the two pictures put the bar on different pixels.
  u64 coverage_changes = 0;
  for (u32 p = 0; p < k_width * k_height; ++p) {
    const bool a = rest_shot.ids[p * k_id_words] != k_no_id;
    const bool b = bent_shot.ids[p * k_id_words] != k_no_id;
    if (a != b) ++coverage_changes;
  }
  MESSAGE("bend " << k_bend << " deg: " << rest_covered << " -> " << bent_covered
                  << " covered pixels, " << coverage_changes << " changed; rest x ["
                  << rest_drawn.x0 << ", " << rest_drawn.x1 << "], bent x [" << bent_drawn.x0
                  << ", " << bent_drawn.x1 << "], predicted x [" << predicted.x0 << ", "
                  << predicted.x1 << "], y [" << predicted.y0 << ", " << predicted.y1
                  << "] against drawn y [" << bent_drawn.y0 << ", " << bent_drawn.y1 << "]");
  CHECK(coverage_changes > u64{rest_covered} / 10);
  // A straight bar seen side on is as wide as the column; a bent one leans its top half out and is
  // visibly wider. That is the check a picture which did not move at all, or moved along the view
  // axis where nothing would show, fails.
  CHECK(bent_drawn.x1 - bent_drawn.x0 > rest_drawn.x1 - rest_drawn.x0 + 8.0f);

  // And it is where the CPU reference says. Two statements, because the two are not the same:
  //
  //   * the drawn rectangle is **inside** the projected one — the picture never reaches past
  //     where `anim::skin_positions` put the vertices, which is what a wrong weight or a missing
  //     influence would break;
  //   * the two ends of the **bend axis** agree to within a pixel and a bit — the tip's
  //     silhouette is a real extremum of the surface and has to be drawn exactly there.
  //
  // The other two edges are only bounded from one side, because the drawn set is a subset of the
  // projected vertices for two honest reasons: the cut is an LOD cut rather than the source mesh,
  // and the bar's end caps are backfacing from this camera and are cone-culled, so the lowest
  // projected vertices are not on screen at all.
  constexpr f32 k_tolerance = 2.0f;
  CHECK(bent_drawn.x0 >= predicted.x0 - k_tolerance);
  CHECK(bent_drawn.x1 <= predicted.x1 + k_tolerance);
  CHECK(bent_drawn.y0 >= predicted.y0 - k_tolerance);
  CHECK(bent_drawn.y1 <= predicted.y1 + k_tolerance);
  CHECK(std::fabs(bent_drawn.x0 - predicted.x0) <= k_tolerance);
  CHECK(std::fabs(bent_drawn.x1 - predicted.x1) <= k_tolerance);
}

TEST_CASE("renderer: culling stays conservative at every phase of a bend") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  SkinnedBar bar;
  build_skinned_bar(bar);
  constexpr u32 k_width = 200;
  constexpr u32 k_height = 200;
  constexpr u32 k_phases = 12;

  // The bound over the whole "clip": the largest displacement any vertex reaches at any phase,
  // which is exactly what a per-instance `bounds_padding` has to be at least as large as.
  Vector<anim::JointMatrix> matrices;
  Vector<Vec3> moved(bar.positions.size());
  f32 bound = 0.0f;
  for (u32 p = 0; p < k_phases; ++p) {
    bar_matrices(bar, 90.0f * static_cast<f32>(p) / static_cast<f32>(k_phases - 1), matrices);
    anim::skin_positions(
        std::span<const Vec3>(bar.positions.data(), bar.positions.size()),
        std::span<const geometry::SkinBinding>(bar.bindings.data(), bar.bindings.size()),
        std::span<const anim::JointMatrix>(matrices.data(), matrices.size()),
        std::span<Vec3>(moved.data(), moved.size()));
    for (u32 v = 0; v < moved.size(); ++v)
      bound = std::max(bound, length(moved[v] - bar.positions[v]));
  }

  // Two scenes of the same bar: one padded by the bound, one by eight times it. If the smaller
  // padding ever dropped a cluster the larger one kept, a pixel would differ — so identical
  // pictures at every phase is the statement that the bound is enough. The camera is close enough
  // that the bar fills the frame, which is where a frustum test can actually bite.
  Rig tight;
  Rig loose;
  REQUIRE_MESSAGE(make_bar_scene(bar, 1, 2, bound, tight.data, tight.error), tight.error);
  REQUIRE_MESSAGE(make_bar_scene(bar, 1, 2, bound * 8.0f, loose.data, loose.error), loose.error);
  // Both scenes must frame the same way, or the two cameras would differ and the comparison would
  // be meaningless; the tighter one's bounds are the ones both use.
  loose.data.center = tight.data.center;
  loose.data.radius = tight.data.radius;
  REQUIRE_MESSAGE(tight.finish(gpu.device, RenderSettings{}, k_width, k_height), tight.error);
  REQUIRE_MESSAGE(loose.finish(gpu.device, RenderSettings{}, k_width, k_height), loose.error);

  const InstanceJoints runs[1] = {{0, 2}};
  CaptureChannels channels;
  channels.ids = true;
  u64 worst_differences = 0;
  u32 fewest_covered = ~u32{0};
  for (u32 p = 0; p < k_phases; ++p) {
    bar_matrices(bar, 90.0f * static_cast<f32>(p) / static_cast<f32>(k_phases - 1), matrices);
    const std::span<const anim::JointMatrix> span(matrices.data(), matrices.size());
    CapturedFrame a;
    CapturedFrame b;
    std::string error;
    REQUIRE_MESSAGE(
        tight.renderer.capture(
            skinned_frame(tight.data, span, std::span<const InstanceJoints>(runs, 1), 12.0f),
            channels, a, &error),
        error);
    REQUIRE_MESSAGE(
        loose.renderer.capture(
            skinned_frame(loose.data, span, std::span<const InstanceJoints>(runs, 1), 12.0f),
            channels, b, &error),
        error);
    u64 differences = 0;
    u32 covered = 0;
    for (u32 i = 0; i < k_width * k_height; ++i) {
      if (a.ids[i * k_id_words] != k_no_id) ++covered;
      for (u32 c = 0; c < k_id_words; ++c) {
        if (a.ids[i * k_id_words + c] != b.ids[i * k_id_words + c]) ++differences;
      }
    }
    worst_differences = std::max(worst_differences, differences);
    fewest_covered = std::min(fewest_covered, covered);
  }
  MESSAGE("bend sweep: bound " << bound << ", " << fewest_covered
                               << " covered pixels at the sparsest phase, " << worst_differences
                               << " id words differ from the 8x-padded scene");
  CHECK(fewest_covered > 1000);   // the bar is on screen at every phase
  CHECK(worst_differences == 0);  // nothing visible was culled at any of them
}

TEST_CASE("renderer: the mesh, vertex and ray paths agree on a posed frame") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  SkinnedBar bar;
  build_skinned_bar(bar);
  constexpr u32 k_width = 200;
  constexpr u32 k_height = 200;
  Vector<anim::JointMatrix> matrices;
  bar_matrices(bar, 45.0f, matrices);
  const std::span<const anim::JointMatrix> span(matrices.data(), matrices.size());
  const InstanceJoints runs[1] = {{0, 2}};
  CaptureChannels channels;
  channels.ids = true;

  auto shoot = [&](RasterMode mode, CapturedFrame& out, std::string& why) {
    Rig rig;
    if (!make_bar_scene(bar, 1, 2, 1.0f, rig.data, rig.error)) {
      why = rig.error;
      return false;
    }
    RenderSettings settings;
    settings.raster = mode;
    settings.shadows = ShadowMode::Off;  // a shadow is not what these three have to agree about
    ResolvedSettings probe;
    resolve_settings(settings, gpu.device.features(), &rig.data, probe);
    const RenderAvailability availability = check_availability(probe, gpu.device.features());
    if (availability != RenderAvailability::Ok) {
      why = unavailable_reason(availability, gpu.device);
      return false;
    }
    if (!rig.finish(gpu.device, settings, k_width, k_height)) {
      why = rig.error;
      return false;
    }
    std::string error;
    const bool ok = rig.renderer.capture(
        skinned_frame(rig.data, span, std::span<const InstanceJoints>(runs, 1), 22.0f), channels,
        out, &error);
    why = error;
    return ok;
  };

  // The vertex path is the reference the other two are held to, because it is the one every
  // device that renders at all has (ADR-0024's baseline tier). The mesh path and the ray path are
  // compared where the device has them and skipped with the missing feature named where it does
  // not: asking a device without mesh shaders for `hw` would resolve to the vertex path and
  // compare it with itself, which checks nothing.
  CapturedFrame vertex;
  std::string why;
  REQUIRE_MESSAGE(shoot(RasterMode::Vertex, vertex, why), why);
  u32 covered = 0;
  covered_rect(vertex, covered);
  CHECK(covered > 1000);

  if (gpu.device.features().mesh_shader) {
    CapturedFrame hardware;
    REQUIRE_MESSAGE(shoot(RasterMode::Hardware, hardware, why), why);
    u64 mesh_differences = 0;
    for (u32 i = 0; i < k_width * k_height; ++i) {
      for (u32 c = 0; c < k_id_words; ++c) {
        if (hardware.ids[i * k_id_words + c] != vertex.ids[i * k_id_words + c]) ++mesh_differences;
      }
    }
    MESSAGE("posed bar: " << covered << " covered pixels, mesh path differs in " << mesh_differences
                          << " id words");
    CHECK(mesh_differences == 0);  // the two rasterizers write the same words, deformed or not
  } else {
    MESSAGE("mesh path unavailable here: " << gpu.device.adapter().name
                                           << " has no VK_EXT_mesh_shader");
  }

  CapturedFrame ray;
  if (!shoot(RasterMode::RayTrace, ray, why)) {
    MESSAGE("ray path unavailable here: " << why);
    return;
  }
  // A ray and a rasterized edge disagree about the pixels a silhouette decides, exactly as they
  // do for a rigid mesh (gfx's ray query test), so coverage is compared rather than every word.
  u64 coverage_differences = 0;
  u64 triangle_differences = 0;
  for (u32 i = 0; i < k_width * k_height; ++i) {
    const bool a = vertex.ids[i * k_id_words] != k_no_id;
    const bool b = ray.ids[i * k_id_words] != k_no_id;
    if (a != b) {
      ++coverage_differences;
    } else if (a && vertex.ids[i * k_id_words + 2] != ray.ids[i * k_id_words + 2]) {
      ++triangle_differences;
    }
  }
  MESSAGE("ray against the rasterizers: " << coverage_differences << " coverage and "
                                          << triangle_differences << " triangle differences of "
                                          << covered << " covered pixels");
  CHECK(static_cast<f64>(coverage_differences) < 0.01 * static_cast<f64>(covered));
  CHECK(static_cast<f64>(triangle_differences) < 0.02 * static_cast<f64>(covered));
}

// ---- normal cones on a deformed instance -----------------------------------------------------
//
// A cluster's normal cone is fit to its **rest** triangles (geometry.md, "Normal cones"), and a
// deformed instance draws every position out of the frame's deformed-vertex pool. A skinned limb
// that turns far enough therefore puts a cluster in front of the camera that its rest-pose cone
// still says faces away, and a cull pass that trusted that cone would cut a hole in the character
// where it faces the viewer. "Culling must never change the picture" (AGENTS.md) is the statement
// this case makes about that: the same posed frame with cones on and off, every id word and every
// depth equal, in every raster path the cull pass feeds.
//
// The fixture is a ball on a joint — a head, or a wrist — bound wholly to the joint at its centre,
// so a turn of that joint spins the ball in place and triangles that faced away at rest face the
// camera. It is a closed sphere with narrow per-cluster cones (the tessellation the gfx cone test
// uses), so its rest-pose picture is exactly the one cones exist to speed up. A second, rigid copy
// stands beside it, so the same frame also shows that the cone test is still on where it is valid:
// with cones on, the rigid ball's back half is culled and the picture does not change.
namespace {

struct SkinnedBall {
  anim::Skeleton skeleton;
  Vector<Vec3> positions;
  Vector<geometry::SkinBinding> bindings;  // parallel to `positions`: all of it on the head joint
  Vector<u32> indices;
  static constexpr u32 k_rings = 24;
  static constexpr u32 k_segments = 48;  // 2,208 triangles
  static constexpr f32 k_radius = 0.5f;
  static constexpr f32 k_height = 1.5f;  // the head joint, and the ball's centre, above the root
};

void build_skinned_ball(SkinnedBall& ball) {
  ball.skeleton.resize(2);
  ball.skeleton.names[0] = "root";
  ball.skeleton.names[1] = "head";
  ball.skeleton.parents[0] = anim::k_no_joint;
  ball.skeleton.parents[1] = 0;
  ball.skeleton.local_bind[0] = Transform3::identity();
  ball.skeleton.local_bind[1] = Transform3::identity();
  ball.skeleton.local_bind[1].position = Vec3{0.0f, SkinnedBall::k_height, 0.0f};
  anim::compute_inverse_bind(ball.skeleton);

  // A UV sphere wound counter-clockwise seen from outside, as `domain/gfx`'s cone test builds it.
  const Vec3 centre{0.0f, SkinnedBall::k_height, 0.0f};
  const f32 r = SkinnedBall::k_radius;
  constexpr u32 rings = SkinnedBall::k_rings;
  constexpr u32 segments = SkinnedBall::k_segments;
  ball.positions.push_back(centre + Vec3{0.0f, r, 0.0f});
  for (u32 ring = 1; ring < rings; ++ring) {
    const f32 theta = k_pi * static_cast<f32>(ring) / static_cast<f32>(rings);
    for (u32 s = 0; s < segments; ++s) {
      const f32 phi = k_two_pi * static_cast<f32>(s) / static_cast<f32>(segments);
      const Vec3 unit{std::sin(theta) * std::cos(phi), std::cos(theta),
                      std::sin(theta) * std::sin(phi)};
      ball.positions.push_back(centre + unit * r);
    }
  }
  ball.positions.push_back(centre - Vec3{0.0f, r, 0.0f});
  const u32 bottom = ball.positions.size() - 1;
  // A constant, so the lambda needs no capture (Clang rejects an unneeded one as an error).
  auto at = [](u32 ring, u32 s) { return 1 + (ring - 1) * segments + (s % segments); };
  for (u32 s = 0; s < segments; ++s) {
    const u32 tri[3] = {0, at(1, s + 1), at(1, s)};
    for (const u32 i : tri)
      ball.indices.push_back(i);
  }
  for (u32 ring = 1; ring + 1 < rings; ++ring) {
    for (u32 s = 0; s < segments; ++s) {
      const u32 a = at(ring, s);
      const u32 b = at(ring, s + 1);
      const u32 c = at(ring + 1, s);
      const u32 d = at(ring + 1, s + 1);
      const u32 quad[6] = {a, b, c, b, d, c};
      for (const u32 i : quad)
        ball.indices.push_back(i);
    }
  }
  for (u32 s = 0; s < segments; ++s) {
    const u32 tri[3] = {at(rings - 1, s), at(rings - 1, s + 1), bottom};
    for (const u32 i : tri)
      ball.indices.push_back(i);
  }
  const u32 joints[4] = {1, 0, 0, 0};
  const f32 weights[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  const geometry::SkinBinding head = geometry::make_skin_binding(joints, weights);
  ball.bindings.resize(ball.positions.size(), head);
}

// The ball as a two-instance `SceneData`: instance 0 skinned by the two joints, instance 1 the
// same mesh rigid, beside it. Filled in by hand for the reason `make_bar_scene` is.
bool make_ball_scene(const SkinnedBall& ball, f32 bounds_padding, SceneData& out,
                     std::string& error) {
  geometry::AttributeSource attributes;
  attributes.skin =
      std::span<const geometry::SkinBinding>(ball.bindings.data(), ball.bindings.size());
  attributes.joint_count = ball.skeleton.joint_count();
  if (!geometry::build_cluster_lod(
          std::span<const Vec3>(ball.positions.data(), ball.positions.size()),
          std::span<const u32>(ball.indices.data(), ball.indices.size()),
          geometry::ClusterLodOptions{}, out.lod, &error, attributes)) {
    return false;
  }
  geometry::ClusterMeshPart part;
  part.cluster_count = out.lod.mesh.clusters.size();
  part.leaf_cluster_count = out.lod.level_cluster_counts[0];
  part.quant_origin = out.lod.mesh.quant_origin;
  part.quant_scale = out.lod.mesh.quant_scale;
  out.parts.push_back(part);
  SourceMesh source;
  source.part_material.push_back(-1);
  source.part_of_cluster.resize(part.cluster_count, 0u);
  out.sources.push_back(std::move(source));
  for (u32 i = 0; i < 2; ++i) {
    const bool skinned = i == 0;
    gfx::InstanceDesc instance{};
    Mat4 world = Mat4::identity();
    world.c[3] = Vec4{static_cast<f32>(i) * 1.5f, 0.0f, 0.0f, 1.0f};
    gfx::set_instance_transform(instance, world);
    instance.first_pair = out.pair_count;
    instance.bounds_padding = skinned ? bounds_padding : 0.0f;
    out.pair_count += part.cluster_count;
    out.instances.push_back(instance);
    out.instance_joints.push_back(skinned ? 2u : 0u);
    out.skinned_instances += skinned ? 1u : 0u;
  }
  out.max_joints = 2;
  update_scene_bounds(out);
  return true;
}

// The ball's skinning matrices for a turn of the head joint by `degrees` about +y.
void ball_matrices(const SkinnedBall& ball, f32 degrees, Vector<anim::JointMatrix>& out) {
  anim::Pose pose;
  anim::rest_pose(ball.skeleton, pose);
  pose.rotation[1] = quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, radians(degrees));
  Vector<Mat4> model(ball.skeleton.joint_count(), Mat4::identity());
  anim::local_to_model(ball.skeleton, pose, std::span<Mat4>(model.data(), model.size()));
  out.resize(ball.skeleton.joint_count());
  anim::skinning_matrices(
      std::span<const Mat4>(model.data(), model.size()),
      std::span<const Mat4>(ball.skeleton.inverse_bind.data(), ball.skeleton.inverse_bind.size()),
      std::span<anim::JointMatrix>(out.data(), out.size()));
}

}  // namespace

TEST_CASE("renderer: a turned joint's clusters are not culled by their rest-pose cones") {
  SkinnedBall ball;
  build_skinned_ball(ball);
  constexpr u32 k_width = 200;
  constexpr u32 k_height = 200;
  constexpr f32 k_turn = 150.0f;  // far enough to bring the back of the ball round, not a mirror

  Vector<anim::JointMatrix> turned;
  ball_matrices(ball, k_turn, turned);
  const std::span<const anim::JointMatrix> joints(turned.data(), turned.size());
  Vector<Vec3> moved(ball.positions.size());
  anim::skin_positions(
      std::span<const Vec3>(ball.positions.data(), ball.positions.size()),
      std::span<const geometry::SkinBinding>(ball.bindings.data(), ball.bindings.size()), joints,
      std::span<Vec3>(moved.data(), moved.size()));
  f32 displacement = 0.0f;
  for (u32 v = 0; v < moved.size(); ++v)
    displacement = std::max(displacement, length(moved[v] - ball.positions[v]));
  CHECK(displacement > 0.9f);  // 2 r sin(75 deg): the ball really turned

  const InstanceJoints runs[2] = {{0, 2}, {0, 0}};
  CaptureChannels channels;
  channels.ids = true;
  channels.depth = true;

  // The witness, on the CPU and so on every machine, a GPU or none: the fixture is the case it
  // claims to be. The frame's cut is the leaves (`lod_px = 0`), and among the skinned instance's
  // leaves there are clusters the rest-pose cone test (`geometry::cluster_backfacing`, the shader's
  // arithmetic) culls from this camera although, turned, a triangle of theirs faces it. Without one
  // of those the comparison below could not fail, and a test that cannot fail says nothing.
  u32 witness_leaves = 0;
  u32 rest_culled = 0;
  u32 wrongly_culled = 0;
  {
    SceneData probe;
    std::string error;
    REQUIRE_MESSAGE(make_ball_scene(ball, displacement, probe, error), error);
    const Vec3 eye =
        relative(skinned_frame(probe, joints, std::span<const InstanceJoints>(runs, 2), 22.0f)
                     .camera.position,
                 WorldPos::origin());
    const geometry::ClusterMesh& mesh = probe.lod.mesh;
    Vector<Vec3> skinned(mesh.vertices.size());
    anim::skin_positions(std::span<const Vec3>(mesh.vertices.data(), mesh.vertices.size()),
                         std::span<const geometry::SkinBinding>(mesh.skin.data(), mesh.skin.size()),
                         joints, std::span<Vec3>(skinned.data(), skinned.size()));
    for (u32 c = 0; c < mesh.clusters.size(); ++c) {
      if (probe.lod.lod[c].level != 0) continue;
      ++witness_leaves;
      const geometry::ClusterDesc& cluster = mesh.clusters[c];
      if (!geometry::cluster_backfacing(cluster, eye)) continue;  // instance 0's world is identity
      ++rest_culled;
      bool faces_camera = false;
      for (u32 t = 0; t < cluster.triangle_count && !faces_camera; ++t) {
        const u32 packed = mesh.triangles[cluster.triangle_offset + t];
        const Vec3 a = skinned[cluster.vertex_offset + geometry::ClusterMesh::unpack(packed, 0)];
        const Vec3 b = skinned[cluster.vertex_offset + geometry::ClusterMesh::unpack(packed, 1)];
        const Vec3 d = skinned[cluster.vertex_offset + geometry::ClusterMesh::unpack(packed, 2)];
        faces_camera = dot(cross(b - a, d - a), eye - a) > 0.0f;
      }
      if (faces_camera) ++wrongly_culled;
    }
  }
  MESSAGE("turned " << k_turn << " deg: " << witness_leaves << " leaf clusters, " << rest_culled
                    << " culled by their rest-pose cones, " << wrongly_culled
                    << " of those face the camera once turned");
  REQUIRE(wrongly_culled > 0);

  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }

  // The picture, both ways, through every raster path the cull pass feeds. The baseline vertex path
  // and the software rasterizer read the same visible list the mesh shaders do, and the ray path
  // builds its geometry from it, so a cluster the cull pass wrongly drops is missing from all four.
  const RasterMode modes[4] = {RasterMode::Hardware, RasterMode::Vertex, RasterMode::Software,
                               RasterMode::RayTrace};
  for (const RasterMode mode : modes) {
    // `hw` on a device without mesh shaders is the vertex path, which has its own turn below.
    if (mode == RasterMode::Hardware && !gpu.device.features().mesh_shader) {
      MESSAGE("hw path unavailable here: " << gpu.device.adapter().name
                                           << " has no VK_EXT_mesh_shader");
      continue;
    }
    CapturedFrame shots[2];
    u32 visible[2] = {0, 0};
    bool available = true;
    for (u32 k = 0; k < 2 && available; ++k) {
      Rig rig;
      REQUIRE_MESSAGE(make_ball_scene(ball, displacement, rig.data, rig.error), rig.error);
      RenderSettings settings;
      settings.raster = mode;
      settings.cone = k == 1;
      settings.shadows = ShadowMode::Off;  // shadows are not what the two have to agree about
      ResolvedSettings resolved;
      resolve_settings(settings, gpu.device.features(), &rig.data, resolved);
      const RenderAvailability availability = check_availability(resolved, gpu.device.features());
      if (availability != RenderAvailability::Ok) {
        const std::string path = raster_name(mode);
        MESSAGE(path << " path unavailable here: " << unavailable_reason(availability, gpu.device));
        available = false;
        break;
      }
      REQUIRE_MESSAGE(rig.finish(gpu.device, settings, k_width, k_height), rig.error);
      FrameDesc frame =
          skinned_frame(rig.data, joints, std::span<const InstanceJoints>(runs, 2), 22.0f);
      frame.lod_px = 0.0f;  // the leaves: the cut the witness above reasoned about
      std::string error;
      REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shots[k], &error), error);
      visible[k] = rig.renderer.stats().visible_pairs();
    }
    if (!available) continue;
    u64 id_differences = 0;
    u64 depth_differences = 0;
    u32 covered = 0;
    u32 per_instance[2] = {0, 0};
    for (u32 p = 0; p < k_width * k_height; ++p) {
      const u32 instance = shots[0].ids[p * k_id_words];
      if (instance != k_no_id) {
        ++covered;
        if (instance < 2) ++per_instance[instance];
      }
      for (u32 c = 0; c < k_id_words; ++c) {
        if (shots[0].ids[p * k_id_words + c] != shots[1].ids[p * k_id_words + c]) ++id_differences;
      }
      if (shots[0].depth[p] != shots[1].depth[p]) ++depth_differences;
    }
    const std::string path = raster_name(mode);
    MESSAGE(path << ": " << covered << " covered pixels (" << per_instance[0] << " skinned, "
                 << per_instance[1] << " rigid), visible pairs " << visible[0]
                 << " with cones off and " << visible[1] << " on, " << id_differences
                 << " id words and " << depth_differences << " depths differ");
    CHECK(per_instance[0] > 1000);  // both balls are on screen
    CHECK(per_instance[1] > 1000);
    CHECK(id_differences == 0);
    CHECK(depth_differences == 0);
    // And the cone test is still on where it is valid: the rigid ball's back half goes.
    CHECK(visible[1] < visible[0]);
  }
}
