// Trees move (docs/subsystems/renderer.md, "The flora stage"; flora.md, "Motion"; roadmap S66).
// The CPU half first — the poses a frame's wind and walker give a tree's parts, on hand-built
// part tables whose answers can be worked out — then, where the flora capability is linked, a
// generated palm drawn through the pool's flora stage: in no wind it is the rigid palm to the bit,
// in a wind it sways, and the same frames with the same walker draw the same bytes.
#if ENGINE_RENDERER_TESTS_FLORA
#include <domain/flora/flora.h>
#endif
#include <domain/geometry/flora_species.h>
#include <domain/gfx/device.h>
#include <domain/gfx/flora_motion.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/flora_motion.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <span>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

using geometry::ClusterFileMotionPart;
using geometry::ClusterFileStructuralPart;

ClusterFileMotionPart make_part(Vec3 pivot, Vec3 direction, f32 length, u32 parent) {
  ClusterFileMotionPart p;
  p.pivot = pivot;
  p.direction = normalize(direction);
  p.length = length;
  p.parent = parent;
  return p;
}

// A palm-like tree: a 6 m trunk standing on +y, and eight 3 m fronds from its top, drooping a
// little, at eighths of a turn.
Vector<ClusterFileMotionPart> palm_parts() {
  Vector<ClusterFileMotionPart> parts;
  parts.push_back(make_part(Vec3{}, Vec3{0.0f, 1.0f, 0.0f}, 6.0f, geometry::k_motion_no_parent));
  for (u32 k = 0; k < 8; ++k) {
    const f32 a = 0.785398163f * static_cast<f32>(k);
    parts.push_back(
        make_part(Vec3{0.0f, 6.0f, 0.0f}, Vec3{std::cos(a), -0.2f, std::sin(a)}, 3.0f, 0));
  }
  return parts;
}

FloraMesh mesh_of(const Vector<ClusterFileMotionPart>& parts,
                  const Vector<ClusterFileStructuralPart>& structure, const char* species) {
  FloraMesh mesh;
  REQUIRE(flora_mesh(std::span<const ClusterFileMotionPart>(parts.data(), parts.size()),
                     std::span<const ClusterFileStructuralPart>(structure.data(), structure.size()),
                     species, mesh));
  return mesh;
}

// Where a point of part `p` at bend weight `w` goes under `poses` (the stage's CPU mirror).
Vec3 bent(const Vector<gfx::FloraPartPose>& poses, const FloraMesh& mesh, u32 p, f32 w) {
  const ClusterFileMotionPart& part = mesh.parts[p];
  const Vec3 rest = part.pivot + part.direction * (part.length * w);
  Vec3 out_p;
  Vec3 out_n;
  gfx::flora_bend(poses[p], w, rest, Vec3{0.0f, 1.0f, 0.0f}, out_p, out_n);
  return out_p;
}

void pose(const FloraMesh& mesh, const FloraWind& wind, f64 t, const FloraPush& push, u32 ticks,
          Vector<FloraPartState>& state, Vector<gfx::FloraPartPose>& poses) {
  flora_part_poses(mesh, wind, t, 7u, push, ticks,
                   std::span<FloraPartState>(state.data(), state.size()),
                   std::span<gfx::FloraPartPose>(poses.data(), poses.size()));
}

// The distance from `p` to the capsule's axis segment.
f32 capsule_distance(const FloraPush& push, Vec3 p) {
  const Vec3 a = push.feet + Vec3{0.0f, push.radius, 0.0f};
  const Vec3 b = push.feet + Vec3{0.0f, push.height - push.radius, 0.0f};
  const Vec3 ab = b - a;
  f32 t = dot(p - a, ab) / dot(ab, ab);
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  return length(p - (a + ab * t));
}

}  // namespace

