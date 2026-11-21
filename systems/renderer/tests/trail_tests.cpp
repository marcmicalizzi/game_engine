// The accumulation leaves no trails (roadmap R194; docs/subsystems/renderer.md, "Sampling over
// time: the accumulation", "What a trail is, and what holds it").
//
// **What a trail is held to.** A mover crosses a ground of another colour for thirty frames with
// the accumulation on, and at the end the same frame is drawn fresh (a reset: one sample, nothing
// of the frames before). The pixels the mover left exactly `a` frames ago (its ids named it at the
// frame `a` before the last and at none after, and no pixel of their 3x3 shows it now, which would
// be its edge's antialiasing) are where a trail lies; the trail at age `a` is the
// mean of |accumulated - fresh| luma over them, in display codes, and it is held against what the
// same comparison gives on pixels the mover never touched (the floor: the offset's own aliasing,
// which a single fresh frame has and the accumulated one has averaged away) and against the
// mover's contrast with the ground. A blend that keeps the mover's history at those pixels leaves
// nine tenths of the contrast at age 1 and still a third at age 8; a history refused there leaves
// the floor.
//
// Every kind of motion the frame draws: a rigid instance the host moves (a scene read whole, and a
// streamed world's scene, whose move marks its tables changed), an instance of the world's tail
// placed again every frame (a game's prop), a skinned walker whose bones bend as it steps, and a
// camera walking past a near post in front of a far wall (parallax, where nothing moves but the
// eye). And the converse, held as it was: a still camera on a still scene converges to the bytes it
// did (accumulation_tests.cpp, "a held view converges").
#include "test_device.h"

#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/anim/skeleton.h>
#include <domain/assets/gltf.h>
#include <domain/geometry/cluster.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/gfx/device.h>
#include <foundation/image/png.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/view_set.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
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
    if (!device.create(renderer_test::device_options(), &error)) {
      why = "no Vulkan device: " + error;
      if (!renderer_test::device_profile().empty()) {
        FAIL_CHECK("ENGINE_GFX_TEST_DEVICE=" << renderer_test::device_profile() << ": " << why);
      }
      return;
    }
    if (!device.features().buffer_int64_atomics) {
      why = std::string(device.adapter().name) + " has no 64-bit buffer atomics";
      return;
    }
    ok = true;
  }
  ~Gpu() { device.destroy(); }
};

struct ScopedTunable {
  tunables::Tunable* tunable = nullptr;
  std::string previous;
  ScopedTunable(const char* name, const char* value) {
    tunable = tunables::find(name);
    REQUIRE_MESSAGE(tunable != nullptr, name);
    tunable->append_value(previous);
    REQUIRE(tunable->set_from_text(value));
  }
  ~ScopedTunable() {
    if (tunable != nullptr) (void)tunable->set_from_text(previous);
  }
};

constexpr u32 k_width = 256;
constexpr u32 k_height = 144;
constexpr u32 k_pixels = k_width * k_height;
constexpr u32 k_frames = 30;

// ---- the fixture's meshes ---------------------------------------------------------------------

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

