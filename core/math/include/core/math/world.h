#pragma once

// World positions (ADR-0053, docs/subsystems/math.md "World positions").
//
// **A position in the world is three f64 and it is its own type**, so that an absolute position
// cannot reach a float32 by accident: at 420 km a float steps by 3.1 cm, and an engine that kept
// absolute positions in floats walked its player down the wrong line there
// (docs/experiments/far-from-origin-2026-10-04.md). The rules the types hold:
//
//   - `WorldPos` is a point, `DVec3` a displacement. Point - point = displacement,
//     point +- displacement = point, and two points do not add.
//   - Nothing narrows to float32 implicitly. `narrow(DVec3)` and `relative(point, origin)` are the
//     two ways down, and both are spelled at the call.
//   - Float32 (`Vec3`) is for local frames: mesh, bone, emitter, body, eye.
//
// **The GPU never sees an absolute position.** What it stores across frames is a `WorldCell` (an
// i32 cell on a 64 m grid and a float32 offset into it); what it computes in a frame is relative
// to the frame's `WorldEye`. `relative(WorldCell, WorldEye)` here is the arithmetic the shaders
// do (`instance_from_eye`, shaders/scene.slang), operation for operation, so a CPU mirror of a
// shader and a test of the rule use the same function.

#include <core/base/types.h>
#include <core/math/math.h>

#include <cmath>

