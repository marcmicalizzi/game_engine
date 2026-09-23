// Loop limit surfaces as sparse operators (docs/subsystems/geometry.md, "Limit surfaces and
// surface bindings"): the fixture cage's operator has the authoring side's size and numbering,
// every row is a nonnegative partition of unity, the operator is affine and keeps a plane, a
// regular patch reproduces the quartic box spline's closed form (positions and normals), the
// boundary is the cubic B-spline of its polygon, the tangent masks are eigenvectors of the rules
// they claim to be, the parent maps add up, corners stay put, bad meshes are refused with a
// sentence, and two builds agree to the bit.
#include "limit_fixtures.h"

#include <domain/geometry/limit_surface.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <string>

using namespace engine;
using namespace engine::geometry;

namespace {

LoopLimitSurface build(std::span<const u32> faces, u32 vertices, u32 level, bool tangents = true,
                       std::span<const u32> corners = {}) {
  LoopSurfaceOptions options;
  options.level = level;
  options.tangents = tangents;
  options.corners = corners;
  LoopLimitSurface surface;
  std::string error;
  const bool ok = build_loop_limit_surface(faces, vertices, options, surface, &error);
  INFO(error);
  REQUIRE(ok);
  return surface;
}

Vector<Vec3> evaluate(const CsrMatrix& matrix, std::span<const Vec3> x) {
  Vector<Vec3> out(matrix.rows());
  apply(matrix, x, std::span<Vec3>(out.data(), out.size()));
  return out;
}

Vector<Vec3> normals_of(const LoopLimitSurface& s, std::span<const Vec3> x) {
  const Vector<Vec3> tu = evaluate(s.tangent_u, x);
  const Vector<Vec3> tv = evaluate(s.tangent_v, x);
  Vector<Vec3> n(tu.size());
  limit_normals(tu, tv, std::span<Vec3>(n.data(), n.size()));
  return n;
}

// In double and through atan2: an f32 cosine cannot resolve an angle below about 2^-11.5 rad.
f64 angle_between(Vec3 a, Vec3 b) {
  const f64 ax = static_cast<f64>(a.x);
  const f64 ay = static_cast<f64>(a.y);
  const f64 az = static_cast<f64>(a.z);
  const f64 bx = static_cast<f64>(b.x);
  const f64 by = static_cast<f64>(b.y);
  const f64 bz = static_cast<f64>(b.z);
  const f64 cx = ay * bz - az * by;
  const f64 cy = az * bx - ax * bz;
  const f64 cz = ax * by - ay * bx;
  return std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), ax * bx + ay * by + az * bz);
}

f64 gap(Vec3 a, Vec3 b) {
  const Vec3 d = a - b;
  return std::sqrt(static_cast<f64>(dot(d, d)));
}

Vector<Vec3> jittered(Vector<Vec3> positions, u64 seed, f32 amount) {
  fixture::Random random(seed);
  for (Vec3& p : positions)
    p = p + Vec3{random.uniform(-amount, amount), random.uniform(-amount, amount),
                 random.uniform(-amount, amount)};
  return positions;
}

// Checks every row of a position operator: strictly increasing columns, positive weights, and a
// sum of one.
void check_partition_of_unity(const CsrMatrix& m) {
  f64 worst_sum = 0.0;
  bool positive = true;
  bool increasing = true;
  for (u32 r = 0; r < m.rows(); ++r) {
    f64 sum = 0.0;
    for (u32 e = m.row_offsets[r]; e < m.row_offsets[r + 1]; ++e) {
      positive = positive && m.weight[e] > 0.0f;
      sum += static_cast<f64>(m.weight[e]);
      if (e > m.row_offsets[r])
        increasing = increasing && m.column_index[e] > m.column_index[e - 1];
      REQUIRE(m.column_index[e] < m.column_count);
    }
    worst_sum = std::max(worst_sum, std::fabs(sum - 1.0));
  }
  CHECK(positive);
  CHECK(increasing);
  CHECK(worst_sum <= 1.0e-7);
}

// A tangent row's weights sum to zero, so a translation of the cage cannot leak into a tangent.
void check_sums_to_zero(const CsrMatrix& m) {
  f64 worst = 0.0;
  for (u32 r = 0; r < m.rows(); ++r) {
    f64 sum = 0.0;
    f64 largest = 0.0;
    for (u32 e = m.row_offsets[r]; e < m.row_offsets[r + 1]; ++e) {
      sum += static_cast<f64>(m.weight[e]);
      largest = std::max(largest, std::fabs(static_cast<f64>(m.weight[e])));
    }
    if (largest > 0.0) worst = std::max(worst, std::fabs(sum) / largest);
  }
  CHECK(worst <= 1.0e-7);
}