// A cube of half-size 1, counter-clockwise from outside.
MeshSource box_mesh() {
  MeshSource m;
  constexpr u32 n = 4;
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

// A flat square of side 2 facing up.
MeshSource sheet_mesh() {
  MeshSource m;
  add_face(m, Vec3{-1.0f, 0.0f, -1.0f}, Vec3{0.0f, 0.0f, 2.0f}, Vec3{2.0f, 0.0f, 0.0f}, 16);
  return m;
}

// The LOD builder's leaves as roots, so every frame draws the same cut and a difference between
// two frames is the motion's and the offset's alone (visibility_tie_tests.cpp has why).
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

// The meshes, in this order: a bright box, a dark box, a bright sheet, a dark sheet. Each is one
// part with a material of its own; the bright ones glow a little so that they are bright whatever
// the light falls like.
enum Mesh : u32 { k_bright_box = 0, k_dark_box, k_bright_sheet, k_dark_sheet, k_mesh_count };

assets::Material material(f32 albedo, f32 glow) {
  assets::Material m;
  m.base_color = Vec4{albedo, albedo, albedo, 1.0f};
  m.emissive = Vec3{glow, glow, glow};
  m.metallic = 0.0f;
  m.roughness = 0.9f;
  return m;
}

SceneInstance placed(u32 mesh, Vec3 at, Vec3 scale) {
  SceneInstance instance;
  instance.mesh = mesh;
  instance.transform.position = at;
  instance.transform.scale = scale;
  return instance;
}

bool make_scene(std::span<const SceneInstance> instances, bool dynamic, SceneData& out,
                std::string& error) {
  geometry::ClusterLodMesh parts[k_mesh_count];
  if (!leaves_as_roots(box_mesh(), parts[k_bright_box], error) ||
      !leaves_as_roots(box_mesh(), parts[k_dark_box], error) ||
      !leaves_as_roots(sheet_mesh(), parts[k_bright_sheet], error) ||
      !leaves_as_roots(sheet_mesh(), parts[k_dark_sheet], error) ||
      !geometry::merge_cluster_meshes(
          std::span<const geometry::ClusterLodMesh>(parts, k_mesh_count), out.lod, out.parts,
          &error)) {
    return false;
  }
  const assets::Material materials[k_mesh_count] = {material(0.9f, 0.6f), material(0.03f, 0.0f),
                                                    material(0.85f, 0.4f), material(0.04f, 0.0f)};
  for (u32 m = 0; m < k_mesh_count; ++m) {
    SourceMesh source;
    source.data.materials.push_back(materials[m]);
    source.part_material.push_back(0);
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
  out.center = WorldPos{};
  out.radius = 14.0f;
  return true;
}

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  std::string error;

  // The picture's own rasterizer on whatever the device is, no shadows, no dither: what changes
  // from frame to frame is the motion and the offset.
  bool finish(const gfx::Device& device) {
    RenderSettings settings;
    settings.raster = RasterMode::Hardware;
    settings.shadows = ShadowMode::Off;
    settings.dither = false;
    resolve_settings(settings, device.features(), &data, resolved);
    const RenderAvailability availability = check_availability(resolved, device.features());
    if (availability != RenderAvailability::Ok) {
      error = unavailable_reason(availability, device);
      return false;
    }
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = k_width;
    rd.height = k_height;
    rd.frames_in_flight = 2;
    return renderer.create(device, scene, resolved, rd, &error);
  }

  bool build(const gfx::Device& device, std::span<const SceneInstance> instances, bool dynamic) {
    return make_scene(instances, dynamic, data, error) && finish(device);
  }
};

FrameDesc frame_at(Vec3 eye, Vec3 target) {
  FrameDesc frame;
  frame.camera.position = absolute(WorldPos::origin(), eye);
  frame.camera.target = absolute(WorldPos::origin(), target);
  return frame;
}

f64 luma(const Vector<u8>& color, u32 p) {
  return 0.2126 * color[p * 4] + 0.7152 * color[p * 4 + 1] + 0.0722 * color[p * 4 + 2];
}

// ---- the measurement --------------------------------------------------------------------------

constexpr u32 k_ages[4] = {1, 2, 4, 8};

struct Trail {
  f64 age[4] = {};     // codes: mean |accumulated - fresh| over the pixels left `k_ages[i]` ago
  u32 pixels[4] = {};  // how many such pixels
  f64 floor = 0.0;     // the same over pixels the mover never touched
  f64 contrast = 0.0;  // the mover's mean luma against the rest's, in the fresh frame
  bool ok = false;
  std::string error;       // why a flight did not finish
  Vector<u8> accumulated;  // the flight's last frame
  Vector<u8> fresh;        // and the same frame drawn fresh
};

// **The captures** (the row's pictures): with `ENGINE_TRAIL_CAPTURES` naming a directory, each
// flight's last accumulated frame and its fresh one are written there as PNGs named for the case.
// Best effort, never asserted on.
void keep_captures(const char* what, const Trail& t) {
  const std::string dir = test::detail::environment("ENGINE_TRAIL_CAPTURES");
  if (dir.empty() || !t.ok) return;
  std::string name;
  for (const char* c = what; *c != 0; ++c)
    name += (std::isalnum(static_cast<unsigned char>(*c)) != 0) ? *c : '-';
  const std::string base = dir + "/" + name;
  (void)image::write_png(base + ".accumulated.png", k_width, k_height, 4,
                         std::span<const u8>(t.accumulated.data(), t.accumulated.size()));
  (void)image::write_png(base + ".fresh.png", k_width, k_height, 4,
                         std::span<const u8>(t.fresh.data(), t.fresh.size()));
}

// One flight: `setup(k, frame)` makes frame `k` (moves the mover, places the camera) and returns
// false with `rig.error` on a failure; `is_mover(ids word)` says which pixels the mover holds.
// Frame 0 resets the history; the last is drawn again fresh after the flight.
template <typename Setup, typename IsMover>
Trail fly(Rig& rig, Setup setup, IsMover is_mover) {
  Trail out;
  Vector<Vector<u8>> masks(k_frames);
  CaptureChannels channels;
  channels.ids = true;
  CapturedFrame shot;
  FrameDesc frame;
  for (u32 k = 0; k < k_frames; ++k) {
    if (!setup(k, frame)) {
      out.error = rig.error;
      return out;
    }
    frame.history_reset = k == 0;
    if (!rig.renderer.capture(frame, channels, shot, &rig.error)) {
      out.error = rig.error;
      return out;
    }
    masks[k].resize(k_pixels);
    for (u32 p = 0; p < k_pixels; ++p)
      masks[k][p] = is_mover(shot.ids[p * k_id_words]) ? 1 : 0;
  }
  const CapturedFrame accumulated = shot;
  frame.history_reset = true;
  CapturedFrame fresh;
  if (!rig.renderer.capture(frame, channels, fresh, &rig.error)) {
    out.error = rig.error;
    return out;
  }
  const u32 last = k_frames - 1;
  f64 floor_sum = 0.0;
  u32 floor_n = 0;
  f64 mover_sum = 0.0;
  u32 mover_n = 0;
  f64 rest_sum = 0.0;
  u32 rest_n = 0;
  for (u32 p = 0; p < k_pixels; ++p) {
    const f64 apart = std::abs(luma(accumulated.color, p) - luma(fresh.color, p));
    bool touched = false;
    for (u32 k = 0; k < k_frames; ++k)
      touched = touched || masks[k][p] != 0;
    // The ids of the two last frames agree (the fresh frame is the same state): the floor's pixels
    // show the same surface in both.
    if (!touched && is_mover(fresh.ids[p * k_id_words]) == false) {
      floor_sum += apart;
      ++floor_n;
    }
    if (is_mover(fresh.ids[p * k_id_words])) {
      mover_sum += luma(fresh.color, p);
      ++mover_n;
    } else if (fresh.ids[p * k_id_words] != k_no_id || !touched) {
      rest_sum += luma(fresh.color, p);
      ++rest_n;
    }
    // A pixel beside where the mover is now holds some of it in a picture averaged over offsets,
    // and that is the edge's antialiasing, not a trail: a thin frond's left pixels are mostly such
    // (the palm: 7 codes at age 1 of what is the coverage of its leaflets). Only pixels whose 3x3
    // in the fresh frame holds none of the mover are a trail's.
    bool beside = false;
    const u32 x = p % k_width;
    const u32 y = p / k_width;
    for (u32 n = 0; n < 9 && !beside; ++n) {
      const i32 nx = static_cast<i32>(x) + static_cast<i32>(n % 3) - 1;
      const i32 ny = static_cast<i32>(y) + static_cast<i32>(n / 3) - 1;
      if (nx < 0 || ny < 0 || nx >= static_cast<i32>(k_width) || ny >= static_cast<i32>(k_height))
        continue;
      beside =
          is_mover(fresh.ids[(static_cast<u32>(ny) * k_width + static_cast<u32>(nx)) * k_id_words]);
    }
    for (u32 i = 0; i < 4; ++i) {
      const u32 a = k_ages[i];
      if (masks[last - a][p] == 0 || beside) continue;
      bool after = false;
      for (u32 k = last - a + 1; k <= last; ++k)
        after = after || masks[k][p] != 0;
      if (after) continue;
      out.age[i] += apart;
      ++out.pixels[i];
    }
  }
  for (u32 i = 0; i < 4; ++i)
    out.age[i] = out.pixels[i] > 0 ? out.age[i] / out.pixels[i] : 0.0;
  out.floor = floor_n > 0 ? floor_sum / floor_n : 0.0;
  out.contrast =
      mover_n > 0 && rest_n > 0 ? std::abs(mover_sum / mover_n - rest_sum / rest_n) : 0.0;
  out.accumulated = accumulated.color;
  out.fresh = fresh.color;
  out.ok = true;
  return out;
}

void report(const char* what, const Trail& t) {
  MESSAGE(std::string(what) << ": contrast " << t.contrast << " codes, floor " << t.floor
                            << "; trail at age 1, 2, 4, 8: " << t.age[0] << " (" << t.pixels[0]
                            << " px), " << t.age[1] << " (" << t.pixels[1] << "), " << t.age[2]
                            << " (" << t.pixels[2] << "), " << t.age[3] << " (" << t.pixels[3]
                            << ")");
  keep_captures(what, t);
}

// What every case holds: the fixture saw its mover leave pixels at every age and the mover stands
// out from its ground; and at every age the trail is within the floor and a twentieth of the
// contrast (a history kept there leaves nine tenths at age 1; a sixteenth of the contrast is a
// little under the blend's one frame's share, which a refused history at the pixel still mixes in
// for the frames its 3x3 kept the mover: the edge it left).
void hold(const Trail& t) {
  REQUIRE_MESSAGE(t.ok, t.error);
  // A mover slower than a pixel and a half a frame leaves no pixel at age 1 outside its edge's 3x3.
  u32 counted = 0;
  for (u32 i = 0; i < 4; ++i)
    counted += t.pixels[i];
  CHECK(counted > 40);
  CHECK(t.pixels[3] > 10);
  CHECK(t.contrast > 40.0);
  for (u32 i = 0; i < 4; ++i) {
    CAPTURE(k_ages[i]);
    CHECK(t.age[i] <= t.floor + t.contrast / 20.0);
  }
}

const Vec3 k_eye{0.0f, 9.0f, 14.0f};
const Vec3 k_target{0.0f, 0.0f, 0.0f};

// ---- a walker: the two-bone bar of dynamic_instances_tests.cpp, bright, on a dark ground ------

struct Bar {
  anim::Skeleton skeleton;
  MeshSource mesh;
  Vector<geometry::SkinBinding> bindings;  // parallel to the mesh's positions
};

// A square column 2 high in 32 slices, bound root to tip by height with a ramp between the quarter
// and three-quarter marks.
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
  const f32 h = 0.3f;
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

SceneInstance bar_at(Vec3 at) {
  // Its joints are the scene's (`instance_joints`), as dynamic_instances_tests.cpp lays a skinned
  // loaded instance out: `make_instance` places rigid instances only.
  SceneInstance standing = placed(0, at, Vec3{1.0f, 1.0f, 1.0f});
  standing.bounds_padding = 1.0f;  // a bend of the tip moves no vertex further than this
  return standing;
}

// Two meshes, the bar (skinned, first, so the merged mesh carries a skin; bright) and a dark sheet;
// the bar standing at `at` as instance 0 and the ground as instance 1.
bool make_walker_scene(const Bar& bar, bool dynamic, Vec3 at, SceneData& out, std::string& error) {
  geometry::ClusterLodMesh parts[2];
  geometry::AttributeSource attributes;
  attributes.normals = std::span<const Vec3>(bar.mesh.normals.data(), bar.mesh.normals.size());
  attributes.skin =
      std::span<const geometry::SkinBinding>(bar.bindings.data(), bar.bindings.size());
  attributes.joint_count = bar.skeleton.joint_count();
  if (!geometry::build_cluster_lod(
          std::span<const Vec3>(bar.mesh.positions.data(), bar.mesh.positions.size()),
          std::span<const u32>(bar.mesh.indices.data(), bar.mesh.indices.size()),
          geometry::ClusterLodOptions{}, parts[0], &error, attributes) ||
      !leaves_as_roots(sheet_mesh(), parts[1], error) ||
      !geometry::merge_cluster_meshes(std::span<const geometry::ClusterLodMesh>(parts, 2), out.lod,
                                      out.parts, &error)) {
    return false;
  }
  const assets::Material materials[2] = {material(0.9f, 0.6f), material(0.04f, 0.0f)};
  for (u32 m = 0; m < 2; ++m) {
    SourceMesh source;
    source.data.materials.push_back(materials[m]);
    source.part_material.push_back(0);
    source.part_of_cluster.resize(out.parts[m].cluster_count, 0u);
    out.sources.push_back(std::move(source));
    out.mesh_fit.push_back(Mat4::identity());
  }
  const SceneInstance list[2] = {bar_at(at), placed(1, Vec3{}, Vec3{12.0f, 1.0f, 12.0f})};
  for (u32 i = 0; i < 2; ++i) {
    const SceneInstance& source = list[i];
    gfx::InstanceDesc instance;
    std::string why;
    if (!make_instance(out, source, out.pair_count, instance, &why)) {
      error = why;
      return false;
    }
    out.pair_count += out.parts[source.mesh].cluster_count;
    out.instances.push_back(instance);
    const u32 joints = i == 0 ? 2u : 0u;
    out.instance_joints.push_back(joints);
    out.skinned_instances += joints > 0 ? 1u : 0u;
  }
  out.max_joints = 2;
  out.dynamic = dynamic;
  update_scene_bounds(out);
  out.center = WorldPos{};
  out.radius = 14.0f;
  return true;
}

// A box crossing a ground of the other brightness under a held camera, moved by the host every
// frame `step` metres along x (`move_instances`), in a scene read whole or one that takes a tail
// (a streamed world's, where every move marks the scene's tables changed).
Trail moved_box(const gfx::Device& device, bool bright_on_dark, bool dynamic, f32 step) {
  const u32 ground = bright_on_dark ? k_dark_sheet : k_bright_sheet;
  const u32 box = bright_on_dark ? k_bright_box : k_dark_box;
  const auto box_at = [&](u32 k) {
    return placed(box, Vec3{-4.0f + step * static_cast<f32>(k), 1.0f, 0.0f},
                  Vec3{0.8f, 0.8f, 0.8f});
  };
  const SceneInstance instances[2] = {placed(ground, Vec3{}, Vec3{12.0f, 1.0f, 12.0f}), box_at(0)};
  Rig rig;
  if (!rig.build(device, std::span<const SceneInstance>(instances, 2), dynamic)) {
    FAIL(rig.error);
    return {};
  }
  REQUIRE(rig.resolved.accumulation == Accumulation::On);
  REQUIRE(rig.scene.dynamic() == dynamic);
  return fly(
      rig,
      [&](u32 k, FrameDesc& frame) {
        frame = frame_at(k_eye, k_target);
        if (k == 0) return true;
        const SceneInstance at = box_at(k);
        const u32 index = 1;
        return rig.renderer.move_instances(std::span<const u32>(&index, 1),
                                           std::span<const SceneInstance>(&at, 1), &rig.error);
      },
      [](u32 id) { return id == 1u; });
}

}  // namespace

