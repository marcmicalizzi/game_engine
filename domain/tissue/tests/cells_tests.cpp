// The ten-node cell (docs/subsystems/tissue.md, "The ten-node cell"): its exact volume and its
// Jacobian's sign from the Bernstein coefficients, checked against det J computed another way (the
// shape functions' gradients, integrated by a Gauss rule on the collapsed cube that is exact for
// the cubic), and its same-node linear subdivision.
#include "../src/cells.h"
#include "../src/mesh_query.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace engine;
using namespace engine::tissue;
using query::D3;

namespace {

// Gmsh's order: the edge node of each corner pair.
constexpr u32 k_node[4][4] = {{~0u, 4, 6, 7}, {4, ~0u, 5, 9}, {6, 5, ~0u, 8}, {7, 9, 8, ~0u}};

// A straight-sided ten-node cell over four corners: every edge node at its edge's midpoint.
void straight(D3 a, D3 b, D3 c, D3 d, D3 x[10]) {
  x[0] = a;
  x[1] = b;
  x[2] = c;
  x[3] = d;
  for (u32 k = 0; k < 6; ++k) {
    const u32 i = cells::k_quadratic_edges[k][0];
    const u32 j = cells::k_quadratic_edges[k][1];
    x[4 + k] = (x[i] + x[j]) * 0.5;
  }
}

// det J at reference point (xi1, xi2, xi3) from the shape functions' gradients: N_v = l_v (2 l_v -
// 1), N_ab = 4 l_a l_b, l_0 = 1 - xi1 - xi2 - xi3. Nothing shared with cells.cpp but the order.
f64 det_j(const D3 x[10], f64 xi1, f64 xi2, f64 xi3) {
  const f64 l[4] = {1.0 - xi1 - xi2 - xi3, xi1, xi2, xi3};
  // d l_m / d xi_k: -1 for m = 0, the identity otherwise.
  const auto dl = [](u32 m, u32 k) { return m == 0 ? -1.0 : (m == k + 1 ? 1.0 : 0.0); };
  D3 col[3];
  for (u32 k = 0; k < 3; ++k) {
    D3 sum;
    for (u32 v = 0; v < 4; ++v)
      sum = sum + x[v] * ((4.0 * l[v] - 1.0) * dl(v, k));
    for (u32 a = 0; a < 4; ++a)
      for (u32 b = a + 1; b < 4; ++b)
        sum = sum + x[k_node[a][b]] * (4.0 * (dl(a, k) * l[b] + l[a] * dl(b, k)));
    col[k] = sum;
  }
  return query::dot(col[0], query::cross(col[1], col[2]));
}

// The integral of det J over the reference tetrahedron: the Duffy map from the unit cube and a
// four-point Gauss-Legendre rule a direction, exact for the degrees involved (det J is cubic and
// the map's Jacobian adds two).
f64 integrated_volume(const D3 x[10]) {
  const f64 g[4] = {0.0694318442029737, 0.3300094782075719, 0.6699905217924281, 0.9305681557970263};
  const f64 w[4] = {0.1739274225687269, 0.3260725774312731, 0.3260725774312731, 0.1739274225687269};
  f64 sum = 0.0;
  for (u32 i = 0; i < 4; ++i)
    for (u32 j = 0; j < 4; ++j)
      for (u32 k = 0; k < 4; ++k) {
        const f64 u = g[i];
        const f64 v = g[j];
        const f64 t = g[k];
        const f64 xi1 = u;
        const f64 xi2 = v * (1.0 - u);
        const f64 xi3 = t * (1.0 - u) * (1.0 - v);
        sum += w[i] * w[j] * w[k] * (1.0 - u) * (1.0 - u) * (1.0 - v) * det_j(x, xi1, xi2, xi3);
      }
  return sum;
}

// The least det J over a lattice of the reference tetrahedron, n points an edge.
f64 sampled_min(const D3 x[10], u32 n, f64* max_out = nullptr) {
  f64 lo = 1e300;
  f64 hi = -1e300;
  for (u32 i = 0; i <= n; ++i)
    for (u32 j = 0; i + j <= n; ++j)
      for (u32 k = 0; i + j + k <= n; ++k) {
        const f64 v =
            det_j(x, static_cast<f64>(i) / n, static_cast<f64>(j) / n, static_cast<f64>(k) / n);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
      }
  if (max_out != nullptr) *max_out = hi;
  return lo;
}

// A reproducible sequence in [-1, 1].
struct Lcg {
  u64 state = 0x2545F4914F6CDD1Dull;
  f64 next() {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<f64>(state >> 11) / static_cast<f64>(1ull << 53) * 2.0 - 1.0;
  }
};

const D3 k_a{0.0, 0.0, 0.0};
const D3 k_b{1.0, 0.1, 0.0};
const D3 k_c{0.2, 0.9, 0.1};
const D3 k_d{0.1, 0.2, 0.8};

}  // namespace

TEST_CASE("cells: a straight ten-node cell is its corner tetrahedron, exactly") {
  D3 x[10];
  straight(k_a, k_b, k_c, k_d, x);
  const f64 v = query::tet_volume(k_a, k_b, k_c, k_d);
  CHECK(std::fabs(cells::quadratic_volume(x) - v) <= 1e-15 * v);
  f64 b[20];
  cells::jacobian_bernstein(x, b);
  for (const f64 c : b)
    CHECK(std::fabs(c - 6.0 * v) <= 1e-14 * v);
  const cells::JacobianCheck j = cells::quadratic_jacobian(x, 4);
  CHECK(j.result == cells::JacobianCheck::Result::positive);
  CHECK(j.depth == 0);
  CHECK(std::fabs(j.lower / j.upper - 1.0) < 1e-12);
}

