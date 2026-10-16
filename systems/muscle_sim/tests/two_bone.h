#pragma once

// A synthetic two-bone skeleton driven by a prescribed analytic joint angle: the pose source the
// path tests and the world test route actuators through, and the analytic length they are held to.
//
// "upper" is the root; "lower" hangs off it at (0, k_bone + extension, 0) and turns about z by
// theta(t) = amplitude sin(2 pi hz t). An actuator runs from k_origin on "upper" (optionally
// through k_via on "upper") to k_insertion on "lower", which puts its path near the r008 actuator's
// 0.148 m, taut, for |theta| <= 0.5 rad.

#include <core/containers/vector.h>
#include <core/math/math.h>
#include <core/time/time.h>
#include <domain/anim/skeleton.h>
#include <systems/muscle_sim/muscle_sim.h>

#include <cmath>
#include <span>
#include <string_view>

namespace engine::muscle_sim::test {

inline constexpr f32 k_bone = 0.3f;
inline constexpr Vec3 k_origin{0.04f, 0.19f, 0.0f};
inline constexpr Vec3 k_via{0.03f, 0.26f, 0.0f};
inline constexpr Vec3 k_insertion{0.0f, 0.03f, 0.0f};
inline constexpr double k_two_pi = 6.283185307179586;

struct Motion {
  double amplitude = 0.4;  // rad
  double hz = 1.0;
  f32 extension = 0.0f;  // m, added to the lower bone's offset from tick `extend_at` on
  u64 extend_at = ~u64{0};

  double angle(double t) const { return amplitude * std::sin(k_two_pi * hz * t); }
  double angle_rate(double t) const {
    return amplitude * k_two_pi * hz * std::cos(k_two_pi * hz * t);
  }
};

inline anim::Skeleton two_bone_skeleton() {
  anim::Skeleton s;
  s.resize(2);
  s.names[0] = "upper";
  s.names[1] = "lower";
  s.parents[0] = anim::k_no_joint;
  s.parents[1] = 0;
  s.local_bind[1].position = Vec3{0.0f, k_bone, 0.0f};
  anim::compute_inverse_bind(s);
  return s;
}

// The skeleton posed at angle theta with the lower bone `extra` metres longer.
inline void pose_two_bone(const anim::Skeleton& s, double theta, f32 extra, std::span<Mat4> out) {
  anim::Pose pose;
  pose.resize(2);
  for (u32 j = 0; j < 2; ++j) {
    pose.translation[j] = Vec3{};
    pose.rotation[j] = Quat::identity();
    pose.scale[j] = Vec3::one();
  }
  pose.translation[1] = Vec3{0.0f, k_bone + extra, 0.0f};
  pose.rotation[1] = quat_from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, static_cast<f32>(theta));
  anim::local_to_model(s, pose, out);
}

// The route's length in closed form, from the same f32 constants widened: origin to insertion, or
// origin to the via point to insertion.
inline double analytic_length(double theta, bool via = false, double extra = 0.0) {
  const double c = std::cos(theta);
  const double sn = std::sin(theta);
  const double ix = static_cast<double>(k_insertion.x);
  const double iy = static_cast<double>(k_insertion.y);
  const double px = ix * c - iy * sn;
  const double py = static_cast<double>(k_bone) + extra + ix * sn + iy * c;
  const double sx = static_cast<double>(via ? k_via.x : k_origin.x);
  const double sy = static_cast<double>(via ? k_via.y : k_origin.y);
  double total = std::hypot(px - sx, py - sy);
  if (via) {
    total += std::hypot(static_cast<double>(k_via.x) - static_cast<double>(k_origin.x),
                        static_cast<double>(k_via.y) - static_cast<double>(k_origin.y));
  }
  return total;
}

// dL/dt of the direct route at time t under `m`.
inline double analytic_rate(const Motion& m, double t) {
  const double theta = m.angle(t);
  const double c = std::cos(theta);
  const double sn = std::sin(theta);
  const double ix = static_cast<double>(k_insertion.x);
  const double iy = static_cast<double>(k_insertion.y);
  const double dx = ix * c - iy * sn - static_cast<double>(k_origin.x);
  const double dy =
      static_cast<double>(k_bone) + ix * sn + iy * c - static_cast<double>(k_origin.y);
  // d(px, py)/dtheta = (-ix sn - iy c, ix c - iy sn)
  const double dl = (dx * (-ix * sn - iy * c) + dy * (ix * c - iy * sn)) / std::hypot(dx, dy);
  return dl * m.angle_rate(t);
}

// The pose source: each character entity rides the two-bone skeleton under its own motion, posed
// at the end of the tick the world is in.
struct TwoBoneSource {
  anim::Skeleton skeleton = two_bone_skeleton();
  Vector<flecs::entity_t> characters;
  Vector<Motion> motions;

  const Motion* motion_of(flecs::entity character) const {
    for (u32 i = 0; i < characters.size(); ++i)
      if (characters[i] == character.id()) return &motions[i];
    return nullptr;
  }

  static bool find_joint(void* context, flecs::entity character, std::string_view bone,
                         u32& joint) {
    const auto* self = static_cast<const TwoBoneSource*>(context);
    if (self->motion_of(character) == nullptr) return false;
    const i32 found = self->skeleton.find(bone);
    if (found < 0) return false;
    joint = static_cast<u32>(found);
    return true;
  }

  static bool model_transforms(void* context, flecs::entity character, std::span<Mat4> out,
                               u32& joints) {
    const auto* self = static_cast<const TwoBoneSource*>(context);
    const Motion* m = self->motion_of(character);
    if (m == nullptr || out.size() < 2) return false;
    const u64 tick = character.world().get<SimTick>().value;
    const f32 extra = tick >= m->extend_at ? m->extension : 0.0f;
    pose_two_bone(self->skeleton, m->angle(static_cast<double>(tick) / 60.0), extra, out.first(2));
    joints = 2;
    return true;
  }

  PoseSource source() {
    PoseSource p;
    p.context = this;
    p.find_joint = find_joint;
    p.model_transforms = model_transforms;
    return p;
  }
};

}  // namespace engine::muscle_sim::test