// The parameter locations of a refined level's vertices, from the level below by midpoints of its
// edges taken in lexicographic order — the numbering rule, independent of the subdivision weights.
Vector<Vec3> refine_parameters(std::span<const u32> faces, const Vector<Vec3>& parameters) {
  Vector<u64> keys;
  for (usize f = 0; f < faces.size() / 3; ++f) {
    for (u32 s = 0; s < 3; ++s) {
      const u32 a = faces[3 * f + s];
      const u32 b = faces[3 * f + (s + 1) % 3];
      keys.push_back((u64{std::min(a, b)} << 32) | u64{std::max(a, b)});
    }
  }
  std::sort(keys.begin(), keys.end());
  Vector<Vec3> out = parameters;
  for (u32 i = 0; i < keys.size(); ++i) {
    if (i > 0 && keys[i] == keys[i - 1]) continue;
    const u32 a = static_cast<u32>(keys[i] >> 32);
    const u32 b = static_cast<u32>(keys[i] & 0xffffffffu);
    out.push_back((parameters[a] + parameters[b]) * 0.5f);
  }
  return out;
}

}  // namespace

TEST_CASE("loop limit: the fixture cage's operator has the authoring side's size and numbering") {
  const Vector<u32> faces = fixture::ringed_disk_faces(5, 24);
  const u32 vertices = fixture::ringed_disk_vertices(5, 24);
  REQUIRE(vertices == 121);
  REQUIRE(faces.size() == 216 * 3);

  // The counts the authoring side's matrices have (the round-two preflight's table): refined
  // vertices, triangles, nonzeros, and the widest row.
  struct Expected {
    u32 level;
    u32 rows;
    u32 triangles;
    u32 nonzeros;
    u32 widest;
  };
  const Expected expected[] = {
      {1, 457, 864, 4177, 27}, {2, 1777, 3456, 19393, 28}, {3, 7009, 13824, 83233, 28}};
  for (const Expected& e : expected) {
    const LoopLimitSurface s = build(faces, vertices, e.level, false);
    CHECK(s.vertex_count() == e.rows);
    CHECK(s.limit.rows() == e.rows);
    CHECK(s.limit.column_count == vertices);
    CHECK(s.triangle_count() == e.triangles);
    CHECK(s.limit.nonzeros() == e.nonzeros);
    CHECK(s.limit.max_row_width() == e.widest);
    CHECK(s.max_boundary_faces == 3);
    MESSAGE("level " << e.level << ": " << s.limit.rows() << " rows, " << s.limit.nonzeros()
                     << " nonzeros, widest " << s.limit.max_row_width() << ", mean "
                     << static_cast<f64>(s.limit.nonzeros()) / static_cast<f64>(s.limit.rows())
                     << ", " << s.limit.bytes()
                     << " bytes as u32 offsets, u32 columns, f32 weights");
  }

  // The numbering: old vertices keep their indices and edges follow in lexicographic order, so
  // edge (0, 1) is vertex 121, (0, 2) is 122, and (1, 2) — after the centre's 24 edges — is 145;
  // triangle (a, b, c) becomes (a, ab, ca), (b, bc, ab), (c, ca, bc), (ab, bc, ca).
  const LoopLimitSurface one = build(faces, vertices, 1, false);
  const u32 first_children[12] = {0, 121, 122, 1, 145, 121, 2, 122, 145, 121, 145, 122};
  for (u32 i = 0; i < 12; ++i)
    CHECK(one.faces[i] == first_children[i]);

  // The pole: its limit is its own one-ring's mask at every level, 25 entries.
  const LoopLimitSurface three = build(faces, vertices, 3, false);
  REQUIRE(three.limit.row_offsets[1] - three.limit.row_offsets[0] == 25);
  const f64 c = 0.375 + 0.25 * std::cos(2.0 * std::numbers::pi / 24.0);
  const f64 beta = (0.625 - c * c) / 24.0;
  const f64 k = 3.0 / (8.0 * beta);
  CHECK(static_cast<f64>(three.limit.weight[0]) == doctest::Approx(k / (k + 24.0)).epsilon(2e-7));
  for (u32 e = 1; e < 25; ++e)
    CHECK(static_cast<f64>(three.limit.weight[e]) ==
          doctest::Approx(1.0 / (k + 24.0)).epsilon(1e-6));
}

