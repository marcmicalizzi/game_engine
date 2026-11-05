// The procedural gait (gait.h; docs/subsystems/animation.md, "The procedural gait"; the sources
// and the fits on docs/experiments/gait-grounding-2026-11-04.md; roadmap C60).
#include <systems/animation/gait.h>

#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/anim/clip.h>
#include <domain/anim/skeleton.h>

#include <cmath>
#include <span>

namespace engine::animation {

namespace {

constexpr f64 k_two_pi = 6.283185307179586;
constexpr f64 k_deg = 0.017453292519943295;
constexpr f64 k_gravity = 9.81;

// ---- the curves ---------------------------------------------------------------------------------
//
// Each sagittal curve is a0 + sum over k = 1..5 of (a_k cos(2 pi k phase) + b_k sin(2 pi k phase)),
// degrees, stored {a0, a1, b1, a2, b2, ..., a5, b5}. The coefficients are a least-squares fit
// (400 samples) to a periodic shape-preserving cubic through the published keypoints — the peaks
// at their phases — listed on the row; the grounding page has the fit script's residuals. A curve
// given by two extremes (the arms) is one harmonic.
struct Series {
  f64 c[11];
};

// Walk, natural cadence. Keypoints (phase %, degrees):
//  hip    Winter 2009 (normative sagittal kinematics) and Perry & Burnfield 2010 (phases): 25 at
//         initial contact, 21 at the end of loading (12), 6 in mid stance (30), -10 peak extension
//         in terminal stance (52), 0 at toe-off (62), 20 in initial swing (75), 28 peak flexion in
//         terminal swing (87). Fit: within 0.25 of every keypoint, 0.38 of the cubic.
//  knee   Winter; Perry: 5 at contact, 18 loading-response peak (15), 4 in mid/terminal stance
//         (40), 40 at toe-off (62), 60 peak in initial swing (73), 25 (88), 3 before contact (97).
//         Fit: within 1.68 (the terminal-swing extension), 1.98 of the cubic.
//  ankle  Winter; Perry (dorsiflexion positive): 0 at contact, -6 in loading (7), 5 (30), 10 peak
//         dorsiflexion in terminal stance (48), -18 peak plantarflexion at toe-off (62), 0 by mid
//         swing (80). Fit: within 1.18, 1.43 of the cubic.
//  shoulder  Murray, Sepic & Barnard 1967 (free speed): -24 extension at the same side's heel
//         strike, 8 flexion half a cycle later. One harmonic.
//  elbow  Murray et al. 1967: 10 flexion with the shoulder extended, 30 with it flexed.
constexpr Series k_walk_hip{{11.7342, 17.3280, -3.9456, -3.7229, -1.4954, 0.1119, 0.9214, -0.3999,
                             -0.0197, -0.0221, 0.0443}};
constexpr Series k_walk_knee{{22.4917, -3.2693, -19.2852, -12.7453, 7.5349, -1.7644, 3.0607,
                              0.3502, 0.8622, -0.6470, -0.3118}};
constexpr Series k_walk_ankle{{-1.2217, -1.1718, 5.0401, 1.5110, -7.5538, -2.4880, 1.7525, 2.7479,
                               -0.4193, -0.5531, -0.4933}};
constexpr Series k_walk_shoulder{{-8.0, -15.7685, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}};
constexpr Series k_walk_elbow{{20.0, -9.8553, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}};

// Run (Novacheck 1998, the running condition at 3.2 m/s; toe-off at 39% of the cycle, so a flight
// phase from 39 to 50% and from 89 to 100%):
//  hip    40 at contact, 20 (20), -8 peak extension at toe-off (39), 20 (60), 55 peak flexion in
//         late swing (85). Fit: within 0.13.
//  knee   20 at contact, 42 stance peak (15), 18 at toe-off (39), 92 swing peak (70), 30 (92).
//         Fit: within 0.19.
//  ankle  3 at contact, 20 peak dorsiflexion (15), -22 peak plantarflexion after toe-off (45),
//         5 in swing (70). Fit: within 0.16.
//  shoulder -35 to 20; elbow held near 90 (75 to 105): the runner's bent arm.
constexpr Series k_run_hip{{25.0042, 20.8234, -18.6793, -3.5375, 1.4669, -2.4073, -1.4635, 0.3111,
                            0.3651, -0.0873, 0.1861}};
constexpr Series k_run_knee{{45.1600, -7.5809, -25.0716, -14.9024, 14.7166, -1.4829, 1.0529,
                             -0.5931, -0.1177, -0.6042, -0.5183}};
constexpr Series k_run_ankle{{0.5000, 12.4136, 0.9534, -7.5984, 7.1910, -0.9783, -1.3685, -1.0538,
                              -0.6136, -0.2956, -0.3686}};
constexpr Series k_run_shoulder{{-7.5, -27.1022, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}};
constexpr Series k_run_elbow{{90.0, -14.7830, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}};

f64 series(const Series& s, f64 phase) noexcept {
  f64 out = s.c[0];
  for (u32 k = 1; k <= 5; ++k) {
    const f64 w = k_two_pi * static_cast<f64>(k) * phase;
    out = out + s.c[2 * k - 1] * std::cos(w) + s.c[2 * k] * std::sin(w);
  }
  return out;
}

// A sinusoid of `harmonic` cycles a gait cycle, `amplitude` at its peak, the peak at `peak`.
f64 wave(f64 amplitude, u32 harmonic, f64 peak, f64 phase) noexcept {
  return amplitude * std::cos(k_two_pi * static_cast<f64>(harmonic) * (phase - peak));
}

// The pelvis and the trunk, one sinusoid each (the right side's sense).
//  Walk (Kadaba, Ramakrishnan & Wootten 1990; Perry & Burnfield 2010): rotation +-4 degrees, the
//  right side forward at its contact; obliquity +-4, the right side highest in its loading
//  response (17%), the swing side dropping; tilt +-1.5 about the mean, twice a cycle, anterior
//  most in terminal stance (45%, 95%); the thorax counter-rotates 3 degrees against the pelvis's 4
//  (Perry), so the trunk turns 7 relative to the pelvis, opposite. The pelvis rises +-2.5 cm, twice
//  a cycle, lowest in double support (5%, 55%) and highest in mid stance; it shifts +-2 cm toward
//  the stance limb, the right most at 30% (Perry; Saunders, Inman & Eberhart 1953).
//  Run (Novacheck 1998): rotation +-5, obliquity +-6 (highest at 12%), tilt +-2 (most anterior at
//  30%), trunk 10 relative; the pelvis lowest in mid stance (20%, 70%) and highest in flight, +-3.5
//  cm, and +-1 cm sideways.
f64 body_curve(GaitBand band, GaitCurve curve, f64 phase) noexcept {
  const bool run = band == GaitBand::Run;
  switch (curve) {
    case GaitCurve::PelvisRotation: return wave(run ? 5.0 : 4.0, 1, 0.0, phase);
    case GaitCurve::PelvisObliquity: return wave(run ? 6.0 : 4.0, 1, run ? 0.12 : 0.17, phase);
    case GaitCurve::PelvisTilt: return wave(run ? 2.0 : 1.5, 2, run ? 0.30 : 0.45, phase);
    case GaitCurve::TrunkRotation: return wave(run ? -10.0 : -7.0, 1, 0.0, phase);
    case GaitCurve::PelvisRise: return wave(run ? -0.035 : -0.025, 2, run ? 0.20 : 0.05, phase);
    case GaitCurve::PelvisLateral: return wave(run ? 0.01 : 0.02, 1, run ? 0.20 : 0.30, phase);
    default: return 0.0;
  }
}

// A rotation of `degrees` about the unit `axis`, built in double and rounded once.
Quat axis_degrees(Vec3 axis, f64 degrees) noexcept {
  const f64 h = 0.5 * degrees * k_deg;
  const f64 s = std::sin(h);
  return Quat{static_cast<f32>(static_cast<f64>(axis.x) * s),
              static_cast<f32>(static_cast<f64>(axis.y) * s),
              static_cast<f32>(static_cast<f64>(axis.z) * s), static_cast<f32>(std::cos(h))};
}

Vec3 column_position(const Mat4& m) noexcept { return Vec3{m.c[3].x, m.c[3].y, m.c[3].z}; }

using SJ = anim::StandardJoint;

constexpr SJ k_required[] = {SJ::Hips,         SJ::LeftUpperLeg,  SJ::LeftLowerLeg,
                             SJ::LeftFoot,     SJ::RightUpperLeg, SJ::RightLowerLeg,
                             SJ::RightFoot};

// The rig's rest pose in model space: positions, and rotations composed down the hierarchy.
struct Rest {
  Vector<Mat4> model;
  Vector<Quat> world;
};

void rest_of(const anim::Skeleton& skeleton, Rest& out) {
  anim::Pose rest;
  anim::rest_pose(skeleton, rest);
  const u32 n = skeleton.joint_count();
  out.model.resize(n);
  anim::local_to_model(skeleton, rest, std::span<Mat4>(out.model.data(), out.model.size()));
  out.world.resize(n);
  for (u32 j = 0; j < n; ++j) {
    const i32 p = skeleton.parents[j];
    const Quat local = skeleton.local_bind[j].rotation;
    out.world[j] = normalize(p >= 0 ? out.world[static_cast<u32>(p)] * local : local);
  }
}

struct Axes {
  Vec3 up{};
  Vec3 right{};
  Vec3 forward{};
  f32 leg_units = 0.0f;  // the hips' height above the feet along `up`, model units
};

bool axes_of(const Rest& rest, const anim::JointMapping& m, Axes& out) {
  const auto at = [&](SJ role) {
    return column_position(rest.model[static_cast<u32>(m[role])]);
  };
  const Vec3 hips = (at(SJ::LeftUpperLeg) + at(SJ::RightUpperLeg)) * 0.5f;
  const Vec3 feet = (at(SJ::LeftFoot) + at(SJ::RightFoot)) * 0.5f;
  const Vec3 rise = hips - feet;
  if (length(rise) <= 1.0e-6f) return false;
  out.up = normalize(rise);
  Vec3 across = at(SJ::RightUpperLeg) - at(SJ::LeftUpperLeg);
  across = across - out.up * dot(across, out.up);
  if (length(across) <= 1.0e-6f) return false;
  out.right = normalize(across);
  out.forward = cross(out.up, out.right);
  out.leg_units = dot(rise, out.up);
  return out.leg_units > 0.0f;
}

constexpr f64 k_leg_from_ankle = 0.530 / (0.530 - 0.039);  // Winter's proportions

struct Ranges {
  f32 lo;
  f32 hi;
};
constexpr Ranges k_walk_range{0.4f, 2.6f};
constexpr Ranges k_run_range{1.8f, 10.0f};

// The single-child chain from `joint`: it and its only child, and that one's, up to `limit`.
void chain_of(const Vector<Vector<u32>>& children, u32 joint, u32 limit, Vector<u32>& out) {
  out.clear();
  u32 at = joint;
  while (true) {
    out.push_back(at);
    if (out.size() >= limit || children[at].size() != 1) break;
    at = children[at][0];
  }
}

void set_role(anim::JointMapping& m, SJ role, u32 joint, u32& filled) {
  i32& slot = m.to_skeleton[static_cast<u32>(role)];
  if (slot >= 0) return;
  slot = static_cast<i32>(joint);
  ++filled;
  ++m.mapped;
}

}  // namespace

anim::JointMapping gait_mapping(const anim::Skeleton& skeleton, const anim::JointMapping& by_name,
                                u32* inferred) {
  anim::JointMapping m = by_name;
  while (m.to_skeleton.size() < anim::k_standard_joint_count)
    m.to_skeleton.push_back(anim::k_no_joint);
  u32 filled = 0;
  if (inferred != nullptr) *inferred = 0;
  std::string missing;
  if (gait_roles_present(m, missing)) return m;
  const u32 n = skeleton.joint_count();
  if (n == 0) return m;
  Rest rest;
  rest_of(skeleton, rest);
  Vector<Vector<u32>> children(n);
  for (u32 j = 0; j < n; ++j) {
    const i32 p = skeleton.parents[j];
    if (p >= 0) children[static_cast<u32>(p)].push_back(j);
  }
  const auto at = [&](u32 j) { return column_position(rest.model[j]); };
  Vector<u32> a;
  Vector<u32> b;
  Vector<u32> c;
  for (u32 hips = 0; hips < n; ++hips) {
    const Vector<u32>& kids = children[hips];
    if (kids.size() < 2) continue;
    for (u32 i = 0; i < kids.size(); ++i) {
      chain_of(children, kids[i], 4, a);
      if (a.size() < 3) continue;
      const Vec3 da = at(a[2]) - at(kids[i]);
      for (u32 k = i + 1; k < kids.size(); ++k) {
        chain_of(children, kids[k], 4, b);
        if (b.size() < 3) continue;
        const Vec3 db = at(b[2]) - at(kids[k]);
        const f32 la = length(da);
        const f32 lb = length(db);
        if (la <= 1.0e-6f || lb <= 1.0e-6f || dot(da, db) < 0.8f * la * lb) continue;
        const Vec3 down = normalize(da + db);
        // Away from a third child (the spine), or downward in model space with none.
        i32 spine = -1;
        for (u32 s = 0; s < kids.size(); ++s) {
          if (s == i || s == k) continue;
          chain_of(children, kids[s], 2, c);
          if (dot(at(c[c.size() - 1]) - at(hips), down) < 0.0f) {
            spine = static_cast<i32>(kids[s]);
            break;
          }
        }
        if (spine < 0 && down.y > -0.5f) continue;
        // The right leg is on the model's -x side.
        const bool a_right = at(kids[i]).x < at(kids[k]).x;
        const Vector<u32>& right = a_right ? a : b;
        const Vector<u32>& left = a_right ? b : a;
        set_role(m, SJ::Hips, hips, filled);
        set_role(m, SJ::RightUpperLeg, right[0], filled);
        set_role(m, SJ::RightLowerLeg, right[1], filled);
        set_role(m, SJ::RightFoot, right[2], filled);
        if (right.size() > 3) set_role(m, SJ::RightToes, right[3], filled);
        set_role(m, SJ::LeftUpperLeg, left[0], filled);
        set_role(m, SJ::LeftLowerLeg, left[1], filled);
        set_role(m, SJ::LeftFoot, left[2], filled);
        if (left.size() > 3) set_role(m, SJ::LeftToes, left[3], filled);
        if (spine >= 0) {
          chain_of(children, static_cast<u32>(spine), 8, c);
          set_role(m, SJ::Spine, c[0], filled);
          if (c.size() > 1) set_role(m, SJ::Chest, c[1], filled);
          // The branch at the chain's end: one side chain each side of it are the arms.
          const u32 branch = c[c.size() - 1];
          i32 arm_r = -1;
          i32 arm_l = -1;
          for (const u32 kid : children[branch]) {
            chain_of(children, kid, 3, a);
            if (a.size() < 2) continue;
            const f32 side = at(kid).x - at(branch).x;
            if (side < 0.0f && (arm_r < 0 || side < at(static_cast<u32>(arm_r)).x - at(branch).x))
              arm_r = static_cast<i32>(kid);
            if (side > 0.0f && (arm_l < 0 || side > at(static_cast<u32>(arm_l)).x - at(branch).x))
              arm_l = static_cast<i32>(kid);
          }
          if (arm_r >= 0 && arm_l >= 0 && children[branch].size() >= 3) {
            for (const i32 arm : {arm_r, arm_l}) {
              chain_of(children, static_cast<u32>(arm), 3, a);
              const bool r = arm == arm_r;
              set_role(m, r ? SJ::RightUpperArm : SJ::LeftUpperArm, a[0], filled);
              set_role(m, r ? SJ::RightLowerArm : SJ::LeftLowerArm, a[1], filled);
              if (a.size() > 2) set_role(m, r ? SJ::RightHand : SJ::LeftHand, a[2], filled);
            }
          }
        }
        if (inferred != nullptr) *inferred = filled;
        return m;
      }
    }
  }
  return m;
}

f64 gait_curve(GaitBand band, GaitCurve curve, f64 phase) noexcept {
  const bool run = band == GaitBand::Run;
  switch (curve) {
    case GaitCurve::Hip: return series(run ? k_run_hip : k_walk_hip, phase);
    case GaitCurve::Knee: return series(run ? k_run_knee : k_walk_knee, phase);
    case GaitCurve::Ankle: return series(run ? k_run_ankle : k_walk_ankle, phase);
    case GaitCurve::Shoulder: return series(run ? k_run_shoulder : k_walk_shoulder, phase);
    case GaitCurve::Elbow: return series(run ? k_run_elbow : k_walk_elbow, phase);
    default: return body_curve(band, curve, phase);
  }
}

f32 gait_stride_m(GaitBand band, f32 speed_mps, f32 leg_length_m) noexcept {
  constexpr f64 k_ref_leg = 0.93;     // 0.530 of a 1.75 m stature (Winter)
  const f64 leg = leg_length_m > 0.0f ? static_cast<f64>(leg_length_m) : k_ref_leg;
  if (band == GaitBand::Walk) {
    const f64 v = static_cast<f64>(speed_mps < k_walk_range.lo   ? k_walk_range.lo
                                   : speed_mps > k_walk_range.hi ? k_walk_range.hi
                                                                 : speed_mps);
    // Calibrated at Perry & Burnfield's free walk: 1.41 m at 1.37 m/s on the reference leg.
    constexpr f64 k_exponent = 0.42;  // Grieve 1968
    const f64 a = (1.41 / k_ref_leg) /
                  std::pow(1.37 / std::sqrt(k_gravity * k_ref_leg), k_exponent);
    return static_cast<f32>(a * leg * std::pow(v / std::sqrt(k_gravity * leg), k_exponent));
  }
  const f64 v = static_cast<f64>(speed_mps < k_run_range.lo   ? k_run_range.lo
                                 : speed_mps > k_run_range.hi ? k_run_range.hi
                                                              : speed_mps);
  const f64 frequency = 1.40 * std::sqrt(k_ref_leg / leg);  // Cavanagh & Kram 1989
  return static_cast<f32>(v / frequency);
}

bool gait_roles_present(const anim::JointMapping& mapping, std::string& missing) {
  missing.clear();
  for (const SJ role : k_required) {
    if (mapping[role] >= 0) continue;
    if (!missing.empty()) missing += ", ";
    missing += anim::standard_joint_name(role);
  }
  return missing.empty();
}

f32 gait_leg_length_m(const anim::Skeleton& skeleton, const anim::JointMapping& mapping,
                      f32 metres_per_unit) {
  std::string missing;
  if (!gait_roles_present(mapping, missing)) return 0.0f;
  Rest rest;
  rest_of(skeleton, rest);
  Axes axes;
  if (!axes_of(rest, mapping, axes)) return 0.0f;
  return static_cast<f32>(static_cast<f64>(axes.leg_units) * static_cast<f64>(metres_per_unit) *
                          k_leg_from_ankle);
}

bool generate_gait_clip(const SkeletonAsset& asset, const anim::JointMapping& mapping,
                        const GaitParams& params, ClipAsset& out, GaitCycle& cycle,
                        std::string& error) {
  const anim::Skeleton& skeleton = asset.skeleton;
  std::string missing;
  if (!gait_roles_present(mapping, missing)) {
    error = "the procedural gait needs the hips and both legs in the standard skeleton's roles; " +
            asset.name + " maps no " + missing;
    return false;
  }
  if (!(params.metres_per_unit > 0.0f) || !(params.speed_mps >= 0.0f)) {
    error = "the procedural gait needs a positive scale and a speed of 0 or more";
    return false;
  }
  Rest rest;
  rest_of(skeleton, rest);
  Axes axes;
  if (!axes_of(rest, mapping, axes)) {
    error = "the procedural gait cannot find " + asset.name +
            "'s body axes: its feet are at its hips' height, or its hips at one point";
    return false;
  }
  const GaitBand band = params.band;
  const bool run = band == GaitBand::Run;
  const f32 speed = params.speed_mps > 0.0f
                        ? params.speed_mps
                        : (run ? k_gait_run_reference_mps : k_gait_walk_reference_mps);
  const f32 leg = params.leg_length_m > 0.0f
                      ? params.leg_length_m
                      : static_cast<f32>(static_cast<f64>(axes.leg_units) *
                                         static_cast<f64>(params.metres_per_unit) *
                                         k_leg_from_ankle);
  cycle = GaitCycle{};
  cycle.band = band;
  cycle.speed_mps = speed;
  cycle.leg_length_m = leg;
  cycle.stride_m = gait_stride_m(band, speed, leg);
  const Ranges range = run ? k_run_range : k_walk_range;
  const f32 timed = speed < range.lo ? range.lo : speed > range.hi ? range.hi : speed;
  cycle.cycle_s = cycle.stride_m / timed;
  cycle.cadence_spm = 120.0f / cycle.cycle_s;
  cycle.stance = run ? 0.39f : 0.62f;
  const u32 segments = static_cast<u32>(std::lround(static_cast<f64>(cycle.cycle_s) *
                                                    static_cast<f64>(k_gait_key_hz)));
  const u32 n = segments < 8 ? 8u : segments;
  cycle.keys = n + 1;

  // The driven joints, each with how its body-frame rotation is built.
  enum class Kind : u8 { Pelvis, Trunk, Hip, Knee, Ankle, Shoulder, Elbow };
  struct Driven {
    u32 joint;
    Kind kind;
    f64 offset;  // 0 for the right side and the pelvis, 0.5 for the left
    f64 share;   // the trunk's share of its rotation
  };
  Vector<Driven> driven;
  const auto add = [&](SJ role, Kind kind, f64 offset, f64 share) {
    const i32 j = mapping[role];
    if (j >= 0) driven.push_back(Driven{static_cast<u32>(j), kind, offset, share});
  };
  add(SJ::Hips, Kind::Pelvis, 0.0, 1.0);
  const bool spine = mapping[SJ::Spine] >= 0;
  const bool chest = mapping[SJ::Chest] >= 0;
  const f64 trunk_share = spine && chest ? 0.5 : 1.0;
  add(SJ::Spine, Kind::Trunk, 0.0, trunk_share);
  add(SJ::Chest, Kind::Trunk, 0.0, trunk_share);
  add(SJ::RightUpperLeg, Kind::Hip, 0.0, 1.0);
  add(SJ::RightLowerLeg, Kind::Knee, 0.0, 1.0);
  add(SJ::RightFoot, Kind::Ankle, 0.0, 1.0);
  add(SJ::LeftUpperLeg, Kind::Hip, 0.5, 1.0);
  add(SJ::LeftLowerLeg, Kind::Knee, 0.5, 1.0);
  add(SJ::LeftFoot, Kind::Ankle, 0.5, 1.0);
  add(SJ::RightUpperArm, Kind::Shoulder, 0.0, 1.0);
  add(SJ::RightLowerArm, Kind::Elbow, 0.0, 1.0);
  add(SJ::LeftUpperArm, Kind::Shoulder, 0.5, 1.0);
  add(SJ::LeftLowerArm, Kind::Elbow, 0.5, 1.0);
  cycle.joints_driven = driven.size();

  out.clip = anim::Clip{};
  out.clip.name = out.name;
  out.clip.joint_count = skeleton.joint_count();
  Vector<f32> times(n + 1);
  for (u32 k = 0; k <= n; ++k)
    times[k] = k == n ? cycle.cycle_s
                      : static_cast<f32>(static_cast<f64>(cycle.cycle_s) * static_cast<f64>(k) /
                                         static_cast<f64>(n));
  Vector<f32> values;
  for (const Driven& d : driven) {
    const Quat w = rest.world[d.joint];
    const Quat w_inverse = conjugate(w);
    const Quat local_rest = skeleton.local_bind[d.joint].rotation;
    values.clear();
    Quat previous = local_rest;
    for (u32 k = 0; k <= n; ++k) {
      Quat q = local_rest;
      if (k == n) {
        q = Quat{values[0], values[1], values[2], values[3]};  // the loop closes exactly
      } else {
        f64 phase = static_cast<f64>(k) / static_cast<f64>(n) + d.offset;
        if (phase >= 1.0) phase = phase - 1.0;
        Quat body = Quat::identity();
        switch (d.kind) {
          case Kind::Pelvis:
            body = axis_degrees(axes.up, gait_curve(band, GaitCurve::PelvisRotation, phase)) *
                   axis_degrees(axes.forward,
                                -gait_curve(band, GaitCurve::PelvisObliquity, phase)) *
                   axis_degrees(axes.right, -gait_curve(band, GaitCurve::PelvisTilt, phase));
            break;
          case Kind::Trunk:
            body = axis_degrees(axes.up,
                                d.share * gait_curve(band, GaitCurve::TrunkRotation, phase));
            break;
          case Kind::Hip: body = axis_degrees(axes.right, gait_curve(band, GaitCurve::Hip, phase));
            break;
          case Kind::Knee:
            body = axis_degrees(axes.right, -gait_curve(band, GaitCurve::Knee, phase));
            break;
          case Kind::Ankle:
            body = axis_degrees(axes.right, gait_curve(band, GaitCurve::Ankle, phase));
            break;
          case Kind::Shoulder:
            body = axis_degrees(axes.right, gait_curve(band, GaitCurve::Shoulder, phase));
            break;
          case Kind::Elbow:
            body = axis_degrees(axes.right, gait_curve(band, GaitCurve::Elbow, phase));
            break;
        }
        // The body-frame turn in the joint's rest frame, after its rest rotation.
        q = normalize(local_rest * (w_inverse * body * w));
        if (dot(q, previous) < 0.0f) q = -q;  // one hemisphere along the track
      }
      previous = q;
      values.push_back(q.x);
      values.push_back(q.y);
      values.push_back(q.z);
      values.push_back(q.w);
    }
    if (!out.clip.add_track(d.joint, anim::k_channel_rotation, anim::k_interp_linear, 4,
                            std::span<const f32>(times.data(), times.size()),
                            std::span<const f32>(values.data(), values.size()))) {
      error = "the procedural gait built a track the clip refused";
      return false;
    }
  }

  // The pelvis's excursion, in the hips' parent frame, model units.
  const u32 hips = static_cast<u32>(mapping[SJ::Hips]);
  const i32 parent = skeleton.parents[hips];
  const Mat4 to_parent = parent >= 0 ? inverse(rest.model[static_cast<u32>(parent)]) : Mat4{};
  const Vec3 rest_t = skeleton.local_bind[hips].position;
  values.clear();
  for (u32 k = 0; k <= n; ++k) {
    const f64 phase = k == n ? 0.0 : static_cast<f64>(k) / static_cast<f64>(n);
    const f64 per_unit = 1.0 / static_cast<f64>(params.metres_per_unit);
    const f32 rise = static_cast<f32>(gait_curve(band, GaitCurve::PelvisRise, phase) * per_unit);
    const f32 side = static_cast<f32>(gait_curve(band, GaitCurve::PelvisLateral, phase) * per_unit);
    const Vec3 model = axes.up * rise + axes.right * side;
    const Vec4 local = to_parent * Vec4{model.x, model.y, model.z, 0.0f};
    values.push_back(rest_t.x + local.x);
    values.push_back(rest_t.y + local.y);
    values.push_back(rest_t.z + local.z);
  }
  if (!out.clip.add_track(hips, anim::k_channel_translation, anim::k_interp_linear, 3,
                          std::span<const f32>(times.data(), times.size()),
                          std::span<const f32>(values.data(), values.size()))) {
    error = "the procedural gait built a track the clip refused";
    return false;
  }
  return out.clip.validate(&error);
}

}  // namespace engine::animation
