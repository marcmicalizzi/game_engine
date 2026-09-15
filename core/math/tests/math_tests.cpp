#include <core/math/math.h>

#include <doctest/doctest.h>

using namespace engine;

TEST_CASE("math: vector arithmetic, dot, cross, normalize") {
  constexpr Vec3 a{1, 2, 3};
  constexpr Vec3 b{4, 5, 6};
  static_assert(a + b == Vec3{5, 7, 9});
  static_assert(b - a == Vec3{3, 3, 3});
  static_assert(a * 2.0f == Vec3{2, 4, 6});
  static_assert(dot(a, b) == 32.0f);
  static_assert(cross(Vec3::unit_x(), Vec3::unit_y()) == Vec3::unit_z());
  static_assert(length_squared(Vec3{3, 4, 0}) == 25.0f);
  CHECK(length(Vec3{3, 4, 0}) == 5.0f);
  CHECK(approx_equal(normalize(Vec3{0, 0, 7}), Vec3::unit_z()));
  CHECK(normalize(Vec3{}) == Vec3{});
  CHECK(approx_equal(lerp(a, b, 0.5f), Vec3{2.5f, 3.5f, 4.5f}));
  CHECK(min(a, b) == a);
  CHECK(max(a, b) == b);
  CHECK(approx_equal(reflect(Vec3{1, -1, 0}, Vec3::unit_y()), Vec3{1, 1, 0}));
  static_assert(Vec2i{1, 2} < Vec2i{1, 3});
  static_assert(Vec4(a, 1.0f).xyz() == a);
}

TEST_CASE("math: quaternion rotations") {
  const Quat q = quat_from_axis_angle(Vec3::unit_y(), k_half_pi);  // 90 degrees about +y
  CHECK(approx_equal(rotate(q, Vec3::unit_x()), Vec3{0, 0, -1}));   // x -> -z (right-handed)
  CHECK(approx_equal(rotate(q, Vec3::unit_z()), Vec3{1, 0, 0}));
  CHECK(approx_equal(length(q), 1.0f));
  CHECK(approx_equal(q * conjugate(q), Quat::identity()));
  CHECK(approx_equal(inverse(q), conjugate(q)));

  // Composition: rotating by q twice equals rotating by 180 degrees.
  const Quat q2 = q * q;
  CHECK(approx_equal(rotate(q2, Vec3::unit_x()), Vec3{-1, 0, 0}, 1.0e-5f));

  // Matrix round trip.
  const Mat3 m = mat3_from_quat(q);
  CHECK(approx_equal(m * Vec3::unit_x(), rotate(q, Vec3::unit_x())));
  CHECK(approx_equal(quat_from_mat3(m), q));
  for (f32 angle : {0.1f, 1.0f, 2.0f, 3.0f}) {
    const Quat r = quat_from_axis_angle(normalize(Vec3{1, 2, -3}), angle);
    CHECK(approx_equal(quat_from_mat3(mat3_from_quat(r)), r, 1.0e-4f));
  }

  // slerp endpoints and midpoint.
  const Quat a = Quat::identity();
  CHECK(approx_equal(slerp(a, q, 0.0f), a));
  CHECK(approx_equal(slerp(a, q, 1.0f), q));
  CHECK(approx_equal(slerp(a, q, 0.5f), quat_from_axis_angle(Vec3::unit_y(), k_half_pi * 0.5f), 1.0e-4f));

  // Euler: yaw alone about +y.
  CHECK(approx_equal(quat_from_euler(k_half_pi, 0, 0), q));
}

TEST_CASE("math: matrix products, transpose, inverse") {
  const Mat4 t = translation({1, 2, 3});
  const Mat4 s = scaling({2, 2, 2});
  const Mat4 r = mat4_from_quat(quat_from_axis_angle(Vec3::unit_z(), k_half_pi));
  const Mat4 m = t * r * s;
  const Vec3 p = transform_point(m, {1, 0, 0});  // scale -> (2,0,0), rotate 90 about z -> (0,2,0), translate
  CHECK(approx_equal(p, Vec3{1, 4, 3}));
  CHECK(approx_equal(transform_direction(m, {1, 0, 0}), Vec3{0, 2, 0}));

  const Mat4 inv = inverse(m);
  const Mat4 id = m * inv;
  for (usize i = 0; i < 4; ++i)
    for (usize j = 0; j < 4; ++j) CHECK(approx_equal(id.at(i, j), i == j ? 1.0f : 0.0f, 1.0e-5f));
  CHECK(approx_equal(transform_point(inv, p), Vec3{1, 0, 0}, 1.0e-5f));
  CHECK(transpose(transpose(m)) == m);
  CHECK(inverse(Mat4{Vec4{}, Vec4{}, Vec4{}, Vec4{}}) == Mat4::identity());  // singular -> identity
  static_assert(determinant(Mat3::identity()) == 1.0f);
  CHECK(approx_equal(determinant(mat3_from_quat(quat_from_axis_angle(Vec3::unit_x(), 0.7f))), 1.0f));
}

