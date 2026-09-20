// What a skinned character's matrix work costs, with nothing else in the frame.
//
// [ADR-0031](../../../docs/adr/0031-minimum-cpu-x86-64-v3.md) measured `animation.tick.lod0` 15%
// slower under `/arch:AVX2` and isolated the whole of it, by subtracting the two tiers that differ
// only in skinning, to **`anim::local_to_model` plus `anim::skinning_matrices`**: 0.967 µs an
// instance at x86-64-v2 against 1.372 µs at v3, +42%, for a 23-joint skeleton. That subtraction is
// an inference from two tick rows measured through the ECS, the pose pool and three systems; this
// file is the direct measurement it stands in for, so the next person who changes these kernels
// does not have to run a 10^4-entity world to see what they did.
//
// **The skeleton is the one the regression was found on.** `systems/animation`'s bench builds a
// 23-joint *chain* (`parent = j - 1`), and that is what is reproduced here rather than a prettier
// humanoid, so the rows below and ADR-0031's per-instance column are the same workload. A chain is
// also the honest worst case for `local_to_model`: every joint reads the matrix the previous
// iteration wrote, so the loop is one long dependency through `out[]` with no parallelism for the
// hardware to find. `anim.skeleton.humanoid_tree` is the counterweight — the same 23 joints with a
// branching parent table — because a real character's spine, two arms and two legs give four
// independent chains and a different amount of that dependency to hide.
//
// **Two population sizes, on purpose.** The `*.one` rows keep one skeleton's arrays in L1, which is
// where a dependency-chain or store-forwarding problem shows up undiluted. The `*.crowd` rows walk
// 256 instances end to end — 256 × 23 × 64 B of model matrices, about 376 KB, past this L2 — so a
// change that wins in L1 by spending bandwidth cannot hide here. A fix has to move both.
//
// Numbers and the machine's state they were taken on: docs/subsystems/anim.md.
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/anim/skeleton.h>
#include <foundation/bench/bench.h>

using namespace engine;

namespace {

constexpr u32 k_joints = 23;      // the humanoid core, as systems/animation's bench uses it
constexpr u32 k_crowd = 256;      // instances in the streaming rows
constexpr u32 k_smoke_crowd = 8;  // CTest runs every bench once, in a debug build

u32 crowd() noexcept { return bench::smoke_mode() ? k_smoke_crowd : k_crowd; }

// A 23-joint parent table. `chain` is `systems/animation`'s bench skeleton; `tree` is a humanoid's
// shape — a spine with a head, two arms and two legs hanging off it — so the two rows differ in
// exactly the thing that decides how much of `local_to_model`'s dependency chain can overlap.
void fill_parents(Vector<i32>& parents, bool tree) {
  parents.assign(k_joints, anim::k_no_joint);
  if (!tree) {
    for (u32 j = 1; j < k_joints; ++j)
      parents[j] = static_cast<i32>(j - 1);
    return;
  }
  // 0 root, 1-4 spine and head, 5-9 and 10-14 arms, 15-18 and 19-22 legs.
  constexpr i32 k_tree[k_joints] = {-1, 0,  1,  2,  3,   // spine + head
                                    2,  5,  6,  7,  8,   // left arm
                                    2,  10, 11, 12, 13,  // right arm
                                    0,  15, 16, 17,      // left leg
                                    0,  19, 20, 21};     // right leg
  for (u32 j = 0; j < k_joints; ++j)
    parents[j] = k_tree[j];
}

// A skeleton and a pose that are not the identity: an identity rotation would let nothing in the
// product be a real multiply, and a bind pose of zeros would make every translation column the
// same. The values are a fixed function of the joint index, so every run of every build starts
// from the same bits and a bit-for-bit comparison between two builds means something.
struct Rig {
  anim::Skeleton skeleton;
  anim::Pose pose;

  explicit Rig(bool tree, u32 seed = 0) {
    skeleton.resize(k_joints);
    fill_parents(skeleton.parents, tree);
    for (u32 j = 0; j < k_joints; ++j) {
      const f32 t = static_cast<f32>(j + seed) * 0.37f;
      skeleton.local_bind[j].position = Vec3{0.05f * t, 0.25f + 0.01f * t, -0.03f * t};
      skeleton.local_bind[j].rotation =
          normalize(Quat{0.1f * t, 0.2f - 0.01f * t, 0.05f * t, 1.0f});
      skeleton.local_bind[j].scale = Vec3{1.0f, 1.0f, 1.0f};
    }
    anim::compute_inverse_bind(skeleton);
    anim::rest_pose(skeleton, pose);
    // Move the pose off the bind pose, the way a sampled clip would.
    for (u32 j = 0; j < k_joints; ++j) {
      const f32 t = static_cast<f32>(j + seed) * 0.11f;
      pose.rotation[j] = normalize(Quat{0.3f * t, 0.1f, 0.2f - 0.02f * t, 0.9f});
      pose.translation[j] = pose.translation[j] + Vec3{0.01f * t, 0.0f, 0.02f * t};
    }
  }
};

// One rig, its model matrices and its skinning matrices, reused for the life of the process: the
// rig costs more to build than the kernel costs to run.
Rig& rig_for(bool tree) {
  static Rig chain{false};
  static Rig humanoid{true};
  return tree ? humanoid : chain;
}

struct Crowd {
  Vector<Rig> rigs;
  Vector<Mat4> model;  // k_crowd runs of k_joints, end to end
  Vector<anim::JointMatrix> matrices;

