#pragma once

// The pose pool: this capability's own hot data (docs/plan/03-data-model.md §3.4, "hot systems own
// their own data"; ADR-0028 decision 2's architectural hedge).
//
// **A pose is not a component.** It is an arena the capability owns, and what the ECS holds is a
// slot index (`SkeletonInstance::pose_slot`). Three reasons, in the order they matter:
//
//   1. A pose is variable length. A component is one type of one size per entity, so a pose as a
//      component is either a fixed-width worst case (a 23-joint character paying for a 100-joint
//      one) or a component holding three `Vector`s — three heap allocations and three pointer
//      chases per instance per tick, which is the layout `domain/anim` spends its first page
//      arguing against.
//   2. Nothing outside this capability reads a pose. The renderer reads skinning matrices, and
//      gameplay reads a joint's model transform through this module. A pose in the ECS would be
//      visible to the protocol, to a persistence walk, and to every query that touches the
//      archetype, for nobody's benefit.
//   3. It is recomputable. A pose is a pure function of (skeleton, clip, time), so it is not what
//      a save contains — `AnimationPlayer` is — and putting it in the world would make the world
//      carry a cache.
//
// **Layout.** Three parallel channel arrays plus one array of 3x4 skinning matrices, with every
// slot's joints contiguous inside each. A slot is a `(first_joint, joints)` run, so sampling one
// instance walks four contiguous runs and the skinning matrices of *every* instance are one span
// the renderer can take whole. There is no per-slot stride and therefore no waste: the arena is
// exactly the sum of the live skeletons' joint counts.
//
// **Free lists are per skeleton**, so a released slot is reused by an instance of the same
// skeleton and its run stays the right length; a slot is never split or coalesced. Reuse is LIFO,
// which makes slot assignment a deterministic function of the acquire/release sequence — the
// property the bit-identical tests rest on.
//
// **When the arrays may move.** `acquire` may reallocate, so no view or span obtained from this
// pool survives one. The capability acquires and releases only from a tier transition, which
// happens outside the parallel systems phase — from the LOD phase's hooks, or from a caller
// between ticks — so within a tick the arena is stable and every instance writes only its own run.
// That is also what makes the sampling systems' disjoint writes safe at any worker count.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/anim/skeleton.h>

#include <span>

namespace engine::animation {

// `SkeletonInstance::pose_slot` when the instance has no pose: LOD3, or not yet given one.
inline constexpr u32 k_no_slot = 0xFFFF'FFFFu;

class PosePool {
 public:
  // One slot's run. Public because the size table pins it: it is per instance, like the
  // components, and its size is bandwidth every acquire and release walks.
  struct Slot {
    u32 first = 0;
    u32 joints = 0;
    u32 skeleton = 0;
    bool live = false;
  };

  PosePool() = default;
  // Reserves the channel arrays for an expected population, so a world that knows its size
  // allocates once. A pool that never reserves still never allocates in steady state, because a
  // released slot is reused rather than returned.
  void reserve(u32 joints);

  // A slot for one instance of `skeleton` with `joints` joints. Reuses this skeleton's most
  // recently released slot when there is one, and otherwise appends. `k_no_slot` for zero joints.
  u32 acquire(u32 skeleton, u32 joints);
  // Gives the slot back. Its contents are left alone: nothing may read a free slot, and the
  // renderer never names one because the instance that did has no `deform` entry any more.
  void release(u32 slot) noexcept;

  bool live(u32 slot) const noexcept { return slot < slots_.size() && slots_[slot].live; }
  u32 first_joint(u32 slot) const noexcept { return slots_[slot].first; }
  u32 joint_count(u32 slot) const noexcept { return slots_[slot].joints; }
  u32 skeleton_of(u32 slot) const noexcept { return slots_[slot].skeleton; }

  anim::PoseView pose(u32 slot) noexcept;
  anim::ConstPoseView pose(u32 slot) const noexcept;

  // This instance's bone matrices, which is the run `gfx::DeformDesc::joints` points at.
  std::span<anim::JointMatrix> matrices(u32 slot) noexcept;
  std::span<const anim::JointMatrix> matrices(u32 slot) const noexcept;

  // Every instance's bone matrices, in slot order, as one contiguous span: the upload buffer.
  // See docs/subsystems/animation.md, "The renderer contract".
  std::span<const anim::JointMatrix> upload_buffer() const noexcept {
    return {matrices_.data(), matrices_.size()};
  }

  u32 slot_count() const noexcept { return slots_.size(); }
  u32 live_count() const noexcept { return live_; }
  // Joints the arena holds, live and free: the length of each channel array and of the upload
  // buffer. It never shrinks, because a released slot keeps its run for the next instance of the
  // same skeleton.
  u32 joint_capacity() const noexcept { return rotation_.size(); }
  // What this pool costs: 40 bytes of pose and 48 of matrix per joint of every slot ever used.
  u64 bytes() const noexcept;
  void clear() noexcept;

 private:
  void grow(u32 joints);

  Vector<Slot> slots_;
  // One free list per skeleton index, so a reused slot always has the right run length.
  Vector<Vector<u32>> free_by_skeleton_;
  Vector<Vec3> translation_;
  Vector<Quat> rotation_;
  Vector<Vec3> scale_;
  Vector<anim::JointMatrix> matrices_;
  u32 live_ = 0;
};

}  // namespace engine::animation
