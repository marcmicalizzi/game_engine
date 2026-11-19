#include <core/math/portable_trig.h>
#include <systems/animation/inertialize.h>

#include <cmath>
#include <initializer_list>

namespace engine::animation {

f32 inertial_curve(f32 x0_in, f32 v0_in, f32 t_in, f32 t1_in) noexcept {
  if (!std::isfinite(x0_in) || !std::isfinite(v0_in) || !std::isfinite(t_in) ||
      !std::isfinite(t1_in) || !(t1_in > 0.0f) || !(t_in < t1_in))
    return 0.0f;
  // On the offset's magnitude: a negative offset is the same curve mirrored.
  const f64 sign = x0_in < 0.0f ? -1.0 : 1.0;
  const f64 x0 = sign * static_cast<f64>(x0_in);
  f64 v0 = sign * static_cast<f64>(v0_in);
  f64 t1 = static_cast<f64>(t1_in);
  const f64 t = t_in > 0.0f ? static_cast<f64>(t_in) : 0.0;
  // The guard (Bollo's rules as Unreal's `CalcInertialFloat` is described; the experiment page):
  // a velocity away from zero is dropped, and a velocity toward it fast enough to carry the curve
  // past zero shortens the transition to the duration at which it would only touch it.
  if (v0 > 0.0) v0 = 0.0;
  if (v0 < 0.0) {
    const f64 reach = -5.0 * x0 / v0;
    if (reach < t1) t1 = reach;
  }
  if (!(t < t1)) return 0.0f;
  // In s = t / t1, with u = v0 t1 and alpha = a0 t1^2: x(s) = x0 + u s + alpha/2 s^2 + c s^3 +
  // b s^4 + a s^5, the three end conditions solved for a, b, c, and alpha for a zero jerk at the
  // end, taken as 0 when negative.
  const f64 s = t / t1;
  const f64 u = v0 * t1;
  f64 alpha = -8.0 * u - 20.0 * x0;
  if (alpha < 0.0) alpha = 0.0;
  const f64 a = -(alpha + 6.0 * u + 12.0 * x0) * 0.5;
  const f64 b = (3.0 * alpha + 16.0 * u + 30.0 * x0) * 0.5;
  const f64 c = -(3.0 * alpha + 12.0 * u + 20.0 * x0) * 0.5;
  f64 x = a;
  x = x * s + b;
  x = x * s + c;
  x = x * s + alpha * 0.5;
  x = x * s + u;
  x = x * s + x0;
  // In real arithmetic x >= 0 on [0, t1) (the header's factorizations); near t1 the rounding of
  // the sum can leave a few 1e-16 below it, which would be the offset passing the target.
  x = x > 0.0 ? x : 0.0;
  return static_cast<f32>(sign * x);
}

namespace {

// The rotation from `target` to `source`, `source * conj(target)` on the shorter arc, as an axis
// times an angle in [0, pi]: the angle is 2 atan2(|v|, w) through the portable arctangent.
Vec3 rotation_offset(Quat source, Quat target) noexcept {
  Quat q = source * conjugate(target);
  const f32 len = std::sqrt(dot(q, q));
  if (len > 0.0f) q = q * (1.0f / len);
  if (q.w < 0.0f) q = -q;
  const f32 v = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
  if (!(v > 0.0f)) return Vec3{0.0f, 0.0f, 0.0f};
  const f32 angle = 2.0f * portable_atan2(v, q.w);
  const f32 k = angle / v;
  return Vec3{q.x * k, q.y * k, q.z * k};
}

}  // namespace

void inertial_capture(anim::ConstPoseView source, anim::ConstPoseView source_prev,
                      anim::ConstPoseView target, anim::ConstPoseView target_prev, f32 dt,
                      std::span<InertialJoint> out) noexcept {
  usize n = out.size();
  for (const anim::ConstPoseView* p : {&source, &source_prev, &target, &target_prev}) {
    if (p->rotation.size() < n) n = p->rotation.size();
    if (p->translation.size() < n) n = p->translation.size();
  }
  const f32 inv_dt = dt > 0.0f ? 1.0f / dt : 0.0f;
  for (usize j = 0; j < n; ++j) {
    const Vec3 dx = source.translation[j] - target.translation[j];
    const Vec3 dx_prev = source_prev.translation[j] - target_prev.translation[j];
    const Vec3 dr = rotation_offset(source.rotation[j], target.rotation[j]);
    const Vec3 dr_prev = rotation_offset(source_prev.rotation[j], target_prev.rotation[j]);
    out[j].dx = dx;
    out[j].dv = (dx - dx_prev) * inv_dt;
    out[j].dr = dr;
    out[j].dw = (dr - dr_prev) * inv_dt;
  }
  for (usize j = n; j < out.size(); ++j)
    out[j] = InertialJoint{};
}

void inertial_apply(std::span<const InertialJoint> offsets, f32 t, f32 duration,
                    anim::PoseView pose) noexcept {
  if (!(t < duration)) return;
  usize n = offsets.size();
  if (pose.rotation.size() < n) n = pose.rotation.size();
  if (pose.translation.size() < n) n = pose.translation.size();
  for (usize j = 0; j < n; ++j) {
    const InertialJoint& o = offsets[j];
    // The translation along its direction, by the curve of its length and its speed along it.
    const f32 x0 = std::sqrt(dot(o.dx, o.dx));
    const f32 inv_x = x0 > 0.0f ? 1.0f / x0 : 0.0f;
    const Vec3 dir = o.dx * inv_x;
    const f32 x = inertial_curve(x0, dot(o.dv, dir), t, duration);
    pose.translation[j] = pose.translation[j] + dir * x;
    // The rotation about its axis, by the curve of its angle and its angular speed about it,
    // turned back into a quaternion through the portable sine and cosine.
    const f32 a0 = std::sqrt(dot(o.dr, o.dr));
    const f32 inv_a = a0 > 0.0f ? 1.0f / a0 : 0.0f;
    const Vec3 axis = o.dr * inv_a;
    const f32 a = inertial_curve(a0, dot(o.dw, axis), t, duration);
    f32 s = 0.0f;
    f32 c = 1.0f;
    portable_sin_cos(a * 0.5f, s, c);
    const Quat turn{axis.x * s, axis.y * s, axis.z * s, c};
    pose.rotation[j] = turn * pose.rotation[j];
  }
}

u32 InertialArena::acquire() {
  if (!free_.empty()) {
    const u32 slot = free_.back();
    free_.pop_back();
    return slot;
  }
  if (slots_ >= k_max_slots) return k_none;
  // A new slot: its block, when it is the first of one (blocks never move, so every other slot's
  // entries stay where a worker may be reading them); `free_` keeps room for every slot, so a
  // release never allocates.
  if (!blocks_)
    blocks_ = std::make_unique<std::unique_ptr<InertialJoint[]>[]>(k_max_slots / k_block_slots);
  const u32 slot = slots_++;
  if (slot % k_block_slots == 0)
    blocks_[slot / k_block_slots] =
        std::make_unique<InertialJoint[]>(usize{k_block_slots} * joints_);
  if (free_.capacity() < slots_) free_.reserve(2u * slots_);
  return slot;
}

void InertialArena::release(u32 slot) noexcept {
  if (slot < slots_) free_.push_back(slot);
}

usize InertialArena::bytes() const noexcept {
  if (!blocks_) return 0;
  const usize blocks = (usize{slots_} + k_block_slots - 1) / k_block_slots;
  return usize{k_max_slots / k_block_slots} * sizeof(std::unique_ptr<InertialJoint[]>) +
         blocks * k_block_slots * joints_ * sizeof(InertialJoint) + free_.capacity() * sizeof(u32);
}

}  // namespace engine::animation