TEST_CASE("trails: a rigid instance the host moves leaves none") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  ScopedTunable mode("renderer.reconstruction.accumulation", "on");
  const struct {
    const char* what;
    bool bright_on_dark;
    bool dynamic;
    f32 step;
  } runs[] = {
      {"bright on dark, read whole, 0.25 m a frame", true, false, 0.25f},
      {"dark on bright, read whole, 0.25 m a frame", false, false, 0.25f},
      {"bright on dark, streamed scene, 0.25 m a frame", true, true, 0.25f},
      {"dark on bright, streamed scene, 0.25 m a frame", false, true, 0.25f},
      {"bright on dark, streamed scene, 0.15 m a frame", true, true, 0.15f},
  };
  for (const auto& run : runs) {
    CAPTURE(run.what);
    const Trail t = moved_box(gpu.device, run.bright_on_dark, run.dynamic, run.step);
    report(run.what, t);
    hold(t);
  }
}

// A game's prop: an instance of the world's tail that the host places again every frame through
// `set_dynamic_instances`. It has no identity across two tails, so the motion pass cannot follow it
// (its vectors are the camera's) and the history test has to find what it left by itself.
TEST_CASE("trails: an instance of the world's tail placed again every frame leaves none") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  ScopedTunable mode("renderer.reconstruction.accumulation", "on");
  for (u32 bright = 0; bright < 2; ++bright) {
    const char* what = bright != 0 ? "a tail's bright box on dark" : "a tail's dark box on bright";
    CAPTURE(what);
    const SceneInstance ground =
        placed(bright != 0 ? k_dark_sheet : k_bright_sheet, Vec3{}, Vec3{12.0f, 1.0f, 12.0f});
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, std::span<const SceneInstance>(&ground, 1), true),
                    rig.error);
    const Trail t = fly(
        rig,
        [&](u32 k, FrameDesc& frame) {
          frame = frame_at(k_eye, k_target);
          const SceneInstance box =
              placed(bright != 0 ? k_bright_box : k_dark_box,
                     Vec3{-4.0f + 0.25f * static_cast<f32>(k), 1.0f, 0.0f}, Vec3{0.8f, 0.8f, 0.8f});
          return rig.renderer.set_dynamic_instances(std::span<const SceneInstance>(&box, 1),
                                                    &rig.error);
        },
        // Whatever slot the tail's box was laid out in: everything but the ground and the sky.
        [](u32 id) { return id != 0u && id != k_no_id; });
    report(what, t);
    hold(t);
  }
}