TEST_CASE("math: Transform3 composes, inverts, and matches its matrix") {
  Transform3 parent;
  parent.position = {10, 0, 0};
  parent.rotation = quat_from_axis_angle(Vec3::unit_y(), k_half_pi);
  parent.scale = {2, 2, 2};
  Transform3 child;
  child.position = {1, 0, 0};
  child.rotation = quat_from_axis_angle(Vec3::unit_x(), 0.3f);

  const Transform3 world = compose(parent, child);
  const Vec3 p{0.5f, 0.25f, -1.0f};
  const Vec3 via_transforms = transform_point(parent, transform_point(child, p));
  CHECK(approx_equal(transform_point(world, p), via_transforms, 1.0e-4f));
  CHECK(approx_equal(transform_point(mat4_from_transform(world), p), via_transforms, 1.0e-4f));

  const Transform3 inv = inverse(world);
  CHECK(approx_equal(transform_point(inv, transform_point(world, p)), p, 1.0e-4f));
  CHECK(Transform3::identity() == Transform3{});
}

TEST_CASE("math: bounds") {
  Aabb3 b = Aabb3::empty();
  CHECK(b.is_empty());
  b.expand({1, 2, 3});
  CHECK_FALSE(b.is_empty());
  b.expand({-1, 0, 5});
  CHECK(b.min == Vec3{-1, 0, 3});
  CHECK(b.max == Vec3{1, 2, 5});
  CHECK(b.center() == Vec3{0, 1, 4});
  CHECK(b.size() == Vec3{2, 2, 2});
  CHECK(b.contains({0, 1, 4}));
  CHECK_FALSE(b.contains({0, 1, 6}));
  CHECK(b.intersects(Aabb3{{0, 0, 0}, {0.5f, 0.5f, 3.5f}}));
  CHECK_FALSE(b.intersects(Aabb3{{2, 2, 2}, {3, 3, 3}}));
  const Aabb3 moved = transform_aabb(translation({10, 10, 10}), b);
  CHECK(moved.min == Vec3{9, 10, 13});
  const Aabb3 rotated = transform_aabb(mat4_from_quat(quat_from_axis_angle(Vec3::unit_y(), k_half_pi)), Aabb3{{0, 0, 0}, {1, 2, 3}});
  CHECK(approx_equal(rotated.size(), Vec3{3, 2, 1}, 1.0e-5f));
}

TEST_CASE("math: view and projection conventions") {
  const Mat4 view = look_at({0, 0, 5}, {0, 0, 0}, Vec3::up());
  // The target lies on the view -z axis; a point on +x stays on +x.
  CHECK(approx_equal(transform_point(view, {0, 0, 0}), Vec3{0, 0, -5}, 1.0e-5f));
  CHECK(approx_equal(transform_point(view, {1, 0, 5}), Vec3{1, 0, 0}, 1.0e-5f));

  const Mat4 proj = perspective_reversed_z(radians(90.0f), 1.0f, 0.1f, 100.0f);
  auto ndc = [&](Vec3 p) {
    const Vec4 c = proj * Vec4(p, 1.0f);
    return c.xyz() / c.w;
  };
  CHECK(approx_equal(ndc({0, 0, -0.1f}).z, 1.0f, 1.0e-4f));   // near -> 1
  CHECK(approx_equal(ndc({0, 0, -100.0f}).z, 0.0f, 1.0e-4f)); // far -> 0
  CHECK(approx_equal(ndc({1, 0, -1}).x, 1.0f, 1.0e-5f));      // 90 degree fov: x = z at the edge
  const Mat4 infinite = perspective_reversed_z(radians(60.0f), 16.0f / 9.0f, 0.5f);
  const Vec4 c = infinite * Vec4(0, 0, -1.0e6f, 1);
  CHECK(c.z / c.w >= 0.0f);
  CHECK(c.z / c.w < 1.0e-5f);

  const Mat4 ortho = orthographic_reversed_z(-2, 2, -1, 1, 0, 10);
  CHECK(approx_equal(transform_point(ortho, {2, 1, 0}), Vec3{1, 1, 1}, 1.0e-6f));
  CHECK(approx_equal(transform_point(ortho, {-2, -1, -10}), Vec3{-1, -1, 0}, 1.0e-6f));
}