  Crowd() {
    const u32 count = crowd();
    rigs.reserve(count);
    for (u32 i = 0; i < count; ++i)
      rigs.emplace_back(false, i);
    model.assign(count * k_joints, Mat4::identity());
    matrices.assign(count * k_joints, anim::JointMatrix{});
  }
};

Crowd& the_crowd() {
  static Crowd crowd;
  return crowd;
}

}  // namespace

// --- one instance, hot -------------------------------------------------------------------------

// The pose-to-model composition alone: 23 `Mat4` products, each one reading the matrix the
// previous joint wrote. ADR-0031's regression lives here.
ENGINE_BENCH(anim_local_to_model_one, "anim.skeleton.local_to_model.one") {
  Rig& rig = rig_for(false);
  Vector<Mat4> model(k_joints, Mat4::identity());
  while (state.keep_running()) {
    anim::local_to_model(rig.skeleton, rig.pose, std::span<Mat4>(model.data(), model.size()));
    bench::keep(model[k_joints - 1]);
  }
  state.set_items(k_joints);
}

// The same composition over a branching humanoid: four independent chains instead of one.
ENGINE_BENCH(anim_local_to_model_tree, "anim.skeleton.humanoid_tree.one") {
  Rig& rig = rig_for(true);
  Vector<Mat4> model(k_joints, Mat4::identity());
  while (state.keep_running()) {
    anim::local_to_model(rig.skeleton, rig.pose, std::span<Mat4>(model.data(), model.size()));
    bench::keep(model[k_joints - 1]);
  }
  state.set_items(k_joints);
}

// **The other half of `local_to_model`, on its own.** The loop does two things per joint: build
// the joint's local matrix from its translation, rotation and scale, and compose it onto the
// parent's. This row is the first of those and nothing else — 23 `core/math::mat4_from_transform`
// calls — so the composition's cost can be got by subtraction and the two halves can be blamed
// separately. It is here rather than in `core/math`'s bench because it exists to explain *this*
// module's numbers: after the kernels were vectorized, `skinning_matrices` came out slightly
// faster at x86-64-v3 than at v2 while `local_to_model` stayed 26% slower, and this row is what
// says which half is still carrying that.
ENGINE_BENCH(anim_local_matrices_one, "anim.skeleton.local_matrices.one") {
  Rig& rig = rig_for(false);
  Vector<Mat4> model(k_joints, Mat4::identity());
  while (state.keep_running()) {
    for (u32 j = 0; j < k_joints; ++j) {
      model[j] = mat4_from_transform(
          Transform3{rig.pose.translation[j], rig.pose.rotation[j], rig.pose.scale[j]});
    }
    bench::keep(model[k_joints - 1]);
  }
  state.set_items(k_joints);
}

// `model[j] * inverse_bind[j]` to 3x4, 23 times: independent products, no chain.
ENGINE_BENCH(anim_skinning_matrices_one, "anim.skeleton.skinning_matrices.one") {
  Rig& rig = rig_for(false);
  Vector<Mat4> model(k_joints, Mat4::identity());
  anim::local_to_model(rig.skeleton, rig.pose, std::span<Mat4>(model.data(), model.size()));
  Vector<anim::JointMatrix> out(k_joints, anim::JointMatrix{});
  while (state.keep_running()) {
    anim::skinning_matrices(
        std::span<const Mat4>(model.data(), model.size()),
        std::span<const Mat4>(rig.skeleton.inverse_bind.data(), rig.skeleton.inverse_bind.size()),
        std::span<anim::JointMatrix>(out.data(), out.size()));
    bench::keep(out[k_joints - 1]);
  }
  state.set_items(k_joints);
}

// **The row ADR-0031 subtracted for.** Both kernels back to back is one instance's whole skinning
// cost, which is what `animation.tick.lod1` minus `animation.tick.lod2` estimated at 0.967 µs (v2)
// and 1.372 µs (v3). Compare this row against that pair, not against the two rows above.
ENGINE_BENCH(anim_skinning_one, "anim.skeleton.skinning.one") {
  Rig& rig = rig_for(false);
  Vector<Mat4> model(k_joints, Mat4::identity());
  Vector<anim::JointMatrix> out(k_joints, anim::JointMatrix{});
  while (state.keep_running()) {
    anim::local_to_model(rig.skeleton, rig.pose, std::span<Mat4>(model.data(), model.size()));
    anim::skinning_matrices(
        std::span<const Mat4>(model.data(), model.size()),
        std::span<const Mat4>(rig.skeleton.inverse_bind.data(), rig.skeleton.inverse_bind.size()),
        std::span<anim::JointMatrix>(out.data(), out.size()));
    bench::keep(out[k_joints - 1]);
  }
  state.set_items(k_joints);
}

// --- a crowd, streaming ------------------------------------------------------------------------

// The same work over 256 instances laid end to end, so the model matrices do not fit in L2 and a
// fix that trades bandwidth for arithmetic cannot look good here by accident.
ENGINE_BENCH(anim_skinning_crowd, "anim.skeleton.skinning.crowd") {
  Crowd& c = the_crowd();
  const u32 count = crowd();
  while (state.keep_running()) {
    for (u32 i = 0; i < count; ++i) {
      const Rig& rig = c.rigs[i];
      std::span<Mat4> model(c.model.data() + usize{i} * k_joints, k_joints);
      anim::local_to_model(rig.skeleton, rig.pose, model);
      anim::skinning_matrices(
          std::span<const Mat4>(model.data(), model.size()),
          std::span<const Mat4>(rig.skeleton.inverse_bind.data(), rig.skeleton.inverse_bind.size()),
          std::span<anim::JointMatrix>(c.matrices.data() + usize{i} * k_joints, k_joints));
    }
    bench::keep(c.matrices[usize{count} * k_joints - 1]);
  }
  state.set_items(u64{count} * k_joints);
}