TEST_CASE("flora motion: every catalogued species has explicit compliance; unknown is rejected") {
  const auto parts = palm_parts();
  for (const auto& info : geometry::k_flora_species) {
    CAPTURE(info.name);
    FloraMesh mesh;
    // A species that never moves (the cover's mats) is no tree: refused, and drawn rigid silently.
    if (info.motion == geometry::FloraMotionKind::none) {
      CHECK_FALSE(flora_mesh({parts.data(), parts.size()}, {}, info.name, mesh));
      continue;
    }
    REQUIRE(flora_mesh({parts.data(), parts.size()}, {}, info.name, mesh));
    CHECK(flora_stiffness(info.name).leaf == info.compliance[3]);
    // R95: the field kind is the catalogue's, with its leaf compliance; the trees are posed by
    // parts.
    CHECK(mesh.field == (info.motion == geometry::FloraMotionKind::field));
    if (mesh.field) CHECK(mesh.field_compliance == info.compliance[3]);
  }
  // The sward is grass posed as a field, cast into the nearest cascade at rest and never traced;
  // every tree keeps the defaults.
  const geometry::FloraSpeciesInfo* sward = geometry::flora_species_info("sward");
  REQUIRE(sward != nullptr);
  CHECK(sward->motion == geometry::FloraMotionKind::field);
  CHECK_FALSE(sward->casters.traced);
  CHECK(sward->casters.cascades == 1);
  CHECK(sward->casters.rest_pose);
  const auto* ivy = geometry::flora_species_info("ivy");
  REQUIRE(ivy != nullptr);
  CHECK(ivy->motion == geometry::FloraMotionKind::parts);
  CHECK(ivy->casters.traced);
  CHECK(ivy->casters.cascades == geometry::k_flora_all_cascades);
  CHECK_FALSE(ivy->casters.rest_pose);
  CHECK(ivy->compliance[1] == doctest::Approx(2.8f * flora_stiffness("oak").limb));
  const geometry::FloraSpeciesInfo* maple = geometry::flora_species_info("maple");
  REQUIRE(maple != nullptr);
  CHECK(maple->motion == geometry::FloraMotionKind::parts);
  CHECK(maple->casters.traced);
  CHECK(maple->casters.cascades == geometry::k_flora_all_cascades);
  CHECK_FALSE(maple->casters.rest_pose);
  FloraMesh mesh;
  CHECK_FALSE(flora_mesh({parts.data(), parts.size()}, {}, "missing_profile", mesh));
}

TEST_CASE("flora motion: no wind and no walker leave every part at rest") {
  const Vector<ClusterFileMotionPart> parts = palm_parts();
  const FloraMesh mesh = mesh_of(parts, {}, "date_palm");
  Vector<FloraPartState> state(mesh.part_count(), FloraPartState{});
  Vector<gfx::FloraPartPose> poses(mesh.part_count(), gfx::FloraPartPose{});
  for (const f64 t : {0.0, 3.25, 1000.0}) {
    pose(mesh, FloraWind{Vec3{1.0f, 0.0f, 0.0f}, 0.0f}, t, FloraPush{}, 1, state, poses);
    for (const gfx::FloraPartPose& p : poses) {
      CHECK(p.axis.w == 0.0f);  // the stage passes the vertex through
      CHECK(p.pivot_angle.w == 0.0f);
    }
  }
}

TEST_CASE("flora motion: a wind from +x bends the crown towards -x, and the trunk a little") {
  const Vector<ClusterFileMotionPart> parts = palm_parts();
  const FloraMesh mesh = mesh_of(parts, {}, "date_palm");
  Vector<FloraPartState> state(mesh.part_count(), FloraPartState{});
  Vector<gfx::FloraPartPose> poses(mesh.part_count(), gfx::FloraPartPose{});
  const FloraWind wind{Vec3{-1.0f, 0.0f, 0.0f}, 1.0f};  // blowing from +x towards -x
  // The tips' displacement, averaged over ten seconds so the flutter's sign does not decide it.
  Vec3 tips{};
  Vec3 trunk_top{};
  u32 samples = 0;
  for (u32 k = 0; k < 40; ++k) {
    pose(mesh, wind, 0.25 * k, FloraPush{}, 1, state, poses);
    for (u32 p = 1; p < mesh.part_count(); ++p)
      tips = tips +
             (bent(poses, mesh, p, 1.0f) - (mesh.parts[p].pivot + mesh.parts[p].direction * 3.0f));
    trunk_top = trunk_top + (bent(poses, mesh, 0, 1.0f) - Vec3{0.0f, 6.0f, 0.0f});
    ++samples;
  }
  tips = tips * (1.0f / static_cast<f32>(samples * (mesh.part_count() - 1)));
  trunk_top = trunk_top * (1.0f / static_cast<f32>(samples));
  MESSAGE("a mean wind from +x: the fronds' tips moved "
          << tips.x << ", " << tips.y << ", " << tips.z << " m; the trunk's top " << trunk_top.x
          << " m");
  CHECK(tips.x < -0.1f);
  CHECK(std::fabs(tips.z) < 0.5f * std::fabs(tips.x));
  CHECK(trunk_top.x < 0.0f);
  CHECK(trunk_top.x > -0.2f);  // a slight bend: a 6 m trunk's top moves centimetres
  // A frond rides its trunk: the trunk's bend moved its pivot.
  const Vec3 pivot = gfx::flora_frame_point(poses[1], mesh.parts[1].pivot);
  CHECK(length(pivot - mesh.parts[1].pivot) > 0.0f);
}

