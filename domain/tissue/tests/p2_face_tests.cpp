// The six-node faces of the material boundary (src/p2_face.h): Gmsh's local faces of the ten-node
// cell, a point of a face, its area, and the bound on how far a curved face is from its four
// chords, each against a case whose answer is known in closed form.
#include "../src/cells.h"
#include "../src/p2_face.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

using namespace engine;
using namespace engine::tissue;
using query::D3;

namespace {

// A face's six nodes: the corners and the straight edges' midpoints.
void straight(const D3 c[3], D3 x[6]) {
  x[0] = c[0];
  x[1] = c[1];
  x[2] = c[2];
  x[3] = (c[0] + c[1]) * 0.5;
  x[4] = (c[1] + c[2]) * 0.5;
  x[5] = (c[2] + c[0]) * 0.5;
}

}  // namespace

TEST_CASE("p2 face: Gmsh's local faces are the cell's own, wound outward, with their edge nodes") {
  // The reference tetrahedron: every local face's corners face away from the corner it lacks, and
  // its three edge nodes are the cell's nodes of its three edges, in (a, b), (b, c), (c, a) order.
  const D3 corner[4] = {D3{0, 0, 0}, D3{1, 0, 0}, D3{0, 1, 0}, D3{0, 0, 1}};
  for (u32 f = 0; f < 4; ++f) {
    const u32* c = p2::k_face_corners[f];
    const D3 n = query::cross(corner[c[1]] - corner[c[0]], corner[c[2]] - corner[c[0]]);
    const D3 centre = (corner[c[0]] + corner[c[1]] + corner[c[2]]) * (1.0 / 3.0);
    CHECK(query::dot(n, centre - corner[p2::k_face_opposite[f]]) > 0.0);
    for (u32 k = 0; k < 3; ++k) {
      CHECK(p2::k_face_nodes[f][k] == c[k]);
      const u32 a = c[k];
      const u32 b = c[(k + 1) % 3];
      const u32 edge_node = p2::k_face_nodes[f][3 + k];
      const u32 e0 = cells::k_quadratic_edges[edge_node - 4][0];
      const u32 e1 = cells::k_quadratic_edges[edge_node - 4][1];
      CHECK(((e0 == a && e1 == b) || (e0 == b && e1 == a)));
    }
  }
}

TEST_CASE(
    "p2 face: a straight face is its triangle, and a lifted one curves by its Bernstein net") {
  const D3 c[3] = {D3{0, 0, 0}, D3{0.02, 0, 0}, D3{0, 0.02, 0}};
  D3 x[6];
  straight(c, x);
  // Its nodes are where they are, its area is the triangle's, and its chords are itself.
  const f64 corner[3] = {0, 1, 0};
  CHECK(query::length(p2::evaluate(x, corner) - c[1]) < 1e-18);
  const f64 mid[3] = {0.5, 0.5, 0};
  CHECK(query::length(p2::evaluate(x, mid) - x[3]) < 1e-18);
  CHECK(std::fabs(p2::area(x) - 0.0002) < 1e-18);
  CHECK(p2::chord_deviation(x) < 1e-18);
  CHECK(p2::least_metric(x) > 0.0);
  // The node of edge (0, 1) lifted h along the normal: the surface is the plane plus 4 l0 l1 h,
  // whose largest departure from the four chords is h / 4 (at the middle of the centre chord's
  // edge (m12, m20)); the Bernstein bound is h / 2 there, conservative by what the convex hull
  // costs, and never below a sampled departure.
  const f64 h = 0.001;
  x[3].z = h;
  const f64 bound = p2::chord_deviation(x);
  CHECK(std::fabs(bound - 0.5 * h) < 1e-15);
  f64 sampled = 0.0;
  const u32 n = 40;
  for (u32 i = 0; i <= n; ++i)
    for (u32 j = 0; i + j <= n; ++j) {
      const f64 l[3] = {1.0 - static_cast<f64>(i + j) / n, static_cast<f64>(i) / n,
                        static_cast<f64>(j) / n};
      const D3 p = p2::evaluate(x, l);
      // The chord's height at this point: linear over the quarter it lies in, m01 the only lifted
      // node, at its weight in that quarter.
      f64 chord = 0.0;
      if (l[0] >= 0.5)
        chord = 2.0 * l[1] * h;  // (c0, m01, m20)
      else if (l[1] >= 0.5)
        chord = 2.0 * l[0] * h;  // (m01, c1, m12)
      else if (l[2] >= 0.5)
        chord = 0.0;  // (m20, m12, c2)
      else
        chord = (1.0 - 2.0 * l[2]) * h;  // (m01, m12, m20)
      sampled = std::max(sampled, std::fabs(p.z - chord));
    }
  CHECK(sampled == doctest::Approx(0.25 * h).epsilon(1e-9));
  CHECK(bound >= sampled);
}
