// S92: a skinned character's non-joint ancestors are part of its pose.
//
// The game's cat was exported from Blender with its joints under an `Armature` node translated
// +0.375 m on y, and stood buried by exactly that much: the library parented a root joint to
// nothing and dropped the Armature from the bind and from every clip. These cases hold the fix to
// three things: the rest pose stands at the ancestor's height, a clip that animates the ancestor
// moves the skeleton, and the file the game rewrote to work around the bug (`ancestor_glb.h`'s
// normalised form) poses **to the bit** like the original wherever the fold is exact.
#include "ancestor_glb.h"
#include "pose_hash.h"

#include <domain/assets/gltf.h>
#include <domain/ecs/identity.h>
#include <domain/sim/scheduler.h>
#include <systems/animation/animation.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string>

using namespace engine;
using namespace engine::animation;

namespace {

struct Loaded {
  engine::test::TempDir tmp{"engine_animation_ancestors"};
  Library library;
  LoadStats stats;
  u32 skeleton = Library::k_not_found;

  explicit Loaded(bool normalised) {
    const std::string path = tmp.file(normalised ? "normalised.glb" : "original.glb");
    REQUIRE(test_fixture::write_ancestor_glb(path, normalised));
    std::string error;
    REQUIRE_MESSAGE(library.load_gltf(path, "cat", &stats, &error), error);
    skeleton = library.skeleton_index(Library::skeleton_id("cat/bar_skin"));
    REQUIRE(skeleton != Library::k_not_found);
  }

  u32 clip(const char* name) const {
    const u32 index = library.clip_index(Library::clip_id(std::string("cat/") + name));
    REQUIRE(index != Library::k_not_found);
    return index;
  }
};

Vector<geometry::SkinBinding> bar_bindings() {
  Vector<geometry::SkinBinding> out;
  for (const auto& w : test_fixture::k_bar_weights) {
    const u32 joints[4] = {0, 1, 0, 0};
    const f32 weights[4] = {w[0], w[1], 0.0f, 0.0f};
    out.push_back(geometry::make_skin_binding(joints, weights));
  }
  return out;
}

// The bar's six vertices where a clip at `time` puts them (the rest pose for `k_not_found`).
Vector<Vec3> posed_bar(const Library& library, u32 skeleton_index, u32 clip_index, f32 time) {
  const anim::Skeleton& skeleton = library.skeleton_data(skeleton_index);
  anim::Pose pose;
  anim::rest_pose(skeleton, pose);
  if (clip_index != Library::k_not_found) library.clip_data(clip_index).sample(time, pose, true);
  Vector<Mat4> model(skeleton.joint_count(), Mat4::identity());
  Vector<anim::JointMatrix> matrices(skeleton.joint_count(), anim::JointMatrix{});
  anim::local_to_model(skeleton, pose, std::span<Mat4>(model.data(), model.size()));
  anim::skinning_matrices(
      std::span<const Mat4>(model.data(), model.size()),
      std::span<const Mat4>(skeleton.inverse_bind.data(), skeleton.inverse_bind.size()),
      std::span<anim::JointMatrix>(matrices.data(), matrices.size()));
  const Vector<geometry::SkinBinding> bindings = bar_bindings();
  Vector<Vec3> out(6);
  anim::skin_positions(std::span<const Vec3>(test_fixture::k_bar_positions, 6),
                       std::span<const geometry::SkinBinding>(bindings.data(), bindings.size()),
                       std::span<const anim::JointMatrix>(matrices.data(), matrices.size()),
                       std::span<Vec3>(out.data(), out.size()));
  return out;
}

f32 lowest(const Vector<Vec3>& points) {
  f32 y = 1e30f;
  for (const Vec3& p : points)
    y = std::min(y, p.y);
  return y;
}

}  // namespace

TEST_CASE("animation ancestors: the rest pose stands at the non-joint ancestor's height") {
  Loaded original(false);
  const anim::Skeleton& skeleton = original.library.skeleton_data(original.skeleton);
  REQUIRE(skeleton.joint_count() == 2);  // the palette is the skin's; the Armature is not a joint
  CHECK(skeleton.parents[0] == anim::k_no_joint);
  CHECK(skeleton.parents[1] == 0);
  // The Armature folded into the root joint's bind, exactly: a translation over the identity.
  CHECK(skeleton.local_bind[0].position == Vec3{0.0f, test_fixture::k_ancestor_height, 0.0f});
  CHECK(skeleton.local_bind[0].rotation == Quat::identity());
  CHECK(skeleton.local_bind[1].position == Vec3{0.0f, 1.0f, 0.0f});
  // Before the fold the bar's foot was at y = 0, buried by the Armature's 0.375.
  const Vector<Vec3> rest =
      posed_bar(original.library, original.skeleton, Library::k_not_found, 0.0f);
  MESSAGE("rest: the bar's lowest vertex at y = " << lowest(rest));
  CHECK(lowest(rest) == test_fixture::k_ancestor_height);
  // The Armature's own channels are read into the root joint, not counted as dropped.
  CHECK(original.stats.clips == 3);
  CHECK(original.stats.skipped_channels == 0);
}