TEST_CASE(
    "loop limit: every row is a nonnegative partition of unity, every tangent row sums to 0") {
  struct Mesh {
    Vector<u32> faces;
    u32 vertices;
    u32 max_level;
  };
  Vector<Mesh> meshes;
  meshes.push_back(
      Mesh{fixture::ringed_disk_faces(5, 24), fixture::ringed_disk_vertices(5, 24), 4});
  meshes.push_back(Mesh{fixture::torus_faces(8, 6), 48, 3});
  meshes.push_back(Mesh{fixture::sheet_faces(5, 4), 20, 3});
  for (u32 k = 1; k <= 7; ++k)
    meshes.push_back(Mesh{fixture::fan_faces(k), k + 2, 3});
  for (const Mesh& mesh : meshes) {
    for (u32 level = 0; level <= mesh.max_level; ++level) {
      const LoopLimitSurface s = build(mesh.faces, mesh.vertices, level);
      check_partition_of_unity(s.limit);
      check_sums_to_zero(s.tangent_u);
      check_sums_to_zero(s.tangent_v);
      CHECK(s.tangent_u.rows() == s.vertex_count());
      CHECK(s.tangent_v.rows() == s.vertex_count());
    }
  }
}

TEST_CASE("loop limit: a transformed cage gives the transformed limit positions") {
  const Vector<u32> faces = fixture::ringed_disk_faces(5, 24);
  const LoopLimitSurface s = build(faces, 121, 3, false);
  fixture::Random random(7);
  Vector<Vec3> x(121);
  for (Vec3& p : x)
    p = Vec3{random.uniform(-1.0f, 1.0f), random.uniform(-1.0f, 1.0f), random.uniform(-1.0f, 1.0f)};

  // A rotation, an anisotropic scale and a shear, then a translation.
  const f64 a = 0.7;
  const f64 m[3][3] = {{1.1 * std::cos(a), -0.9 * std::sin(a), 0.2},
                       {1.1 * std::sin(a), 0.9 * std::cos(a), 0.0},
                       {0.0, 0.15, 1.0}};
  const f64 t[3] = {0.25, -0.5, 0.125};
  const auto transform = [&](const f64 p[3], f64 out[3]) {
    for (u32 r = 0; r < 3; ++r)
      out[r] = m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + t[r];
  };
  Vector<Vec3> moved(121);
  for (u32 i = 0; i < 121; ++i) {
    const f64 p[3] = {static_cast<f64>(x[i].x), static_cast<f64>(x[i].y), static_cast<f64>(x[i].z)};
    f64 q[3];
    transform(p, q);
    moved[i] = Vec3{static_cast<f32>(q[0]), static_cast<f32>(q[1]), static_cast<f32>(q[2])};
  }
  const Vector<Vec3> limit_of_moved = evaluate(s.limit, moved);

  // The other side exactly: the same f32 weights accumulated in double on the untransformed cage,
  // then transformed in double, so the comparison sees only the f32 kernel's own rounding.
  f64 worst = 0.0;
  for (u32 r = 0; r < s.limit.rows(); ++r) {
    f64 p[3] = {0.0, 0.0, 0.0};
    for (u32 e = s.limit.row_offsets[r]; e < s.limit.row_offsets[r + 1]; ++e) {
      const Vec3 c = x[s.limit.column_index[e]];
      const f64 w = static_cast<f64>(s.limit.weight[e]);
      p[0] += w * static_cast<f64>(c.x);
      p[1] += w * static_cast<f64>(c.y);
      p[2] += w * static_cast<f64>(c.z);
    }
    f64 q[3];
    transform(p, q);
    const Vec3 got = limit_of_moved[r];
    worst = std::max({worst, std::fabs(static_cast<f64>(got.x) - q[0]),
                      std::fabs(static_cast<f64>(got.y) - q[1]),
                      std::fabs(static_cast<f64>(got.z) - q[2])});
  }
  MESSAGE("affine invariance, level 3, a unit-box cage: worst coordinate error " << worst);
  CHECK(worst <= 1.0e-6);
}

TEST_CASE("loop limit: apply matches a double-precision evaluation, in lanes and past them") {
  // `apply` copies the cage into 16-byte lanes when it has at most 1,024 nodes and falls back to a
  // scalar loop above that; a 12 x 12 sheet takes the first path and a 33 x 33 one (1,089 nodes)
  // the second, and both must be the operator's own weights summed, to f32 rounding.
  for (const u32 n : {12u, 33u}) {
    const Vector<u32> faces = fixture::sheet_faces(n, n);
    const LoopLimitSurface s = build(faces, n * n, 2, false);
    fixture::Random random(60 + n);
    Vector<Vec3> x(n * n);
    for (Vec3& p : x)
      p = Vec3{random.uniform(-1.0f, 1.0f), random.uniform(-1.0f, 1.0f),
               random.uniform(-1.0f, 1.0f)};
    const Vector<Vec3> y = evaluate(s.limit, x);
    f64 worst = 0.0;
    for (u32 r = 0; r < s.limit.rows(); ++r) {
      f64 p[3] = {0.0, 0.0, 0.0};
      for (u32 e = s.limit.row_offsets[r]; e < s.limit.row_offsets[r + 1]; ++e) {
        const Vec3 c = x[s.limit.column_index[e]];
        const f64 w = static_cast<f64>(s.limit.weight[e]);
        p[0] += w * static_cast<f64>(c.x);
        p[1] += w * static_cast<f64>(c.y);
        p[2] += w * static_cast<f64>(c.z);
      }
      worst = std::max({worst, std::fabs(static_cast<f64>(y[r].x) - p[0]),
                        std::fabs(static_cast<f64>(y[r].y) - p[1]),
                        std::fabs(static_cast<f64>(y[r].z) - p[2])});
    }
    INFO(n * n << " control vertices");
    CHECK(worst <= 1.0e-6);
  }
}