TEST_CASE("flora motion: a load-bearing limb moves less than the twig and the leaves it carries") {
  // Trunk, a load-bearing limb, a twig on it, a leaf spray on the twig — the structural table's
  // flags and radii are what sort them.
  Vector<ClusterFileMotionPart> parts;
  parts.push_back(make_part(Vec3{}, Vec3{0.0f, 1.0f, 0.0f}, 8.0f, geometry::k_motion_no_parent));
  parts.push_back(make_part(Vec3{0.0f, 4.0f, 0.0f}, Vec3{0.0f, 0.5f, 1.0f}, 4.0f, 0));
  parts.push_back(make_part(Vec3{0.0f, 5.5f, 3.0f}, Vec3{0.0f, 0.3f, 1.0f}, 1.5f, 1));
  parts.push_back(make_part(Vec3{0.0f, 5.9f, 4.4f}, Vec3{0.0f, 0.0f, 1.0f}, 0.4f, 2));
  Vector<ClusterFileStructuralPart> structure;
  structure.push_back(
      ClusterFileStructuralPart{0, 0.4f, 0.2f, geometry::k_structural_load_bearing});
  structure.push_back(
      ClusterFileStructuralPart{1, 0.15f, 0.08f, geometry::k_structural_load_bearing});
  structure.push_back(ClusterFileStructuralPart{2, 0.03f, 0.01f, 0});
  structure.push_back(ClusterFileStructuralPart{3, 0.0f, 0.0f, 0});
  const FloraMesh mesh = mesh_of(parts, structure, "broadleaf");
  CHECK(mesh.compliance[1] < mesh.compliance[2]);
  CHECK(mesh.compliance[2] < mesh.compliance[3]);
  CHECK(mesh.pushable[0] == 0);
  CHECK(mesh.pushable[1] == 0);  // load-bearing: never pushed
  CHECK(mesh.pushable[2] == 1);
  CHECK(flora_part_collides(
      std::span<const ClusterFileStructuralPart>(structure.data(), structure.size()), 1));
  CHECK_FALSE(flora_part_collides(
      std::span<const ClusterFileStructuralPart>(structure.data(), structure.size()), 2));
  CHECK(flora_part_collides({}, 0));  // with no table, the trunk alone
  CHECK_FALSE(flora_part_collides({}, 1));
  // The table decides even for zero; field/none kinds never collide, even if flagged.
  const ClusterFileStructuralPart root[] = {{0, 0.0f, 0.0f, 0u}};
  CHECK_FALSE(flora_part_collides(root, 0, geometry::FloraMotionKind::parts));
  CHECK_FALSE(flora_part_collides({}, 0, geometry::FloraMotionKind::field));
  CHECK_FALSE(flora_part_collides({}, 0, geometry::FloraMotionKind::none));
  CHECK_FALSE(flora_part_collides({structure.data(), structure.size()}, 0,
                                  geometry::FloraMotionKind::field));
  CHECK_FALSE(flora_part_collides({structure.data(), structure.size()}, 0,
                                  geometry::FloraMotionKind::none));
  // A shrub's stout stem is a collider though nothing hangs from it, and is never pushed
  // (flora.md, "The shrub").
  Vector<ClusterFileStructuralPart> stout = structure;
  stout[2].flags = geometry::k_structural_collider;
  CHECK(flora_part_collides(std::span<const ClusterFileStructuralPart>(stout.data(), stout.size()),
                            2));
  CHECK(mesh_of(parts, stout, "shrub").pushable[2] == 0);
  CHECK(flora_stiffness("tuft").leaf > flora_stiffness("willow").leaf);
  Vector<FloraPartState> state(mesh.part_count(), FloraPartState{});
  Vector<gfx::FloraPartPose> poses(mesh.part_count(), gfx::FloraPartPose{});
  f32 angle[4] = {};
  for (u32 k = 0; k < 40; ++k) {
    pose(mesh, FloraWind{Vec3{1.0f, 0.0f, 0.0f}, 1.0f}, 0.25 * k, FloraPush{}, 1, state, poses);
    for (u32 p = 0; p < 4; ++p)
      angle[p] += std::fabs(poses[p].pivot_angle.w);
  }
  MESSAGE("mean bend over ten seconds: trunk " << angle[0] / 40 << ", limb " << angle[1] / 40
                                               << ", twig " << angle[2] / 40 << ", leaves "
                                               << angle[3] / 40 << " rad");
  CHECK(angle[1] < angle[2]);
  CHECK(angle[2] < angle[3]);
  CHECK(angle[0] < angle[1]);
}

