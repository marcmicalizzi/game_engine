// A clip's travel (`clip_displacement_bound`) is the same number however its times are walked
// (A47).
//
// The bound is the largest displacement over a set of evaluation times: every key time of every
// track plus a uniform grid. A clip as an exporter writes it keys every track at the same times, so
// the old walk evaluated each distinct time once per track — 111 times over for a 37-joint rig with
// a translation, a rotation and a scale track per joint — and each evaluation walks every track's
// keys from the front: quadratic in both, and seconds on the frame's thread for a game's first
// play of a clip (docs/subsystems/animation.md, "What a clip's first play costs"). The bound now
// evaluates each distinct time once. A maximum over the same set of poses is the same float
// whatever order or multiplicity the set is walked in, so the answer must be the old one **to the
// bit**; this file keeps the old walk as the reference and holds the new one to it on clips of
// every shape a file can carry: baked (every track at the same times), sparse (each track its own
// times), step, cubic, a clip whose keys start late, a one-key clip, and the Khronos Fox where it
// has been fetched.

#include <domain/assets/gltf.h>
#include <systems/animation/library.h>

#include <doctest/doctest.h>
#include <test_paths.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>

using namespace engine;
using namespace engine::animation;

namespace {

// The walk as it stood before A47, line for line: `clip.times` as it is, duplicates and all.
f32 reference_bound(const anim::Skeleton& skeleton, const anim::Clip& clip,
                    const JointBounds& bounds, u32 samples = 64) {
  f32 out = 0.0f;
  const u32 joint_count = skeleton.joint_count();
  if (joint_count == 0) return out;
  Vector<f32> times;
  times.push_back(0.0f);
  for (const f32 t : clip.times) {
    if (t >= 0.0f && t <= clip.duration) times.push_back(t);
  }
  for (u32 i = 0; i < samples && clip.duration > 0.0f; ++i)
    times.push_back(clip.duration * static_cast<f32>(i) / static_cast<f32>(samples));
  anim::Pose pose;
  Vector<Mat4> model(joint_count, Mat4::identity());
  Vector<anim::JointMatrix> matrices(joint_count, anim::JointMatrix{});
  for (const f32 time : times) {
    anim::rest_pose(skeleton, pose);
    clip.sample(time, pose, false);
    anim::local_to_model(skeleton, pose, std::span<Mat4>(model.data(), model.size()));
    anim::skinning_matrices(
        std::span<const Mat4>(model.data(), model.size()),
        std::span<const Mat4>(skeleton.inverse_bind.data(), skeleton.inverse_bind.size()),
        std::span<anim::JointMatrix>(matrices.data(), matrices.size()));
    for (u32 j = 0; j < joint_count && j < bounds.sphere.size(); ++j) {
      const Vec4 sphere = bounds.sphere[j];
      if (sphere.w < 0.0f) continue;
      const anim::JointMatrix& m = matrices[j];
      f32 sum = 0.0f;
      f32 moved[3] = {};
      const f32 p[3] = {sphere.x, sphere.y, sphere.z};
      for (u32 r = 0; r < 3; ++r) {
        const f32 a[3] = {m.rows[r].x, m.rows[r].y, m.rows[r].z};
        f32 row = m.rows[r].w;
        for (u32 c = 0; c < 3; ++c) {
          const f32 d = a[c] - (r == c ? 1.0f : 0.0f);
          sum += d * d;
          row += d * p[c];
        }
        moved[r] = row;
      }
      const f32 displacement =
          length(Vec3{moved[0], moved[1], moved[2]}) + std::sqrt(sum) * sphere.w;
      out = std::max(out, displacement);
    }
  }
  return out;
}

enum class Shape { baked, sparse, step, cubic, late, one_key };

// A chain of `joints` joints, every one keyed on all three paths in the given shape. The values are
// a function of the key and joint indices alone, so the fixture is the same on every compiler.
bool build(Library& library, u32 joints, u32 keys, Shape shape, std::string& error) {
  assets::MeshData mesh;
  mesh.nodes.resize(joints);
  for (u32 j = 0; j < joints; ++j) {
    mesh.nodes[j].name = "joint" + std::to_string(j);
    mesh.nodes[j].parent = j == 0 ? -1 : static_cast<i32>(j - 1);
    mesh.nodes[j].local.position = Vec3{0.0f, j == 0 ? 0.0f : 0.2f, 0.0f};
  }
  assets::Skin skin;
  skin.name = "rig";
  for (u32 j = 0; j < joints; ++j)
    skin.joints.push_back(static_cast<i32>(j));
  mesh.skins.push_back(skin);
  assets::Animation animation;
  animation.name = "clip";
  const u32 count = shape == Shape::one_key ? 1u : keys;
  for (u32 j = 0; j < joints; ++j) {
    for (u32 path = 0; path < 3; ++path) {
      assets::AnimationSampler sampler;
      sampler.interpolation = shape == Shape::step    ? assets::k_interp_step
                              : shape == Shape::cubic ? assets::k_interp_cubic
                                                      : assets::k_interp_linear;
      const u32 components = path == 1 ? 4u : 3u;
      sampler.components = static_cast<u8>(components);
      const u32 per_key = shape == Shape::cubic ? 3u : 1u;
      const u32 track = j * 3 + path;
      for (u32 k = 0; k < count; ++k) {
        // Baked: every track at k / 30. Sparse: each track its own spacing, so almost every time is
        // its own. Late: the first key at a third of a second, so the grid's early points clamp.
        f32 time = static_cast<f32>(k) / 30.0f;
        if (shape == Shape::sparse) time = static_cast<f32>(k) * (0.031f + 0.0007f * track);
        if (shape == Shape::late) time += 1.0f / 3.0f;
        sampler.times.push_back(time);
        for (u32 part = 0; part < per_key; ++part) {
          const bool tangent = per_key == 3 && part != 1;
          const f32 swing = 0.05f * static_cast<f32>((k * 7 + track * 3) % 11) - 0.25f;
          if (path == 0) {
            sampler.values.push_back(tangent ? 0.1f * swing : 0.01f * swing);
            sampler.values.push_back(tangent ? 0.0f : (j == 0 ? 0.0f : 0.2f));
            sampler.values.push_back(tangent ? -0.1f * swing : 0.0f);
          } else if (path == 1) {
            const f32 half = 0.5f * swing;
            sampler.values.push_back(tangent ? 0.2f * swing : 0.0f);
            sampler.values.push_back(tangent ? 0.0f : 0.3f * half);
            sampler.values.push_back(tangent ? -0.2f * swing : half);
            sampler.values.push_back(tangent ? 0.0f : 1.0f);
          } else {
            for (u32 c = 0; c < components; ++c)
              sampler.values.push_back(tangent ? 0.05f * swing : 1.0f + 0.02f * swing);
          }
        }
      }
      assets::AnimationChannel channel;
      channel.node = static_cast<i32>(j);
      channel.sampler = static_cast<u32>(animation.samplers.size());
      channel.path = path == 0   ? assets::k_path_translation
                     : path == 1 ? assets::k_path_rotation
                                 : assets::k_path_scale;
      animation.samplers.push_back(sampler);
      animation.channels.push_back(channel);
    }
  }
  mesh.animations.push_back(animation);
  return library.add(mesh, "rig", nullptr, &error);
}

JointBounds spheres(u32 joints) {
  JointBounds out;
  out.sphere.resize(joints);
  for (u32 j = 0; j < joints; ++j) {
    // One slot nothing binds to, which the bound skips, and spheres off the joints' own axis.
    const f32 radius = j == 2 ? -1.0f : 0.03f + 0.01f * static_cast<f32>(j % 4);
    out.sphere[j] =
        Vec4{0.02f * static_cast<f32>(j % 3), 0.2f * static_cast<f32>(j), 0.01f, radius};
  }
  return out;
}

u32 bits(f32 x) { return std::bit_cast<u32>(x); }

}  // namespace

