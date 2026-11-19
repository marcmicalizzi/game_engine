// Inertialization, S35 stage 4 (docs/subsystems/animation.md, "Graphs: inertialization"; the
// curve's sources and derivation: docs/experiments/animation-graphs.md, "Stage 4's curve, read and
// derived"): the curve's end conditions and its guard, a transition between two moving poses
// continuous in value and velocity at its start and the target's to the bit at its end, one
// interrupted by a second, and the slot arena allocating nothing in steady state.
//
// Tolerances. The sources state none for continuity (Bollo's talk matches the value and the
// velocity exactly in real arithmetic); the ones below are the f32 rounding of the poses and,
// for a velocity, the finite difference's own error over the step it is taken across, each said
// where it is used.
#include <core/hash/hash.h>
#include <core/math/portable_trig.h>
#include <core/memory/memory.h>
#include <systems/animation/inertialize.h>

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace engine;
using namespace engine::animation;

namespace {

u64 next_random(u64& s) {
  s ^= s << 13;
  s ^= s >> 7;
  s ^= s << 17;
  return s;
}
f32 uniform(u64& s, f32 lo, f32 hi) {
  return lo + (hi - lo) * static_cast<f32>(next_random(s) >> 40) / 16777216.0f;
}

Quat about_z(f32 angle) {
  f32 s = 0.0f;
  f32 c = 1.0f;
  portable_sin_cos(angle * 0.5f, s, c);
  return Quat{0.0f, 0.0f, s, c};
}
// A quaternion's angle about +z (the poses below turn about +z alone).
f32 angle_z(Quat q) { return 2.0f * portable_atan2(q.z, q.w); }

// A pose of `n` joints moving linearly: joint j at p + v t (+ j along y), turning about +z at
// `w` from `a`.
struct Motion {
  Vec3 p;
  Vec3 v;
  f32 a = 0.0f;
  f32 w = 0.0f;
  void at(f32 t, anim::Pose& out) const {
    for (u32 j = 0; j < out.joint_count(); ++j) {
      out.translation[j] = p + v * t + Vec3{0.0f, static_cast<f32>(j), 0.0f};
      out.rotation[j] = about_z(a + w * t);
      out.scale[j] = Vec3{1.0f, 1.0f, 1.0f};
    }
  }
};

bool same_bits(const anim::Pose& a, const anim::Pose& b) {
  for (u32 j = 0; j < a.joint_count(); ++j) {
    if (std::bit_cast<u32>(a.translation[j].x) != std::bit_cast<u32>(b.translation[j].x) ||
        std::bit_cast<u32>(a.translation[j].y) != std::bit_cast<u32>(b.translation[j].y) ||
        std::bit_cast<u32>(a.translation[j].z) != std::bit_cast<u32>(b.translation[j].z) ||
        !(a.rotation[j] == b.rotation[j]))
      return false;
  }
  return true;
}

constexpr f32 k_dt = 1.0f / 64.0f;
constexpr u32 k_joints = 4;

// What the transition shows `tau` seconds after its tick: the target then, plus the offsets.
void shown(const Motion& target, f32 t0, f32 tau, std::span<const InertialJoint> offsets,
           f32 duration, anim::Pose& out) {
  target.at(t0 + tau, out);
  inertial_apply(offsets, tau, duration, anim::PoseView(out));
}

}  // namespace