TEST_CASE("animation ancestors: a clip that animates the ancestor moves the skeleton") {
  Loaded original(false);
  const u32 rise = original.clip("rise");
  const u32 slide = original.clip("slide");
  // The ancestor's track lands on the root joint, as one track of the same keys.
  const anim::Clip& clip = original.library.clip_data(rise);
  u32 root_tracks = 0;
  for (const anim::Track& track : clip.tracks) {
    if (track.joint != 0) continue;
    ++root_tracks;
    CHECK(track.channel == anim::k_channel_translation);
    CHECK(track.key_count == 2);
  }
  CHECK(root_tracks == 1);
  for (const f32 time : {0.0f, 0.25f, 0.5f, 0.75f}) {
    // rise: the Armature 0.375 -> 0.875 on y; slide: the root 0 -> 0.5 over the still Armature.
    const f32 expected = test_fixture::k_ancestor_height + 0.5f * time;
    const f32 risen = lowest(posed_bar(original.library, original.skeleton, rise, time));
    const f32 slid = lowest(posed_bar(original.library, original.skeleton, slide, time));
    CHECK(risen == doctest::Approx(expected).epsilon(1e-6));
    CHECK(slid == doctest::Approx(expected).epsilon(1e-6));
  }
}

TEST_CASE("animation ancestors: the normalised file poses identically, to the bit") {
  Loaded original(false);
  Loaded normalised(true);
  const anim::Skeleton& a = original.library.skeleton_data(original.skeleton);
  const anim::Skeleton& b = normalised.library.skeleton_data(normalised.skeleton);
  REQUIRE(a.joint_count() == b.joint_count());
  for (u32 j = 0; j < a.joint_count(); ++j) {
    CHECK(a.parents[j] == b.parents[j]);
    CHECK(a.local_bind[j] == b.local_bind[j]);
    CHECK(a.inverse_bind[j] == b.inverse_bind[j]);
  }
  CHECK(
      test_fixture::pose_hash(original.library, original.skeleton, Library::k_not_found, {0}) ==
      test_fixture::pose_hash(normalised.library, normalised.skeleton, Library::k_not_found, {0}));
  // "rise" moves the ancestor (one track of the root's own kind: mapped key by key) and "slide" the
  // root over a still ancestor (its keys offset by the ancestor): both exact, so both hash equal,
  // inside the tip's slerp interval too — the same arithmetic on both sides.
  for (const char* name : {test_fixture::k_rise, test_fixture::k_slide}) {
    const auto times = {0.0f, 0.125f, 0.25f, 0.5f, 0.6f, 0.75f, 0.999f, 1.25f};
    const u64 ha =
        test_fixture::pose_hash(original.library, original.skeleton, original.clip(name), times);
    const u64 hb = test_fixture::pose_hash(normalised.library, normalised.skeleton,
                                           normalised.clip(name), times);
    MESSAGE(std::string(name) << ": original " << ha << ", normalised " << hb);
    CHECK(ha == hb);
  }
  // "both" animates the ancestor and the root's translation together: two inputs to one channel,
  // which the fold resamples. The sum of two linear tracks is linear, so the resampled track is
  // the normalised one up to the float rounding of its added keys.
  const u32 both_a = original.clip(test_fixture::k_both);
  const u32 both_b = normalised.clip(test_fixture::k_both);
  f32 worst = 0.0f;
  for (u32 i = 0; i <= 64; ++i) {
    const f32 time = static_cast<f32>(i) / 64.0f;
    const Vector<Vec3> pa = posed_bar(original.library, original.skeleton, both_a, time);
    const Vector<Vec3> pb = posed_bar(normalised.library, normalised.skeleton, both_b, time);
    for (u32 v = 0; v < pa.size(); ++v)
      worst = std::max(worst, length(pa[v] - pb[v]));
  }
  MESSAGE("both: resampled against normalised, worst vertex " << worst << " m");
  CHECK(worst <= 1.0e-6f);
}

