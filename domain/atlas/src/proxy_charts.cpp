// Charts cut on a simplified proxy and carried onto the full mesh (proxy_charts.h,
// docs/subsystems/atlas.md "Why a proxy").
//
// Why not chart the full mesh directly: xatlas grows a chart only while the chart still projects
// onto its best-fit plane with no face flipped, stops it where a face turns more than about 73
// degrees from that plane, and makes a chart of its own of any face whose every neighbour is more
// than 90 degrees away. On a generated mesh — a remesh's folds, spikes and slivers, a thin part's
// hundreds of small facets — that leaves hundreds of charts of one to a few faces and LSCM charts
// of a few hundred, and 30 to 60% of the vertices on a chart border: about the fragmentation of
// the atlas it replaces, so the LOD is no freer than before (E10's "Repack" section has the
// numbers). A proxy simplified to a few thousand triangles has none of those small features, so
// xatlas cuts it into a few dozen charts; the full mesh then takes those charts by proximity, and
// its UVs by projection onto them.
//
// Everything below is a function of the input alone: sorts break ties on indices, the grid is
// walked in a fixed order, and xatlas runs single-threaded (cmake/EngineAtlas.cmake).

#include "proxy_charts.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <meshoptimizer.h>
#include <xatlas.h>

namespace engine::atlas {

namespace {

// A proxy chart with fewer faces than this is not a target: its faces are the proxy's own folds
// and spikes (xatlas's one-face "planar" charts and few-face "ortho" ones), and a fine triangle
// near one takes the nearest real chart instead, extrapolated across it.
constexpr u32 k_min_target_faces = 4;

// A chart component of fewer fine triangles than this, beside another chart, is absorbed by that
// chart: a speck of one chart inside another is a seam loop around a handful of triangles that
// holds the LOD for nothing.
constexpr u32 k_absorb_triangles = 32;

// How many majority passes smooth the chart borders: a triangle whose other two neighbours are in
// one other chart joins it, which takes the one-triangle notches out of a border.
constexpr u32 k_majority_passes = 3;

struct D3 {
  f64 x = 0.0;
  f64 y = 0.0;
  f64 z = 0.0;
};

D3 operator-(D3 a, D3 b) noexcept { return D3{a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 operator+(D3 a, D3 b) noexcept { return D3{a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 operator*(D3 a, f64 s) noexcept { return D3{a.x * s, a.y * s, a.z * s}; }
f64 dot(D3 a, D3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross(D3 a, D3 b) noexcept {
  return D3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

D3 point_at(const PointMesh& m, u32 p) noexcept {
  return D3{static_cast<f64>(m.positions[p * 3]), static_cast<f64>(m.positions[p * 3 + 1]),
            static_cast<f64>(m.positions[p * 3 + 2])};
}

// Ericson, Real-Time Collision Detection 5.1.5: the point of triangle abc closest to p.
D3 closest_on_triangle(D3 p, D3 a, D3 b, D3 c) noexcept {
  const D3 ab = b - a;
  const D3 ac = c - a;
  const D3 ap = p - a;
  const f64 d1 = dot(ab, ap);
  const f64 d2 = dot(ac, ap);
  if (d1 <= 0.0 && d2 <= 0.0) return a;
  const D3 bp = p - b;
  const f64 d3 = dot(ab, bp);
  const f64 d4 = dot(ac, bp);
  if (d3 >= 0.0 && d4 <= d3) return b;
  const f64 vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) return a + ab * (d1 / (d1 - d3));
  const D3 cp = p - c;
  const f64 d5 = dot(ab, cp);
  const f64 d6 = dot(ac, cp);
  if (d6 >= 0.0 && d5 <= d6) return c;
  const f64 vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) return a + ac * (d2 / (d2 - d6));
  const f64 va = d3 * d6 - d5 * d4;
  if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
    return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
  const f64 denom = va + vb + vc;
  if (denom == 0.0) return a;
  const f64 v = vb / denom;
  const f64 w = vc / denom;
  return a + ab * v + ac * w;
}

// Barycentrics of p's projection onto the plane of abc, unclamped: a point beside the triangle
// extrapolates its affine map rather than stopping at its edge.
void plane_barycentrics(D3 p, D3 a, D3 b, D3 c, f64 out[3]) noexcept {
  const D3 v0 = b - a;
  const D3 v1 = c - a;
  const D3 v2 = p - a;
  const f64 d00 = dot(v0, v0);
  const f64 d01 = dot(v0, v1);
  const f64 d11 = dot(v1, v1);
  const f64 d20 = dot(v2, v0);
  const f64 d21 = dot(v2, v1);
  const f64 denom = d00 * d11 - d01 * d01;
  if (std::fabs(denom) < 1e-30) {
    out[0] = out[1] = out[2] = 1.0 / 3.0;
    return;
  }
  out[1] = (d11 * d20 - d01 * d21) / denom;
  out[2] = (d00 * d21 - d01 * d20) / denom;
  out[0] = 1.0 - out[1] - out[2];
}

struct ProxyFace {
  u32 point[3] = {};
  Vec2 uv[3] = {};
  i32 chart = -1;  // xatlas chart, -1 when xatlas refused the face
  D3 normal;       // unit, or zero for a degenerate face
};

// A uniform grid over the target faces, each face in every cell its box touches.
class FaceGrid {
 public:
  void build(const Vector<ProxyFace>& faces, const Vector<u32>& targets, const PointMesh& mesh,
             f64 cell) {
    faces_ = &faces;
    mesh_ = &mesh;
    lo_ = D3{std::numeric_limits<f64>::max(), std::numeric_limits<f64>::max(),
             std::numeric_limits<f64>::max()};
    D3 hi{-lo_.x, -lo_.y, -lo_.z};
    for (u32 p = 0; p < mesh.point_count(); ++p) {
      const D3 q = point_at(mesh, p);
      lo_ = D3{std::min(lo_.x, q.x), std::min(lo_.y, q.y), std::min(lo_.z, q.z)};
      hi = D3{std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
    }
    cell_ = std::max(cell, 1e-6);
    const f64 extent[3] = {hi.x - lo_.x, hi.y - lo_.y, hi.z - lo_.z};
    // At most about two million cells: past that the grid costs more memory than it saves time.
    for (;;) {
      for (u32 a = 0; a < 3; ++a)
        n_[a] = static_cast<u32>(std::clamp(std::ceil(extent[a] / cell_), 1.0, 256.0));
      if (static_cast<u64>(n_[0]) * n_[1] * n_[2] <= (1u << 21)) break;
      cell_ *= 1.25;
    }
    const u32 cells = n_[0] * n_[1] * n_[2];
    Vector<u32> count(cells + 1, 0u);
    auto for_cells = [&](u32 f, auto&& fn) {
      const ProxyFace& face = faces[f];
      D3 flo = point_at(mesh, face.point[0]);
      D3 fhi = flo;
      for (u32 k = 1; k < 3; ++k) {
        const D3 q = point_at(mesh, face.point[k]);
        flo = D3{std::min(flo.x, q.x), std::min(flo.y, q.y), std::min(flo.z, q.z)};
        fhi = D3{std::max(fhi.x, q.x), std::max(fhi.y, q.y), std::max(fhi.z, q.z)};
      }
      u32 c0[3];
      u32 c1[3];
      cell_of(flo, c0);
      cell_of(fhi, c1);
      for (u32 z = c0[2]; z <= c1[2]; ++z)
        for (u32 y = c0[1]; y <= c1[1]; ++y)
          for (u32 x = c0[0]; x <= c1[0]; ++x)
            fn((z * n_[1] + y) * n_[0] + x);
    };
    for (const u32 f : targets)
      for_cells(f, [&](u32 c) { ++count[c + 1]; });
    for (u32 c = 0; c < cells; ++c)
      count[c + 1] += count[c];
    offsets_ = count;
    items_.assign(count[cells], 0u);
    Vector<u32> fill(cells, 0u);
    for (const u32 f : targets)
      for_cells(f, [&](u32 c) { items_[offsets_[c] + fill[c]++] = f; });
    stamp_.assign(faces.size(), ~0u);
  }

  // The target face nearest `q` (chart `only` alone when it is not -1), a face turned away from
  // `normal` counting `penalty` further than it is. ~0u when there is none.
  u32 nearest(D3 q, D3 normal, bool has_normal, f64 penalty, i32 only) {
    ++query_;
    u32 qc[3];
    cell_of(q, qc);
    const u32 reach = std::max({n_[0], n_[1], n_[2]});
    f64 best = std::numeric_limits<f64>::infinity();
    u32 best_face = ~0u;
    for (u32 r = 0; r <= reach; ++r) {
      const i64 lo[3] = {static_cast<i64>(qc[0]) - r, static_cast<i64>(qc[1]) - r,
                         static_cast<i64>(qc[2]) - r};
      const i64 hi[3] = {static_cast<i64>(qc[0]) + r, static_cast<i64>(qc[1]) + r,
                         static_cast<i64>(qc[2]) + r};
      for (i64 z = std::max<i64>(lo[2], 0); z <= std::min<i64>(hi[2], n_[2] - 1); ++z) {
        for (i64 y = std::max<i64>(lo[1], 0); y <= std::min<i64>(hi[1], n_[1] - 1); ++y) {
          for (i64 x = std::max<i64>(lo[0], 0); x <= std::min<i64>(hi[0], n_[0] - 1); ++x) {
            // The shell only: cells inside it were visited by an earlier ring.
            if (x != lo[0] && x != hi[0] && y != lo[1] && y != hi[1] && z != lo[2] && z != hi[2])
              continue;
            const u32 c = static_cast<u32>((z * n_[1] + y) * n_[0] + x);
            for (u32 i = offsets_[c]; i < offsets_[c + 1]; ++i) {
              const u32 f = items_[i];
              if (stamp_[f] == query_) continue;
              stamp_[f] = query_;
              const ProxyFace& face = (*faces_)[f];
              if (only >= 0 && face.chart != only) continue;
              const D3 a = point_at(*mesh_, face.point[0]);
              const D3 b = point_at(*mesh_, face.point[1]);
              const D3 cc = point_at(*mesh_, face.point[2]);
              const D3 d = q - closest_on_triangle(q, a, b, cc);
              f64 dist = std::sqrt(dot(d, d));
              if (has_normal && dot(normal, face.normal) < 0.0) dist += penalty;
              if (dist < best || (dist == best && f < best_face)) {
                best = dist;
                best_face = f;
              }
            }
          }
        }
      }
      // Everything within r cells of q's cell is visited, so nothing unvisited is nearer than
      // r cells; a penalty only makes a face further, so the bound still holds.
      if (best_face != ~0u && best <= static_cast<f64>(r) * cell_) break;
    }
    return best_face;
  }

 private:
  void cell_of(D3 q, u32 out[3]) const noexcept {
    const f64 v[3] = {(q.x - lo_.x) / cell_, (q.y - lo_.y) / cell_, (q.z - lo_.z) / cell_};
    for (u32 a = 0; a < 3; ++a)
      out[a] = static_cast<u32>(std::clamp(std::floor(v[a]), 0.0, static_cast<f64>(n_[a] - 1)));
  }

  const Vector<ProxyFace>* faces_ = nullptr;
  const PointMesh* mesh_ = nullptr;
  D3 lo_;
  f64 cell_ = 1.0;
  u32 n_[3] = {1, 1, 1};
  Vector<u32> offsets_;
  Vector<u32> items_;
  Vector<u32> stamp_;
  u32 query_ = 0;
};

u32 find_root(Vector<u32>& parent, u32 v) noexcept {
  while (parent[v] != v) {
    parent[v] = parent[parent[v]];
    v = parent[v];
  }
  return v;
}

// ---- least-squares conformal maps (Lévy, Petitjean, Ray and Maillot, SIGGRAPH 2002) -----------
//
// For each triangle, in its own plane with local complex coordinates z_j, a linear map U is
// conformal when sum_j U_j (z_{j+1} - z_{j+2}) = 0; the energy is that residual squared, weighted
// by 1/area, summed over the triangles — two real rows a triangle. Two vertices are pinned (it is
// otherwise invariant under similarities) and the normal equations are solved by
// Jacobi-preconditioned conjugate gradients, from a start the caller gives. With this sign
// convention the identity has zero energy, so the result winds the way the triangles do.

constexpr u32 k_cg_max_iterations = 2000;
constexpr f64 k_cg_tolerance = 1e-10;  // on the squared residual, relative to the right side's

struct Triplet {
  u32 row = 0;
  u32 col = 0;
  f64 value = 0.0;
};

// One connected piece: `vertices` (local) are its UV vertices, `triangles` its triangles as local
// corner indices; `uv` holds the start and receives the result. False leaves `uv` as it was.
bool conformal_map(const Vector<D3>& position, const Vector<u32>& corners, Vector<D3>& uv) {
  const u32 n = position.size();
  const u32 tri = corners.size() / 3;
  if (n < 3 || tri == 0) return false;
  // Pins: the two vertices furthest apart along the start's larger extent.
  u32 pin_a = 0;
  u32 pin_b = 0;
  {
    f64 lo_u = uv[0].x, hi_u = uv[0].x, lo_v = uv[0].y, hi_v = uv[0].y;
    for (u32 i = 1; i < n; ++i) {
      lo_u = std::min(lo_u, uv[i].x);
      hi_u = std::max(hi_u, uv[i].x);
      lo_v = std::min(lo_v, uv[i].y);
      hi_v = std::max(hi_v, uv[i].y);
    }
    const bool along_u = hi_u - lo_u >= hi_v - lo_v;
    for (u32 i = 1; i < n; ++i) {
      const f64 key = along_u ? uv[i].x : uv[i].y;
      if (key < (along_u ? uv[pin_a].x : uv[pin_a].y)) pin_a = i;
      if (key > (along_u ? uv[pin_b].x : uv[pin_b].y)) pin_b = i;
    }
    if (pin_a == pin_b) return false;
  }
  // The start, turned the way the solution winds (the triangles' own way) and placed on the pins.
  {
    f64 area = 0.0;
    for (u32 t = 0; t < tri; ++t) {
      const D3 a = uv[corners[t * 3]];
      const D3 b = uv[corners[t * 3 + 1]];
      const D3 c = uv[corners[t * 3 + 2]];
      area += (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    }
    if (area < 0.0) {
      for (D3& q : uv)
        q.x = -q.x;
    }
    const D3 pa = position[pin_a];
    const D3 pb = position[pin_b];
    const D3 d3 = pb - pa;
    const f64 target = std::sqrt(dot(d3, d3));
    const f64 dx = uv[pin_b].x - uv[pin_a].x;
    const f64 dy = uv[pin_b].y - uv[pin_a].y;
    const f64 len = std::sqrt(dx * dx + dy * dy);
    if (target <= 0.0 || len <= 0.0) return false;
    // The similarity taking the start's pins to (0, 0) and (target, 0).
    const f64 s = target / len;
    const f64 cs = dx / len;
    const f64 sn = dy / len;
    const D3 origin = uv[pin_a];
    for (D3& q : uv) {
      const f64 x = q.x - origin.x;
      const f64 y = q.y - origin.y;
      q = D3{(x * cs + y * sn) * s, (-x * sn + y * cs) * s, 0.0};
    }
    uv[pin_a] = D3{0.0, 0.0, 0.0};
    uv[pin_b] = D3{target, 0.0, 0.0};
  }
  // Free unknowns: two per unpinned vertex.
  Vector<u32> free_of(n, ~0u);
  u32 free_count = 0;
  for (u32 i = 0; i < n; ++i) {
    if (i != pin_a && i != pin_b) free_of[i] = free_count++;
  }
  const u32 unknowns = free_count * 2;
  Vector<f64> rhs(unknowns, 0.0);
  Vector<Triplet> triplets;
  triplets.reserve(tri * 72u);
  for (u32 t = 0; t < tri; ++t) {
    const u32 v[3] = {corners[t * 3], corners[t * 3 + 1], corners[t * 3 + 2]};
    const D3 e1 = position[v[1]] - position[v[0]];
    const D3 e2 = position[v[2]] - position[v[0]];
    const D3 nrm = cross(e1, e2);
    const f64 area2 = std::sqrt(dot(nrm, nrm));
    const f64 l1 = std::sqrt(dot(e1, e1));
    if (area2 <= 1e-20 || l1 <= 1e-20) continue;  // no plane: no constraint
    const D3 xaxis = e1 * (1.0 / l1);
    const f64 z[3][2] = {{0.0, 0.0}, {l1, 0.0}, {dot(e2, xaxis), area2 / l1}};
    const f64 w = 1.0 / std::sqrt(area2);
    // Row entries (column, value) for the real and the imaginary row.
    u32 col[6];
    f64 re[6];
    f64 im[6];
    for (u32 j = 0; j < 3; ++j) {
      const u32 j1 = (j + 1) % 3;
      const u32 j2 = (j + 2) % 3;
      const f64 wr = (z[j1][0] - z[j2][0]) * w;
      const f64 wi = (z[j1][1] - z[j2][1]) * w;
      col[j * 2] = v[j] * 2;
      col[j * 2 + 1] = v[j] * 2 + 1;
      re[j * 2] = wr;       // u_j
      re[j * 2 + 1] = -wi;  // v_j
      im[j * 2] = wi;
      im[j * 2 + 1] = wr;
    }
    for (const f64* row : {re, im}) {
      // The pinned part of the row goes to the right side.
      f64 pinned = 0.0;
      for (u32 k = 0; k < 6; ++k) {
        const u32 vertex = col[k] / 2;
        if (free_of[vertex] != ~0u) continue;
        pinned += row[k] * ((col[k] & 1u) == 0 ? uv[vertex].x : uv[vertex].y);
      }
      for (u32 a = 0; a < 6; ++a) {
        const u32 va = col[a] / 2;
        if (free_of[va] == ~0u) continue;
        const u32 ra = free_of[va] * 2 + (col[a] & 1u);
        rhs[ra] -= row[a] * pinned;
        for (u32 b = 0; b < 6; ++b) {
          const u32 vb = col[b] / 2;
          if (free_of[vb] == ~0u) continue;
          triplets.push_back(Triplet{ra, free_of[vb] * 2 + (col[b] & 1u), row[a] * row[b]});
        }
      }
    }
  }
  if (triplets.empty()) return false;
  // Stable, because duplicates of one (row, col) carry different values and are summed in the order
  // this leaves them: std::sort's order among equal keys is its implementation's, so MSVC's library
  // and libstdc++ summed them differently, which was one reason a repacked container differed
  // between Windows and Linux (atlas.md, "Determinism"). Stable keeps insertion order.
  std::stable_sort(triplets.begin(), triplets.end(), [](const Triplet& x, const Triplet& y) {
    if (x.row != y.row) return x.row < y.row;
    return x.col < y.col;
  });
  // CSR, duplicates summed in sorted order.
  Vector<u32> row_start(unknowns + 1, 0u);
  Vector<u32> cols;
  Vector<f64> values;
  cols.reserve(triplets.size() / 4);
  values.reserve(triplets.size() / 4);
  for (u32 i = 0; i < triplets.size();) {
    u32 j = i;
    f64 sum = 0.0;
    while (j < triplets.size() && triplets[j].row == triplets[i].row &&
           triplets[j].col == triplets[i].col)
      sum += triplets[j++].value;
    cols.push_back(triplets[i].col);
    values.push_back(sum);
    ++row_start[triplets[i].row + 1];
    i = j;
  }
  for (u32 r = 0; r < unknowns; ++r)
    row_start[r + 1] += row_start[r];
  Vector<f64> diag(unknowns, 0.0);
  for (u32 r = 0; r < unknowns; ++r) {
    for (u32 k = row_start[r]; k < row_start[r + 1]; ++k)
      if (cols[k] == r) diag[r] = values[k];
  }
  auto multiply = [&](const Vector<f64>& x, Vector<f64>& y) {
    for (u32 r = 0; r < unknowns; ++r) {
      f64 s = 0.0;
      for (u32 k = row_start[r]; k < row_start[r + 1]; ++k)
        s += values[k] * x[cols[k]];
      y[r] = s;
    }
  };
  Vector<f64> x(unknowns, 0.0);
  for (u32 i = 0; i < n; ++i) {
    if (free_of[i] == ~0u) continue;
    x[free_of[i] * 2] = uv[i].x;
    x[free_of[i] * 2 + 1] = uv[i].y;
  }
  Vector<f64> r(unknowns, 0.0);
  Vector<f64> z(unknowns, 0.0);
  Vector<f64> p(unknowns, 0.0);
  Vector<f64> q(unknowns, 0.0);
  multiply(x, q);
  f64 rhs_norm = 0.0;
  for (u32 i = 0; i < unknowns; ++i) {
    r[i] = rhs[i] - q[i];
    rhs_norm += rhs[i] * rhs[i];
  }
  if (rhs_norm <= 0.0) rhs_norm = 1.0;
  f64 rz = 0.0;
  for (u32 i = 0; i < unknowns; ++i) {
    z[i] = diag[i] > 0.0 ? r[i] / diag[i] : r[i];
    p[i] = z[i];
    rz += r[i] * z[i];
  }
  for (u32 iteration = 0; iteration < k_cg_max_iterations; ++iteration) {
    f64 rr = 0.0;
    for (u32 i = 0; i < unknowns; ++i)
      rr += r[i] * r[i];
    if (rr <= k_cg_tolerance * rhs_norm) break;
    multiply(p, q);
    f64 pq = 0.0;
    for (u32 i = 0; i < unknowns; ++i)
      pq += p[i] * q[i];
    if (pq <= 0.0) break;
    const f64 alpha = rz / pq;
    for (u32 i = 0; i < unknowns; ++i) {
      x[i] += alpha * p[i];
      r[i] -= alpha * q[i];
    }
    f64 rz_next = 0.0;
    for (u32 i = 0; i < unknowns; ++i) {
      z[i] = diag[i] > 0.0 ? r[i] / diag[i] : r[i];
      rz_next += r[i] * z[i];
    }
    const f64 beta = rz_next / rz;
    rz = rz_next;
    for (u32 i = 0; i < unknowns; ++i)
      p[i] = z[i] + beta * p[i];
  }
  for (u32 i = 0; i < unknowns; ++i) {
    if (!std::isfinite(x[i])) return false;
  }
  for (u32 i = 0; i < n; ++i) {
    if (free_of[i] == ~0u) continue;
    uv[i] = D3{x[free_of[i] * 2], x[free_of[i] * 2 + 1], 0.0};
  }
  return true;
}

// What a piece's UVs are like: its surface and UV areas (both doubled), how many triangles wind
// against the rest, and whether it is sane — finite, with UV area, and no wider than a hundred
// times the side of a square of its area (a conformal map of a piece that is nearly closed, or
// whose pins were badly placed, can put a vertex a hundred million units away).
struct Layout {
  f64 surface = 0.0;
  f64 flat = 0.0;
  u32 folded = 0;
  bool sane = false;
};

Layout measure_layout(const Vector<D3>& position, const Vector<u32>& corners,
                      const Vector<D3>& uv) {
  Layout out;
  f64 signed_area = 0.0;
  f64 lo_u = std::numeric_limits<f64>::max(), hi_u = -lo_u, lo_v = lo_u, hi_v = -lo_u;
  bool finite = true;
  for (const D3& q : uv) {
    finite = finite && std::isfinite(q.x) && std::isfinite(q.y);
    lo_u = std::min(lo_u, q.x);
    hi_u = std::max(hi_u, q.x);
    lo_v = std::min(lo_v, q.y);
    hi_v = std::max(hi_v, q.y);
  }
  Vector<f64> area(corners.size() / 3, 0.0);
  for (u32 t = 0; t + 2 < corners.size(); t += 3) {
    const D3 n3 = cross(position[corners[t + 1]] - position[corners[t]],
                        position[corners[t + 2]] - position[corners[t]]);
    out.surface += std::sqrt(dot(n3, n3));
    const D3 a = uv[corners[t]];
    const D3 b = uv[corners[t + 1]];
    const D3 c = uv[corners[t + 2]];
    const f64 s = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    area[t / 3] = s;
    signed_area += s;
    out.flat += std::fabs(s);
  }
  for (const f64 s : area) {
    if (s * signed_area < 0.0) ++out.folded;
  }
  const f64 side = std::sqrt(out.flat);
  out.sane = finite && out.flat > 0.0 && std::max(hi_u - lo_u, hi_v - lo_v) <= 100.0 * side;
  return out;
}

// Every connected piece of every chart of `out`, parameterized again by `conformal_map` from the
// UVs it has, and scaled so its UV area is its surface area — the packer scales every chart by
// one texel density, so the pieces have to agree on what a unit is.
void refine_conformal(const PointMesh& mesh, ProxyUvMesh& out) {
  const u32 vertices = out.uv.size();
  const u32 triangles = out.corners.size() / 3;
  Vector<u32> parent(vertices);
  for (u32 v = 0; v < vertices; ++v)
    parent[v] = v;
  for (u32 t = 0; t < triangles; ++t) {
    const u32 a = find_root(parent, out.corners[t * 3]);
    for (u32 k = 1; k < 3; ++k) {
      const u32 b = find_root(parent, out.corners[t * 3 + k]);
      if (a != b) parent[std::max(a, b)] = std::min(a, b);
    }
  }
  // Triangles and vertices grouped by piece, each in ascending order.
  struct Item {
    u32 piece = 0;
    u32 index = 0;
  };
  Vector<Item> piece_triangles(triangles);
  for (u32 t = 0; t < triangles; ++t)
    piece_triangles[t] = Item{find_root(parent, out.corners[t * 3]), t};
  std::sort(piece_triangles.begin(), piece_triangles.end(), [](const Item& x, const Item& y) {
    if (x.piece != y.piece) return x.piece < y.piece;
    return x.index < y.index;
  });
  Vector<u32> local(vertices, ~0u);
  Vector<u32> members;
  Vector<D3> position;
  Vector<D3> uv;
  Vector<u32> corners;
  for (u32 i = 0; i < piece_triangles.size();) {
    u32 j = i;
    while (j < piece_triangles.size() && piece_triangles[j].piece == piece_triangles[i].piece)
      ++j;
    members.clear();
    position.clear();
    uv.clear();
    corners.clear();
    for (u32 k = i; k < j; ++k) {
      const u32 t = piece_triangles[k].index;
      for (u32 c = 0; c < 3; ++c) {
        const u32 v = out.corners[t * 3 + c];
        if (local[v] == ~0u) {
          local[v] = members.size();
          members.push_back(v);
          position.push_back(point_at(mesh, out.uv_point[v]));
          uv.push_back(D3{static_cast<f64>(out.uv[v].x), static_cast<f64>(out.uv[v].y), 0.0});
        }
        corners.push_back(local[v]);
      }
    }
    // The conformal map is kept when it is sane — finite, with area, not absurdly stretched — and
    // folds no more triangles than the projection did; otherwise the projection stays. Either way
    // the piece is scaled to its surface area.
    const Layout projected = measure_layout(position, corners, uv);
    Vector<D3> conformal = uv;
    const bool solved = conformal_map(position, corners, conformal);
    const Layout mapped = solved ? measure_layout(position, corners, conformal) : Layout{};
    const bool take =
        solved && mapped.sane && (mapped.folded <= projected.folded || !projected.sane);
    if (!take && !projected.sane) {
      // Neither is usable (a vertex extrapolated far off a sliver of the proxy): the piece is
      // projected onto the plane of its mean normal instead, which is bounded by construction.
      D3 normal_sum{};
      for (u32 t = 0; t + 2 < corners.size(); t += 3)
        normal_sum = normal_sum + cross(position[corners[t + 1]] - position[corners[t]],
                                        position[corners[t + 2]] - position[corners[t]]);
      f64 len = std::sqrt(dot(normal_sum, normal_sum));
      const D3 axis = len > 0.0 ? normal_sum * (1.0 / len) : D3{0.0, 0.0, 1.0};
      const D3 helper = std::fabs(axis.x) < 0.9 ? D3{1.0, 0.0, 0.0} : D3{0.0, 1.0, 0.0};
      D3 tangent = cross(helper, axis);
      len = std::sqrt(dot(tangent, tangent));
      tangent = tangent * (1.0 / len);
      const D3 bitangent = cross(axis, tangent);
      for (u32 m = 0; m < members.size(); ++m)
        uv[m] = D3{dot(position[m], tangent), dot(position[m], bitangent), 0.0};
    }
    const Layout fallback =
        !take && !projected.sane ? measure_layout(position, corners, uv) : projected;
    const Vector<D3>& chosen = take ? conformal : uv;
    const Layout& layout = take ? mapped : fallback;
    // An insane projection (a piece xatlas's proxy squashed) keeps the proxy's own units rather
    // than being blown up by the area rule.
    const f64 scale = layout.sane ? std::sqrt(layout.surface / layout.flat) : 1.0;
    for (u32 m = 0; m < members.size(); ++m)
      out.uv[members[m]] =
          Vec2{static_cast<f32>(chosen[m].x * scale), static_cast<f32>(chosen[m].y * scale)};
    for (const u32 v : members)
      local[v] = ~0u;
    i = j;
  }
}

}  // namespace

bool proxy_uv_mesh(const PointMesh& mesh, u32 target_triangles, f32 max_chart_cost,
                   ProxyUvMesh& out, std::string& error) {
  out = ProxyUvMesh{};
  const u32 triangles = mesh.triangle_count();
  const u32 points = mesh.point_count();

  // ---- the proxy -----------------------------------------------------------------------------
  // Positions only, topology kept (meshoptimizer's simplifier never tears a surface), borders free
  // to slide along themselves; an error bound of the whole mesh, so the triangle target decides.
  Vector<u32> coarse(mesh.indices.size());
  f32 reached_error = 0.0f;
  const usize kept = meshopt_simplify(
      coarse.data(), mesh.indices.data(), mesh.indices.size(), mesh.positions.data(), points,
      sizeof(f32) * 3, static_cast<usize>(target_triangles) * 3, 1.0f, 0, &reached_error);
  coarse.resize(static_cast<u32>(kept));
  out.proxy_triangles = coarse.size() / 3;
  if (coarse.empty()) {
    error = "the proxy simplified to nothing";
    return false;
  }

  // ---- xatlas on the proxy -------------------------------------------------------------------
  // Its own points, compacted in first-use order; the UVs only have to be a parameterization per
  // chart, so the packing here is xatlas's estimate and is thrown away later.
  Vector<u32> local_of(points, ~0u);
  Vector<u32> local_point;
  Vector<u32> local_indices;
  local_indices.reserve(coarse.size());
  for (const u32 p : coarse) {
    if (local_of[p] == ~0u) {
      local_of[p] = local_point.size();
      local_point.push_back(p);
    }
    local_indices.push_back(local_of[p]);
  }
  Vector<f32> local_positions;
  local_positions.reserve(local_point.size() * 3);
  for (const u32 p : local_point) {
    for (u32 c = 0; c < 3; ++c)
      local_positions.push_back(mesh.positions[p * 3 + c]);
  }
  xatlas::Atlas* atlas = xatlas::Create();
  xatlas::MeshDecl decl;
  decl.vertexPositionData = local_positions.data();
  decl.vertexPositionStride = sizeof(f32) * 3;
  decl.vertexCount = local_point.size();
  decl.indexData = local_indices.data();
  decl.indexCount = local_indices.size();
  decl.indexFormat = xatlas::IndexFormat::UInt32;
  if (xatlas::AddMesh(atlas, decl, 1) != xatlas::AddMeshError::Success) {
    xatlas::Destroy(atlas);
    error = "xatlas refused the proxy";
    return false;
  }
  xatlas::ChartOptions chart_options;
  chart_options.maxCost = max_chart_cost;
  xatlas::ComputeCharts(atlas, chart_options);
  xatlas::PackOptions estimate;
  estimate.padding = 1;
  xatlas::PackCharts(atlas, estimate);
  if (atlas->meshCount != 1 || atlas->meshes[0].indexCount != local_indices.size()) {
    xatlas::Destroy(atlas);
    error = "xatlas returned a proxy of another shape";
    return false;
  }
  const xatlas::Mesh& charted = atlas->meshes[0];
  const u32 proxy_chart_count = charted.chartCount;
  out.proxy_charts = proxy_chart_count;
  const u32 proxy_count = coarse.size() / 3;
  Vector<ProxyFace> faces(proxy_count);
  Vector<u32> chart_faces(proxy_chart_count, 0u);
  for (u32 f = 0; f < proxy_count; ++f) {
    ProxyFace& face = faces[f];
    for (u32 k = 0; k < 3; ++k) {
      const xatlas::Vertex& v = charted.vertexArray[charted.indexArray[f * 3 + k]];
      face.point[k] = coarse[f * 3 + k];
      face.uv[k] = Vec2{v.uv[0], v.uv[1]};
      if (k == 0) face.chart = v.atlasIndex >= 0 ? v.chartIndex : -1;
    }
    const D3 n = cross(point_at(mesh, face.point[1]) - point_at(mesh, face.point[0]),
                       point_at(mesh, face.point[2]) - point_at(mesh, face.point[0]));
    const f64 len = std::sqrt(dot(n, n));
    face.normal = len > 0.0 ? n * (1.0 / len) : D3{};
    if (face.chart >= 0) ++chart_faces[static_cast<u32>(face.chart)];
  }
  xatlas::Destroy(atlas);

  // The target charts: the proxy's real ones, not its one-face folds.
  u32 largest = 0;
  for (const u32 n : chart_faces)
    largest = std::max(largest, n);
  const u32 min_faces = std::min(k_min_target_faces, largest);
  Vector<u32> targets;
  f64 edge_sum = 0.0;
  for (u32 f = 0; f < proxy_count; ++f) {
    if (faces[f].chart < 0 || chart_faces[static_cast<u32>(faces[f].chart)] < min_faces) continue;
    targets.push_back(f);
    for (u32 k = 0; k < 3; ++k) {
      const D3 e = point_at(mesh, faces[f].point[(k + 1) % 3]) - point_at(mesh, faces[f].point[k]);
      edge_sum += std::sqrt(dot(e, e));
    }
  }
  if (targets.empty()) {
    error = "xatlas charted nothing on the proxy";
    return false;
  }
  for (u32 c = 0; c < proxy_chart_count; ++c)
    out.target_charts += chart_faces[c] >= min_faces ? 1u : 0u;
  const f64 mean_edge = edge_sum / (static_cast<f64>(targets.size()) * 3.0);
  FaceGrid grid;
  grid.build(faces, targets, mesh, mean_edge * 2.0);
  // A face turned away from a triangle is taken only when no face turned towards it is within
  // this: which is what keeps the two sides of a thin wall on their own charts.
  const f64 penalty = mean_edge * 4.0;

  // ---- every fine triangle to its nearest target face
  // ----------------------------------------------
  Vector<u32> face_of(triangles, ~0u);
  Vector<D3> centroid(triangles);
  Vector<D3> normal(triangles);
  Vector<u8> has_normal(triangles, u8{0});
  for (u32 t = 0; t < triangles; ++t) {
    const D3 a = point_at(mesh, mesh.indices[t * 3]);
    const D3 b = point_at(mesh, mesh.indices[t * 3 + 1]);
    const D3 c = point_at(mesh, mesh.indices[t * 3 + 2]);
    centroid[t] = (a + b + c) * (1.0 / 3.0);
    const D3 n = cross(b - a, c - a);
    const f64 len = std::sqrt(dot(n, n));
    if (len > 0.0) {
      normal[t] = n * (1.0 / len);
      has_normal[t] = 1u;
    }
    face_of[t] = grid.nearest(centroid[t], normal[t], has_normal[t] != 0, penalty, -1);
  }
  Vector<i32> chart_of(triangles, -1);
  for (u32 t = 0; t < triangles; ++t)
    chart_of[t] = faces[face_of[t]].chart;

  // ---- borders: neighbours over manifold edges --------------------------------------------------
  Vector<u32> neighbour(triangles * 3u, ~0u);
  {
    struct EdgeRef {
      u64 key = 0;
      u32 triangle = 0;
      u32 side = 0;
    };
    Vector<EdgeRef> edges;
    edges.reserve(triangles * 3u);
    for (u32 t = 0; t < triangles; ++t) {
      for (u32 k = 0; k < 3; ++k) {
        const u64 a = mesh.indices[t * 3 + k];
        const u64 b = mesh.indices[t * 3 + (k + 1) % 3];
        if (a == b) continue;
        edges.push_back(EdgeRef{(std::min(a, b) << 32) | std::max(a, b), t, k});
      }
    }
    std::sort(edges.begin(), edges.end(), [](const EdgeRef& x, const EdgeRef& y) {
      if (x.key != y.key) return x.key < y.key;
      if (x.triangle != y.triangle) return x.triangle < y.triangle;
      return x.side < y.side;
    });
    for (u32 i = 0; i < edges.size();) {
      u32 j = i;
      while (j < edges.size() && edges[j].key == edges[i].key)
        ++j;
      if (j - i == 2) {  // manifold: the two triangles are neighbours across it
        neighbour[edges[i].triangle * 3 + edges[i].side] = edges[i + 1].triangle;
        neighbour[edges[i + 1].triangle * 3 + edges[i + 1].side] = edges[i].triangle;
      }
      i = j;
    }
  }

  // Majority passes, each a pure function of the one before.
  Vector<i32> next_chart;
  Vector<u32> next_face;
  for (u32 pass = 0; pass < k_majority_passes; ++pass) {
    next_chart = chart_of;
    next_face = face_of;
    u32 moved = 0;
    for (u32 t = 0; t < triangles; ++t) {
      const u32 n0 = neighbour[t * 3];
      const u32 n1 = neighbour[t * 3 + 1];
      const u32 n2 = neighbour[t * 3 + 2];
      const u32 ns[3] = {n0, n1, n2};
      for (u32 a = 0; a < 3; ++a) {
        for (u32 b = a + 1; b < 3; ++b) {
          if (ns[a] == ~0u || ns[b] == ~0u) continue;
          const i32 ca = chart_of[ns[a]];
          if (ca == chart_of[t] || ca != chart_of[ns[b]]) continue;
          if (next_chart[t] == chart_of[t] || ca < next_chart[t]) {
            next_chart[t] = ca;
            next_face[t] = face_of[std::min(ns[a], ns[b])];
          }
        }
      }
      if (next_chart[t] != chart_of[t]) ++moved;
    }
    chart_of.swap(next_chart);
    face_of.swap(next_face);
    out.reassigned += moved;
    if (moved == 0) break;
  }

  // Specks: a component of one chart under k_absorb_triangles beside another chart joins the chart
  // it shares the most edges with, and takes its nearest face there.
  {
    Vector<u32> parent(triangles);
    for (u32 t = 0; t < triangles; ++t)
      parent[t] = t;
    for (u32 t = 0; t < triangles; ++t) {
      for (u32 k = 0; k < 3; ++k) {
        const u32 n = neighbour[t * 3 + k];
        if (n == ~0u || chart_of[n] != chart_of[t]) continue;
        const u32 a = find_root(parent, t);
        const u32 b = find_root(parent, n);
        if (a != b) parent[std::max(a, b)] = std::min(a, b);
      }
    }
    Vector<u32> size(triangles, 0u);
    for (u32 t = 0; t < triangles; ++t)
      ++size[find_root(parent, t)];
    // Per small component, the neighbouring chart with the most shared edges (ties: lower chart).
    Vector<i32> join(triangles, -1);
    Vector<u32> join_edges(triangles, 0u);
    {
      struct Shared {
        u32 root = 0;
        i32 chart = 0;
      };
      Vector<Shared> shared;
      for (u32 t = 0; t < triangles; ++t) {
        const u32 root = find_root(parent, t);
        if (size[root] >= k_absorb_triangles) continue;
        for (u32 k = 0; k < 3; ++k) {
          const u32 n = neighbour[t * 3 + k];
          if (n == ~0u || chart_of[n] == chart_of[t]) continue;
          shared.push_back(Shared{root, chart_of[n]});
        }
      }
      std::sort(shared.begin(), shared.end(), [](const Shared& x, const Shared& y) {
        if (x.root != y.root) return x.root < y.root;
        return x.chart < y.chart;
      });
      for (u32 i = 0; i < shared.size();) {
        u32 j = i;
        while (j < shared.size() && shared[j].root == shared[i].root &&
               shared[j].chart == shared[i].chart)
          ++j;
        const u32 count = j - i;
        const u32 root = shared[i].root;
        if (count > join_edges[root]) {
          join_edges[root] = count;
          join[root] = shared[i].chart;
        }
        i = j;
      }
    }
    for (u32 t = 0; t < triangles; ++t) {
      const u32 root = find_root(parent, t);
      if (join[root] < 0) continue;
      chart_of[t] = join[root];
      face_of[t] = grid.nearest(centroid[t], normal[t], has_normal[t] != 0, penalty, join[root]);
      ++out.reassigned;
    }
  }

  // ---- a UV per (point, chart): the projection onto the chart's nearest face among those its
  // triangles there were given
  // ---------------------------------------------------------------------
  struct Corner {
    u32 point = 0;
    i32 chart = 0;
    u32 corner = 0;
  };
  Vector<Corner> corners(triangles * 3u);
  for (u32 t = 0; t < triangles; ++t) {
    for (u32 k = 0; k < 3; ++k)
      corners[t * 3 + k] = Corner{mesh.indices[t * 3 + k], chart_of[t], t * 3 + k};
  }
  std::sort(corners.begin(), corners.end(), [](const Corner& x, const Corner& y) {
    if (x.point != y.point) return x.point < y.point;
    if (x.chart != y.chart) return x.chart < y.chart;
    return x.corner < y.corner;
  });
  out.corners.assign(triangles * 3u, 0u);
  for (u32 i = 0; i < corners.size();) {
    u32 j = i;
    while (j < corners.size() && corners[j].point == corners[i].point &&
           corners[j].chart == corners[i].chart)
      ++j;
    const D3 p = point_at(mesh, corners[i].point);
    u32 best_face = ~0u;
    f64 best = std::numeric_limits<f64>::infinity();
    for (u32 k = i; k < j; ++k) {
      const u32 f = face_of[corners[k].corner / 3];
      const D3 d = p - closest_on_triangle(p, point_at(mesh, faces[f].point[0]),
                                           point_at(mesh, faces[f].point[1]),
                                           point_at(mesh, faces[f].point[2]));
      const f64 dist = dot(d, d);
      if (dist < best || (dist == best && f < best_face)) {
        best = dist;
        best_face = f;
      }
    }
    const ProxyFace& face = faces[best_face];
    f64 l[3];
    plane_barycentrics(p, point_at(mesh, face.point[0]), point_at(mesh, face.point[1]),
                       point_at(mesh, face.point[2]), l);
    auto carried = [&](f32 a, f32 b, f32 c) {
      return static_cast<f32>(l[0] * static_cast<f64>(a) + l[1] * static_cast<f64>(b) +
                              l[2] * static_cast<f64>(c));
    };
    const Vec2 uv{carried(face.uv[0].x, face.uv[1].x, face.uv[2].x),
                  carried(face.uv[0].y, face.uv[1].y, face.uv[2].y)};
    const u32 vertex = out.uv.size();
    out.uv.push_back(uv);
    out.uv_point.push_back(corners[i].point);
    for (u32 k = i; k < j; ++k)
      out.corners[corners[k].corner] = vertex;
    i = j;
  }

  // Charts renumbered densely in first-use order.
  Vector<u32> dense(proxy_chart_count, ~0u);
  out.face_chart.assign(triangles, 0u);
  for (u32 t = 0; t < triangles; ++t) {
    u32& d = dense[static_cast<u32>(chart_of[t])];
    if (d == ~0u) d = out.charts++;
    out.face_chart[t] = d;
  }

  // The projection is only the proxy's parameterization carried across, and where the full
  // surface turns against the proxy's — a thin part, a concavity, a tooth — it folds. So each
  // connected piece of each chart is parameterized again, on its own triangles, by least-squares
  // conformal maps started from the projection; the proxy has then only decided where the charts
  // are.
  refine_conformal(mesh, out);

  // The folds that are left, counted against each chart's own winding (the sign of its UV area).
  Vector<f64> winding(out.charts, 0.0);
  Vector<f64> area(triangles, 0.0);
  for (u32 t = 0; t < triangles; ++t) {
    const Vec2 a = out.uv[out.corners[t * 3]];
    const Vec2 b = out.uv[out.corners[t * 3 + 1]];
    const Vec2 c = out.uv[out.corners[t * 3 + 2]];
    const f64 ax = static_cast<f64>(a.x);
    const f64 ay = static_cast<f64>(a.y);
    area[t] = (static_cast<f64>(b.x) - ax) * (static_cast<f64>(c.y) - ay) -
              (static_cast<f64>(c.x) - ax) * (static_cast<f64>(b.y) - ay);
    winding[out.face_chart[t]] += area[t];
  }
  for (u32 t = 0; t < triangles; ++t) {
    if (area[t] * winding[out.face_chart[t]] < 0.0) ++out.folded;
  }
  return true;
}

}  // namespace engine::atlas