TEST_CASE("loop limit: a planar cage stays in its plane, with the plane's normal") {
  const Vector<u32> faces = fixture::ringed_disk_faces(5, 24);
  // The dome's footprint laid into a tilted plane, then jittered within the plane.
  const Vector<Vec3> flat = fixture::ringed_disk_dome(5, 24, 0.7f, 0.86f, 0.0f);
  const Vec3 e1 = normalize(Vec3{1.0f, 0.2f, 0.3f});
  const Vec3 e2 = normalize(cross(Vec3{-0.1f, 0.4f, 1.0f}, e1));
  const Vec3 n = cross(e1, e2);
  const Vec3 origin{0.3f, -0.2f, 0.5f};
  fixture::Random random(11);
  Vector<Vec3> x;
  for (const Vec3& p : flat) {
    const f32 s = p.x + random.uniform(-0.02f, 0.02f);
    const f32 t = p.y + random.uniform(-0.02f, 0.02f);
    x.push_back(origin + e1 * s + e2 * t);
  }
  const LoopLimitSurface s = build(faces, 121, 3);
  const Vector<Vec3> y = evaluate(s.limit, x);
  f64 off_plane = 0.0;
  for (const Vec3& p : y)
    off_plane = std::max(off_plane, std::fabs(static_cast<f64>(dot(p - origin, n))));
  CHECK(off_plane <= 1.0e-6);

  const Vector<Vec3> normals = normals_of(s, x);
  f64 worst = 0.0;
  for (const Vec3& m : normals)
    worst = std::max(worst, angle_between(m, n));
  MESSAGE("planar cage: " << off_plane << " off the plane, normals within " << worst << " rad");
  CHECK(worst <= 1.0e-5);
}

TEST_CASE("loop limit: a regular patch reproduces the quartic box spline, positions and normals") {
  // A regular sheet with a cubic height field. Loop's scheme on the regular three-direction
  // lattice is the quartic box spline M_222, whose limit of samples of a cubic f is exactly
  //   f(u) + (h^2 / 12) tr([[2, 1], [1, 2]] H(u)) = f(u) + (h^2 / 6) (f_xx + f_xy + f_yy)
  // (the box spline's covariance is (h^2 / 12) times the sum of its six direction outer products,
  // and its odd central moments vanish), with x and y reproduced exactly. Computed here in closed
  // form, independently of the masks; only vertices whose support stays clear of the sheet's
  // boundary rules are compared.
  constexpr u32 n = 12;
  constexpr f64 h = 0.125;
  const f64 c[10] = {0.1, 0.2, -0.15, 0.3, -0.2, 0.25, 0.12, -0.08, 0.05, -0.1};
  const auto f = [&](f64 x, f64 y) {
    return c[0] + c[1] * x + c[2] * y + c[3] * x * x + c[4] * x * y + c[5] * y * y +
           c[6] * x * x * x + c[7] * x * x * y + c[8] * x * y * y + c[9] * y * y * y;
  };
  const auto limit_height = [&](f64 x, f64 y) {
    const f64 fxx = 2.0 * c[3] + 6.0 * c[6] * x + 2.0 * c[7] * y;
    const f64 fxy = c[4] + 2.0 * c[7] * x + 2.0 * c[8] * y;
    const f64 fyy = 2.0 * c[5] + 2.0 * c[8] * x + 6.0 * c[9] * y;
    return f(x, y) + h * h / 6.0 * (fxx + fxy + fyy);
  };
  const auto limit_normal = [&](f64 x, f64 y) {
    const f64 fx =
        c[1] + 2.0 * c[3] * x + c[4] * y + 3.0 * c[6] * x * x + 2.0 * c[7] * x * y + c[8] * y * y;
    const f64 fy =
        c[2] + c[4] * x + 2.0 * c[5] * y + c[7] * x * x + 2.0 * c[8] * x * y + 3.0 * c[9] * y * y;
    const f64 zx = fx + h * h / 6.0 * (6.0 * c[6] + 2.0 * c[7] + 2.0 * c[8]);
    const f64 zy = fy + h * h / 6.0 * (2.0 * c[7] + 2.0 * c[8] + 6.0 * c[9]);
    return Vec3{static_cast<f32>(-zx), static_cast<f32>(-zy), 1.0f};
  };

  const Vector<u32> faces = fixture::sheet_faces(n, n);
  Vector<Vec3> x;
  Vector<Vec3> parameters;
  for (u32 j = 0; j < n; ++j) {
    for (u32 i = 0; i < n; ++i) {
      const f64 u = h * static_cast<f64>(i);
      const f64 v = h * static_cast<f64>(j);
      x.push_back(Vec3{static_cast<f32>(u), static_cast<f32>(v), static_cast<f32>(f(u, v))});
      parameters.push_back(Vec3{static_cast<f32>(u), static_cast<f32>(v), 0.0f});
    }
  }
  constexpr u32 level = 3;
  Vector<u32> level_faces = faces;
  for (u32 k = 0; k < level; ++k) {
    parameters = refine_parameters(level_faces, parameters);
    level_faces = build(faces, n * n, k + 1, false).faces;
  }
  const LoopLimitSurface s = build(faces, n * n, level);
  REQUIRE(parameters.size() == s.vertex_count());
  const Vector<Vec3> y = evaluate(s.limit, x);
  const Vector<Vec3> normals = normals_of(s, x);

  const f64 margin = 2.0 * h;
  const f64 far = h * static_cast<f64>(n - 1) - margin;
  u32 compared = 0;
  f64 worst_xy = 0.0;
  f64 worst_z = 0.0;
  f64 worst_angle = 0.0;
  for (u32 i = 0; i < s.vertex_count(); ++i) {
    const f64 u = static_cast<f64>(parameters[i].x);
    const f64 v = static_cast<f64>(parameters[i].y);
    if (u < margin || v < margin || u > far || v > far) continue;
    ++compared;
    worst_xy = std::max({worst_xy, std::fabs(static_cast<f64>(y[i].x) - u),
                         std::fabs(static_cast<f64>(y[i].y) - v)});
    worst_z = std::max(worst_z, std::fabs(static_cast<f64>(y[i].z) - limit_height(u, v)));
    worst_angle = std::max(worst_angle, angle_between(normals[i], limit_normal(u, v)));
  }
  MESSAGE("box spline: " << compared << " interior vertices, x/y within " << worst_xy
                         << ", height within " << worst_z << ", normals within " << worst_angle
                         << " rad");
  CHECK(compared > 3000);
  CHECK(worst_xy <= 1.0e-6);
  CHECK(worst_z <= 1.0e-6);
  CHECK(worst_angle <= 1.0e-5);
}