TEST_CASE("animation: a clip's travel is the old walk's to the bit, on every shape of clip") {
  const Shape shapes[] = {Shape::baked, Shape::sparse, Shape::step,
                          Shape::cubic, Shape::late,   Shape::one_key};
  const char* names[] = {"baked", "sparse", "step", "cubic", "late", "one key"};
  for (u32 s = 0; s < 6; ++s) {
    CAPTURE(names[s]);
    Library library;
    std::string error;
    REQUIRE_MESSAGE(build(library, 9, 31, shapes[s], error), error);
    REQUIRE(library.skeleton_count() == 1);
    REQUIRE(library.clip_count() == 1);
    const anim::Skeleton& skeleton = library.skeleton_data(0);
    const anim::Clip& clip = library.clip_data(0);
    const JointBounds bounds = spheres(skeleton.joint_count());
    for (const u32 samples : {0u, 7u, 64u}) {
      CAPTURE(samples);
      const f32 want = reference_bound(skeleton, clip, bounds, samples);
      const f32 got = clip_displacement_bound(skeleton, clip, bounds, samples);
      CHECK(bits(got) == bits(want));
      if (s != 5) CHECK(want > 0.0f);  // every shape but the one-key clip moves something
    }
  }
}

TEST_CASE("animation: a baked clip's travel walks each distinct time once") {
  // The cost the forest met (A47): 37 joints, three tracks each, two seconds at 30 keys a second.
  // The answer is the reference's; the time is what changed. A debug build's clock says little, so
  // the case checks the ratio of the two walks on the same fixture, where the old one evaluates
  // 111 x 61 + 65 times and the new one 61 + 65 (the grid's points between keys are distinct).
  Library library;
  std::string error;
  REQUIRE_MESSAGE(build(library, 37, 61, Shape::baked, error), error);
  const anim::Skeleton& skeleton = library.skeleton_data(0);
  const anim::Clip& clip = library.clip_data(0);
  const JointBounds bounds = spheres(skeleton.joint_count());
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  const f32 want = reference_bound(skeleton, clip, bounds);
  const auto t1 = clock::now();
  const f32 got = clip_displacement_bound(skeleton, clip, bounds);
  const auto t2 = clock::now();
  CHECK(bits(got) == bits(want));
  const f64 old_ms = std::chrono::duration<f64, std::milli>(t1 - t0).count();
  const f64 new_ms = std::chrono::duration<f64, std::milli>(t2 - t1).count();
  MESSAGE("37 joints, 111 tracks, 61 keys: old walk " << old_ms << " ms, new " << new_ms << " ms");
  // 6,836 evaluations against 126: a factor of 54 in the count. Held to 10, which a machine under
  // any load keeps, since both walks run back to back on the same core.
  CHECK(old_ms > 10.0 * new_ms);
}

