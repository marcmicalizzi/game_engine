#include <core/base/assert.h>
#include <systems/animation/pose_pool.h>

namespace engine::animation {

void PosePool::reserve(u32 joints) {
  translation_.reserve(joints);
  rotation_.reserve(joints);
  scale_.reserve(joints);
  matrices_.reserve(joints);
}

void PosePool::grow(u32 joints) {
  const u32 first = rotation_.size();
  const u32 wanted = first + joints;
  // **Reserve geometrically, then resize.** `Vector::resize` reserves *exactly* what it was asked
  // for, so a pool that appends one skeleton's worth of joints at a time and resizes to the new
  // total reallocates and copies on every acquire — O(n^2) in the population, which is not slow so
  // much as never finished: filling 65,536 slots this way spun for ten CPU-minutes in a debug
  // build before this line existed. Doubling makes the amortized cost a constant, and a pool that
  // was told its population up front (`AnimationConfig::reserve_joints`) never gets here twice.
  if (wanted > rotation_.capacity()) {
    const u32 doubled = rotation_.capacity() < wanted / 2 ? wanted : rotation_.capacity() * 2;
    const u32 capacity = doubled > wanted ? doubled : wanted;
    translation_.reserve(capacity);
    rotation_.reserve(capacity);
    scale_.reserve(capacity);
    matrices_.reserve(capacity);
  }
  translation_.resize(wanted);
  rotation_.resize(wanted);
  scale_.resize(wanted);
  matrices_.resize(wanted);
}

u32 PosePool::acquire(u32 skeleton, u32 joints) {
  if (joints == 0) return k_no_slot;
  if (skeleton >= free_by_skeleton_.size()) free_by_skeleton_.resize(skeleton + 1);
  Vector<u32>& free_list = free_by_skeleton_[skeleton];
  if (!free_list.empty()) {
    // LIFO, so the slot a sequence of acquires and releases hands out is a function of the
    // sequence alone: two runs of the same world agree, and so do one worker and eight.
    const u32 slot = free_list.back();
    free_list.pop_back();
    ENGINE_ASSERT(slots_[slot].joints == joints,
                  "animation: a free list holds slots of one skeleton, so of one length");
    slots_[slot].live = true;
    ++live_;
    return slot;
  }
  Slot slot;
  slot.first = rotation_.size();
  slot.joints = joints;
  slot.skeleton = skeleton;
  slot.live = true;
  grow(joints);
  slots_.push_back(slot);
  ++live_;
  return slots_.size() - 1;
}

void PosePool::release(u32 slot) noexcept {
  if (slot >= slots_.size() || !slots_[slot].live) return;
  slots_[slot].live = false;
  --live_;
  const u32 skeleton = slots_[slot].skeleton;
  if (skeleton >= free_by_skeleton_.size()) free_by_skeleton_.resize(skeleton + 1);
  free_by_skeleton_[skeleton].push_back(slot);
}

anim::PoseView PosePool::pose(u32 slot) noexcept {
  const Slot& s = slots_[slot];
  return anim::PoseView{std::span<Vec3>(translation_.data() + s.first, s.joints),
                        std::span<Quat>(rotation_.data() + s.first, s.joints),
                        std::span<Vec3>(scale_.data() + s.first, s.joints)};
}

anim::ConstPoseView PosePool::pose(u32 slot) const noexcept {
  const Slot& s = slots_[slot];
  return anim::ConstPoseView{std::span<const Vec3>(translation_.data() + s.first, s.joints),
                             std::span<const Quat>(rotation_.data() + s.first, s.joints),
                             std::span<const Vec3>(scale_.data() + s.first, s.joints)};
}

std::span<anim::JointMatrix> PosePool::matrices(u32 slot) noexcept {
  const Slot& s = slots_[slot];
  return {matrices_.data() + s.first, s.joints};
}

std::span<const anim::JointMatrix> PosePool::matrices(u32 slot) const noexcept {
  const Slot& s = slots_[slot];
  return {matrices_.data() + s.first, s.joints};
}

u64 PosePool::bytes() const noexcept {
  const u64 joints = rotation_.size();
  return joints * (sizeof(Vec3) * 2 + sizeof(Quat) + sizeof(anim::JointMatrix)) +
         u64{slots_.size()} * sizeof(Slot);
}

void PosePool::clear() noexcept {
  slots_.clear();
  free_by_skeleton_.clear();
  translation_.clear();
  rotation_.clear();
  scale_.clear();
  matrices_.clear();
  live_ = 0;
}

}  // namespace engine::animation