TEST_CASE("flora motion: the walker pushes a frond's tip out of its capsule and lets it go") {
  // A 3 m frond at chest height along +x from a trunk at the origin, and a walker standing across
  // it 1.5 m out, the frond's axis 5 cm from the capsule's.
  Vector<ClusterFileMotionPart> parts;
  parts.push_back(make_part(Vec3{}, Vec3{0.0f, 1.0f, 0.0f}, 4.0f, geometry::k_motion_no_parent));
  parts.push_back(make_part(Vec3{0.0f, 1.3f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 3.0f, 0));
  const FloraMesh mesh = mesh_of(parts, {}, "date_palm");
  Vector<FloraPartState> state(mesh.part_count(), FloraPartState{});
  Vector<gfx::FloraPartPose> poses(mesh.part_count(), gfx::FloraPartPose{});
  FloraPush push;
  push.active = true;
  push.feet = Vec3{1.5f, 0.0f, 0.05f};
  const FloraWind calm{Vec3{1.0f, 0.0f, 0.0f}, 0.0f};
  // Before any tick of contact: at rest, the frond through the capsule.
  pose(mesh, calm, 0.0, push, 0, state, poses);
  CHECK(poses[1].axis.w == 0.0f);
  CHECK(capsule_distance(push, bent(poses, mesh, 1, 0.5f)) < push.radius);
  // Eased in over the ease's ticks, a tick a frame.
  f32 halfway = 0.0f;
  for (u32 k = 1; k <= k_flora_push_ease_ticks; ++k) {
    pose(mesh, calm, k / 60.0, push, 1, state, poses);
    if (k == k_flora_push_ease_ticks / 2) halfway = poses[1].pivot_angle.w;
  }
  const f32 full = poses[1].pivot_angle.w;
  f32 nearest = 1.0e9f;
  for (u32 s = 0; s <= 20; ++s)
    nearest = std::fmin(nearest, capsule_distance(push, bent(poses, mesh, 1, s / 20.0f)));
  const f32 tip = capsule_distance(push, bent(poses, mesh, 1, 1.0f));
  MESSAGE("pushed: " << full << " rad (" << halfway << " halfway through the ease); the frond's "
                     << "nearest point " << nearest << " m from the capsule's axis, its tip "
                     << tip);
  CHECK(halfway > 0.0f);
  CHECK(halfway < full);
  CHECK(nearest > push.radius);
  CHECK(tip > push.radius);
  // Away from the walker: the frond swung towards -z, the side the walker is not.
  CHECK(bent(poses, mesh, 1, 1.0f).z < -0.1f);
  // The trunk is never pushed.
  CHECK(poses[0].axis.w == 0.0f);
  // The walker leaves; the frond eases back to rest and is passed through again.
  push.active = false;
  for (u32 k = 0; k <= k_flora_push_ease_ticks; ++k)
    pose(mesh, calm, 1.0 + k / 60.0, push, 1, state, poses);
  CHECK(poses[1].axis.w == 0.0f);
  CHECK(state[1].ease == 0.0f);
}

TEST_CASE("flora motion: parentless sward blades bend at contact and recover with fixed roots") {
  Vector<ClusterFileMotionPart> parts;
  parts.push_back(make_part({.1f, 0, 0}, {0, 1, 0}, .5f, geometry::k_motion_no_parent));
  parts.push_back(make_part({2, 0, 0}, {0, 1, 0}, .5f, geometry::k_motion_no_parent));
  Vector<ClusterFileStructuralPart> structure(2, ClusterFileStructuralPart{});
  const FloraMesh mesh = mesh_of(parts, structure, "sward");
  REQUIRE(mesh.pushable[0] != 0);
  Vector<FloraPartState> state(2, FloraPartState{});
  Vector<gfx::FloraPartPose> poses(2, gfx::FloraPartPose{});
  FloraPush push;
  push.active = true;
  const FloraWind calm{{1, 0, 0}, 0};
  for (u32 k = 1; k <= k_flora_push_ease_ticks; ++k)
    pose(mesh, calm, k / 60.0, push, 1, state, poses);
  CHECK(poses[0].pivot_angle.w > 0);
  CHECK(bent(poses, mesh, 0, 1).x > .1f);
  CHECK(bent(poses, mesh, 0, 0).x == .1f);
  CHECK(bent(poses, mesh, 0, 0).y == 0);
  CHECK(poses[1].axis.w == 0);
  push.active = false;
  pose(mesh, calm, 1, push, 1, state, poses);
  CHECK(state[0].ease > 0);  // brief recovery, not persistent walked-track history
  for (u32 k = 0; k < k_flora_push_ease_ticks; ++k)
    pose(mesh, calm, 1 + k / 60.0, push, 1, state, poses);
  CHECK(state[0].ease == 0);
  CHECK(poses[0].axis.w == 0);
  const FloraMesh palm = mesh_of(parts, structure, "date_palm");
  CHECK(palm.pushable[0] == 0);  // tree-root policy is unchanged
}

TEST_CASE("flora motion: the same inputs give the same poses, bit for bit") {
  const Vector<ClusterFileMotionPart> parts = palm_parts();
  const FloraMesh mesh = mesh_of(parts, {}, "date_palm");
  auto run = [&](Vector<gfx::FloraPartPose>& out) {
    Vector<FloraPartState> state(mesh.part_count(), FloraPartState{});
    out.resize(mesh.part_count(), gfx::FloraPartPose{});
    FloraPush push;
    push.height = 7.0f;  // a capsule tall enough to reach the crown
    for (u32 k = 0; k < 90; ++k) {
      push.active = k > 10 && k < 60;
      push.feet = Vec3{-3.0f + 0.1f * static_cast<f32>(k), 0.0f, 0.4f};
      pose(mesh, FloraWind{Vec3{0.0f, 0.0f, 1.0f}, 1.3f}, k / 60.0, push, 1, state, out);
    }
  };
  Vector<gfx::FloraPartPose> a;
  Vector<gfx::FloraPartPose> b;
  run(a);
  run(b);
  CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(gfx::FloraPartPose)) == 0);
}

