// The geometric queries the validators stand on (src/mesh_query.h), each against a case whose
// answer is known in closed form: a unit box, a regular tetrahedron, a translated patch.
#include "../src/mesh_query.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace engine;
using namespace engine::tissue;
using namespace engine::tissue::query;

namespace {

// The unit box [0, 1]^3, wound outward.
const Vec3 k_box[] = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0}, Vec3{0, 1, 0},
                      Vec3{0, 0, 1}, Vec3{1, 0, 1}, Vec3{1, 1, 1}, Vec3{0, 1, 1}};
const u32 k_box_faces[] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                           1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};

Vector<D3> box_points() {
  Vector<D3> out;
  for (const Vec3& p : k_box)
    out.push_back(d3(p));
  return out;
}

}  // namespace

TEST_CASE("mesh query: the winding number is 1 inside a closed surface and 0 outside") {
  const Vector<D3> box = box_points();
  CHECK(std::fabs(winding_number(D3{0.5, 0.5, 0.5}, box, k_box_faces) - 1.0) < 1e-12);
  CHECK(std::fabs(winding_number(D3{0.01, 0.99, 0.5}, box, k_box_faces) - 1.0) < 1e-12);
  CHECK(std::fabs(winding_number(D3{1.5, 0.5, 0.5}, box, k_box_faces)) < 1e-12);
  CHECK(std::fabs(winding_number(D3{-3.0, 7.0, 2.0}, box, k_box_faces)) < 1e-12);
  CHECK(std::fabs(enclosed_volume(box, k_box_faces) - 1.0) < 1e-15);
}

TEST_CASE("mesh query: nearest points carry the side of the surface they are on") {
  SignedSurface box;
  box.build(k_box, k_box_faces);
  const SignedSurface::Result inside = box.query(D3{0.5, 0.5, 0.9});
  CHECK(std::fabs(inside.distance + 0.1) < 1e-12);
  const SignedSurface::Result outside = box.query(D3{0.5, 0.5, 1.25});
  CHECK(std::fabs(outside.distance - 0.25) < 1e-12);
  // Off a corner and off an edge the pseudonormal still gives the right sign.
  const SignedSurface::Result corner = box.query(D3{1.1, 1.1, 1.1});
  CHECK(corner.distance > 0.0);
  CHECK(std::fabs(corner.distance - std::sqrt(0.03)) < 1e-12);
  const SignedSurface::Result edge = box.query(D3{1.2, 0.5, 1.2});
  CHECK(edge.distance > 0.0);
  const SignedSurface::Result near_corner_inside = box.query(D3{0.99, 0.98, 0.97});
  CHECK(near_corner_inside.distance < 0.0);
}

TEST_CASE("mesh query: rays find the first triangle and which side they leave through") {
  TriangleBvh box;
  box.build(k_box, k_box_faces);
  const TriangleBvh::Hit up = box.first_hit(D3{0.3, 0.4, 0.5}, D3{0, 0, 1}, 0.0, 10.0);
  CHECK(up.triangle != k_none);
  CHECK(std::fabs(up.t - 0.5) < 1e-12);
  CHECK(up.facing > 0.99);  // leaving through the top's front
  const TriangleBvh::Hit in = box.first_hit(D3{0.3, 0.4, 2.0}, D3{0, 0, -1}, 0.0, 10.0);
  CHECK(std::fabs(in.t - 1.0) < 1e-12);
  CHECK(in.facing < -0.99);  // entering through the top's front
  CHECK(box.first_hit(D3{2.0, 2.0, 2.0}, D3{0, 0, 1}, 0.0, 10.0).triangle == k_none);
  CHECK(box.first_hit(D3{0.3, 0.4, 0.5}, D3{0, 0, 1}, 0.0, 0.4).triangle == k_none);
}