TEST_CASE("inertialization: the curve starts at the offset with its velocity and ends at rest") {
  struct Case {
    f32 x0, v0, t1;
  };
  // Toward zero within the guard, at rest, and a negative offset (the mirror).
  for (const Case c : {Case{0.3f, -0.5f, 0.2f}, Case{1.0f, 0.0f, 0.2f}, Case{2.0f, -40.0f, 0.4f},
                       Case{-0.25f, 1.5f, 0.3f}, Case{0.05f, -0.2f, 0.25f}}) {
    CAPTURE(c.x0);
    CAPTURE(c.v0);
    CHECK(inertial_curve(c.x0, c.v0, 0.0f, c.t1) == c.x0);  // exactly the offset
    // Its velocity: a forward difference over h has an error of h/2 times the acceleration at
    // the start, which the curve keeps at most 20 x0 / t1^2 (the zero-jerk rule's bound).
    const f32 h = c.t1 * 1.0e-4f;
    const f32 fd = (inertial_curve(c.x0, c.v0, h, c.t1) - c.x0) / h;
    const f32 accel = 20.0f * std::fabs(c.x0) / (c.t1 * c.t1);
    CHECK(std::fabs(fd - c.v0) <= h * accel + 1.0e-3f * (std::fabs(c.v0) + std::fabs(c.x0) / c.t1));
    // At and past its duration, exactly zero; just before it, at rest to third order.
    CHECK(inertial_curve(c.x0, c.v0, c.t1, c.t1) == 0.0f);
    CHECK(inertial_curve(c.x0, c.v0, 2.0f * c.t1, c.t1) == 0.0f);
    const f32 near = inertial_curve(c.x0, c.v0, 0.99f * c.t1, c.t1);
    CHECK(std::fabs(near) <= 2.0e-5f * (std::fabs(c.x0) + std::fabs(c.v0) * c.t1));
  }
  // What is not a curve is zero, never a NaN.
  CHECK(inertial_curve(1.0f, 0.0f, 0.1f, 0.0f) == 0.0f);
  CHECK(inertial_curve(std::nanf(""), 0.0f, 0.1f, 0.2f) == 0.0f);
  CHECK(inertial_curve(1.0f, 0.0f, std::nanf(""), 0.2f) == 0.0f);
}

TEST_CASE("inertialization: the curve never crosses zero, over a sweep of offsets and speeds") {
  // The guard's promise (the header's three rules): whatever the offset, the velocity and the
  // duration, the offset never passes the target — it keeps its sign or is zero — and never
  // grows past where it started.
  u64 seed = 0x5d35'0004'0001ull;
  u32 shortened = 0;
  u32 dropped = 0;
  for (u32 i = 0; i < 20000; ++i) {
    const f32 x0 = uniform(seed, -2.0f, 2.0f);
    const f32 v0 = uniform(seed, -60.0f, 60.0f);
    const f32 t1 = uniform(seed, 0.02f, 1.0f);
    shortened += (x0 * v0 < 0.0f && -5.0f * x0 / v0 < t1) ? 1u : 0u;
    dropped += x0 * v0 > 0.0f ? 1u : 0u;
    for (u32 k = 0; k <= 64; ++k) {
      const f32 x = inertial_curve(x0, v0, t1 * static_cast<f32>(k) / 64.0f, t1);
      CHECK_MESSAGE(x * x0 >= 0.0f, "x0 " << x0 << " v0 " << v0 << " t1 " << t1 << " k " << k);
      CHECK_MESSAGE(std::fabs(x) <= std::fabs(x0), "x0 " << x0 << " v0 " << v0 << " t1 " << t1);
    }
  }
  // Both rules were exercised, many times.
  CHECK(shortened > 1000);
  CHECK(dropped > 1000);
}