// **A tile that leaves** (R194 leg 2): a tail's box stands still under a still camera, then the
// tail lets it go (a tile leaving the rings; where a far forest's stand-in or the ground comes in
// its place). The ground it uncovers was as still as the box: last frame's motion there matches,
// and only the names say the surface is gone, through the slot the change wrote. Held at the age it
// left, 1 and 8 frames before the last.
TEST_CASE("trails: a tile that leaves leaves none") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  ScopedTunable mode("renderer.reconstruction.accumulation", "on");
  for (const u32 age : {1u, 8u}) {
    CAPTURE(age);
    const SceneInstance ground = placed(k_dark_sheet, Vec3{}, Vec3{12.0f, 1.0f, 12.0f});
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, std::span<const SceneInstance>(&ground, 1), true),
                    rig.error);
    const u32 leaves = k_frames - age;  // the first frame without it
    const Trail t = fly(
        rig,
        [&](u32 k, FrameDesc& frame) {
          frame = frame_at(k_eye, k_target);
          if (k != 0 && k != leaves) return true;
          const SceneInstance box =
              placed(k_bright_box, Vec3{0.5f, 1.0f, 0.0f}, Vec3{1.5f, 1.0f, 1.5f});
          return rig.renderer.set_dynamic_instances(
              std::span<const SceneInstance>(&box, k == 0 ? 1u : 0u), &rig.error);
        },
        [](u32 id) { return id != 0u && id != k_no_id; });
    const std::string what = "a tile's box that left " + std::to_string(age) + " frames ago";
    report(what.c_str(), t);
    REQUIRE_MESSAGE(t.ok, t.error);
    const u32 i = age == 1 ? 0u : 3u;
    CHECK(t.pixels[i] > 100);
    // The fresh frame holds no box, so the contrast is the moved box's case's for this pair of
    // meshes (145 codes): a kept history leaves 0.9^age of it.
    CHECK(t.age[i] <= t.floor + 145.0 / 20.0);
  }
}