TEST_CASE("mesh query: triangle pairs intersect when they cross and not when they miss") {
  const D3 a0{0, 0, 0}, a1{1, 0, 0}, a2{0, 1, 0};
  CHECK(triangles_intersect(a0, a1, a2, D3{0.2, 0.2, -1}, D3{0.2, 0.2, 1}, D3{0.6, 0.2, 0}));
  CHECK_FALSE(
      triangles_intersect(a0, a1, a2, D3{0.2, 0.2, 0.1}, D3{0.8, 0.2, 0.1}, D3{0.2, 0.8, 0.1}));
  CHECK_FALSE(triangles_intersect(a0, a1, a2, D3{2, 2, -1}, D3{2, 2, 1}, D3{3, 2, 0}));
  // Coplanar: overlapping, and apart.
  CHECK(triangles_intersect(a0, a1, a2, D3{0.1, 0.1, 0}, D3{2, 0.1, 0}, D3{0.1, 2, 0}));
  CHECK_FALSE(triangles_intersect(a0, a1, a2, D3{2, 2, 0}, D3{3, 2, 0}, D3{2, 3, 0}));
  // Two boxes, one sunk halfway into the other: their surfaces cross; one well inside: they do not.
  TriangleBvh box;
  box.build(k_box, k_box_faces);
  Vector<Vec3> shifted;
  for (const Vec3& p : k_box)
    shifted.push_back(p + Vec3{0.5f, 0.25f, 0.3f});
  TriangleBvh other;
  other.build(shifted, k_box_faces);
  Vector<std::pair<u32, u32>> witnesses;
  CHECK(box.intersections(other, 4, &witnesses) > 0);
  CHECK(!witnesses.empty());
  Vector<Vec3> small;
  for (const Vec3& p : k_box)
    small.push_back(p * 0.2f + Vec3{0.4f, 0.4f, 0.4f});
  TriangleBvh inner;
  inner.build(small, k_box_faces);
  CHECK(box.intersections(inner, 4, nullptr) == 0);
}

TEST_CASE("mesh query: tetrahedron quality is 1 for a regular one, 0 flat, negative inverted") {
  const D3 a{0, 0, 0};
  const D3 b{1, 0, 0};
  const D3 c{0.5, std::sqrt(3.0) / 2.0, 0};
  const D3 d{0.5, std::sqrt(3.0) / 6.0, std::sqrt(2.0 / 3.0)};
  CHECK(std::fabs(tet_sicn(a, b, c, d) - 1.0) < 1e-12);
  CHECK(std::fabs(tet_sicn(b, c, a, d) - 1.0) < 1e-12);                          // any corner first
  CHECK(std::fabs(tet_sicn(a * 7.0, b * 7.0, c * 7.0, d * 7.0) - 1.0) < 1e-12);  // scale free
  CHECK(std::fabs(tet_sicn(a, b, c, D3{0.5, 0.3, 0.0})) < 1e-12);                // flat
  CHECK(tet_sicn(a, c, b, d) < -0.999);                                          // inverted
  CHECK(tet_sicn(a, b, c, D3{0.5, 0.3, 0.05}) < 0.3);                            // a sliver
  CHECK(tet_volume(a, b, c, d) > 0.0);
}

TEST_CASE("mesh query: a swept volume is the volume between the two positions") {
  // A flat 1 m square of two triangles, moved 3 mm along its normal: 0.003 cubic metres.
  const D3 p[4] = {D3{0, 0, 0}, D3{1, 0, 0}, D3{1, 1, 0}, D3{0, 1, 0}};
  const D3 lift{0, 0, 0.003};
  const f64 v = swept_volume(p[0], p[1], p[2], p[0] + lift, p[1] + lift, p[2] + lift) +
                swept_volume(p[0], p[2], p[3], p[0] + lift, p[2] + lift, p[3] + lift);
  CHECK(std::fabs(v - 0.003) < 1e-15);
  // A closed surface swept by a rigid motion changes no volume.
  const Vector<D3> box = box_points();
  f64 total = 0.0;
  for (u32 t = 0; t < 36; t += 3) {
    const auto moved = [](D3 x) {
      return D3{x.x * 0.8 - x.y * 0.6 + 0.3, x.x * 0.6 + x.y * 0.8, x.z - 0.2};
    };
    total += swept_volume(box[k_box_faces[t]], box[k_box_faces[t + 1]], box[k_box_faces[t + 2]],
                          moved(box[k_box_faces[t]]), moved(box[k_box_faces[t + 1]]),
                          moved(box[k_box_faces[t + 2]]));
  }
  CHECK(std::fabs(total) < 1e-12);
  // And one scaled by 2 in z gains exactly its volume.
  f64 grown = 0.0;
  for (u32 t = 0; t < 36; t += 3) {
    const auto moved = [](D3 x) { return D3{x.x, x.y, 2.0 * x.z}; };
    grown += swept_volume(box[k_box_faces[t]], box[k_box_faces[t + 1]], box[k_box_faces[t + 2]],
                          moved(box[k_box_faces[t]]), moved(box[k_box_faces[t + 1]]),
                          moved(box[k_box_faces[t + 2]]));
  }
  CHECK(std::fabs(grown - 1.0) < 1e-12);
}