TEST_CASE("inertialization: a transition is continuous in value and velocity and lands exactly") {
  // The source slides toward the target and turns toward it (a relative velocity toward zero,
  // inside the guard), so the curve takes the source's velocity on unchanged.
  const Motion source{
      {0.8f, 0.0f, 0.0f}, {-0.5f, 0.1f, 0.0f}, 2.5f, -2.0f};  // at t0: 0.3 m, 0.5 rad
  const Motion target{{0.0f, 0.0f, 0.0f}, {0.0f, 0.1f, 0.0f}, 0.0f, 0.0f};
  const f32 t0 = 1.0f;
  const f32 duration = 0.2f;
  anim::Pose s0, s1, d0, d1, out, next;
  for (anim::Pose* p : {&s0, &s1, &d0, &d1, &out, &next})
    p->resize(k_joints);
  source.at(t0, s0);
  source.at(t0 - k_dt, s1);
  target.at(t0, d0);
  target.at(t0 - k_dt, d1);
  std::vector<InertialJoint> offsets(k_joints);
  inertial_capture(s0, s1, d0, d1, k_dt, offsets);

  // At its tick it shows the source: to the f32 rounding of the poses (an ulp or two of 1 m).
  shown(target, t0, 0.0f, offsets, duration, out);
  for (u32 j = 0; j < k_joints; ++j) {
    CHECK(std::fabs(out.translation[j].x - s0.translation[j].x) <= 1.0e-6f);
    CHECK(std::fabs(out.translation[j].y - s0.translation[j].y) <= 1.0e-6f);
    CHECK(std::fabs(angle_z(out.rotation[j]) - angle_z(s0.rotation[j])) <= 1.0e-6f);
  }
  // Its velocity just after the tick is the source's: a forward difference over h = 1 ms, whose
  // error is h/2 times the start's acceleration (zero here: the zero-jerk rule's a0 is negative,
  // so 0) plus the poses' rounding over h (1e-6 m / 1e-3 s).
  const f32 h = 1.0e-3f;
  shown(target, t0, h, offsets, duration, next);
  for (u32 j = 0; j < k_joints; ++j) {
    CHECK(std::fabs((next.translation[j].x - out.translation[j].x) / h - source.v.x) <= 2.0e-3f);
    CHECK(std::fabs((next.translation[j].y - out.translation[j].y) / h - source.v.y) <= 2.0e-3f);
    const f32 w = (angle_z(next.rotation[j]) - angle_z(out.rotation[j])) / h;
    CHECK(std::fabs(w - source.w) <= 2.0e-3f);
  }
  // At its duration and after it, the target's pose to the bit; just before it, within a
  // millimetre and a milliradian.
  anim::Pose exact;
  exact.resize(k_joints);
  for (const f32 tau : {duration, duration + k_dt}) {
    shown(target, t0, tau, offsets, duration, out);
    target.at(t0 + tau, exact);
    CHECK(same_bits(out, exact));
  }
  // One tick before it, within what the curve's factorization allows there (a0 = 0 here):
  // (1 - s)^3 (10 x0 + 4 |v0| t1) at s = (t1 - dt) / t1 — 1.6 mm and 3.1 mrad for these two.
  shown(target, t0, duration - k_dt, offsets, duration, out);
  target.at(t0 + duration - k_dt, exact);
  const f32 rest = 1.0f - (duration - k_dt) / duration;
  const f32 rest3 = rest * rest * rest;
  CHECK(std::fabs(out.translation[0].x - exact.translation[0].x) <=
        rest3 * (10.0f * 0.3f + 4.0f * 0.5f * duration));
  CHECK(std::fabs(angle_z(out.rotation[0]) - angle_z(exact.rotation[0])) <=
        rest3 * (10.0f * 0.5f + 4.0f * 2.0f * duration));
}