TEST_CASE("loop limit: the boundary is the cubic B-spline of the boundary polygon") {
  // The rim of the fixture (ring 5, vertices 97 .. 120), with every node jittered so nothing is
  // symmetric. At level L the refined boundary loop has 24 * 2^L vertices and vertex i of it,
  // walked from rim vertex 97 toward 98, is the closed uniform cubic B-spline of the rim at
  // parameter i / 2^L, evaluated here from the B-spline's own basis.
  const Vector<u32> faces = fixture::ringed_disk_faces(5, 24);
  const Vector<Vec3> x =
      jittered(fixture::ringed_disk_dome(5, 24, 0.07f, 0.086f, 0.08f), 3, 0.005f);
  constexpr u32 level = 3;
  constexpr u32 rim = 97;
  const LoopLimitSurface s = build(faces, 121, level, false);
  const Vector<Vec3> y = evaluate(s.limit, x);

  // The refined boundary: edges one triangle uses.
  Vector<u64> keys;
  for (u32 f = 0; f < s.triangle_count(); ++f)
    for (u32 c = 0; c < 3; ++c) {
      const u32 a = s.faces[3 * f + c];
      const u32 b = s.faces[3 * f + (c + 1) % 3];
      keys.push_back((u64{std::min(a, b)} << 32) | u64{std::max(a, b)});
    }
  std::sort(keys.begin(), keys.end());
  Vector<u32> next_a(s.vertex_count(), ~0u);
  Vector<u32> next_b(s.vertex_count(), ~0u);
  for (u32 i = 0; i < keys.size(); ++i) {
    const bool single =
        (i == 0 || keys[i] != keys[i - 1]) && (i + 1 == keys.size() || keys[i] != keys[i + 1]);
    if (!single) continue;
    const u32 a = static_cast<u32>(keys[i] >> 32);
    const u32 b = static_cast<u32>(keys[i] & 0xffffffffu);
    (next_a[a] == ~0u ? next_a[a] : next_b[a]) = b;
    (next_a[b] == ~0u ? next_a[b] : next_b[b]) = a;
  }
  // Leave rim vertex 97 toward the refined vertex that lies on control edge (97, 98).
  u32 edge_97_98 = ~0u;
  for (u32 e = 0; e < s.control_edges.size() / 2; ++e)
    if (s.control_edges[2 * e] == rim && s.control_edges[2 * e + 1] == rim + 1) edge_97_98 = e;
  REQUIRE(edge_97_98 != ~0u);
  const auto on_first_edge = [&](u32 v) {
    return s.vertex_parent[v].kind == LoopParentKind::edge &&
           s.vertex_parent[v].index == edge_97_98;
  };
  const u32 loop_length = 24u << level;
  Vector<u32> loop;
  loop.push_back(rim);
  u32 previous = rim;
  u32 current = on_first_edge(next_a[rim]) ? next_a[rim] : next_b[rim];
  REQUIRE(on_first_edge(current));
  while (current != rim && loop.size() <= loop_length) {
    loop.push_back(current);
    const u32 next = next_a[current] == previous ? next_b[current] : next_a[current];
    previous = current;
    current = next;
  }
  REQUIRE(loop.size() == loop_length);

  const auto control = [&](i64 j) {
    const Vec3 p = x[rim + static_cast<u32>(((j % 24) + 24) % 24)];
    return p;
  };
  f64 worst = 0.0;
  for (u32 i = 0; i < loop_length; ++i) {
    const f64 parameter = static_cast<f64>(i) / static_cast<f64>(1u << level);
    const i64 j = static_cast<i64>(std::floor(parameter));
    const f64 t = parameter - static_cast<f64>(j);
    const f64 w[4] = {(1.0 - t) * (1.0 - t) * (1.0 - t) / 6.0,
                      (3.0 * t * t * t - 6.0 * t * t + 4.0) / 6.0,
                      (-3.0 * t * t * t + 3.0 * t * t + 3.0 * t + 1.0) / 6.0, t * t * t / 6.0};
    f64 expected[3] = {0.0, 0.0, 0.0};
    for (i64 d = 0; d < 4; ++d) {
      const Vec3 p = control(j - 1 + d);
      expected[0] += w[d] * static_cast<f64>(p.x);
      expected[1] += w[d] * static_cast<f64>(p.y);
      expected[2] += w[d] * static_cast<f64>(p.z);
    }
    const Vec3 got = y[loop[i]];
    worst = std::max({worst, std::fabs(static_cast<f64>(got.x) - expected[0]),
                      std::fabs(static_cast<f64>(got.y) - expected[1]),
                      std::fabs(static_cast<f64>(got.z) - expected[2])});
    // And nothing inside the rim reaches a boundary row.
    for (u32 e = s.limit.row_offsets[loop[i]]; e < s.limit.row_offsets[loop[i] + 1]; ++e)
      CHECK(s.limit.column_index[e] >= rim);
  }
  MESSAGE("boundary: " << loop_length << " refined rim vertices within " << worst
                       << " of the cubic B-spline");
  CHECK(worst <= 1.0e-7);
}

