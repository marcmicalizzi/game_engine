// The declared energy the equilibrium-gap row evaluates (src/energy.h): zero at rest, a gradient
// that is the energy's own (against central differences), and the two calibrations that make the
// declared moduli mean what they say — a dilation sees the bulk modulus K for any cell, and the
// five independent shears see mu on average for any cell — plus the anisotropy a single regular
// cell keeps, which is what the law is, not an error of it.
#include "../src/energy.h"
#include "../src/mesh_query.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace engine;
using namespace engine::tissue;
using namespace engine::tissue::query;

namespace {

constexpr f64 k_bulk = 100.0e3;
constexpr f64 k_shear = 1.0e3;

// Two irregular tetrahedra sharing a face, both positive.
const D3 k_points[] = {D3{0.00, 0.00, 0.00}, D3{0.031, 0.002, -0.004}, D3{0.004, 0.027, 0.003},
                       D3{0.006, 0.005, 0.034}, D3{0.030, 0.029, 0.026}};
const u32 k_tets[] = {0, 1, 2, 3, 1, 4, 2, 3};

// A symmetric 3x3 strain applied about the origin: x' = (I + s B) x.
struct Sym {
  f64 m[3][3] = {};
};
D3 apply(const Sym& b, f64 s, D3 p) {
  return D3{p.x + s * (b.m[0][0] * p.x + b.m[0][1] * p.y + b.m[0][2] * p.z),
            p.y + s * (b.m[1][0] * p.x + b.m[1][1] * p.y + b.m[1][2] * p.z),
            p.z + s * (b.m[2][0] * p.x + b.m[2][1] * p.y + b.m[2][2] * p.z)};
}

// An orthonormal basis of the traceless symmetric strains (Frobenius norm one each).
Vector<Sym> shear_basis() {
  Vector<Sym> out;
  const f64 r2 = 1.0 / std::sqrt(2.0);
  const f64 r6 = 1.0 / std::sqrt(6.0);
  Sym b;
  b = Sym{};
  b.m[0][1] = b.m[1][0] = r2;
  out.push_back(b);
  b = Sym{};
  b.m[0][2] = b.m[2][0] = r2;
  out.push_back(b);
  b = Sym{};
  b.m[1][2] = b.m[2][1] = r2;
  out.push_back(b);
  b = Sym{};
  b.m[0][0] = r2;
  b.m[1][1] = -r2;
  out.push_back(b);
  b = Sym{};
  b.m[0][0] = r6;
  b.m[1][1] = r6;
  b.m[2][2] = -2.0 * r6;
  out.push_back(b);
  return out;
}

energy::BulkEdgeEnergy build(std::span<const D3> points, std::span<const u32> tets) {
  Vector<energy::CellMaterial> materials;
  for (u32 t = 0; t < tets.size() / 4; ++t)
    materials.push_back(energy::CellMaterial{k_bulk, k_shear});
  energy::BulkEdgeEnergy model;
  std::string error;
  REQUIRE_MESSAGE(model.build(points, tets, materials, &error), error);
  return model;
}

f64 total(const energy::BulkEdgeEnergy& model, std::span<const D3> x) {
  const energy::BulkEdgeEnergy::Terms t = model.energy(x);
  return t.bulk + t.edge;
}

f64 rest_volume(std::span<const D3> p, std::span<const u32> tets) {
  f64 sum = 0.0;
  for (u32 t = 0; t + 3 < tets.size(); t += 4)
    sum += tet_volume(p[tets[t]], p[tets[t + 1]], p[tets[t + 2]], p[tets[t + 3]]);
  return sum;
}

}  // namespace

TEST_CASE("energy: the declared energy and its gradient are zero at rest") {
  const energy::BulkEdgeEnergy model = build(k_points, k_tets);
  CHECK(model.cell_count() == 2);
  CHECK(model.edge_count() == 9);
  const energy::BulkEdgeEnergy::Terms t = model.energy(k_points);
  CHECK(t.bulk == 0.0);
  CHECK(t.edge == 0.0);
  Vector<D3> bulk(5);
  Vector<D3> edge(5);
  model.add_gradient(k_points, std::span<D3>(bulk.data(), 5), std::span<D3>(edge.data(), 5));
  for (u32 i = 0; i < 5; ++i) {
    CHECK(length(bulk[i]) == 0.0);
    CHECK(length(edge[i]) == 0.0);
  }
}