TEST_CASE("inertialization: a transition interrupted by a second carries its offset on") {
  // A first transition toward `a`, interrupted halfway by a second toward `b`: the second's
  // source is what the first showed (its offset included), so the picture does not jump.
  const Motion source{
      {0.4f, 0.0f, 0.0f}, {-0.6f, 0.0f, 0.0f}, 1.0f, -1.5f};  // at t0: 0.1 m, 0.25 rad
  const Motion a{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f};
  const Motion b{{-0.1f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, -0.2f, 0.0f};
  const f32 t0 = 0.5f;
  const f32 duration = 0.25f;
  anim::Pose s0, s1, d0, d1, before, before_prev, after, after_next;
  for (anim::Pose* p : {&s0, &s1, &d0, &d1, &before, &before_prev, &after, &after_next})
    p->resize(k_joints);
  source.at(t0, s0);
  source.at(t0 - k_dt, s1);
  a.at(t0, d0);
  a.at(t0 - k_dt, d1);
  std::vector<InertialJoint> first(k_joints);
  inertial_capture(s0, s1, d0, d1, k_dt, first);

  const f32 tau = 8.0f * k_dt;  // the second transition's tick, halfway through the first
  shown(a, t0, tau, first, duration, before);
  shown(a, t0, tau - k_dt, first, duration, before_prev);
  b.at(t0 + tau, d0);
  b.at(t0 + tau - k_dt, d1);
  std::vector<InertialJoint> second(k_joints);
  inertial_capture(before, before_prev, d0, d1, k_dt, second);
  shown(b, t0 + tau, 0.0f, second, duration, after);
  for (u32 j = 0; j < k_joints; ++j) {
    CHECK(std::fabs(after.translation[j].x - before.translation[j].x) <= 1.0e-6f);
    CHECK(std::fabs(angle_z(after.rotation[j]) - angle_z(before.rotation[j])) <= 1.0e-6f);
  }
  // Its velocity: the second's start velocity is the first's backward difference over the tick
  // (what was shown), which differs from the first's own velocity at `tau` by about half a tick
  // of its deceleration; held to that, a tenth of the speed.
  shown(b, t0 + tau, 1.0e-3f, second, duration, after_next);
  const f32 v_before = (before.translation[0].x - before_prev.translation[0].x) / k_dt;
  const f32 v_after = (after_next.translation[0].x - after.translation[0].x) / 1.0e-3f;
  CHECK(std::fabs(v_after - v_before) <= 0.1f * std::fabs(v_before) + 1.0e-3f);
  // And it lands on `b` exactly.
  anim::Pose exact;
  exact.resize(k_joints);
  shown(b, t0 + tau, duration, second, duration, after);
  b.at(t0 + tau + duration, exact);
  CHECK(same_bits(after, exact));
}

TEST_CASE("inertialization: the curve and the offsets are pinned (MSVC; GCC and Clang reproduce)") {
  // Portable to the bit by construction (`+ - * /`, sqrt and the portable trigonometry); the
  // hash of a sweep of curves and a transition's poses, taken on MSVC on 2026-11-19.
  u64 h = k_hash_seed;
  u64 seed = 0x5d35'0004'0002ull;
  for (u32 i = 0; i < 4096; ++i) {
    const f32 x0 = uniform(seed, -2.0f, 2.0f);
    const f32 v0 = uniform(seed, -30.0f, 30.0f);
    const f32 t1 = uniform(seed, 0.05f, 0.5f);
    const f32 t = uniform(seed, 0.0f, 0.5f);
    h = hash_combine(h, std::bit_cast<u32>(inertial_curve(x0, v0, t, t1)));
  }
  const Motion source{{0.3f, 0.2f, -0.1f}, {-0.5f, 0.1f, 0.3f}, 0.9f, -2.5f};
  const Motion target{{0.0f, 0.1f, 0.0f}, {0.2f, 0.1f, 0.0f}, -0.3f, 0.5f};
  anim::Pose s0, s1, d0, d1, out;
  for (anim::Pose* p : {&s0, &s1, &d0, &d1, &out})
    p->resize(k_joints);
  source.at(1.0f, s0);
  source.at(1.0f - k_dt, s1);
  target.at(1.0f, d0);
  target.at(1.0f - k_dt, d1);
  std::vector<InertialJoint> offsets(k_joints);
  inertial_capture(s0, s1, d0, d1, k_dt, offsets);
  h = hash_bytes(offsets.data(), offsets.size() * sizeof(InertialJoint), h);
  for (u32 k = 0; k < 16; ++k) {
    shown(target, 1.0f, static_cast<f32>(k) * k_dt, offsets, 0.2f, out);
    h = hash_bytes(out.translation.data(), sizeof(Vec3) * k_joints, h);
    h = hash_bytes(out.rotation.data(), sizeof(Quat) * k_joints, h);
  }
  constexpr u64 k_pinned = 0x51889d72b6e7daedull;
  if (h != k_pinned) std::printf("inertialization hash: 0x%016llx\n", (unsigned long long)h);
  CHECK(h == k_pinned);
}

TEST_CASE("inertialization: the slot arena allocates nothing in steady state") {
  InertialArena arena(23);
  CHECK(arena.bytes() == 0);  // nothing until a transition holds a slot
  // A crowd whose changes overlap, at most 64 in flight: after the first round has grown the
  // arena to its most, further rounds reuse its slots and allocate nothing.
  std::vector<u32> held;
  const auto round = [&] {
    for (u32 i = 0; i < 64; ++i)
      held.push_back(arena.acquire());
    for (u32 i = 0; i < 64; ++i) {
      arena.release(held.back());
      held.pop_back();
    }
  };
  held.reserve(64);
  round();
  const usize bytes = arena.bytes();
  CHECK(arena.held() == 0);
  CHECK(bytes >= 64u * 23u * sizeof(InertialJoint));
  const u64 before = mem::allocations_on_thread();
  for (u32 r = 0; r < 100; ++r)
    round();
  CHECK(mem::allocations_on_thread() == before);
  CHECK(arena.bytes() == bytes);
  // Last in, first out: the slot just released is the next one handed out.
  const u32 x = arena.acquire();
  arena.release(x);
  CHECK(arena.acquire() == x);
  CHECK(arena.slot(x).size() == 23);
}
