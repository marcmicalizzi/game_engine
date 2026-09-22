// The hot functions of `core/math`, one row each, so that a baseline change or a rewrite of any
// of them has a number to move before it reaches a module that uses it.
//
// **Why this file exists.** [ADR-0031](../../../docs/adr/0031-minimum-cpu-x86-64-v3.md) moved the
// tree to x86-64-v3 and `domain/anim`'s bench then found `mat4_from_transform` **77% slower** at
// v3 than at v2 under MSVC (docs/subsystems/anim.md). This module had no bench, so that was found
// from two modules away, by subtraction. Every function here is inline and used by nearly every
// module in the tree; a codegen regression in one of them is paid everywhere and shows up nowhere
// in particular.
//
// **The shape of every row.** A batch of `k_count` independent inputs, laid out the way a caller
// that loops over them lays them out, written to an output array, with the batch the unit an
// iteration measures (`set_items(k_count)`, so the table's per-item column is per call). The
// inputs are a fixed function of the index — no `<random>`, whose distributions are not specified
// to agree between standard libraries — so every build starts from the same bits and a
// bit-for-bit comparison between two builds of this file means something. 64 elements keep every
// array in L1 (the largest, 64 `Mat4`s, is 4 KiB), because what these rows are for is codegen:
// a store-forwarding stall or a wider copy shows undiluted there, and a cache miss would drown it.
//
// Two rows are *chains* rather than batches — each call reads what the previous one wrote — since
// a forward pass over a hierarchy is how `Mat4` products and `compose` are really used
// (`anim::local_to_model` is one), and a latency problem hides in a batch of independent calls.
//
// Numbers, the machine's state they were taken in, and what the rows found:
// docs/subsystems/math.md.
#include <core/base/types.h>
#include <core/math/math.h>
#include <foundation/bench/bench.h>

using namespace engine;

namespace {

constexpr u32 k_count = 64;

// A rotation that is not the identity and not axis-aligned, so no product in the expansion is a
// multiply by zero or one the compiler could see through if it ever inlined far enough.
Quat quat_at(u32 i) noexcept {
  const f32 t = static_cast<f32>(i) * 0.37f;
  return normalize(Quat{0.1f + 0.03f * t, 0.2f - 0.01f * t, 0.05f * t, 1.0f});
}
Vec3 vec_at(u32 i) noexcept {
  const f32 t = static_cast<f32>(i) * 0.11f;
  return Vec3{0.05f * t - 1.0f, 0.25f + 0.01f * t, -0.03f * t + 0.5f};
}
Vec3 scale_at(u32 i) noexcept {
  const f32 t = static_cast<f32>(i) * 0.013f;
  return Vec3{1.0f + t, 1.0f - 0.5f * t, 1.0f + 0.25f * t};
}
Transform3 transform_at(u32 i) noexcept { return {vec_at(i), quat_at(i), scale_at(i)}; }

// The inputs, built once. Both layouts of a transform are here: an array of `Transform3` (AoS),
// and three parallel arrays (SoA) the way `anim::Pose` keeps them, which is the shape the
// regression was found in — the caller assembles a `Transform3` from three streams per call.
struct Inputs {
  Transform3 transforms[k_count];
  Vec3 translation[k_count];
  Quat rotation[k_count];
  Vec3 scale[k_count];
  Quat quats[k_count];
  Quat quats_b[k_count];
  Vec3 points[k_count];
  Mat4 matrices[k_count];
  Mat4 matrices_b[k_count];
  Aabb3 boxes[k_count];

  Inputs() {
    for (u32 i = 0; i < k_count; ++i) {
      transforms[i] = transform_at(i);
      translation[i] = transforms[i].position;
      rotation[i] = transforms[i].rotation;
      scale[i] = transforms[i].scale;
      quats[i] = quat_at(i);
      quats_b[i] = quat_at(i + 101);
      points[i] = vec_at(i + 7);
      matrices[i] = mat4_from_transform(transform_at(i + 13));
      matrices_b[i] = mat4_from_transform(transform_at(i + 29));
      const Vec3 c = vec_at(i + 3);
      boxes[i] = Aabb3{c - Vec3{0.5f, 0.25f, 0.75f}, c + Vec3{0.5f, 0.25f, 0.75f}};
    }
  }
};

const Inputs& inputs() {
  static const Inputs in;
  return in;
}

// Outputs are per-row statics rather than stack arrays so that the stores are real stores to
// memory the optimizer cannot prove dead — the same memory a caller writes into.
template <class T>
struct Out {
  T values[k_count];
};

}  // namespace