// A walker: a skinned actor the host steps along 25 cm a frame while its upper half swings back and
// forth through 35 degrees either way, in a scene read whole and in a streamed world's (where,
// before R194, its every move marked the tables changed, and with them the bones' last frame
// unread: the swing had no vectors). Its pixels move by its steps and its bones at once.
TEST_CASE("trails: a skinned walker leaves none") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  ScopedTunable mode("renderer.reconstruction.accumulation", "on");
  Bar bar;
  build_bar(bar);
  const InstanceJoints runs[1] = {{0, 2}};
  for (u32 dynamic = 0; dynamic < 2; ++dynamic) {
    const char* what = dynamic != 0 ? "a walker, streamed scene" : "a walker, read whole";
    CAPTURE(what);
    const auto at = [](u32 k) { return Vec3{-4.0f + 0.25f * static_cast<f32>(k), 0.0f, 0.0f}; };
    Rig rig;
    REQUIRE_MESSAGE(make_walker_scene(bar, dynamic != 0, at(0), rig.data, rig.error), rig.error);
    REQUIRE_MESSAGE(rig.finish(gpu.device), rig.error);
    REQUIRE(rig.resolved.deform_pass);
    Vector<anim::JointMatrix> matrices;
    const Trail t = fly(
        rig,
        [&](u32 k, FrameDesc& frame) {
          frame = frame_at(k_eye, k_target);
          bend(bar, 35.0f * std::sin(0.7f * static_cast<f32>(k)), matrices);
          frame.joints = std::span<const anim::JointMatrix>(matrices.data(), matrices.size());
          frame.instance_joints = std::span<const InstanceJoints>(runs, 1);
          if (k == 0) return true;
          const SceneInstance step = bar_at(at(k));
          const u32 index = 0;
          return rig.renderer.move_instances(std::span<const u32>(&index, 1),
                                             std::span<const SceneInstance>(&step, 1), &rig.error);
        },
        [](u32 id) { return id == 0u; });
    report(what, t);
    hold(t);
  }
}