namespace engine {

// A displacement in f64: the difference of two world positions, or a velocity times a time,
// before it is known to be small.
struct DVec3 {
  f64 x = 0, y = 0, z = 0;
  constexpr DVec3() = default;
  constexpr DVec3(f64 x_, f64 y_, f64 z_) : x(x_), y(y_), z(z_) {}
  // A float32 vector widens exactly, so this one is implicit.
  // Spelled as casts: Clang's -Wdouble-promotion (an error under warnings-as-errors) reports an
  // implicit float-to-double conversion even where it is exact.
  constexpr DVec3(Vec3 v)
      : x(static_cast<f64>(v.x)), y(static_cast<f64>(v.y)), z(static_cast<f64>(v.z)) {}
  constexpr bool operator==(const DVec3&) const = default;
};

constexpr DVec3 operator+(DVec3 a, DVec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr DVec3 operator-(DVec3 a, DVec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr DVec3 operator-(DVec3 v) noexcept { return {-v.x, -v.y, -v.z}; }
constexpr DVec3 operator*(DVec3 v, f64 s) noexcept { return {v.x * s, v.y * s, v.z * s}; }
constexpr DVec3 operator*(f64 s, DVec3 v) noexcept { return v * s; }
constexpr DVec3 operator/(DVec3 v, f64 s) noexcept { return {v.x / s, v.y / s, v.z / s}; }
constexpr f64 dot(DVec3 a, DVec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr f64 length_squared(DVec3 v) noexcept { return dot(v, v); }
inline f64 length(DVec3 v) noexcept { return std::sqrt(dot(v, v)); }

// The one way a displacement becomes float32: at the call, by name. Rounds to nearest; the caller
// knows the displacement is small enough for a float to hold it as finely as it needs.
constexpr Vec3 narrow(DVec3 v) noexcept {
  return {static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z)};
}

// A point in the world, metres, y up.
struct WorldPos {
  f64 x = 0, y = 0, z = 0;
  constexpr WorldPos() = default;
  constexpr WorldPos(f64 x_, f64 y_, f64 z_) : x(x_), y(y_), z(z_) {}
  constexpr bool operator==(const WorldPos&) const = default;
  static constexpr WorldPos origin() noexcept { return {0.0, 0.0, 0.0}; }
};

constexpr DVec3 operator-(WorldPos a, WorldPos b) noexcept {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
constexpr WorldPos operator+(WorldPos p, DVec3 d) noexcept {
  return {p.x + d.x, p.y + d.y, p.z + d.z};
}
constexpr WorldPos operator-(WorldPos p, DVec3 d) noexcept {
  return {p.x - d.x, p.y - d.y, p.z - d.z};
}
constexpr WorldPos& operator+=(WorldPos& p, DVec3 d) noexcept { return p = p + d; }
constexpr WorldPos& operator-=(WorldPos& p, DVec3 d) noexcept { return p = p - d; }

// Where `p` is in the float32 frame whose origin is `origin`: the other way down to a float.
constexpr Vec3 relative(WorldPos p, WorldPos origin) noexcept { return narrow(p - origin); }
// The point a local-frame position names.
constexpr WorldPos absolute(WorldPos origin, Vec3 local) noexcept { return origin + DVec3{local}; }

constexpr WorldPos lerp(WorldPos a, WorldPos b, f64 t) noexcept { return a + (b - a) * t; }

// ---- cells: the form a position takes in GPU memory ------------------------------------------

// The grid's pitch, metres. A power of two, so a difference of cells times it is exact in a
// float, and small enough that a local, which rounds at 3.8 um at the most, puts a placement
// within 2 um of where it was authored. i32 cells of it reach 1.37e11 m.
inline constexpr f64 k_world_cell_m = 64.0;
inline constexpr f32 k_world_cell_m_f32 = 64.0f;
// The farthest coordinate a cell can name; `world_cell_valid` is the test, and input from
// outside (a file, the wire, a command line) is validated with it rather than asserted.
inline constexpr f64 k_world_extent_m = 2147483647.0 * k_world_cell_m;

constexpr bool world_cell_valid(WorldPos p) noexcept {
  return p.x >= -k_world_extent_m && p.x < k_world_extent_m && p.y >= -k_world_extent_m &&
         p.y < k_world_extent_m && p.z >= -k_world_extent_m && p.z < k_world_extent_m;
}

// A position as the GPU stores it: 24 bytes, the same as three doubles.
struct WorldCell {
  Vec3i cell;  // floor(p / 64)
  Vec3 local;  // p - cell * 64, in [0, 64], rounded to float32
  constexpr bool operator==(const WorldCell&) const = default;
};

// The frame's origin as the GPU is given it: the eye's cell and local, and what the local's
// rounding left over, so that cell * 64 + local + residual is the eye to f64's own precision.
struct WorldEye {
  Vec3i cell;
  Vec3 local;
  Vec3 residual;
  constexpr bool operator==(const WorldEye&) const = default;
};

namespace world_detail {
inline void split(f64 v, i32& cell, f32& local, f32& residual) noexcept {
  const f64 c = std::floor(v / k_world_cell_m);
  const f64 offset = v - c * k_world_cell_m;  // exact: c * 64 is, and the difference is below 64
  cell = static_cast<i32>(c);
  local = static_cast<f32>(offset);  // may round up to 64.0f exactly, which the arithmetic takes
  residual = static_cast<f32>(offset - static_cast<f64>(local));
}
}  // namespace world_detail

// `p` must be `world_cell_valid`.
inline WorldCell to_cell(WorldPos p) noexcept {
  WorldCell out;
  f32 unused = 0.0f;
  world_detail::split(p.x, out.cell.x, out.local.x, unused);
  world_detail::split(p.y, out.cell.y, out.local.y, unused);
  world_detail::split(p.z, out.cell.z, out.local.z, unused);
  return out;
}

inline WorldEye to_eye(WorldPos p) noexcept {
  WorldEye out;
  world_detail::split(p.x, out.cell.x, out.local.x, out.residual.x);
  world_detail::split(p.y, out.cell.y, out.local.y, out.residual.y);
  world_detail::split(p.z, out.cell.z, out.local.z, out.residual.z);
  return out;
}

constexpr WorldPos to_world(const WorldCell& c) noexcept {
  return {static_cast<f64>(c.cell.x) * k_world_cell_m + static_cast<f64>(c.local.x),
          static_cast<f64>(c.cell.y) * k_world_cell_m + static_cast<f64>(c.local.y),
          static_cast<f64>(c.cell.z) * k_world_cell_m + static_cast<f64>(c.local.z)};
}

constexpr WorldPos to_world(const WorldEye& e) noexcept {
  return {static_cast<f64>(e.cell.x) * k_world_cell_m +
              (static_cast<f64>(e.local.x) + static_cast<f64>(e.residual.x)),
          static_cast<f64>(e.cell.y) * k_world_cell_m +
              (static_cast<f64>(e.local.y) + static_cast<f64>(e.residual.y)),
          static_cast<f64>(e.cell.z) * k_world_cell_m +
              (static_cast<f64>(e.local.z) + static_cast<f64>(e.residual.z))};
}

// **The shaders' arithmetic, in float32 and in their order** (`instance_from_eye`): where a
// stored position is from the frame's eye, one axis at a time. Three terms — the cells'
// difference times the pitch, which is exact (to 2^24 cells, a billion metres), the two locals,
// and the eye's residual — **summed so that no intermediate is larger than the result needs**,
// because a float sum rounds at the size of what it produces and a large intermediate would
// leave its rounding in a small result:
//
//   - the point's cell is above the eye's: (cells - eye.local) + local. Near the eye that means
//     the eye is at the top of its cell and the point at the bottom of the next, so the first
//     difference is small and exact;
//   - otherwise: (cells + local) - eye.local, small and exact the other way round, and plainly
//     local - eye.local in the eye's own cell;
//   - the residual last.
//
// (The naive (local - eye.local) first measured 3 um off for a point 3 m from an eye that stood
// by a cell's edge: two roundings at 61 m in a 3 m result.) The error is within three float
// steps at the size of the result wherever the eye is. Moving the position and the eye by the
// same whole number of cells changes no operand, which is why a scene translated by whole cells
// draws the same bytes.
constexpr f32 relative_axis(i32 cell, f32 local, i32 eye_cell, f32 eye_local,
                            f32 eye_residual) noexcept {
  // The difference in i64 here, because a signed overflow is undefined in C++ where the shader's
  // int wraps; the two agree wherever the difference fits, which is everywhere a picture reaches.
  const f32 cells =
      static_cast<f32>(static_cast<i64>(cell) - static_cast<i64>(eye_cell)) * k_world_cell_m_f32;
  const f32 from_local = cells > 0.0f ? (cells - eye_local) + local : (cells + local) - eye_local;
  return from_local - eye_residual;
}

constexpr Vec3 relative(const WorldCell& p, const WorldEye& eye) noexcept {
  return {relative_axis(p.cell.x, p.local.x, eye.cell.x, eye.local.x, eye.residual.x),
          relative_axis(p.cell.y, p.local.y, eye.cell.y, eye.local.y, eye.residual.y),
          relative_axis(p.cell.z, p.local.z, eye.cell.z, eye.local.z, eye.residual.z)};
}

}  // namespace engine