#if ENGINE_RENDERER_TESTS_FLORA

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

// A small palm whose crown comes down to a walker's reach: a young palm with a heavy skirt, so a
// debug build solves and clusters it in a moment.
constexpr const char* k_palm =
    R"({"species":"date_palm","palm":{"seed":4,"height_m":3.5,"frond_count":10,)"
    R"("leaflet_count":12,"dead_skirt_fronds":3,"triangle_budget":9000}})";

bool load_palm(const test::TempDir& tmp, SceneData& data, std::string& error) {
  const std::string path = tmp.file("young.flora.json");
  {
    std::ofstream f(path, std::ios::binary);
    f << k_palm;
  }
  SceneDesc desc;
  desc.meshes.push_back(path);
  desc.ddc = tmp.file("ddc");
  desc.cache = true;
  return load_scene(desc, data, error);
}

// One frame of the palm at frame index `index`, from 9 m off, its colour and ids.
struct Shot {
  Vector<u8> color;
  Vector<u32> ids;
  GpuScene::FloraStats stats;
};

bool draw(Gpu& gpu, const SceneData& data, const RenderSettings& settings,
          std::span<const FrameFlora> frames, Shot& out, std::string& error) {
  ResolvedSettings resolved;
  resolve_settings(settings, gpu.device.features(), &data, resolved);
  GpuScene scene;
  if (!scene.create(gpu.device, data, resolved, &error)) return false;
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = 320;
  rd.height = 320;
  if (!renderer.create(gpu.device, scene, resolved, rd, &error)) return false;
  CaptureChannels channels;
  channels.ids = true;
  CapturedFrame shot;
  for (u32 f = 0; f < frames.size(); ++f) {
    FrameDesc frame;
    frame.camera.position = WorldPos{0.0, 2.0, 9.0};
    frame.camera.target = WorldPos{0.0, 2.0, 0.0};
    frame.frame_index = 60 + f;
    frame.flora = frames[f];
    if (!renderer.render_offscreen(frame, &error)) return false;
    if (f + 1 == frames.size() && !renderer.capture(frame, channels, shot, &error)) return false;
  }
  out.color = std::move(shot.color);
  out.ids = std::move(shot.ids);
  out.stats = scene.flora_stats();
  renderer.destroy();
  scene.destroy();
  return true;
}

template <typename T>
bool same(const Vector<T>& a, const Vector<T>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0;
}

u32 differing(const Vector<u8>& a, const Vector<u8>& b) {
  u32 n = 0;
  for (u32 i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4)
    n += std::memcmp(a.data() + i, b.data() + i, 4) != 0 ? 1u : 0u;
  return n;
}

}  // namespace