TEST_CASE("cells: a curved cell's volume is the integral of det J, exactly") {
  Lcg random;
  for (u32 trial = 0; trial < 50; ++trial) {
    D3 x[10];
    straight(k_a, k_b, k_c, k_d, x);
    for (u32 k = 0; k < 10; ++k)
      x[k] = x[k] + D3{random.next(), random.next(), random.next()} * 0.08;
    const f64 exact = integrated_volume(x);
    CAPTURE(trial);
    CHECK(std::fabs(cells::quadratic_volume(x) - exact) <= 1e-13 * std::fabs(exact));
  }
}

TEST_CASE("cells: the Bernstein bounds hold det J, and decide its sign") {
  Lcg random;
  u32 inside_only = 0;
  for (u32 trial = 0; trial < 400; ++trial) {
    D3 x[10];
    straight(k_a, k_b, k_c, k_d, x);
    // Mild trials first, then edge nodes moved far enough to fold some cells.
    const f64 scale = trial < 200 ? 0.05 : 0.35;
    for (u32 k = 4; k < 10; ++k)
      x[k] = x[k] + D3{random.next(), random.next(), random.next()} * scale;
    f64 hi = 0.0;
    const f64 lo = sampled_min(x, 24, &hi);
    const cells::JacobianCheck j = cells::quadratic_jacobian(x, 6);
    CAPTURE(trial);
    CAPTURE(lo);
    // Unless the search stopped at the piece that proved the cell inverted, the bounds cover the
    // whole cell and bracket every sample; and a certified cell has no sample at or below zero.
    if (j.result != cells::JacobianCheck::Result::inverted) {
      CHECK(j.lower <= lo + 1e-12);
      CHECK(j.upper >= hi - 1e-12);
    }
    if (j.result == cells::JacobianCheck::Result::positive) CHECK(lo > 0.0);
    // A sample at or below zero is an inverted cell, found as one, wherever the zero is.
    if (lo < -1e-9) CHECK(j.result == cells::JacobianCheck::Result::inverted);
    // An inverted verdict is backed by a point: the sampled value is det J somewhere.
    if (j.result == cells::JacobianCheck::Result::inverted) CHECK(j.sampled <= 0.0);
    if (lo < -1e-9) {
      bool corners_positive = true;
      const f64 corner[4][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
      for (const auto& c : corner)
        corners_positive = corners_positive && det_j(x, c[0], c[1], c[2]) > 0.0;
      if (corners_positive) ++inside_only;
    }
  }
  // Some of the folded cells are positive at every corner and inverted inside: the case only the
  // subdivision finds.
  MESSAGE(inside_only << " cells inverted inside with every corner positive");
  CHECK(inside_only > 0);
}

TEST_CASE("cells: the same-node subdivision tiles the cell, whichever diagonal it takes") {
  // The reference cell squeezed to half along each diagonal of its octahedron in turn, which makes
  // that diagonal the shortest (a linear map keeps the edge nodes at their midpoints), then the
  // reference cell itself, where all three tie and the first is taken.
  const u32 cell[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  const D3 reference[4] = {D3{0, 0, 0}, D3{1, 0, 0}, D3{0, 1, 0}, D3{0, 0, 1}};
  const D3 diagonals[3] = {D3{0.5, -0.5, -0.5}, D3{-0.5, 0.5, -0.5}, D3{-0.5, -0.5, 0.5}};
  const u32 ends[3][2] = {{4, 8}, {6, 9}, {7, 5}};
  for (u32 squeeze = 0; squeeze < 4; ++squeeze) {
    D3 corners[4];
    for (u32 k = 0; k < 4; ++k) {
      corners[k] = reference[k];
      if (squeeze < 3) {
        const D3 n = query::unit(diagonals[squeeze]);
        corners[k] = corners[k] - n * (0.5 * query::dot(corners[k], n));
      }
    }
    D3 x[10];
    straight(corners[0], corners[1], corners[2], corners[3], x);
    const f64 total = query::tet_volume(x[0], x[1], x[2], x[3]);
    REQUIRE(total > 0.0);
    u32 pieces[32];
    cells::subdivide(cell, x, pieces);
    f64 sum = 0.0;
    for (u32 p = 0; p < 8; ++p) {
      const f64 v = query::tet_volume(x[pieces[4 * p]], x[pieces[4 * p + 1]], x[pieces[4 * p + 2]],
                                      x[pieces[4 * p + 3]]);
      CAPTURE(squeeze);
      CAPTURE(p);
      CHECK(v > 0.0);
      sum += v;
    }
    CHECK(std::fabs(sum - total) <= 1e-14 * total);
    // The diagonal it chose is an edge of four of its pieces: the squeezed one, or the first.
    const u32 chosen = squeeze < 3 ? squeeze : 0;
    u32 carrying = 0;
    for (u32 p = 0; p < 8; ++p) {
      bool a = false;
      bool b = false;
      for (u32 k = 0; k < 4; ++k) {
        a = a || pieces[4 * p + k] == ends[chosen][0];
        b = b || pieces[4 * p + k] == ends[chosen][1];
      }
      if (a && b) ++carrying;
    }
    CAPTURE(squeeze);
    CHECK(carrying == 4);
  }
}

TEST_CASE("cells: the kinds this build reads, and their blocks") {
  CHECK(cells::nodes_per_cell(CageKind::Tetrahedral) == 4);
  CHECK(cells::nodes_per_cell(CageKind::TetrahedralQuadratic) == 10);
  CHECK(cells::nodes_per_cell(CageKind::Lattice) == 0);
  CHECK(cells::cell_block(CageKind::TetrahedralQuadratic) == BlockKind::QuadraticTetrahedra);
  CHECK(cells::cell_block(CageKind::Tetrahedral) == BlockKind::Tetrahedra);
}
