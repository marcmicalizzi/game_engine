#pragma once

// Linear algebra for the engine. Conventions:
//   - right-handed, y up, -z forward (camera looks down -z in view space)
//   - column-major matrices, column vectors: p' = M * p; M = T * R * S
//   - quaternions {x, y, z, w} with w the scalar part; rotation of v is q * v * q^-1
//   - angles in radians
//   - projections produce zero-to-one depth with reversed Z (near = 1, far = 0), which is
//     what the renderer will use; the Vulkan y-flip happens in the viewport, not here
// Everything is trivially copyable with the layout a GPU expects (Vec3 is 12 bytes, not 16).

#include <core/base/types.h>

#include <cmath>
#include <compare>

namespace engine {

inline constexpr f32 k_pi = 3.14159265358979323846f;
inline constexpr f32 k_two_pi = 2.0f * k_pi;
inline constexpr f32 k_half_pi = 0.5f * k_pi;
inline constexpr f32 k_epsilon = 1.0e-6f;

constexpr f32 radians(f32 degrees) noexcept { return degrees * (k_pi / 180.0f); }
constexpr f32 degrees(f32 radians) noexcept { return radians * (180.0f / k_pi); }
constexpr f32 clamp(f32 v, f32 lo, f32 hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
constexpr f32 saturate(f32 v) noexcept { return clamp(v, 0.0f, 1.0f); }
constexpr f32 lerp(f32 a, f32 b, f32 t) noexcept { return a + (b - a) * t; }
constexpr bool approx_equal(f32 a, f32 b, f32 eps = 1.0e-5f) noexcept {
  const f32 d = a - b;
  return (d < 0 ? -d : d) <= eps;
}
constexpr f32 sign(f32 v) noexcept { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }
constexpr i32 floor_to_int(f32 v) noexcept {
  const i32 i = static_cast<i32>(v);
  return v < static_cast<f32>(i) ? i - 1 : i;
}

// --- vectors ------------------------------------------------------------------------------------

struct Vec2 {
  f32 x = 0, y = 0;
  constexpr Vec2() = default;
  constexpr Vec2(f32 x_, f32 y_) : x(x_), y(y_) {}
  constexpr explicit Vec2(f32 s) : x(s), y(s) {}
  constexpr bool operator==(const Vec2&) const = default;
  constexpr f32& operator[](usize i) noexcept { return i == 0 ? x : y; }
  constexpr f32 operator[](usize i) const noexcept { return i == 0 ? x : y; }
};

struct Vec3 {
  f32 x = 0, y = 0, z = 0;
  constexpr Vec3() = default;
  constexpr Vec3(f32 x_, f32 y_, f32 z_) : x(x_), y(y_), z(z_) {}
  constexpr explicit Vec3(f32 s) : x(s), y(s), z(s) {}
  constexpr Vec3(Vec2 v, f32 z_) : x(v.x), y(v.y), z(z_) {}
  constexpr bool operator==(const Vec3&) const = default;
  constexpr f32& operator[](usize i) noexcept { return i == 0 ? x : (i == 1 ? y : z); }
  constexpr f32 operator[](usize i) const noexcept { return i == 0 ? x : (i == 1 ? y : z); }
  constexpr Vec2 xy() const noexcept { return {x, y}; }
  static constexpr Vec3 zero() noexcept { return {0, 0, 0}; }
  static constexpr Vec3 one() noexcept { return {1, 1, 1}; }
  static constexpr Vec3 unit_x() noexcept { return {1, 0, 0}; }
  static constexpr Vec3 unit_y() noexcept { return {0, 1, 0}; }
  static constexpr Vec3 unit_z() noexcept { return {0, 0, 1}; }
  static constexpr Vec3 up() noexcept { return unit_y(); }
  static constexpr Vec3 forward() noexcept { return {0, 0, -1}; }
  static constexpr Vec3 right() noexcept { return unit_x(); }
};

struct Vec4 {
  f32 x = 0, y = 0, z = 0, w = 0;
  constexpr Vec4() = default;
  constexpr Vec4(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
  constexpr explicit Vec4(f32 s) : x(s), y(s), z(s), w(s) {}
  constexpr Vec4(Vec3 v, f32 w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
  constexpr bool operator==(const Vec4&) const = default;
  constexpr f32& operator[](usize i) noexcept { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }
  constexpr f32 operator[](usize i) const noexcept { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }
  constexpr Vec3 xyz() const noexcept { return {x, y, z}; }
};

struct Vec2i {
  i32 x = 0, y = 0;
  constexpr bool operator==(const Vec2i&) const = default;
  constexpr auto operator<=>(const Vec2i&) const = default;
};

struct Vec3i {
  i32 x = 0, y = 0, z = 0;
  constexpr bool operator==(const Vec3i&) const = default;
  constexpr auto operator<=>(const Vec3i&) const = default;
};

#define ENGINE_VEC_OPS(V, ...)                                                        \
  constexpr V operator+(V a, V b) noexcept { return V{__VA_ARGS__(+)}; }               \
  constexpr V operator-(V a, V b) noexcept { return V{__VA_ARGS__(-)}; }               \
  constexpr V operator*(V a, V b) noexcept { return V{__VA_ARGS__(*)}; }               \
  constexpr V operator/(V a, V b) noexcept { return V{__VA_ARGS__(/)}; }               \
  constexpr V& operator+=(V& a, V b) noexcept { return a = a + b; }                    \
  constexpr V& operator-=(V& a, V b) noexcept { return a = a - b; }                    \
  constexpr V& operator*=(V& a, V b) noexcept { return a = a * b; }                    \
  constexpr V& operator/=(V& a, V b) noexcept { return a = a / b; }

#define ENGINE_VEC2_BODY(op) a.x op b.x, a.y op b.y
#define ENGINE_VEC3_BODY(op) a.x op b.x, a.y op b.y, a.z op b.z
#define ENGINE_VEC4_BODY(op) a.x op b.x, a.y op b.y, a.z op b.z, a.w op b.w
ENGINE_VEC_OPS(Vec2, ENGINE_VEC2_BODY)
ENGINE_VEC_OPS(Vec3, ENGINE_VEC3_BODY)
ENGINE_VEC_OPS(Vec4, ENGINE_VEC4_BODY)
#undef ENGINE_VEC_OPS
#undef ENGINE_VEC2_BODY
#undef ENGINE_VEC3_BODY
#undef ENGINE_VEC4_BODY

constexpr Vec2 operator*(Vec2 v, f32 s) noexcept { return {v.x * s, v.y * s}; }
constexpr Vec2 operator*(f32 s, Vec2 v) noexcept { return v * s; }
constexpr Vec2 operator/(Vec2 v, f32 s) noexcept { return {v.x / s, v.y / s}; }
constexpr Vec2 operator-(Vec2 v) noexcept { return {-v.x, -v.y}; }
constexpr Vec3 operator*(Vec3 v, f32 s) noexcept { return {v.x * s, v.y * s, v.z * s}; }
constexpr Vec3 operator*(f32 s, Vec3 v) noexcept { return v * s; }
constexpr Vec3 operator/(Vec3 v, f32 s) noexcept { return {v.x / s, v.y / s, v.z / s}; }
constexpr Vec3 operator-(Vec3 v) noexcept { return {-v.x, -v.y, -v.z}; }
constexpr Vec4 operator*(Vec4 v, f32 s) noexcept { return {v.x * s, v.y * s, v.z * s, v.w * s}; }
constexpr Vec4 operator*(f32 s, Vec4 v) noexcept { return v * s; }
constexpr Vec4 operator/(Vec4 v, f32 s) noexcept { return {v.x / s, v.y / s, v.z / s, v.w / s}; }
constexpr Vec4 operator-(Vec4 v) noexcept { return {-v.x, -v.y, -v.z, -v.w}; }
constexpr Vec2i operator+(Vec2i a, Vec2i b) noexcept { return {a.x + b.x, a.y + b.y}; }
constexpr Vec2i operator-(Vec2i a, Vec2i b) noexcept { return {a.x - b.x, a.y - b.y}; }
constexpr Vec3i operator+(Vec3i a, Vec3i b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr Vec3i operator-(Vec3i a, Vec3i b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

constexpr f32 dot(Vec2 a, Vec2 b) noexcept { return a.x * b.x + a.y * b.y; }
constexpr f32 dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr f32 dot(Vec4 a, Vec4 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
constexpr Vec3 cross(Vec3 a, Vec3 b) noexcept {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
constexpr f32 length_squared(Vec2 v) noexcept { return dot(v, v); }
constexpr f32 length_squared(Vec3 v) noexcept { return dot(v, v); }
constexpr f32 length_squared(Vec4 v) noexcept { return dot(v, v); }
inline f32 length(Vec2 v) noexcept { return std::sqrt(length_squared(v)); }
inline f32 length(Vec3 v) noexcept { return std::sqrt(length_squared(v)); }
inline f32 length(Vec4 v) noexcept { return std::sqrt(length_squared(v)); }
inline f32 distance(Vec3 a, Vec3 b) noexcept { return length(a - b); }

// Returns the zero vector for zero-length input rather than NaN.
inline Vec2 normalize(Vec2 v) noexcept {
  const f32 l = length(v);
  return l > k_epsilon ? v / l : Vec2{};
}
inline Vec3 normalize(Vec3 v) noexcept {
  const f32 l = length(v);
  return l > k_epsilon ? v / l : Vec3{};
}
inline Vec4 normalize(Vec4 v) noexcept {
  const f32 l = length(v);
  return l > k_epsilon ? v / l : Vec4{};
}

constexpr Vec3 lerp(Vec3 a, Vec3 b, f32 t) noexcept { return a + (b - a) * t; }
constexpr Vec3 min(Vec3 a, Vec3 b) noexcept { return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z}; }
constexpr Vec3 max(Vec3 a, Vec3 b) noexcept { return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z}; }
constexpr Vec3 abs(Vec3 v) noexcept { return {v.x < 0 ? -v.x : v.x, v.y < 0 ? -v.y : v.y, v.z < 0 ? -v.z : v.z}; }
constexpr bool approx_equal(Vec3 a, Vec3 b, f32 eps = 1.0e-5f) noexcept {
  return approx_equal(a.x, b.x, eps) && approx_equal(a.y, b.y, eps) && approx_equal(a.z, b.z, eps);
}
constexpr bool approx_equal(Vec4 a, Vec4 b, f32 eps = 1.0e-5f) noexcept {
  return approx_equal(a.x, b.x, eps) && approx_equal(a.y, b.y, eps) && approx_equal(a.z, b.z, eps) && approx_equal(a.w, b.w, eps);
}
constexpr Vec3 reflect(Vec3 v, Vec3 n) noexcept { return v - n * (2.0f * dot(v, n)); }

// --- quaternion ---------------------------------------------------------------------------------

struct Quat {
  f32 x = 0, y = 0, z = 0, w = 1;
  constexpr Quat() = default;
  constexpr Quat(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
  constexpr bool operator==(const Quat&) const = default;
  static constexpr Quat identity() noexcept { return {0, 0, 0, 1}; }
  constexpr Vec3 vector_part() const noexcept { return {x, y, z}; }
};

inline Quat quat_from_axis_angle(Vec3 axis, f32 angle) noexcept {
  const Vec3 a = normalize(axis);
  const f32 h = angle * 0.5f;
  const f32 s = std::sin(h);
  return {a.x * s, a.y * s, a.z * s, std::cos(h)};
}

// Yaw about +y, then pitch about +x, then roll about -z (forward): the usual camera order.
inline Quat quat_from_euler(f32 yaw, f32 pitch, f32 roll) noexcept;

constexpr Quat operator*(Quat a, Quat b) noexcept {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
constexpr Quat operator*(Quat q, f32 s) noexcept { return {q.x * s, q.y * s, q.z * s, q.w * s}; }
constexpr Quat operator+(Quat a, Quat b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
constexpr Quat operator-(Quat q) noexcept { return {-q.x, -q.y, -q.z, -q.w}; }
constexpr f32 dot(Quat a, Quat b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
constexpr Quat conjugate(Quat q) noexcept { return {-q.x, -q.y, -q.z, q.w}; }
inline f32 length(Quat q) noexcept { return std::sqrt(dot(q, q)); }
inline Quat normalize(Quat q) noexcept {
  const f32 l = length(q);
  return l > k_epsilon ? q * (1.0f / l) : Quat::identity();
}
// For unit quaternions the inverse is the conjugate; this handles the general case.
inline Quat inverse(Quat q) noexcept {
  const f32 d = dot(q, q);
  return d > k_epsilon ? conjugate(q) * (1.0f / d) : Quat::identity();
}

constexpr Vec3 rotate(Quat q, Vec3 v) noexcept {
  // v' = v + 2w(u x v) + 2(u x (u x v)) with u the vector part; exact for unit q.
  const Vec3 u = q.vector_part();
  const Vec3 t = cross(u, v) * 2.0f;
  return v + t * q.w + cross(u, t);
}
inline Quat quat_from_euler(f32 yaw, f32 pitch, f32 roll) noexcept {
  return quat_from_axis_angle(Vec3::unit_y(), yaw) * quat_from_axis_angle(Vec3::unit_x(), pitch) *
         quat_from_axis_angle(Vec3::forward(), roll);
}
inline Quat slerp(Quat a, Quat b, f32 t) noexcept {
  f32 cos_theta = dot(a, b);
  if (cos_theta < 0.0f) {  // take the short arc
    b = -b;
    cos_theta = -cos_theta;
  }
  if (cos_theta > 0.9995f) return normalize(a + (b + (-a)) * t);  // nearly parallel: nlerp
  const f32 theta = std::acos(cos_theta);
  const f32 s = std::sin(theta);
  const f32 wa = std::sin((1.0f - t) * theta) / s;
  const f32 wb = std::sin(t * theta) / s;
  return a * wa + b * wb;
}
constexpr bool approx_equal(Quat a, Quat b, f32 eps = 1.0e-5f) noexcept {
  // q and -q are the same rotation.
  const bool same = approx_equal(a.x, b.x, eps) && approx_equal(a.y, b.y, eps) && approx_equal(a.z, b.z, eps) && approx_equal(a.w, b.w, eps);
  const bool neg = approx_equal(a.x, -b.x, eps) && approx_equal(a.y, -b.y, eps) && approx_equal(a.z, -b.z, eps) && approx_equal(a.w, -b.w, eps);
  return same || neg;
}

// --- matrices -----------------------------------------------------------------------------------

struct Mat3 {
  Vec3 c[3];  // columns
  constexpr Mat3() : c{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}} {}
  constexpr Mat3(Vec3 c0, Vec3 c1, Vec3 c2) : c{c0, c1, c2} {}
  constexpr bool operator==(const Mat3&) const = default;
  static constexpr Mat3 identity() noexcept { return Mat3(); }
  constexpr f32 at(usize row, usize col) const noexcept { return c[col][row]; }
  constexpr f32& at(usize row, usize col) noexcept { return c[col][row]; }
};

struct Mat4 {
  Vec4 c[4];  // columns
  constexpr Mat4() : c{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}} {}
  constexpr Mat4(Vec4 c0, Vec4 c1, Vec4 c2, Vec4 c3) : c{c0, c1, c2, c3} {}
  constexpr bool operator==(const Mat4&) const = default;
  static constexpr Mat4 identity() noexcept { return Mat4(); }
  constexpr f32 at(usize row, usize col) const noexcept { return c[col][row]; }
  constexpr f32& at(usize row, usize col) noexcept { return c[col][row]; }
  constexpr const f32* data() const noexcept { return &c[0].x; }
};

constexpr Vec3 operator*(const Mat3& m, Vec3 v) noexcept { return m.c[0] * v.x + m.c[1] * v.y + m.c[2] * v.z; }
constexpr Mat3 operator*(const Mat3& a, const Mat3& b) noexcept { return {a * b.c[0], a * b.c[1], a * b.c[2]}; }
constexpr Vec4 operator*(const Mat4& m, Vec4 v) noexcept { return m.c[0] * v.x + m.c[1] * v.y + m.c[2] * v.z + m.c[3] * v.w; }
constexpr Mat4 operator*(const Mat4& a, const Mat4& b) noexcept { return {a * b.c[0], a * b.c[1], a * b.c[2], a * b.c[3]}; }

// Transforms a point (w = 1) or a direction (w = 0) by an affine matrix.
constexpr Vec3 transform_point(const Mat4& m, Vec3 p) noexcept { return (m * Vec4(p, 1.0f)).xyz(); }
constexpr Vec3 transform_direction(const Mat4& m, Vec3 d) noexcept { return (m * Vec4(d, 0.0f)).xyz(); }

constexpr Mat3 transpose(const Mat3& m) noexcept {
  return {{m.at(0, 0), m.at(0, 1), m.at(0, 2)}, {m.at(1, 0), m.at(1, 1), m.at(1, 2)}, {m.at(2, 0), m.at(2, 1), m.at(2, 2)}};
}
constexpr Mat4 transpose(const Mat4& m) noexcept {
  Mat4 r;
  for (usize i = 0; i < 4; ++i)
    for (usize j = 0; j < 4; ++j) r.at(i, j) = m.at(j, i);
  return r;
}

constexpr f32 determinant(const Mat3& m) noexcept {
  return m.at(0, 0) * (m.at(1, 1) * m.at(2, 2) - m.at(1, 2) * m.at(2, 1)) -
         m.at(0, 1) * (m.at(1, 0) * m.at(2, 2) - m.at(1, 2) * m.at(2, 0)) +
         m.at(0, 2) * (m.at(1, 0) * m.at(2, 1) - m.at(1, 1) * m.at(2, 0));
}

// General 4x4 inverse by cofactor expansion. Returns identity for singular matrices.
constexpr Mat4 inverse(const Mat4& m) noexcept {
  const f32 a00 = m.at(0, 0), a01 = m.at(0, 1), a02 = m.at(0, 2), a03 = m.at(0, 3);
  const f32 a10 = m.at(1, 0), a11 = m.at(1, 1), a12 = m.at(1, 2), a13 = m.at(1, 3);
  const f32 a20 = m.at(2, 0), a21 = m.at(2, 1), a22 = m.at(2, 2), a23 = m.at(2, 3);
  const f32 a30 = m.at(3, 0), a31 = m.at(3, 1), a32 = m.at(3, 2), a33 = m.at(3, 3);

  const f32 b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10, b02 = a00 * a13 - a03 * a10;
  const f32 b03 = a01 * a12 - a02 * a11, b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12;
  const f32 b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30, b08 = a20 * a33 - a23 * a30;
  const f32 b09 = a21 * a32 - a22 * a31, b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;

  const f32 det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
  if (det > -1.0e-12f && det < 1.0e-12f) return Mat4::identity();
  const f32 inv = 1.0f / det;

  Mat4 r;
  r.at(0, 0) = (a11 * b11 - a12 * b10 + a13 * b09) * inv;
  r.at(0, 1) = (-a01 * b11 + a02 * b10 - a03 * b09) * inv;
  r.at(0, 2) = (a31 * b05 - a32 * b04 + a33 * b03) * inv;
  r.at(0, 3) = (-a21 * b05 + a22 * b04 - a23 * b03) * inv;
  r.at(1, 0) = (-a10 * b11 + a12 * b08 - a13 * b07) * inv;
  r.at(1, 1) = (a00 * b11 - a02 * b08 + a03 * b07) * inv;
  r.at(1, 2) = (-a30 * b05 + a32 * b02 - a33 * b01) * inv;
  r.at(1, 3) = (a20 * b05 - a22 * b02 + a23 * b01) * inv;
  r.at(2, 0) = (a10 * b10 - a11 * b08 + a13 * b06) * inv;
  r.at(2, 1) = (-a00 * b10 + a01 * b08 - a03 * b06) * inv;
  r.at(2, 2) = (a30 * b04 - a31 * b02 + a33 * b00) * inv;
  r.at(2, 3) = (-a20 * b04 + a21 * b02 - a23 * b00) * inv;
  r.at(3, 0) = (-a10 * b09 + a11 * b07 - a12 * b06) * inv;
  r.at(3, 1) = (a00 * b09 - a01 * b07 + a02 * b06) * inv;
  r.at(3, 2) = (-a30 * b03 + a31 * b01 - a32 * b00) * inv;
  r.at(3, 3) = (a20 * b03 - a21 * b01 + a22 * b00) * inv;
  return r;
}

constexpr Mat4 translation(Vec3 t) noexcept {
  Mat4 m;
  m.c[3] = Vec4(t, 1.0f);
  return m;
}
constexpr Mat4 scaling(Vec3 s) noexcept {
  Mat4 m;
  m.at(0, 0) = s.x;
  m.at(1, 1) = s.y;
  m.at(2, 2) = s.z;
  return m;
}
constexpr Mat3 mat3_from_quat(Quat q) noexcept {
  const f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  const f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
  const f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  return {{1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy)},
          {2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx)},
          {2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy)}};
}
constexpr Mat4 mat4_from_quat(Quat q) noexcept {
  const Mat3 r = mat3_from_quat(q);
  return {Vec4(r.c[0], 0), Vec4(r.c[1], 0), Vec4(r.c[2], 0), {0, 0, 0, 1}};
}
inline Quat quat_from_mat3(const Mat3& m) noexcept {
  const f32 trace = m.at(0, 0) + m.at(1, 1) + m.at(2, 2);
  Quat q;
  if (trace > 0.0f) {
    const f32 s = std::sqrt(trace + 1.0f) * 2.0f;
    q = {(m.at(2, 1) - m.at(1, 2)) / s, (m.at(0, 2) - m.at(2, 0)) / s, (m.at(1, 0) - m.at(0, 1)) / s, 0.25f * s};
  } else if (m.at(0, 0) > m.at(1, 1) && m.at(0, 0) > m.at(2, 2)) {
    const f32 s = std::sqrt(1.0f + m.at(0, 0) - m.at(1, 1) - m.at(2, 2)) * 2.0f;
    q = {0.25f * s, (m.at(0, 1) + m.at(1, 0)) / s, (m.at(0, 2) + m.at(2, 0)) / s, (m.at(2, 1) - m.at(1, 2)) / s};
  } else if (m.at(1, 1) > m.at(2, 2)) {
    const f32 s = std::sqrt(1.0f + m.at(1, 1) - m.at(0, 0) - m.at(2, 2)) * 2.0f;
    q = {(m.at(0, 1) + m.at(1, 0)) / s, 0.25f * s, (m.at(1, 2) + m.at(2, 1)) / s, (m.at(0, 2) - m.at(2, 0)) / s};
  } else {
    const f32 s = std::sqrt(1.0f + m.at(2, 2) - m.at(0, 0) - m.at(1, 1)) * 2.0f;
    q = {(m.at(0, 2) + m.at(2, 0)) / s, (m.at(1, 2) + m.at(2, 1)) / s, 0.25f * s, (m.at(1, 0) - m.at(0, 1)) / s};
  }
  return normalize(q);
}

// --- transform ----------------------------------------------------------------------------------

struct Transform3 {
  Vec3 position{};
  Quat rotation = Quat::identity();
  Vec3 scale = Vec3::one();
  constexpr bool operator==(const Transform3&) const = default;
  static constexpr Transform3 identity() noexcept { return {}; }
};

constexpr Mat4 mat4_from_transform(const Transform3& t) noexcept {
  const Mat3 r = mat3_from_quat(t.rotation);
  return {Vec4(r.c[0] * t.scale.x, 0), Vec4(r.c[1] * t.scale.y, 0), Vec4(r.c[2] * t.scale.z, 0), Vec4(t.position, 1)};
}
constexpr Vec3 transform_point(const Transform3& t, Vec3 p) noexcept { return rotate(t.rotation, p * t.scale) + t.position; }
// Parent-then-child composition: the result applies `child` in `parent`'s space.
inline Transform3 compose(const Transform3& parent, const Transform3& child) noexcept {
  return {transform_point(parent, child.position), normalize(parent.rotation * child.rotation), parent.scale * child.scale};
}
inline Transform3 inverse(const Transform3& t) noexcept {
  const Quat inv_rot = conjugate(t.rotation);
  const Vec3 inv_scale{1.0f / t.scale.x, 1.0f / t.scale.y, 1.0f / t.scale.z};
  return {rotate(inv_rot, -t.position) * inv_scale, inv_rot, inv_scale};
}

// --- bounds -------------------------------------------------------------------------------------

struct Aabb3 {
  Vec3 min{};
  Vec3 max{};
  constexpr bool operator==(const Aabb3&) const = default;
  // An empty box that expands correctly from the first point.
  static constexpr Aabb3 empty() noexcept { return {Vec3(3.4e38f), Vec3(-3.4e38f)}; }
  constexpr bool is_empty() const noexcept { return min.x > max.x || min.y > max.y || min.z > max.z; }
  constexpr Vec3 center() const noexcept { return (min + max) * 0.5f; }
  constexpr Vec3 extent() const noexcept { return (max - min) * 0.5f; }
  constexpr Vec3 size() const noexcept { return max - min; }
  constexpr void expand(Vec3 p) noexcept {
    min = engine::min(min, p);
    max = engine::max(max, p);
  }
  constexpr void expand(const Aabb3& b) noexcept {
    if (b.is_empty()) return;
    expand(b.min);
    expand(b.max);
  }
  constexpr bool contains(Vec3 p) const noexcept {
    return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
  }
  constexpr bool intersects(const Aabb3& b) const noexcept {
    return min.x <= b.max.x && max.x >= b.min.x && min.y <= b.max.y && max.y >= b.min.y && min.z <= b.max.z && max.z >= b.min.z;
  }
};

// Transforms all eight corners and re-fits; conservative but exact for axis-aligned results.
constexpr Aabb3 transform_aabb(const Mat4& m, const Aabb3& b) noexcept {
  Aabb3 r = Aabb3::empty();
  for (u32 i = 0; i < 8; ++i) {
    const Vec3 corner{(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z};
    r.expand(transform_point(m, corner));
  }
  return r;
}

// --- projections and views ----------------------------------------------------------------------

// Right-handed view matrix looking from `eye` toward `target`.
inline Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up) noexcept {
  const Vec3 f = normalize(target - eye);
  const Vec3 s = normalize(cross(f, up));
  const Vec3 u = cross(s, f);
  Mat4 m;
  m.at(0, 0) = s.x;
  m.at(0, 1) = s.y;
  m.at(0, 2) = s.z;
  m.at(1, 0) = u.x;
  m.at(1, 1) = u.y;
  m.at(1, 2) = u.z;
  m.at(2, 0) = -f.x;
  m.at(2, 1) = -f.y;
  m.at(2, 2) = -f.z;
  m.at(0, 3) = -dot(s, eye);
  m.at(1, 3) = -dot(u, eye);
  m.at(2, 3) = dot(f, eye);
  return m;
}

// Perspective with reversed zero-to-one depth: near plane maps to 1, far to 0. Infinite far
// plane when `far` is 0. Better float precision across the depth range than forward depth.
inline Mat4 perspective_reversed_z(f32 fov_y, f32 aspect, f32 near, f32 far = 0.0f) noexcept {
  const f32 t = 1.0f / std::tan(fov_y * 0.5f);
  Mat4 m;
  m.at(0, 0) = t / aspect;
  m.at(1, 1) = t;
  if (far <= 0.0f) {
    m.at(2, 2) = 0.0f;
    m.at(2, 3) = near;
  } else {
    m.at(2, 2) = near / (far - near);
    m.at(2, 3) = far * near / (far - near);
  }
  m.at(3, 2) = -1.0f;
  m.at(3, 3) = 0.0f;
  return m;
}

// Orthographic with reversed zero-to-one depth.
constexpr Mat4 orthographic_reversed_z(f32 left, f32 right, f32 bottom, f32 top, f32 near, f32 far) noexcept {
  Mat4 m;
  m.at(0, 0) = 2.0f / (right - left);
  m.at(1, 1) = 2.0f / (top - bottom);
  m.at(2, 2) = 1.0f / (far - near);
  m.at(0, 3) = -(right + left) / (right - left);
  m.at(1, 3) = -(top + bottom) / (top - bottom);
  m.at(2, 3) = far / (far - near);
  return m;
}

}  // namespace engine