TEST_CASE("flora motion: a palm in no wind draws the rigid palm to the bit; in a wind it sways") {
  const test::TempDir tmp("renderer_flora_motion");
  REQUIRE(tmp.ok());
  SceneData data;
  std::string error;
  REQUIRE_MESSAGE(load_palm(tmp, data, error), error);
  REQUIRE(data.sources.size() == 1);
  REQUIRE(data.sources[0].has_motion());
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  RenderSettings settings;
  // The traced shadows build a rigid instance's structures from the float positions and a deformed
  // one's from the pool, which is the 16-bit grid's: a different surface by a grid step, so the
  // comparison is drawn under the maps, which rasterize the grid on both paths.
  settings.shadows = ShadowMode::Cascaded;

  // The rigid palm, padded as the swaying one is so both cut the same DAG (the padding is the one
  // thing besides the stage the flora path changes about an instance).
  FloraMesh tree;
  REQUIRE(flora_mesh(
      std::span<const geometry::ClusterFileMotionPart>(data.sources[0].motion_parts.data(),
                                                       data.sources[0].motion_parts.size()),
      std::span<const geometry::ClusterFileStructuralPart>(data.sources[0].structural_parts.data(),
                                                           data.sources[0].structural_parts.size()),
      data.sources[0].species, tree));
  const f32 padding = flora_bounds_padding(
      tree, std::span<const Vec3>(data.lod.mesh.vertices.data(), data.lod.mesh.vertices.size()),
      std::span<const geometry::ClusterFileMotionVertex>(data.sources[0].motion_vertices.data(),
                                                         data.sources[0].motion_vertices.size()));
  MESSAGE("the young palm: " << tree.part_count() << " parts, its spheres padded " << padding
                             << " m for the sway");
  CHECK(padding > 0.0f);
  data.instances[0].bounds_padding = padding;  // the swaying one takes the same, the larger of two
  RenderSettings off = settings;
  off.flora_motion = false;
  FrameFlora calm;
  calm.wind_set = true;
  calm.wind_strength = 0.0f;
  Shot rest;
  REQUIRE_MESSAGE(draw(gpu, data, off, std::span<const FrameFlora>(&calm, 1), rest, error), error);
  Shot still;
  REQUIRE_MESSAGE(draw(gpu, data, settings, std::span<const FrameFlora>(&calm, 1), still, error),
                  error);
  CHECK(still.stats.instances == 1);
  CHECK(still.stats.parts == tree.part_count());
  // The visibility is the rigid palm's to the bit: every pixel names the same triangle at the same
  // depth (the ids carry both), and the pool holds the rest pose bit for bit (gfx's flora stage
  // test). The colour may not be, by a level on a pixel or two: the pool's floats are the pool
  // pass's dequantization of the grid and the rigid path's are the resolve's own, two compilations
  // of `load_quantized_position` whose multiply-add the driver rounds differently in the last bit
  // (E25's note in deform.slang), and the resolve reconstructs the surface point from them.
  const u32 off_by = differing(rest.color, still.color);
  u32 worst = 0;
  for (u32 i = 0; i < rest.color.size() && i < still.color.size(); ++i) {
    const u32 d = rest.color[i] > still.color[i] ? rest.color[i] - still.color[i]
                                                 : still.color[i] - rest.color[i];
    worst = d > worst ? d : worst;
  }
  MESSAGE("a calm palm against the rigid one: ids identical: "
          << same(rest.ids, still.ids) << "; " << off_by << " of " << rest.color.size() / 4
          << " pixels differ, by at most " << worst << " of 255");
  CHECK(same(rest.ids, still.ids));
  CHECK(off_by <= 4);
  CHECK(worst <= 1);
  // In a wind from +x it sways: the same frame twice is the same bytes, and not the rest pose's.
  FrameFlora windy;
  windy.wind_set = true;
  windy.wind_to = Vec2{-1.0f, 0.0f};
  windy.wind_strength = 1.5f;
  Shot a;
  Shot b;
  REQUIRE_MESSAGE(draw(gpu, data, settings, std::span<const FrameFlora>(&windy, 1), a, error),
                  error);
  REQUIRE_MESSAGE(draw(gpu, data, settings, std::span<const FrameFlora>(&windy, 1), b, error),
                  error);
  const u32 moved = differing(rest.color, a.color);
  MESSAGE("in a wind of 1.5 from +x: " << moved << " of " << rest.color.size() / 4
                                       << " pixels differ from the rest pose");
  CHECK(moved > 200);
  CHECK(same(a.color, b.color));
  CHECK(same(a.ids, b.ids));
}

TEST_CASE("flora motion: a walker through the crown is replayed to the same bytes") {
  const test::TempDir tmp("renderer_flora_push");
  REQUIRE(tmp.ok());
  SceneData data;
  std::string error;
  REQUIRE_MESSAGE(load_palm(tmp, data, error), error);
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  RenderSettings settings;
  settings.shadows = ShadowMode::Cascaded;
  // A recorded walk: thirty frames from 1.5 m east of the trunk to the west, under the crown, in a
  // light wind, a capsule as tall as the palm so it reaches the fronds whatever their height.
  Vector<FrameFlora> walk;
  for (u32 f = 0; f < 30; ++f) {
    FrameFlora frame;
    frame.wind_set = true;
    frame.wind_to = Vec2{0.0f, 1.0f};
    frame.wind_strength = 0.5f;
    frame.walker = true;
    frame.walker_feet = WorldPos{1.5 - 0.05 * f, 0.0, 0.6};
    frame.walker_height = 3.5f;
    walk.push_back(frame);
  }
  Shot live;
  Shot replay;
  REQUIRE_MESSAGE(
      draw(gpu, data, settings, std::span<const FrameFlora>(walk.data(), walk.size()), live, error),
      error);
  REQUIRE_MESSAGE(draw(gpu, data, settings, std::span<const FrameFlora>(walk.data(), walk.size()),
                       replay, error),
                  error);
  MESSAGE("the walk bent " << live.stats.pushed_parts << " of " << live.stats.parts
                           << " parts aside by its last frame");
  CHECK(live.stats.pushed_parts > 0);
  CHECK(same(live.color, replay.color));
  CHECK(same(live.ids, replay.ids));
}