TEST_CASE("animation: the Fox's three clips keep their travel to the bit") {
  // A real exporter's clips (the Khronos Fox, CC-BY 4.0, fetched by tools/fetch-samples.ps1).
  const std::string path = test::data_path(ENGINE_SOURCE_DIR "/content/samples/Fox/Fox.glb",
                                           "content/samples/Fox/Fox.glb");
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    MESSAGE("the Fox is not fetched here (tools/fetch-samples.ps1); the case checks nothing");
    return;
  }
  assets::MeshData mesh;
  std::string error;
  REQUIRE_MESSAGE(assets::load_gltf(path, mesh, &error), error);
  Library library;
  REQUIRE_MESSAGE(library.add(mesh, "fox", nullptr, &error), error);
  REQUIRE(library.skeleton_count() >= 1);
  const anim::Skeleton& skeleton = library.skeleton_data(0);
  // The spheres from the Fox's own skin: one primitive, cluster order or not (the bounds are a box
  // and a farthest distance, which no order changes).
  Vector<Vec3> positions;
  Vector<geometry::SkinBinding> bindings;
  for (u32 v = 0; v < mesh.positions.size() && v < mesh.skin_bindings.size(); ++v) {
    positions.push_back(mesh.positions[v]);
    bindings.push_back(mesh.skin_bindings[v]);
  }
  REQUIRE(!positions.empty());
  JointBounds bounds;
  joint_influence_bounds(std::span<const Vec3>(positions.data(), positions.size()),
                         std::span<const geometry::SkinBinding>(bindings.data(), bindings.size()),
                         skeleton.joint_count(), bounds);
  u32 clips = 0;
  for (u32 c = 0; c < library.clip_count(); ++c) {
    if (library.clip(c).skeleton != 0) continue;
    CAPTURE(library.clip(c).name);
    const anim::Clip& clip = library.clip_data(c);
    const f32 want = reference_bound(skeleton, clip, bounds);
    const f32 got = clip_displacement_bound(skeleton, clip, bounds);
    CHECK(bits(got) == bits(want));
    CHECK(want > 0.0f);
    ++clips;
  }
  CHECK(clips == 3);
}