#if ENGINE_RENDERER_TESTS_FLORA
// A swaying tree: a young palm in a wind of 1.5 under a held camera, its clock a fifth of a second
// a frame, against the sky. Its fronds' pixels move by the flora stage's poses (the pool's second
// write); what they uncover is sky, which must not keep them.
TEST_CASE("trails: a swaying tree leaves none against the sky") {
  const test::TempDir tmp("renderer_trail_palm");
  REQUIRE(tmp.ok());
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  ScopedTunable mode("renderer.reconstruction.accumulation", "on");
  const std::string path = tmp.file("young.flora.json");
  {
    std::ofstream f(path, std::ios::binary);
    f << R"({"species":"date_palm","palm":{"seed":4,"height_m":3.5,"frond_count":10,)"
         R"("leaflet_count":12,"dead_skirt_fronds":3,"triangle_budget":9000}})";
  }
  SceneDesc desc;
  desc.meshes.push_back(path);
  desc.ddc = tmp.file("ddc");
  desc.cache = true;
  Rig rig;
  REQUIRE_MESSAGE(load_scene(desc, rig.data, rig.error), rig.error);
  REQUIRE_MESSAGE(rig.finish(gpu.device), rig.error);
  FrameFlora wind;
  wind.wind_set = true;
  wind.wind_to = Vec2{-1.0f, 0.0f};
  wind.wind_strength = 1.5f;
  const Trail t = fly(
      rig,
      [&](u32 k, FrameDesc& frame) {
        frame.camera.position = WorldPos{0.0, 2.6, 6.0};
        frame.camera.target = WorldPos{0.0, 2.6, 0.0};
        frame.frame_index = 60 + k;
        frame.world_time_s = 2.0 + 0.2 * static_cast<f64>(k);
        frame.flora = wind;
        return true;
      },
      [](u32 id) { return id == 0u; });
  report("a swaying palm", t);
  REQUIRE_MESSAGE(t.ok, t.error);
  // The fronds swing back and forth, so what they left at an age is not all sky for long: held at
  // the ages the fixture leaves enough pixels at, and to the same bound.
  CHECK(t.contrast > 20.0);
  for (u32 i = 0; i < 4; ++i) {
    CAPTURE(k_ages[i]);
    if (t.pixels[i] > 10) CHECK(t.age[i] <= t.floor + t.contrast / 20.0);
  }
}

// ---- what a layout change costs (R194 leg 2) ---------------------------------------------------
//
// The young palm in a streamed world's scene (the load's prefix), and a tail of one palm behind the
// camera that the host places again on every frame, or once: a tile arriving and leaving where it
// is not seen. Every frame of the first kind changes the tables' layout. On R194's first leg such a
// frame held every pixel to its depth, and dropped the swaying tree's last-frame inputs, so the
// crown in view took less history; now it holds only the slots the change wrote.

struct PalmFlight {
  Vector<Vector<u8>> color;  // every frame's picture
  Vector<Vector<u32>> ids;   // and its first id word a pixel
  std::string error;
  bool ok = false;
};

bool palm_rig(const test::TempDir& tmp, const gfx::Device& device, Rig& rig) {
  const std::string path = tmp.file("young.flora.json");
  {
    std::ofstream f(path, std::ios::binary);
    f << R"({"species":"date_palm","palm":{"seed":4,"height_m":3.5,"frond_count":10,)"
         R"("leaflet_count":12,"dead_skirt_fronds":3,"triangle_budget":9000}})";
  }
  SceneDesc desc;
  desc.meshes.push_back(path);
  desc.ddc = tmp.file("ddc");
  desc.cache = true;
  if (!load_scene(desc, rig.data, rig.error)) return false;
  rig.data.dynamic = true;
  return rig.finish(device);
}

// `frames` frames with the camera stepping `step` metres a frame sideways past the crown, the wind
// at `wind`; `every` places the tail again on every frame (else on the first alone); `fresh` draws
// every frame from a reset (one sample: the picture the accumulation is there to settle).
PalmFlight fly_palm(Rig& rig, u32 frames, f32 step, f32 wind, bool every, bool fresh) {
  PalmFlight out;
  CaptureChannels channels;
  channels.ids = true;
  CapturedFrame shot;
  FrameFlora flora;
  flora.wind_set = true;
  flora.wind_to = Vec2{-1.0f, 0.0f};
  flora.wind_strength = wind;
  for (u32 k = 0; k < frames; ++k) {
    if (k == 0 || every) {
      // Behind the camera (which looks down -z from z = 6), at one of two places.
      SceneInstance tree;
      tree.mesh = 0;
      tree.transform.position = Vec3{-30.0f + 4.0f * static_cast<f32>(k % 2), 0.0f, 40.0f};
      if (!rig.renderer.set_dynamic_instances(std::span<const SceneInstance>(&tree, 1),
                                              &rig.error)) {
        out.error = rig.error;
        return out;
      }
    }
    FrameDesc frame;
    const f64 x = -0.2 + static_cast<f64>(step) * k;
    frame.camera.position = WorldPos{x, 2.6, 6.0};
    frame.camera.target = WorldPos{x, 2.6, 0.0};
    frame.frame_index = 60 + k;
    frame.world_time_s = 2.0 + 0.2 * static_cast<f64>(k);
    frame.flora = flora;
    frame.history_reset = k == 0 || fresh;
    if (!rig.renderer.capture(frame, channels, shot, &rig.error)) {
      out.error = rig.error;
      return out;
    }
    out.color.push_back(shot.color);
    Vector<u32> ids(k_pixels);
    for (u32 p = 0; p < k_pixels; ++p)
      ids[p] = shot.ids[p * k_id_words];
    out.ids.push_back(std::move(ids));
  }
  out.ok = true;
  return out;
}