TEST_CASE("animation ancestors: a non-joint node between two joints is folded into the child") {
  // Built directly rather than through glTF, because no exporter here writes one: a node with a
  // translation and an animated rotation between the root and the tip. The tip's parent is the
  // root, its bind carries the node's offset, and the node's rotation turns the tip — its rotation
  // key by key, its offset (a rotation carrying a translation) resampled.
  assets::MeshData mesh;
  mesh.nodes.resize(4);
  mesh.nodes[0].name = "Armature";
  mesh.nodes[0].local.position = Vec3{0.0f, test_fixture::k_ancestor_height, 0.0f};
  mesh.nodes[1].name = "joint_root";
  mesh.nodes[1].parent = 0;
  mesh.nodes[2].name = "offset";
  mesh.nodes[2].parent = 1;
  mesh.nodes[2].local.position = Vec3{0.0f, 0.5f, 0.0f};
  mesh.nodes[3].name = "joint_tip";
  mesh.nodes[3].parent = 2;
  mesh.nodes[3].local.position = Vec3{0.0f, 0.5f, 0.0f};
  assets::Skin skin;
  skin.name = "bar_skin";
  skin.joints.push_back(1);
  skin.joints.push_back(3);
  skin.inverse_bind.push_back(Mat4::identity());
  skin.inverse_bind.push_back(Mat4::identity());
  mesh.skins.push_back(skin);
  assets::Animation turn;
  turn.name = "turn";
  assets::AnimationSampler sampler;
  sampler.times.push_back(0.0f);
  sampler.times.push_back(1.0f);
  const f32 quarter = 0.70710678f;
  for (const f32 v : {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, quarter, quarter})
    sampler.values.push_back(v);
  sampler.components = 4;
  turn.samplers.push_back(sampler);
  assets::AnimationChannel channel;
  channel.node = 2;
  channel.sampler = 0;
  channel.path = assets::k_path_rotation;
  turn.channels.push_back(channel);
  turn.duration = 1.0f;
  mesh.animations.push_back(turn);

  Library library;
  std::string error;
  LoadStats stats;
  REQUIRE_MESSAGE(library.add(mesh, "between", &stats, &error), error);
  CHECK(stats.skipped_channels == 0);
  const u32 skeleton_index = library.skeleton_index(Library::skeleton_id("between/bar_skin"));
  REQUIRE(skeleton_index != Library::k_not_found);
  const anim::Skeleton& skeleton = library.skeleton_data(skeleton_index);
  CHECK(skeleton.parents[1] == 0);
  CHECK(skeleton.local_bind[1].position == Vec3{0.0f, 1.0f, 0.0f});
  const u32 clip_index = library.clip_index(Library::clip_id("between/turn"));
  REQUIRE(clip_index != Library::k_not_found);

  // The truth: the tip's model position is the root's, plus the node's offset, plus the node's
  // rotation (about +z, from none to a quarter turn) applied to the tip's own 0.5 up.
  f32 at_keys = 0.0f;
  f32 between = 0.0f;
  for (u32 i = 0; i <= 32; ++i) {
    const f32 time = static_cast<f32>(i) / 32.0f;
    anim::Pose pose;
    anim::rest_pose(skeleton, pose);
    library.clip_data(clip_index).sample(time, pose, false);
    Vector<Mat4> model(2, Mat4::identity());
    anim::local_to_model(skeleton, pose, std::span<Mat4>(model.data(), model.size()));
    const f32 angle = 1.5707963f * time;
    const Vec3 truth{-0.5f * std::sin(angle),
                     test_fixture::k_ancestor_height + 0.5f + 0.5f * std::cos(angle), 0.0f};
    const f32 off = length(model[1].c[3].xyz() - truth);
    // The resampled offset is exact at its keys — every quarter of the interval — and a chord of
    // the arc between them.
    if (i % 8 == 0) {
      at_keys = std::max(at_keys, off);
    } else {
      between = std::max(between, off);
    }
  }
  MESSAGE("intervening node: tip off by " << at_keys << " m at the resampled keys, " << between
                                          << " m between them");
  CHECK(at_keys <= 1.0e-5f);
  CHECK(between <= 0.0125f);  // a chord of radius 0.5 over a sixteenth turn sags 0.0096
}