// --- transform to matrix: the regression's row, in both layouts -------------------------------

// The shape `anim::local_to_model` has and the one ADR-0031's follow-up measured: three parallel
// streams, a `Transform3` assembled per call, one `Mat4` stored per call.
ENGINE_BENCH(math_mat4_from_transform_soa, "math.mat4_from_transform.soa") {
  const Inputs& in = inputs();
  static Out<Mat4> out;
  while (state.keep_running()) {
    for (u32 i = 0; i < k_count; ++i)
      out.values[i] =
          mat4_from_transform(Transform3{in.translation[i], in.rotation[i], in.scale[i]});
    bench::keep(out.values[k_count - 1]);
  }
  state.set_items(k_count);
}

// The same from an array of `Transform3`s: a scene's instances, a physics body's pose.
ENGINE_BENCH(math_mat4_from_transform_aos, "math.mat4_from_transform.aos") {
  const Inputs& in = inputs();
  static Out<Mat4> out;
  while (state.keep_running()) {
    for (u32 i = 0; i < k_count; ++i)
      out.values[i] = mat4_from_transform(in.transforms[i]);
    bench::keep(out.values[k_count - 1]);
  }
  state.set_items(k_count);
}

ENGINE_BENCH(math_mat4_from_quat, "math.mat4_from_quat") {
  const Inputs& in = inputs();
  static Out<Mat4> out;
  while (state.keep_running()) {
    for (u32 i = 0; i < k_count; ++i)
      out.values[i] = mat4_from_quat(in.quats[i]);
    bench::keep(out.values[k_count - 1]);
  }
  state.set_items(k_count);
}

ENGINE_BENCH(math_mat3_from_quat, "math.mat3_from_quat") {
  const Inputs& in = inputs();
  static Out<Mat3> out;
  while (state.keep_running()) {
    for (u32 i = 0; i < k_count; ++i)
      out.values[i] = mat3_from_quat(in.quats[i]);
    bench::keep(out.values[k_count - 1]);
  }
  state.set_items(k_count);
}

// --- matrix products -----------------------------------------------------------------------------

ENGINE_BENCH(math_mat4_mul_batch, "math.mat4_mul.batch") {
  const Inputs& in = inputs();
  static Out<Mat4> out;
  while (state.keep_running()) {
    for (u32 i = 0; i < k_count; ++i)
      out.values[i] = in.matrices[i] * in.matrices_b[i];
    bench::keep(out.values[k_count - 1]);
  }
  state.set_items(k_count);
}

// Each product reads the one before it, through memory, the way a hierarchy's forward pass does.
ENGINE_BENCH(math_mat4_mul_chain, "math.mat4_mul.chain") {
  const Inputs& in = inputs();
  static Out<Mat4> out;
  while (state.keep_running()) {
    out.values[0] = in.matrices[0];
    for (u32 i = 1; i < k_count; ++i)
      out.values[i] = out.values[i - 1] * in.matrices_b[i];
    bench::keep(out.values[k_count - 1]);
  }
  state.set_items(k_count - 1);
}

ENGINE_BENCH(math_mat4_inverse, "math.mat4_inverse") {
  const Inputs& in = inputs();
  static Out<Mat4> out;
  while (state.keep_running()) {
    for (u32 i = 0; i < k_count; ++i)
      out.values[i] = inverse(in.matrices[i]);
    bench::keep(out.values[k_count - 1]);
  }
  state.set_items(k_count);
}

ENGINE_BENCH(math_transform_point_mat4, "math.transform_point.mat4") {
  const Inputs& in = inputs();
  static Out<Vec3> out;
  const Mat4& m = in.matrices[5];
  while (state.keep_running()) {
    for (u32 i = 0; i < k_count; ++i)
      out.values[i] = transform_point(m, in.points[i]);
    bench::keep(out.values[k_count - 1]);
  }
  state.set_items(k_count);
}