TEST_CASE("mesh query: triangle distances, and a nearly coplanar pair decided in its plane") {
  // Two parallel unit triangles 2 mm apart, one over the other: 2 mm.
  const D3 a0{0, 0, 0}, a1{1, 0, 0}, a2{0, 1, 0};
  const D3 up{0, 0, 0.002};
  CHECK(std::fabs(triangle_distance(a0, a1, a2, a0 + up, a1 + up, a2 + up) - 0.002) < 1e-15);
  // Side by side in one plane, 0.5 apart along x: the edge-to-edge distance.
  const D3 right{1.5, 0, 0};
  CHECK(std::fabs(triangle_distance(a0, a1, a2, a0 + right, a1 + right, a2 + right) - 0.5) < 1e-15);
  // Crossing in general position: zero, and both tests agree.
  const D3 b0{0.25, 0.25, -0.5}, b1{0.25, 0.25, 0.5}, b2{0.9, 0.9, 0.0};
  CHECK(triangles_cross(a0, a1, a2, b0, b1, b2));
  CHECK(triangles_intersect(a0, a1, a2, b0, b1, b2));
  CHECK(triangle_distance(a0, a1, a2, b0, b1, b2) == 0.0);
  // Nearly coplanar and apart in the plane — a flat surface's own chords, a picometre off flat —
  // is apart; nearly coplanar and overlapping in the plane is crossing.
  const D3 noise{0, 0, 1e-12};
  const D3 far0 = a0 + right + noise, far1 = a1 + right, far2 = a2 + right - noise;
  CHECK_FALSE(triangles_cross(a0, a1, a2, far0, far1, far2));
  CHECK(triangle_distance(a0, a1, a2, far0, far1, far2) > 0.49);
  const D3 shift{0.2, 0.2, 0};
  CHECK(triangles_cross(a0, a1, a2, a0 + shift + noise, a1 + shift, a2 + shift - noise));
  // Segments: skew, parallel, and end to end.
  CHECK(std::fabs(segment_distance(D3{0, 0, 0}, D3{1, 0, 0}, D3{0.5, -1, 1}, D3{0.5, 1, 1}) - 1.0) <
        1e-15);
  CHECK(std::fabs(segment_distance(D3{0, 0, 0}, D3{1, 0, 0}, D3{0, 1, 0}, D3{1, 1, 0}) - 1.0) <
        1e-15);
  CHECK(std::fabs(segment_distance(D3{0, 0, 0}, D3{1, 0, 0}, D3{3, 0, 0}, D3{4, 0, 0}) - 2.0) <
        1e-15);
}

TEST_CASE("mesh query: the candidate pairs of two hierarchies are every pair within reach") {
  // Two rows of small triangles 1 mm apart: within 1.5 mm every facing pair is a candidate, within
  // 0.5 mm none is.
  Vector<D3> a;
  Vector<D3> b;
  Vector<u32> t;
  for (u32 i = 0; i < 8; ++i) {
    const f64 x = 0.01 * i;
    a.push_back(D3{x, 0, 0});
    a.push_back(D3{x + 0.008, 0, 0});
    a.push_back(D3{x, 0.008, 0});
    b.push_back(D3{x, 0, 0.001});
    b.push_back(D3{x + 0.008, 0, 0.001});
    b.push_back(D3{x, 0.008, 0.001});
    t.push_back(3 * i);
    t.push_back(3 * i + 1);
    t.push_back(3 * i + 2);
  }
  TriangleBvh ba;
  TriangleBvh bb;
  ba.build(std::span<const D3>(a.data(), a.size()), std::span<const u32>(t.data(), t.size()));
  bb.build(std::span<const D3>(b.data(), b.size()), std::span<const u32>(t.data(), t.size()));
  Vector<std::pair<u32, u32>> pairs;
  ba.candidate_pairs(bb, 0.0015, pairs);
  u32 facing = 0;
  for (const auto& [i, j] : pairs)
    facing += i == j ? 1u : 0u;
  CHECK(facing == 8);
  ba.candidate_pairs(bb, 0.0005, pairs);
  CHECK(pairs.empty());
}