TEST_CASE("loop limit: the tangent masks are eigenvectors of the rules they are built from") {
  // A limit tangent mask l is a left eigenvector of the local subdivision matrix S: l S = lambda l,
  // so applying it to a vertex's one-ring one level finer gives exactly lambda times the coarser
  // answer, for any positions. Boundary masks start at a fixed boundary neighbour, so each
  // tangent scales by its own eigenvalue: 1/2 along the boundary, and across it 1/4 for one face
  // and 3/8 + cos(pi / k) / 4 for k faces. Interior masks start wherever the ring's smallest index
  // falls, so there the pair turns within the tangent plane and the normal keeps its direction.
  for (u32 k = 1; k <= 7; ++k) {
    const Vector<u32> faces = fixture::fan_faces(k);
    const u32 vertices = k + 2;
    fixture::Random random(100 + k);
    Vector<Vec3> x;
    for (u32 i = 0; i < vertices; ++i) {
      const f64 phi = std::numbers::pi * static_cast<f64>(i == 0 ? 0 : i - 1) / static_cast<f64>(k);
      x.push_back(i == 0 ? Vec3{0.0f, 0.0f, 0.3f}
                         : Vec3{static_cast<f32>(std::cos(phi)) + random.uniform(-0.1f, 0.1f),
                                static_cast<f32>(std::sin(phi)) + random.uniform(-0.1f, 0.1f),
                                random.uniform(-0.2f, 0.2f)});
    }
    const LoopLimitSurface coarse = build(faces, vertices, 1);
    const LoopLimitSurface fine = build(faces, vertices, 2);
    CHECK(coarse.max_boundary_faces == std::max(k, k == 1 ? 1u : 2u));
    const Vector<Vec3> u1 = evaluate(coarse.tangent_u, x);
    const Vector<Vec3> v1 = evaluate(coarse.tangent_v, x);
    const Vector<Vec3> u2 = evaluate(fine.tangent_u, x);
    const Vector<Vec3> v2 = evaluate(fine.tangent_v, x);
    for (u32 v = 0; v < vertices; ++v) {
      const u32 faces_at_v = v == 0 ? k : (v == 1 || v == k + 1 ? 1u : 2u);
      const f64 lambda =
          faces_at_v == 1 ? 0.25 : 0.375 + 0.25 * std::cos(std::numbers::pi / faces_at_v);
      INFO("fan of " << k << ", vertex " << v << " with " << faces_at_v << " faces");
      CHECK(gap(u2[v], u1[v] * 0.5f) <= 1.0e-5 * gap(u1[v], Vec3{}));
      CHECK(gap(v2[v], v1[v] * static_cast<f32>(lambda)) <= 1.0e-5 * gap(v1[v], Vec3{}));
    }
  }

  const Vector<u32> faces = fixture::ringed_disk_faces(5, 24);
  const Vector<Vec3> x =
      jittered(fixture::ringed_disk_dome(5, 24, 0.07f, 0.086f, 0.08f), 5, 0.004f);
  const LoopLimitSurface coarse = build(faces, 121, 1);
  const LoopLimitSurface fine = build(faces, 121, 2);
  const Vector<Vec3> n1 = normals_of(coarse, x);
  const Vector<Vec3> n2 = normals_of(fine, x);
  f64 worst = 0.0;
  for (u32 v = 0; v < 121; ++v)
    worst = std::max(worst, angle_between(n1[v], n2[v]));
  MESSAGE("control vertices' limit normals, level 1 against level 2: within " << worst << " rad");
  CHECK(worst <= 1.0e-5);
}