TEST_CASE("animation ancestors: the bounds padding covers the posed mesh as it rises") {
  // The cull pass tests a skinned instance by its rest-pose spheres inflated by the padding. The
  // bar's bind-space sphere is at y = 1; the rest pose already stands 0.375 above it and "rise"
  // lifts it 0.5 more. Every posed vertex must be inside the bind sphere grown by the padding, or
  // the cull would drop the character as it rises.
  Loaded original(false);
  const anim::Skeleton& skeleton = original.library.skeleton_data(original.skeleton);
  const Vector<geometry::SkinBinding> bindings = bar_bindings();
  JointBounds bounds;
  joint_influence_bounds(std::span<const Vec3>(test_fixture::k_bar_positions, 6),
                         std::span<const geometry::SkinBinding>(bindings.data(), bindings.size()),
                         skeleton.joint_count(), bounds);
  const Vec3 center{0.0f, 1.0f, 0.0f};
  f32 radius = 0.0f;
  for (const Vec3& p : test_fixture::k_bar_positions)
    radius = std::max(radius, length(p - center));

  // The rest pose alone moves the bar by the ancestor's offset, and the skeleton's bound says so.
  const f32 rest_bound = clip_displacement_bound(skeleton, anim::Clip{}, bounds, 0);
  CHECK(rest_bound == doctest::Approx(test_fixture::k_ancestor_height).epsilon(1e-6));
  const f32 skeleton_bound =
      skeleton_displacement_bound(original.library, original.skeleton, bounds);
  CHECK(skeleton_bound >= rest_bound);

  for (const char* name : {test_fixture::k_rise, test_fixture::k_slide, test_fixture::k_both}) {
    const u32 clip = original.clip(name);
    const f32 padding = clip_displacement_bound(skeleton, original.library.clip_data(clip), bounds);
    f32 worst_outside = -1e30f;
    for (u32 i = 0; i < 997; ++i) {
      const f32 time = static_cast<f32>(i) / 997.0f;
      for (const Vec3& p : posed_bar(original.library, original.skeleton, clip, time))
        worst_outside = std::max(worst_outside, length(p - center) - (radius + padding));
    }
    MESSAGE(std::string(name) << ": padding " << padding << " m, the worst posed vertex "
                              << -worst_outside << " m inside the padded sphere");
    CHECK(padding >= 0.875f);  // the rest's 0.375 and the clip's 0.5, its last key included
    CHECK(worst_outside <= 0.0f);
    CHECK(skeleton_bound >= padding);
  }
}

TEST_CASE("animation ancestors: the LOD tiers transition the same for both forms") {
  // The original and the normalised file through the same tier changes, tick by tick: demoted to
  // LOD3 (the slot released), promoted back (re-acquired, the playhead advanced analytically), and
  // every tick's bone matrices bit-identical between the two.
  Loaded original(false);
  Loaded normalised(true);
  ecs::SimWorldConfig config;
  config.hz = 64;
  ecs::SimWorld sim_a(config);
  ecs::SimWorld sim_b(config);
  AnimationSystem anim_a(original.library);
  AnimationSystem anim_b(normalised.library);
  anim_a.install(sim_a);
  anim_b.install(sim_b);
  const Id128 skeleton = Library::skeleton_id("cat/bar_skin");
  const Id128 rise = Library::clip_id("cat/rise");
  const flecs::entity a = sim_a.world().entity("cat");
  const flecs::entity b = sim_b.world().entity("cat");
  REQUIRE(anim_a.attach(a, skeleton, rise));
  REQUIRE(anim_b.attach(b, skeleton, rise));

  u32 compared = 0;
  u32 differing = 0;
  auto compare = [&]() {
    const SkeletonInstance& ia = *a.try_get<SkeletonInstance>();
    const SkeletonInstance& ib = *b.try_get<SkeletonInstance>();
    REQUIRE((ia.pose_slot == k_no_slot) == (ib.pose_slot == k_no_slot));
    if (ia.pose_slot == k_no_slot) return;
    const u32 joints = anim_a.joint_count(ia.pose_slot);
    REQUIRE(anim_b.joint_count(ib.pose_slot) == joints);
    const anim::JointMatrix* ma = anim_a.joint_matrices().data() + anim_a.first_joint(ia.pose_slot);
    const anim::JointMatrix* mb = anim_b.joint_matrices().data() + anim_b.first_joint(ib.pose_slot);
    for (u32 j = 0; j < joints; ++j) {
      ++compared;
      differing += std::memcmp(&ma[j], &mb[j], sizeof(anim::JointMatrix)) != 0 ? 1u : 0u;
    }
  };
  const u8 tiers[] = {0, 1, 2, 3, 3, 0, 2, 0};
  for (const u8 tier : tiers) {
    anim_a.set_tier(a, tier);
    anim_b.set_tier(b, tier);
    for (u32 i = 0; i < 11; ++i) {
      sim_a.step();
      sim_b.step();
      compare();
    }
  }
  MESSAGE("LOD walk: " << compared << " bone matrices compared, " << differing << " differ");
  CHECK(compared > 0);
  CHECK(differing == 0);
}