// **Shimmer**: the mean over the crown's edge pixels (their 3x3 in the fresh flight's last frame
// holds the palm and something else) of luma's standard deviation over frames `from` to the end,
// in display codes. The crown is still (no wind) and the camera all but still, so what varies is
// the offsets' aliasing, which a history settles and a refused one shows.
f64 shimmer(const PalmFlight& flight, const PalmFlight& fresh, u32 from, u32& pixels) {
  const Vector<u32>& ids = fresh.ids.back();
  f64 sum = 0.0;
  pixels = 0;
  for (u32 p = 0; p < k_pixels; ++p) {
    const u32 x = p % k_width;
    const u32 y = p / k_width;
    bool palm = false;
    bool other = false;
    for (u32 n = 0; n < 9; ++n) {
      const i32 nx = static_cast<i32>(x) + static_cast<i32>(n % 3) - 1;
      const i32 ny = static_cast<i32>(y) + static_cast<i32>(n / 3) - 1;
      if (nx < 0 || ny < 0 || nx >= static_cast<i32>(k_width) || ny >= static_cast<i32>(k_height))
        continue;
      const u32 id = ids[static_cast<u32>(ny) * k_width + static_cast<u32>(nx)];
      palm = palm || id == 0u;
      other = other || id != 0u;
    }
    if (!palm || !other) continue;
    f64 s = 0.0;
    f64 s2 = 0.0;
    const u32 n = static_cast<u32>(flight.color.size()) - from;
    for (u32 k = from; k < flight.color.size(); ++k) {
      const f64 l = luma(flight.color[k], p);
      s += l;
      s2 += l * l;
    }
    const f64 mean = s / n;
    sum += std::sqrt(std::max(s2 / n - mean * mean, 0.0));
    ++pixels;
  }
  return pixels > 0 ? sum / pixels : 0.0;
}

u32 frames_apart(const PalmFlight& a, const PalmFlight& b) {
  u32 apart = 0;
  for (u32 k = 0; k < a.color.size() && k < b.color.size(); ++k)
    apart += std::memcmp(a.color[k].data(), b.color[k].data(), a.color[k].size()) != 0 ? 1u : 0u;
  return apart;
}

f64 mean_apart(const Vector<u8>& a, const Vector<u8>& b) {
  f64 sum = 0.0;
  for (u32 p = 0; p < k_pixels; ++p)
    sum += std::abs(luma(a, p) - luma(b, p));
  return sum / k_pixels;
}

TEST_CASE("trails: a layout change elsewhere costs the picture nothing") {
  const test::TempDir tmp("renderer_trail_layout");
  REQUIRE(tmp.ok());
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  ScopedTunable mode("renderer.reconstruction.accumulation", "on");
  constexpr u32 k_flight = 40;
  // A swaying crown under a camera stepping 1 cm a frame: its vectors are the pool's second
  // write's, which reads last frame's poses, and its edges keep their dilation and their names.
  PalmFlight once;
  PalmFlight every;
  PalmFlight leg1;
  {
    Rig rig;
    REQUIRE_MESSAGE(palm_rig(tmp, gpu.device, rig), rig.error);
    REQUIRE(rig.resolved.deform_pass);
    once = fly_palm(rig, k_flight, 0.01f, 1.5f, false, false);
    REQUIRE_MESSAGE(once.ok, once.error);
  }
  {
    Rig rig;
    REQUIRE_MESSAGE(palm_rig(tmp, gpu.device, rig), rig.error);
    every = fly_palm(rig, k_flight, 0.01f, 1.5f, true, false);
    REQUIRE_MESSAGE(every.ok, every.error);
  }
  {
    ScopedTunable follow("renderer.reconstruction.layout_follow", "false");
    Rig rig;
    REQUIRE_MESSAGE(palm_rig(tmp, gpu.device, rig), rig.error);
    leg1 = fly_palm(rig, k_flight, 0.01f, 1.5f, true, false);
    REQUIRE_MESSAGE(leg1.ok, leg1.error);
  }
  const u32 apart = frames_apart(once, every);
  const u32 apart_leg1 = frames_apart(once, leg1);
  const f64 codes_leg1 = mean_apart(once.color.back(), leg1.color.back());
  MESSAGE("a swaying crown, the tail placed again every frame behind the camera: "
          << apart << " of " << k_flight << " frames differ from the tail placed once ("
          << apart_leg1 << " with every pixel held to its depth, as R194's first leg did, its last "
          << "frame " << codes_leg1 << " codes a pixel apart)");
  // Not a byte of the picture moves for a change the picture does not show...
  CHECK(apart == 0u);
  // ...where the first leg's rule moved it: the case can fail.
  CHECK(apart_leg1 > k_flight / 2);
}