TEST_CASE("loop limit: limit normals face the way the triangles wind") {
  const Vector<u32> faces = fixture::ringed_disk_faces(5, 24);
  const Vector<Vec3> x = fixture::ringed_disk_dome(5, 24, 0.07f, 0.086f, 0.08f);
  const LoopLimitSurface s = build(faces, 121, 3);
  const Vector<Vec3> y = evaluate(s.limit, x);
  const Vector<Vec3> normals = normals_of(s, x);
  f64 worst = 0.0;  // largest angle between a corner's limit normal and its triangle's normal
  for (u32 f = 0; f < s.triangle_count(); ++f) {
    const Vec3 a = y[s.faces[3 * f]];
    const Vec3 b = y[s.faces[3 * f + 1]];
    const Vec3 c = y[s.faces[3 * f + 2]];
    const Vec3 face = cross(b - a, c - a);
    for (u32 i = 0; i < 3; ++i)
      worst = std::max(worst, angle_between(normals[s.faces[3 * f + i]], face));
  }
  MESSAGE("dome, level 3: triangle normals within " << worst * 180.0 / std::numbers::pi
                                                    << " degrees of their corners' limit normals");
  CHECK(worst < std::numbers::pi / 18.0);
  // The pole of the dome looks straight up.
  CHECK(angle_between(normals[0], Vec3{0.0f, 0.0f, 1.0f}) <= 1.0e-5);
}

TEST_CASE("loop limit: parent maps name the control vertex, edge or face a point came from") {
  const Vector<u32> faces = fixture::ringed_disk_faces(5, 24);
  constexpr u32 level = 3;
  const LoopLimitSurface s = build(faces, 121, level, false);
  // A disk has V - E + F = 1, so 336 edges; each keeps 2^L - 1 points and each face
  // (2^L - 1)(2^L - 2) / 2 inside it.
  REQUIRE(s.control_edges.size() == 2 * 336);
  u32 by_kind[3] = {0, 0, 0};
  for (const LoopParent& p : s.vertex_parent)
    ++by_kind[static_cast<u32>(p.kind)];
  CHECK(by_kind[0] == 121);
  CHECK(by_kind[1] == 336 * 7);
  CHECK(by_kind[2] == 216 * 21);

  Vector<u32> per_face(216, 0u);
  for (u32 f = 0; f < s.triangle_count(); ++f) {
    const u32 parent = s.face_parent[f];
    REQUIRE(parent < 216);
    ++per_face[parent];
    const u32* control = faces.data() + 3 * parent;
    for (u32 c = 0; c < 3; ++c) {
      const LoopParent p = s.vertex_parent[s.faces[3 * f + c]];
      bool belongs = false;
      if (p.kind == LoopParentKind::face) belongs = p.index == parent;
      if (p.kind == LoopParentKind::vertex)
        belongs = p.index == control[0] || p.index == control[1] || p.index == control[2];
      if (p.kind == LoopParentKind::edge) {
        const u32 a = s.control_edges[2 * p.index];
        const u32 b = s.control_edges[2 * p.index + 1];
        u32 shared = 0;
        for (u32 i = 0; i < 3; ++i)
          shared += (control[i] == a || control[i] == b) ? 1u : 0u;
        belongs = shared == 2;
      }
      CHECK(belongs);
    }
  }
  for (const u32 count : per_face)
    CHECK(count == 64);
}

