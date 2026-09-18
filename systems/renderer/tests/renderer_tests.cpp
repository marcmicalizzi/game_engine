// The renderer, headless. Every case here builds a scene from a cube GLB written at test time,
// renders it into an offscreen color target, and reads the result back: no window, no surface,
// no swapchain, and no file in the tree. On a machine with no Vulkan device, or one that cannot
// write the visibility buffer, the cases record a skip, exactly as engine-view exits 3.
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
#include <domain/gfx/device.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
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
// material and the embedded texture, because nothing here is about materials.
bool write_cube_glb(const std::string& path) {
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

  std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
      "\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.8,0.5,0.2,1],"
      "\"metallicFactor\":0,\"roughnessFactor\":0.6}}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\","
      "\"min\":[-0.5,-0.5,-0.5],\"max\":[0.5,0.5,0.5]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":24,\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5123,\"count\":36,\"type\":\"SCALAR\"}],"
      "\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
      n(normal_offset) + "},{\"buffer\":0,\"byteOffset\":" + n(normal_offset) +
      ",\"byteLength\":" + n(uv_offset - normal_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + n(uv_offset) +
      ",\"byteLength\":" + n(index_offset - uv_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + n(index_offset) + ",\"byteLength\":" + n(index_bytes) +
      "}],\"buffers\":[{\"byteLength\":" + n(static_cast<u32>(bin.size())) + "}]}";
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
    if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
      error = availability_message(check_availability(resolved, device.features()));
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
  return Vec3{world.x, world.y, world.z} * (1.0f / world.w);
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

  // A path that cannot shadow says so and leaves everything else alone.
  settings.raster = RasterMode::Software;
  settings.shadows = ShadowMode::RayTraced;
  settings.rt_templates = true;
  resolve_settings(settings, features, nullptr, resolved);
  CHECK_FALSE(resolved.shadows);
  CHECK_FALSE(resolved.rt_chain);
  CHECK_FALSE(resolved.settings.rt_templates);
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
  frame.camera.position = Vec3{0.0f, 0.0f, 12.0f};
  frame.camera.target = Vec3{0.0f, 0.0f, 0.0f};
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
  camera.position = Vec3{0.0f, 0.0f, k_distance};
  camera.target = Vec3{0.0f, 0.0f, 0.0f};
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
  // the one top-level structure built from the union of the views' cuts.
  for (const RasterMode mode : {RasterMode::Hardware, RasterMode::RayTrace}) {
    if (mode == RasterMode::RayTrace && !(gpu.device.features().cluster_acceleration_structure &&
                                          gpu.device.features().ray_query)) {
      MESSAGE("no cluster acceleration structures here: the ray path is skipped");
      continue;
    }
    if (mode == RasterMode::Hardware && !gpu.device.features().mesh_shader) continue;
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
      INFO("raster " << raster_name(mode) << ", view " << v << " names instance " << centre[0]);
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
  const Mat4 world = rig.data.instances[0].world;
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
    if (check_availability(probe, gpu.device.features()) != RenderAvailability::Ok) {
      why = availability_message(check_availability(probe, gpu.device.features()));
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

  CapturedFrame hardware;
  std::string why;
  REQUIRE_MESSAGE(shoot(RasterMode::Hardware, hardware, why), why);
  u32 hardware_covered = 0;
  covered_rect(hardware, hardware_covered);
  CHECK(hardware_covered > 1000);

  CapturedFrame vertex;
  REQUIRE_MESSAGE(shoot(RasterMode::Vertex, vertex, why), why);
  u64 vertex_differences = 0;
  for (u32 i = 0; i < k_width * k_height; ++i) {
    for (u32 c = 0; c < k_id_words; ++c) {
      if (hardware.ids[i * k_id_words + c] != vertex.ids[i * k_id_words + c]) ++vertex_differences;
    }
  }
  MESSAGE("posed bar: " << hardware_covered << " covered pixels, vertex path differs in "
                        << vertex_differences << " id words");
  CHECK(vertex_differences == 0);  // the two rasterizers write the same words, deformed or not

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
    const bool a = hardware.ids[i * k_id_words] != k_no_id;
    const bool b = ray.ids[i * k_id_words] != k_no_id;
    if (a != b) {
      ++coverage_differences;
    } else if (a && hardware.ids[i * k_id_words + 2] != ray.ids[i * k_id_words + 2]) {
      ++triangle_differences;
    }
  }
  MESSAGE("ray against mesh shader: " << coverage_differences << " coverage and "
                                      << triangle_differences << " triangle differences of "
                                      << hardware_covered << " covered pixels");
  CHECK(static_cast<f64>(coverage_differences) < 0.01 * static_cast<f64>(hardware_covered));
  CHECK(static_cast<f64>(triangle_differences) < 0.02 * static_cast<f64>(hardware_covered));
}