TEST_CASE("trails: a crown shimmers no more for a layout change elsewhere") {
  const test::TempDir tmp("renderer_trail_shimmer");
  REQUIRE(tmp.ok());
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  ScopedTunable mode("renderer.reconstruction.accumulation", "on");
  constexpr u32 k_flight = 48;
  constexpr u32 k_settled = 16;
  // A still crown (no wind) and a camera stepping a millimetre a frame: walking, all but still.
  const auto flight = [&](bool every, bool fresh, bool follow) {
    ScopedTunable rule("renderer.reconstruction.layout_follow", follow ? "true" : "false");
    Rig rig;
    REQUIRE_MESSAGE(palm_rig(tmp, gpu.device, rig), rig.error);
    PalmFlight f = fly_palm(rig, k_flight, 0.001f, 0.0f, every, fresh);
    REQUIRE_MESSAGE(f.ok, f.error);
    return f;
  };
  const PalmFlight raw = flight(false, true, true);
  const PalmFlight quiet = flight(false, false, true);
  const PalmFlight changing = flight(true, false, true);
  const PalmFlight leg1 = flight(true, false, false);
  u32 pixels = 0;
  const f64 s_raw = shimmer(raw, raw, k_settled, pixels);
  const f64 s_quiet = shimmer(quiet, raw, k_settled, pixels);
  const f64 s_changing = shimmer(changing, raw, k_settled, pixels);
  const f64 s_leg1 = shimmer(leg1, raw, k_settled, pixels);
  MESSAGE("crown edge shimmer over "
          << pixels << " px, luma sd over frames " << k_settled << " to " << k_flight - 1
          << ": each frame fresh " << s_raw << ", accumulated with no layout change " << s_quiet
          << ", with one every frame " << s_changing
          << ", with one every frame under the first leg's rule " << s_leg1);
  REQUIRE(pixels > 100);
  // The accumulation settles the crown's edge, and a layout change it does not touch leaves it so.
  CHECK(s_quiet < 0.5 * s_raw);
  CHECK(s_changing == s_quiet);
  // The first leg's rule gave some of it back: the measure can tell.
  CHECK(s_leg1 > s_quiet);
}
#endif

// **A still camera on a still scene converges as it did** (R194's converse): with nothing moving
// every vector is zero, no pixel is at a moving boundary, every tap matches, and the history test
// is the one before R194, so the sixteenth frame is the same bytes. Its hash is printed for a pair
// between builds (the row's log has main's and this one's, the same number); here a replay from a
// reset draws the same bytes. accumulation_tests.cpp holds the converged picture to its mean.
TEST_CASE("trails: a still view converges as before") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  ScopedTunable mode("renderer.reconstruction.accumulation", "on");
  SceneInstance wall = placed(k_bright_sheet, Vec3{0.0f, 0.0f, -25.0f}, Vec3{30.0f, 1.0f, 30.0f});
  wall.transform.rotation = quat_from_axis_angle(Vec3{1.0f, 0.0f, 0.0f}, radians(90.0f));
  const SceneInstance instances[3] = {
      wall, placed(k_dark_box, Vec3{0.0f, 0.0f, -5.0f}, Vec3{0.15f, 3.0f, 0.15f}),
      placed(k_bright_box, Vec3{1.5f, -1.0f, -9.0f}, Vec3{0.5f, 0.5f, 0.5f})};
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, std::span<const SceneInstance>(instances, 3), false),
                  rig.error);
  const auto draw = [&](CapturedFrame& out) {
    FrameDesc frame = frame_at(Vec3{-0.3f, 0.5f, 0.0f}, Vec3{-0.3f, 0.5f, -10.0f});
    for (u32 k = 0; k < 16; ++k) {
      frame.history_reset = k == 0;
      REQUIRE_MESSAGE(rig.renderer.capture(frame, CaptureChannels{}, out, &rig.error), rig.error);
    }
  };
  CapturedFrame held;
  CapturedFrame again;
  draw(held);
  draw(again);
  u64 hash = 0xcbf29ce484222325ull;
  for (const u8 b : held.color)
    hash = (hash ^ b) * 0x100000001b3ull;
  MESSAGE("a still view's sixteenth frame: FNV-1a " << hash);
  CHECK(std::memcmp(held.color.data(), again.color.data(), held.color.size()) == 0);
}

// Walking past things: the camera steps sideways 4 cm a frame, a dark post 5 m away in front of a
// bright wall 25 m away; nothing moves but the eye, so every vector is right and the trail is the
// history test's alone: the wall the post uncovers must not keep the post.
TEST_CASE("trails: a camera walking past a near post leaves none on the far wall") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  ScopedTunable mode("renderer.reconstruction.accumulation", "on");
  for (u32 dynamic = 0; dynamic < 2; ++dynamic) {
    const char* what = dynamic != 0 ? "parallax, streamed scene" : "parallax, read whole";
    CAPTURE(what);
    // The wall: a bright sheet stood up facing +z; the post: a dark box 0.3 wide, 6 tall.
    SceneInstance wall = placed(k_bright_sheet, Vec3{0.0f, 0.0f, -25.0f}, Vec3{30.0f, 1.0f, 30.0f});
    wall.transform.rotation = quat_from_axis_angle(Vec3{1.0f, 0.0f, 0.0f}, radians(90.0f));
    const SceneInstance post =
        placed(k_dark_box, Vec3{0.0f, 0.0f, -5.0f}, Vec3{0.15f, 3.0f, 0.15f});
    const SceneInstance instances[2] = {wall, post};
    Rig rig;
    REQUIRE_MESSAGE(
        rig.build(gpu.device, std::span<const SceneInstance>(instances, 2), dynamic != 0),
        rig.error);
    const Trail t = fly(
        rig,
        [&](u32 k, FrameDesc& frame) {
          const f32 x = -0.6f + 0.04f * static_cast<f32>(k);
          frame = frame_at(Vec3{x, 0.5f, 0.0f}, Vec3{x, 0.5f, -10.0f});
          return true;
        },
        [](u32 id) { return id == 1u; });
    report(what, t);
    hold(t);
  }
}