TEST_CASE("loop limit: a tagged corner stays where it is") {
  const Vector<u32> faces = fixture::fan_faces(3);
  const Vector<Vec3> x = {Vec3{0.0f, 0.0f, 0.25f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.5f, 0.8f, 0.1f},
                          Vec3{-0.5f, 0.8f, -0.1f}, Vec3{-1.0f, 0.0f, 0.0f}};
  const u32 corners[] = {0};
  for (u32 level = 0; level <= 3; ++level) {
    const LoopLimitSurface s = build(faces, 5, level, true, corners);
    REQUIRE(s.limit.row_offsets[1] - s.limit.row_offsets[0] == 1);
    CHECK(s.limit.column_index[0] == 0);
    CHECK(s.limit.weight[0] == 1.0f);
    const Vector<Vec3> y = evaluate(s.limit, x);
    CHECK(y[0] == x[0]);
    check_partition_of_unity(s.limit);
    // Not a boundary vertex of the smooth kind, so it does not count against the rules' limit.
    CHECK(s.max_boundary_faces == 2);
  }
}

TEST_CASE("loop limit: two builds agree to the bit") {
  const Vector<u32> faces = fixture::ringed_disk_faces(5, 24);
  const LoopLimitSurface a = build(faces, 121, 3);
  const LoopLimitSurface b = build(faces, 121, 3);
  const auto same = [](const auto& x, const auto& y) {
    return x.size() == y.size() &&
           (x.size() == 0 || std::memcmp(x.data(), y.data(), x.size() * sizeof(x[0])) == 0);
  };
  CHECK(same(a.faces, b.faces));
  CHECK(same(a.face_parent, b.face_parent));
  CHECK(same(a.vertex_parent, b.vertex_parent));
  CHECK(same(a.control_edges, b.control_edges));
  for (const auto& [m, n] : {std::pair{&a.limit, &b.limit}, std::pair{&a.tangent_u, &b.tangent_u},
                             std::pair{&a.tangent_v, &b.tangent_v}}) {
    CHECK(same(m->row_offsets, n->row_offsets));
    CHECK(same(m->column_index, n->column_index));
    CHECK(same(m->weight, n->weight));
  }
  const Vector<Vec3> x =
      jittered(fixture::ringed_disk_dome(5, 24, 0.07f, 0.086f, 0.08f), 9, 0.003f);
  const Vector<Vec3> ya = evaluate(a.limit, x);
  const Vector<Vec3> yb = evaluate(b.limit, x);
  CHECK(same(ya, yb));
}

TEST_CASE("loop limit: meshes it cannot subdivide are refused with a sentence") {
  const auto refuse = [](std::span<const u32> faces, u32 vertices, u32 level,
                         std::span<const u32> corners = {}) {
    LoopSurfaceOptions options;
    options.level = level;
    options.corners = corners;
    LoopLimitSurface s;
    std::string error;
    const bool ok = build_loop_limit_surface(faces, vertices, options, s, &error);
    CHECK_FALSE(ok);
    CHECK_FALSE(error.empty());
    return error;
  };
  const Vector<u32> disk = fixture::ringed_disk_faces(5, 24);
  CHECK(refuse(disk, 121, 5).find("level") != std::string::npos);
  CHECK(refuse(disk, 120, 1).find("out of range") != std::string::npos);
  const u32 repeated[] = {0, 1, 1};
  CHECK(refuse(repeated, 3, 1).find("repeats") != std::string::npos);
  // Three triangles on one edge.
  const u32 book[] = {0, 1, 2, 1, 0, 3, 0, 1, 4};
  CHECK(refuse(book, 5, 1).find("shared by 3") != std::string::npos);
  // Two triangles crossing their shared edge the same way.
  const u32 flipped[] = {0, 1, 2, 0, 1, 3};
  CHECK(refuse(flipped, 4, 1).find("wound the same way") != std::string::npos);
  // Two fans touching at vertex 0 (a bowtie): consistent edges, no single ring.
  const u32 bowtie[] = {0, 1, 2, 0, 3, 4};
  CHECK_FALSE(refuse(bowtie, 5, 1).empty());
  // Two closed cones sharing an apex: every edge is fine, the vertex is not.
  const u32 cones[] = {0, 1, 2, 0, 2, 3, 0, 3, 1, 1, 3, 2,   // a tetrahedron on 0..3
                       0, 4, 5, 0, 5, 6, 0, 6, 4, 4, 6, 5};  // and one on 0, 4..6
  CHECK(refuse(cones, 7, 1).find("more than one fan") != std::string::npos);
  const u32 corner[] = {200};
  CHECK(refuse(disk, 121, 1, corner).find("corner") != std::string::npos);
}