#endif  // ENGINE_RENDERER_TESTS_FLORA

#if ENGINE_RENDERER_TESTS_FLORA
TEST_CASE("flora motion: continued stem caps stay inside the scaffold in strong wind") {
  struct Case {
    const char* species;
    u32 budget;
    u32 age;
  };
  for (const Case entry :
       {Case{"maple", 0, 0}, Case{"beech", 0, 0}, Case{"birch", 0, 0}, Case{"maple", 8000, 0},
        Case{"beech", 8000, 0}, Case{"birch", 8000, 0}, Case{"oak", 24000, 600}}) {
    const char* species = entry.species;
    CAPTURE(entry.budget);
    CAPTURE(entry.age);
    CAPTURE(std::string_view(species));
    flora::FloraFile file;
    file.species = species;
    file.tree.seed = 17;
    file.tree.age_years = static_cast<f32>(entry.age);
    if (entry.budget != 0) file.tree.triangle_budget = file.birch.triangle_budget = entry.budget;
    if (entry.age != 0) {
      file.tree.height_m = 12;
      file.tree.trunk_radius_m = .6f;
      file.tree.site_quality = 1.5f;
      file.tree.growing_space = 1;
    }
    file.tree.card_bake_px = file.tree.impostor_px = 0;
    file.birch.seed = 17;
    file.birch.stems = 1;
    file.birch.card_bake_px = file.birch.impostor_px = 0;
    assets::MeshData generated;
    flora::Motion motion;
    std::string error;
    REQUIRE_MESSAGE(flora::solve_flora(file, generated, motion, nullptr, nullptr, error), error);
    struct Ring {
      u32 first;
      u32 count;
      u32 part;
      f32 weight;
      Vec3 center;
    };
    Vector<Ring> rings;
    // Tube rings are contiguous and duplicate the UV seam. Restrict this scan to named wood:
    // the parent-first trunk is followed by its scaffold tubes, before recursive shoots/leaves.
    for (u32 first = 0; first < generated.positions.size();) {
      const u32 part = motion.vertex_part[first];
      if (part > 0 && motion.parts[part].parent != 0) break;
      const f32 weight = motion.vertex_weight[first];
      u32 end = first + 1;
      while (end < generated.positions.size() && motion.vertex_part[end] == part &&
             motion.vertex_weight[end] == weight)
        ++end;
      if (end - first >= 4 &&
          length(generated.positions[first] - generated.positions[end - 1]) < 1e-5f) {
        Vec3 center{};
        for (u32 v = first; v + 1 < end; ++v)
          center = center + generated.positions[v];
        center = center * (1.0f / static_cast<f32>(end - first - 1));
        rings.push_back({first, end - first - 1, part, weight, center});
      }
      first = end;
    }
    const Ring* bridge = nullptr;
    const Ring* cap = nullptr;
    for (u32 i = 0; i + 1 < rings.size(); ++i) {
      if (rings[i].part == 0 && rings[i].weight == 1) {
        bridge = &rings[i];
        cap = &rings[i + 1];
        break;
      }
    }
    REQUIRE(bridge != nullptr);
    REQUIRE(cap != nullptr);
    REQUIRE(cap->part != 0);
    REQUIRE(cap->count == bridge->count);
    const Ring* child = nullptr;
    f32 best = 1e30f;
    for (const Ring& ring : rings) {
      if (&ring == cap || ring.part != cap->part) continue;
      const f32 distance = length(ring.center - cap->center);
      if (distance < best) {
        child = &ring;
        best = distance;
      }
    }
    REQUIRE(child != nullptr);
    REQUIRE(best < .03f);
    REQUIRE(child->weight == cap->weight);
    const FloraMesh mesh = mesh_of(motion.parts, motion.structure, species);
    Vector<FloraPartState> state(mesh.part_count(), FloraPartState{});
    Vector<gfx::FloraPartPose> poses(mesh.part_count(), gfx::FloraPartPose{});
    Vector<u32> child_indices;
    for (u32 t = 0; t < generated.primitives[0].index_count; t += 3) {
      const u32 a = generated.indices[t], b = generated.indices[t + 1],
                c = generated.indices[t + 2];
      if (motion.vertex_part[a] == child->part && motion.vertex_part[b] == child->part &&
          motion.vertex_part[c] == child->part) {
        child_indices.push_back(a);
        child_indices.push_back(b);
        child_indices.push_back(c);
      }
    }
    f32 worst_margin = 1e30f;
    u32 outside = 0;
    f32 least_area_ratio = 1e30f, greatest_area_ratio = 0, least_orientation = 1;
    for (u32 direction = 0; direction < 8; ++direction) {
      const f32 angle = static_cast<f32>(direction) * .785398163f;
      const FloraWind wind{Vec3{std::cos(angle), 0, std::sin(angle)}, k_flora_max_wind};
      for (u32 sample = 0; sample < 24; ++sample) {
        pose(mesh, wind, static_cast<f64>(sample) * .375, FloraPush{}, 1, state, poses);
        const auto deform = [&](const Ring& ring, u32 offset) {
          Vec3 p, n;
          gfx::flora_bend(poses[ring.part],
                          flora::decode_bend_weight(flora::encode_bend_weight(ring.weight)),
                          generated.positions[ring.first + offset],
                          generated.normals[ring.first + offset], p, n);
          return p;
        };
        for (u32 side = 0; side < cap->count; ++side) {
          const u32 next = (side + 1) % cap->count;
          for (u32 tri = 0; tri < 2; ++tri) {
            const Vec3 rest_a = generated.positions[bridge->first + side];
            const Vec3 rest_b = generated.positions[(tri == 0 ? bridge->first : cap->first) + next];
            const Vec3 rest_c = generated.positions[cap->first + (tri == 0 ? next : side)];
            const Vec3 moved_a = deform(*bridge, side);
            const Vec3 moved_b = tri == 0 ? deform(*bridge, next) : deform(*cap, next);
            const Vec3 moved_c = deform(*cap, tri == 0 ? next : side);
            const Vec3 rest_area = cross(rest_b - rest_a, rest_c - rest_a);
            const Vec3 moved_area = cross(moved_b - moved_a, moved_c - moved_a);
            const f32 ratio = length(moved_area) / length(rest_area);
            least_area_ratio = std::min(least_area_ratio, ratio);
            greatest_area_ratio = std::max(greatest_area_ratio, ratio);
            least_orientation =
                std::min(least_orientation, dot(normalize(rest_area), normalize(moved_area)));
          }
        }
        const Vec3 a0 = deform(*child, 0), a1 = deform(*child, 1), a2 = deform(*child, 2);
        const Vec3 normal = normalize(cross(a1 - a0, a2 - a1));
        // Test the actual deformed scaffold surface as well as its projected ring. Cast along
        // the ring normal toward the branch tip, so a cap sliding axially out is detected too.
        const auto hit = [&](Vec3 origin, Vec3 a, Vec3 b, Vec3 c) {
          const Vec3 e1 = b - a, e2 = c - a;
          const Vec3 h = cross(normal, e2);
          const f32 det = dot(e1, h);
          if (std::abs(det) < 1e-9f) return false;
          const f32 inv = 1.0f / det;
          const Vec3 delta = origin - a;
          const f32 u = inv * dot(delta, h);
          if (u < 0 || u > 1) return false;
          const Vec3 q = cross(delta, e1);
          const f32 v = inv * dot(normal, q);
          return v >= 0 && u + v <= 1 && inv * dot(e2, q) > 1e-6f;
        };
        const auto bent_vertex = [&](u32 index) {
          Vec3 p, n;
          gfx::flora_bend(
              poses[child->part],
              flora::decode_bend_weight(flora::encode_bend_weight(motion.vertex_weight[index])),
              generated.positions[index], generated.normals[index], p, n);
          return p;
        };
        for (u32 v = 0; v < cap->count; ++v) {
          const Vec3 point = deform(*cap, v);
          u32 crossings = 0;
          for (u32 t = 0; t < child_indices.size(); t += 3) {
            const u32 a = child_indices[t], b = child_indices[t + 1], c = child_indices[t + 2];
            if (hit(point, bent_vertex(a), bent_vertex(b), bent_vertex(c))) ++crossings;
          }
          if ((crossings & 1u) == 0u) ++outside;
        }
        for (u32 edge = 0; edge < child->count; ++edge) {
          const Vec3 a = deform(*child, edge);
          const Vec3 b = deform(*child, (edge + 1) % child->count);
          for (u32 v = 0; v < cap->count; ++v) {
            const f32 margin = dot(cross(b - a, deform(*cap, v) - a), normal) / length(b - a);
            worst_margin = std::min(worst_margin, margin);
          }
        }
      }
    }
    MESSAGE(std::string_view(species) << " strong-wind cap containment margin: " << worst_margin
                                      << " m; outside samples: " << outside);
    MESSAGE("bridge area ratios " << least_area_ratio << ".." << greatest_area_ratio
                                  << "; least normal agreement " << least_orientation);
    CHECK(least_area_ratio > .25f);
    CHECK(greatest_area_ratio < 4.0f);
    CHECK(least_orientation > 0);
    CHECK(outside == 0);
    CHECK(worst_margin >= -1e-5f);
  }
}
#endif