ENGINE_BENCH(math_transform_aabb, "math.transform_aabb") {
  const Inputs& in = inputs();
  static Out<Aabb3> out;
  const Mat4& m = in.matrices[9];
  while (state.keep_running()) {
    for (u32 i = 0; i < k_count; ++i)
      out.values[i] = transform_aabb(m, in.boxes[i]);
    bench::keep(out.values[k_count - 1]);
  }
  state.set_items(k_count);
}

// --- transforms ----------------------------------------------------------------------------------

ENGINE_BENCH(math_compose_batch, "math.compose.batch") {
  const Inputs& in = inputs();
  static Out<Transform3> out;
  while (state.keep_running()) {
    for (u32 i = 0; i < k_count; ++i)
      out.values[i] = compose(in.transforms[i], in.transforms[(i + 17) % k_count]);
    bench::keep(out.values[k_count - 1]);
  }
  state.set_items(k_count);
}

ENGINE_BENCH(math_compose_chain, "math.compose.chain") {
  const Inputs& in = inputs();
  static Out<Transform3> out;
  while (state.keep_running()) {
    out.values[0] = in.transforms[0];
    for (u32 i = 1; i < k_count; ++i)
      out.values[i] = compose(out.values[i - 1], in.transforms[i]);
    bench::keep(out.values[k_count - 1]);
  }
  state.set_items(k_count - 1);
}

ENGINE_BENCH(math_transform_inverse, "math.transform_inverse") {
  const Inputs& in = inputs();
  static Out<Transform3> out;
  while (state.keep_running()) {
    for (u32 i = 0; i < k_count; ++i)
      out.values[i] = inverse(in.transforms[i]);
    bench::keep(out.values[k_count - 1]);
  }
  state.set_items(k_count);
}

ENGINE_BENCH(math_transform_point_transform3, "math.transform_point.transform3") {
  const Inputs& in = inputs();
  static Out<Vec3> out;
  const Transform3& t = in.transforms[11];
  while (state.keep_running()) {
    for (u32 i = 0; i < k_count; ++i)
      out.values[i] = transform_point(t, in.points[i]);
    bench::keep(out.values[k_count - 1]);
  }
  state.set_items(k_count);
}

// --- quaternions ---------------------------------------------------------------------------------

ENGINE_BENCH(math_quat_mul, "math.quat.mul") {
  const Inputs& in = inputs();
  static Out<Quat> out;
  while (state.keep_running()) {
    for (u32 i = 0; i < k_count; ++i)
      out.values[i] = in.quats[i] * in.quats_b[i];
    bench::keep(out.values[k_count - 1]);
  }
  state.set_items(k_count);
}

ENGINE_BENCH(math_quat_rotate, "math.quat.rotate") {
  const Inputs& in = inputs();
  static Out<Vec3> out;
  while (state.keep_running()) {
    for (u32 i = 0; i < k_count; ++i)
      out.values[i] = rotate(in.quats[i], in.points[i]);
    bench::keep(out.values[k_count - 1]);
  }
  state.set_items(k_count);
}

ENGINE_BENCH(math_quat_normalize, "math.quat.normalize") {
  const Inputs& in = inputs();
  static Out<Quat> out;
  while (state.keep_running()) {
    for (u32 i = 0; i < k_count; ++i)
      out.values[i] = normalize(in.quats[i] * 1.5f);
    bench::keep(out.values[k_count - 1]);
  }
  state.set_items(k_count);
}

// The sampler's inner call: `anim::Clip::sample` slerps once per rotation track per key pair.
ENGINE_BENCH(math_quat_slerp, "math.quat.slerp") {
  const Inputs& in = inputs();
  static Out<Quat> out;
  while (state.keep_running()) {
    for (u32 i = 0; i < k_count; ++i)
      out.values[i] = slerp(in.quats[i], in.quats_b[i], 0.3f);
    bench::keep(out.values[k_count - 1]);
  }
  state.set_items(k_count);
}