TEST_CASE("energy: the gradient is the energy's own, against central differences") {
  const energy::BulkEdgeEnergy model = build(k_points, k_tets);
  // A deformation well outside the linear range, so both terms are nonlinear.
  Vector<D3> x;
  const D3 moves[] = {D3{0.001, -0.002, 0.0005}, D3{-0.003, 0.001, 0.002}, D3{0.002, 0.004, -0.001},
                      D3{-0.001, -0.002, -0.004}, D3{0.003, -0.001, 0.002}};
  for (u32 i = 0; i < 5; ++i)
    x.push_back(k_points[i] + moves[i]);
  Vector<D3> bulk(5);
  Vector<D3> edge(5);
  model.add_gradient(x, std::span<D3>(bulk.data(), 5), std::span<D3>(edge.data(), 5));
  const f64 h = 1.0e-7;
  f64 worst = 0.0;
  f64 largest = 0.0;
  for (u32 i = 0; i < 5; ++i) {
    for (u32 axis = 0; axis < 3; ++axis) {
      Vector<D3> plus = x;
      Vector<D3> minus = x;
      f64* p = axis == 0 ? &plus[i].x : axis == 1 ? &plus[i].y : &plus[i].z;
      f64* m = axis == 0 ? &minus[i].x : axis == 1 ? &minus[i].y : &minus[i].z;
      *p += h;
      *m -= h;
      const f64 numeric = (total(model, plus) - total(model, minus)) / (2.0 * h);
      const D3 g = bulk[i] + edge[i];
      const f64 analytic = axis == 0 ? g.x : axis == 1 ? g.y : g.z;
      worst = std::max(worst, std::fabs(numeric - analytic));
      largest = std::max(largest, std::fabs(analytic));
    }
  }
  MESSAGE("largest gradient component " << largest << " N, worst difference " << worst << " N");
  CHECK(largest > 1.0);
  CHECK(worst < 1.0e-6 * largest);
}

TEST_CASE("energy: a dilation sees the bulk modulus, whatever the cells' shapes") {
  const energy::BulkEdgeEnergy model = build(k_points, k_tets);
  const f64 v0 = rest_volume(k_points, k_tets);
  for (const f64 s : {1.0e-4, -1.0e-4}) {
    Vector<D3> x;
    for (const D3& p : k_points)
      x.push_back(p * (1.0 + s));
    // (K / 2) (tr e)^2 V0 at small strain; the edges carry 5 mu / 3 of it.
    const f64 expected = 0.5 * k_bulk * 9.0 * s * s * v0;
    const f64 got = total(model, x);
    MESSAGE("dilation " << s << ": " << got << " J against " << expected << " J");
    CHECK(std::fabs(got - expected) < 1.0e-3 * expected);
  }
}

TEST_CASE("energy: the five independent shears see the shear modulus on average, for any cell") {
  // One irregular cell alone, so nothing averages across cells.
  const D3 cell[] = {k_points[0], k_points[1], k_points[2], k_points[3]};
  const u32 tet[] = {0, 1, 2, 3};
  const energy::BulkEdgeEnergy model = build(cell, tet);
  const f64 v0 = rest_volume(cell, tet);
  const f64 s = 1.0e-4;
  f64 sum = 0.0;
  f64 smallest = 1.0e300;
  f64 largest = 0.0;
  for (const Sym& b : shear_basis()) {
    Vector<D3> x;
    for (const D3& p : cell)
      x.push_back(apply(b, s, p));
    const f64 e = total(model, x);
    sum += e;
    smallest = std::min(smallest, e);
    largest = std::max(largest, e);
  }
  // mu |e|^2 V0 with |e| = s.
  const f64 expected = k_shear * s * s * v0;
  MESSAGE("mean over five shears " << sum / 5.0 << " J against " << expected << " J; one shear "
                                   << smallest / expected << " to " << largest / expected
                                   << " of it");
  CHECK(std::fabs(sum / 5.0 - expected) < 1.0e-3 * expected);
  // The cell alone is anisotropic, which the average hides and the law does not claim otherwise.
  CHECK(largest > 1.05 * smallest);
}

TEST_CASE(
    "energy: a regular cell carries 1.25 mu along off-diagonal shears and 0.625 mu along "
    "diagonal ones") {
  const D3 cell[] = {D3{1, 1, 1}, D3{1, -1, -1}, D3{-1, 1, -1}, D3{-1, -1, 1}};
  const u32 tet[] = {0, 2, 1, 3};
  CHECK(tet_volume(cell[0], cell[2], cell[1], cell[3]) > 0.0);
  const energy::BulkEdgeEnergy model = build(cell, tet);
  const f64 v0 = rest_volume(cell, tet);
  const f64 s = 1.0e-5;
  const Vector<Sym> basis = shear_basis();
  const f64 factors[] = {1.25, 1.25, 1.25, 0.625, 0.625};
  for (u32 k = 0; k < 5; ++k) {
    Vector<D3> x;
    for (const D3& p : cell)
      x.push_back(apply(basis[k], s, p));
    const f64 ratio = total(model, x) / (k_shear * s * s * v0);
    CHECK(std::fabs(ratio - factors[k]) < 1.0e-3);
  }
}

TEST_CASE("energy: build refuses what the law cannot represent") {
  energy::BulkEdgeEnergy model;
  std::string error;
  // Poisson's ratio below 1/4: K < 5 mu / 3.
  const energy::CellMaterial cork[] = {energy::CellMaterial{1.0e3, 1.0e3}};
  const D3 cell[] = {k_points[0], k_points[1], k_points[2], k_points[3]};
  const u32 tet[] = {0, 1, 2, 3};
  CHECK_FALSE(model.build(cell, tet, cork, &error));
  CHECK(error.find("5 mu / 3") != std::string::npos);
  // An inverted rest.
  const u32 inverted[] = {0, 2, 1, 3};
  const energy::CellMaterial tissue[] = {energy::CellMaterial{k_bulk, k_shear}};
  CHECK_FALSE(model.build(cell, inverted, tissue, &error));
  CHECK(error.find("positive rest volume") != std::string::npos);
}
