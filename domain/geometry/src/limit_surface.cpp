#include <core/base/assert.h>
#include <domain/geometry/limit_surface.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

// SSE2 is guaranteed by x86-64 itself (see `apply` below, and domain/anim's skeleton.cpp for the
// same pattern); the `#else` is a non-x86 build.
#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__) || defined(__SSE2__)
#define ENGINE_GEOMETRY_SSE 1
#include <emmintrin.h>
#else
#define ENGINE_GEOMETRY_SSE 0
#endif

namespace engine::geometry {

u32 CsrMatrix::max_row_width() const noexcept {
  u32 widest = 0;
  for (u32 r = 0; r < rows(); ++r)
    widest = std::max(widest, row_offsets[r + 1] - row_offsets[r]);
  return widest;
}

usize CsrMatrix::bytes() const noexcept {
  return usize{row_offsets.size()} * sizeof(u32) + usize{column_index.size()} * sizeof(u32) +
         usize{weight.size()} * sizeof(f32);
}

// The per-frame matvec, and why it is shaped this way. A row is about twelve entries (the fixture's
// level-3 mean is 11.9, the widest 28), so the naive loop — one running sum a row — is a chain of
// dependent additions: twelve add latencies a row, whatever the core's width. Measured on the
// fixture that was 1.24 ns a nonzero, against 0.70 ns for what follows, in the same run
// (docs/subsystems/geometry.md; the bench keeps the first kernel as a row). Two changes take the
// chain apart:
//
// - **Four partial sums a row**, entry e going to sum e mod 4 and the four added pairwise at the
//   end, so four independent chains of three instead of one of twelve.
// - **The cage copied once into (x, y, z, 0) lanes** — 16 bytes a node, a few kilobytes for any
//   cage the budgets allow (800 nodes at most, ADR-0029) — so an entry is one aligned 128-bit load,
//   one broadcast weight, a multiply and an add, instead of three scalar gathers and three of each.
//
// SSE2 is x86-64's own baseline, so this is not a run-time dispatch; the scalar path is for a
// non-x86 build and for a matrix wider than the stack copy, and it sums in the same order. The
// entries run end to end, so `e` carries over from one row to the next and the matrix is one
// sequential stream; the only irregular access is the gather from the copied cage, which is in L1.
namespace {

// Control points the stack copy holds: above the 800-node cage limit, and 16 KB of stack.
constexpr u32 k_lane_columns = 1024;

void apply_scalar(const CsrMatrix& matrix, const Vec3* x, Vec3* y) noexcept {
  const u32 rows = matrix.rows();
  const u32* offsets = matrix.row_offsets.data();
  const u32* column = matrix.column_index.data();
  const f32* weight = matrix.weight.data();
  u32 e = 0;
  for (u32 r = 0; r < rows; ++r) {
    const u32 end = offsets[r + 1];
    Vec3 s[4] = {};
    for (; e + 4 <= end; e += 4)
      for (u32 k = 0; k < 4; ++k)
        s[k] = s[k] + x[column[e + k]] * weight[e + k];
    for (; e < end; ++e)
      s[0] = s[0] + x[column[e]] * weight[e];
    y[r] = (s[0] + s[1]) + (s[2] + s[3]);
  }
}

}  // namespace

void apply(const CsrMatrix& matrix, std::span<const Vec3> columns, std::span<Vec3> out) noexcept {
  ENGINE_ASSERT(columns.size() == matrix.column_count, "one column position per control vertex");
  ENGINE_ASSERT(out.size() == matrix.rows(), "one output per row");
#if ENGINE_GEOMETRY_SSE
  if (matrix.column_count <= k_lane_columns) {
    alignas(16) f32 lanes[k_lane_columns * 4];
    for (u32 c = 0; c < matrix.column_count; ++c) {
      lanes[4 * c] = columns[c].x;
      lanes[4 * c + 1] = columns[c].y;
      lanes[4 * c + 2] = columns[c].z;
      lanes[4 * c + 3] = 0.0f;
    }
    const u32 rows = matrix.rows();
    const u32* offsets = matrix.row_offsets.data();
    const u32* column = matrix.column_index.data();
    const f32* weight = matrix.weight.data();
    Vec3* y = out.data();
    u32 e = 0;
    for (u32 r = 0; r < rows; ++r) {
      const u32 end = offsets[r + 1];
      __m128 s0 = _mm_setzero_ps();
      __m128 s1 = _mm_setzero_ps();
      __m128 s2 = _mm_setzero_ps();
      __m128 s3 = _mm_setzero_ps();
      for (; e + 4 <= end; e += 4) {
        s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_load_ps(lanes + 4 * column[e]), _mm_set1_ps(weight[e])));
        s1 = _mm_add_ps(
            s1, _mm_mul_ps(_mm_load_ps(lanes + 4 * column[e + 1]), _mm_set1_ps(weight[e + 1])));
        s2 = _mm_add_ps(
            s2, _mm_mul_ps(_mm_load_ps(lanes + 4 * column[e + 2]), _mm_set1_ps(weight[e + 2])));
        s3 = _mm_add_ps(
            s3, _mm_mul_ps(_mm_load_ps(lanes + 4 * column[e + 3]), _mm_set1_ps(weight[e + 3])));
      }
      for (; e < end; ++e)
        s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_load_ps(lanes + 4 * column[e]), _mm_set1_ps(weight[e])));
      alignas(16) f32 sum[4];
      _mm_store_ps(sum, _mm_add_ps(_mm_add_ps(s0, s1), _mm_add_ps(s2, s3)));
      y[r] = Vec3{sum[0], sum[1], sum[2]};
    }
    return;
  }
#endif
  apply_scalar(matrix, columns.data(), out.data());
}

void limit_normals(std::span<const Vec3> tangent_u, std::span<const Vec3> tangent_v,
                   std::span<Vec3> out) noexcept {
  ENGINE_ASSERT(tangent_u.size() == tangent_v.size() && out.size() == tangent_u.size(),
                "one normal per tangent pair");
  for (usize i = 0; i < out.size(); ++i) {
    const Vec3 n = cross(tangent_u[i], tangent_v[i]);
    const f32 length = std::sqrt(dot(n, n));
    const f32 inverse = length > 0.0f ? 1.0f / length : 0.0f;
    out[i] = n * inverse;
  }
}

namespace {

constexpr u32 k_none = ~0u;

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

// Loop's vertex weight, as the authoring side computes it.
f64 loop_beta(u32 n) noexcept {
  const f64 c = 0.375 + 0.25 * std::cos(2.0 * std::numbers::pi / static_cast<f64>(n));
  return (0.625 - c * c) / static_cast<f64>(n);
}

// One level's topology: the triangles, the edges in lexicographic order with the triangles on
// each side, and every vertex's neighbours and boundary neighbours.
struct Topology {
  u32 vertex_count = 0;
  Vector<u32> faces;          // three per triangle
  Vector<u32> edges;          // two per edge, a < b, lexicographic
  Vector<u32> edge_opposite;  // two per edge: the vertex opposite it in each triangle, in
                              // triangle order; the second is k_none on a boundary edge
  Vector<u32> face_edges;     // three per triangle: the edges of corners (0,1), (1,2), (2,0)
  Vector<u32> ring_offsets;   // vertex_count + 1
  Vector<u32> ring;           // neighbours, ascending
  Vector<u32> boundary;       // two per vertex: its boundary neighbours, k_none when interior

  u32 face_count() const noexcept { return faces.size() / 3; }
  u32 edge_count() const noexcept { return edges.size() / 2; }
  bool on_boundary(u32 v) const noexcept { return boundary[2 * v] != k_none; }
  u32 valence(u32 v) const noexcept { return ring_offsets[v + 1] - ring_offsets[v]; }
};

struct HalfEdge {
  u32 lo = 0;
  u32 hi = 0;
  u32 face = 0;
  u32 slot = 0;  // corner `slot` to corner `slot + 1`
};

std::string edge_name(u32 a, u32 b) {
  return "(" + std::to_string(a) + ", " + std::to_string(b) + ")";
}

bool build_topology(u32 vertex_count, Vector<u32> faces, Topology& out, std::string* error) {
  out = Topology{};
  out.vertex_count = vertex_count;
  out.faces = std::move(faces);
  const u32 face_count = out.face_count();

  Vector<HalfEdge> half(face_count * 3);
  for (u32 f = 0; f < face_count; ++f) {
    for (u32 s = 0; s < 3; ++s) {
      const u32 a = out.faces[3 * f + s];
      const u32 b = out.faces[3 * f + (s + 1) % 3];
      half[3 * f + s] = HalfEdge{std::min(a, b), std::max(a, b), f, s};
    }
  }
  std::sort(half.begin(), half.end(), [](const HalfEdge& x, const HalfEdge& y) {
    if (x.lo != y.lo) return x.lo < y.lo;
    if (x.hi != y.hi) return x.hi < y.hi;
    if (x.face != y.face) return x.face < y.face;
    return x.slot < y.slot;
  });

  out.face_edges.assign(face_count * 3, k_none);
  out.boundary.assign(vertex_count * 2, k_none);
  Vector<u32> degree(vertex_count, 0u);
  for (u32 i = 0; i < half.size();) {
    u32 j = i + 1;
    while (j < half.size() && half[j].lo == half[i].lo && half[j].hi == half[i].hi)
      ++j;
    const u32 uses = j - i;
    const u32 a = half[i].lo;
    const u32 b = half[i].hi;
    if (uses > 2)
      return fail(error, "edge " + edge_name(a, b) + " is shared by " + std::to_string(uses) +
                             " triangles; the control mesh must be a 2-manifold");
    const u32 edge = out.edge_count();
    out.edges.push_back(a);
    out.edges.push_back(b);
    for (u32 k = i; k < j; ++k) {
      const HalfEdge& h = half[k];
      out.face_edges[3 * h.face + h.slot] = edge;
      out.edge_opposite.push_back(out.faces[3 * h.face + (h.slot + 2) % 3]);
    }
    if (uses == 2) {
      // The two triangles must cross the edge in opposite directions, or the mesh is not oriented.
      const bool first_forward = out.faces[3 * half[i].face + half[i].slot] == a;
      const bool second_forward = out.faces[3 * half[i + 1].face + half[i + 1].slot] == a;
      if (first_forward == second_forward)
        return fail(error, "edge " + edge_name(a, b) + " is wound the same way by triangles " +
                               std::to_string(half[i].face) + " and " +
                               std::to_string(half[i + 1].face) +
                               "; the control mesh must be consistently oriented");
    } else {
      out.edge_opposite.push_back(k_none);
      for (const u32 v : {a, b}) {
        const u32 other = v == a ? b : a;
        if (out.boundary[2 * v] == k_none) {
          out.boundary[2 * v] = other;
        } else if (out.boundary[2 * v + 1] == k_none) {
          out.boundary[2 * v + 1] = other;
        } else {
          return fail(error, "vertex " + std::to_string(v) +
                                 " lies on more than two boundary edges; the control mesh must "
                                 "be a 2-manifold (tag a corner instead of pinching two fans)");
        }
      }
    }
    ++degree[a];
    ++degree[b];
    i = j;
  }

  // Neighbours from the edges. Edges are lexicographic, so every vertex receives its smaller
  // neighbours first in ascending order and then its larger ones in ascending order: each ring is
  // sorted, as the authoring side's `sorted(adj)` is.
  out.ring_offsets.assign(vertex_count + 1, 0u);
  for (u32 v = 0; v < vertex_count; ++v)
    out.ring_offsets[v + 1] = out.ring_offsets[v] + degree[v];
  out.ring.assign(out.ring_offsets[vertex_count], 0u);
  Vector<u32> fill(out.ring_offsets.begin(), out.ring_offsets.end() - 1);
  for (u32 e = 0; e < out.edge_count(); ++e) {
    const u32 a = out.edges[2 * e];
    const u32 b = out.edges[2 * e + 1];
    out.ring[fill[a]++] = b;
    out.ring[fill[b]++] = a;
  }
  return true;
}

// Every vertex's one-ring in counter-clockwise order, walked across its triangles: `p_0 .. p_{n-1}`
// for an interior vertex, `p_0 .. p_k` from one boundary neighbour to the other for a boundary one.
// This is also the manifold check the edge table cannot make: a vertex whose triangles form two
// fans (two cones touching at a point) has consistent edges and no single ring.
struct Fans {
  Vector<u32> offsets;  // vertex_count + 1
  Vector<u32> ring;
  Vector<u32> faces;  // triangles around each vertex (k); n for an interior vertex
};

bool build_fans(const Topology& t, Fans& out, std::string* error) {
  const u32 n = t.vertex_count;
  const u32 face_count = t.face_count();
  Vector<u32> incident_offsets(n + 1, 0u);
  for (u32 i = 0; i < face_count * 3; ++i)
    ++incident_offsets[t.faces[i] + 1];
  for (u32 v = 0; v < n; ++v)
    incident_offsets[v + 1] += incident_offsets[v];
  Vector<u32> incident(face_count * 3, 0u);
  Vector<u32> fill(incident_offsets.begin(), incident_offsets.end() - 1);
  for (u32 i = 0; i < face_count * 3; ++i)
    incident[fill[t.faces[i]]++] = i;  // a corner: face * 3 + slot

  out.offsets.assign(n + 1, 0u);
  out.ring.clear();
  out.ring.reserve(t.ring.size());
  out.faces.assign(n, 0u);
  Vector<u32> wedge_a;
  Vector<u32> wedge_b;
  Vector<u8> used;
  for (u32 v = 0; v < n; ++v) {
    wedge_a.clear();
    wedge_b.clear();
    for (u32 i = incident_offsets[v]; i < incident_offsets[v + 1]; ++i) {
      const u32 f = incident[i] / 3;
      const u32 s = incident[i] % 3;
      wedge_a.push_back(t.faces[3 * f + (s + 1) % 3]);
      wedge_b.push_back(t.faces[3 * f + (s + 2) % 3]);
    }
    const u32 k = wedge_a.size();
    out.faces[v] = k;
    if (k == 0) {
      out.offsets[v + 1] = out.ring.size();
      continue;
    }
    // A fan's start is a wedge whose `a` no other wedge ends at; an interior vertex has none.
    u32 start = k_none;
    u32 starts = 0;
    for (u32 w = 0; w < k; ++w) {
      bool ends_here = false;
      for (u32 x = 0; x < k; ++x)
        ends_here = ends_here || wedge_b[x] == wedge_a[w];
      if (!ends_here) {
        ++starts;
        start = w;
      }
    }
    const bool open = starts == 1;
    if (starts > 1)
      return fail(error, "vertex " + std::to_string(v) +
                             " joins more than one fan of triangles; the control mesh must be "
                             "a 2-manifold");
    if (!open) {
      start = 0;
      for (u32 w = 1; w < k; ++w)
        if (wedge_a[w] < wedge_a[start]) start = w;
    }
    used.assign(k, 0);
    u32 at = start;
    out.ring.push_back(wedge_a[at]);
    for (u32 step = 0; step < k; ++step) {
      used[at] = 1;
      const u32 next_vertex = wedge_b[at];
      if (open && step + 1 == k) {
        out.ring.push_back(next_vertex);
        break;
      }
      u32 next = k_none;
      for (u32 x = 0; x < k; ++x)
        if (wedge_a[x] == next_vertex && used[x] == 0) next = x;
      if (next == k_none) {
        // A closed fan returns to its start after exactly k wedges; anything else is two fans.
        if (!open && step + 1 == k && next_vertex == wedge_a[start]) break;
        return fail(error, "vertex " + std::to_string(v) +
                               " joins more than one fan of triangles; the control mesh must be "
                               "a 2-manifold");
      }
      out.ring.push_back(next_vertex);
      at = next;
    }
    out.offsets[v + 1] = out.ring.size();
    const u32 ring_size = out.offsets[v + 1] - out.offsets[v];
    if (ring_size != t.valence(v) || open != t.on_boundary(v))
      return fail(error, "vertex " + std::to_string(v) +
                             " joins more than one fan of triangles; the control mesh must be "
                             "a 2-manifold");
  }
  return true;
}

// Rows over the control vertices, in double: one level's vertices as combinations of the control
// positions. Composed level by level, then rounded to f32 once.
struct Rows {
  Vector<u32> offsets;
  Vector<u32> columns;
  Vector<f64> values;
  Rows() { offsets.push_back(0); }
  u32 count() const noexcept { return offsets.size() - 1; }
};

// A dense scratch row the width of the control mesh, with the touched columns listed so that
// clearing it costs the row and not the width. Contributions accumulate in the order they are
// added, which is the stencil's own order, so the double result is the same on every run.
class Accumulator {
 public:
  explicit Accumulator(u32 width) : value_(width, 0.0), mark_(width, u8{0}) {}

  void add(const Rows& rows, u32 row, f64 scale) {
    for (u32 e = rows.offsets[row]; e < rows.offsets[row + 1]; ++e) {
      const u32 c = rows.columns[e];
      if (mark_[c] == 0) {
        mark_[c] = 1;
        touched_.push_back(c);
      }
      value_[c] += scale * rows.values[e];
    }
  }

  // Appends the accumulated row. `cancel` drops entries that cancelled to below that fraction of
  // the row's largest magnitude: tangent rows mix signs, and an entry at 1e-17 of its neighbours
  // is arithmetic noise that f32 could not represent next to them anyway. Position rows never
  // cancel (every weight is positive), so they pass 0 and keep every entry.
  void emit(Rows& out, f64 cancel) {
    std::sort(touched_.begin(), touched_.end());
    f64 largest = 0.0;
    for (const u32 c : touched_)
      largest = std::max(largest, std::fabs(value_[c]));
    for (const u32 c : touched_) {
      const f64 v = value_[c];
      if (v != 0.0 && std::fabs(v) > cancel * largest) {
        out.columns.push_back(c);
        out.values.push_back(v);
      }
      value_[c] = 0.0;
      mark_[c] = 0;
    }
    touched_.clear();
    out.offsets.push_back(out.columns.size());
  }

 private:
  Vector<f64> value_;
  Vector<u8> mark_;
  Vector<u32> touched_;
};

constexpr f64 k_tangent_cancel = 1.0e-10;

// Rounds a double operator to f32 and puts each row's rounding residual on its largest entry, so
// a position row sums to 1 and a tangent row to 0 as closely as f32 can say it.
void finalize(const Rows& rows, u32 columns, f64 row_sum, CsrMatrix& out) {
  out.column_count = columns;
  out.row_offsets.assign(rows.offsets.begin(), rows.offsets.end());
  out.column_index.assign(rows.columns.begin(), rows.columns.end());
  out.weight.resize(rows.values.size());
  for (u32 r = 0; r < rows.count(); ++r) {
    const u32 first = rows.offsets[r];
    const u32 last = rows.offsets[r + 1];
    if (first == last) continue;
    f64 sum = 0.0;
    u32 largest = first;
    for (u32 e = first; e < last; ++e) {
      out.weight[e] = static_cast<f32>(rows.values[e]);
      sum += static_cast<f64>(out.weight[e]);
      if (std::fabs(rows.values[e]) > std::fabs(rows.values[largest])) largest = e;
    }
    out.weight[largest] = static_cast<f32>(static_cast<f64>(out.weight[largest]) + (row_sum - sum));
  }
}

// One subdivision step's rows: every old vertex by its vertex rule, then every edge by its edge
// rule, in the order the new level numbers them.
void subdivide_rows(const Topology& t, const Vector<u8>& corner, const Rows& in, Accumulator& acc,
                    Rows& out) {
  out = Rows{};
  for (u32 v = 0; v < t.vertex_count; ++v) {
    const u32 n = t.valence(v);
    if (corner[v] != 0 || n == 0) {
      acc.add(in, v, 1.0);
    } else if (t.on_boundary(v)) {
      acc.add(in, v, 0.75);
      acc.add(in, t.boundary[2 * v], 0.125);
      acc.add(in, t.boundary[2 * v + 1], 0.125);
    } else {
      const f64 beta = loop_beta(n);
      acc.add(in, v, 1.0 - static_cast<f64>(n) * beta);
      for (u32 i = t.ring_offsets[v]; i < t.ring_offsets[v + 1]; ++i)
        acc.add(in, t.ring[i], beta);
    }
    acc.emit(out, 0.0);
  }
  for (u32 e = 0; e < t.edge_count(); ++e) {
    const u32 a = t.edges[2 * e];
    const u32 b = t.edges[2 * e + 1];
    const u32 d = t.edge_opposite[2 * e + 1];
    if (d == k_none) {
      acc.add(in, a, 0.5);
      acc.add(in, b, 0.5);
    } else {
      acc.add(in, a, 0.375);
      acc.add(in, b, 0.375);
      acc.add(in, t.edge_opposite[2 * e], 0.125);
      acc.add(in, d, 0.125);
    }
    acc.emit(out, 0.0);
  }
}

void limit_rows(const Topology& t, const Vector<u8>& corner, const Rows& in, Accumulator& acc,
                Rows& out) {
  out = Rows{};
  for (u32 v = 0; v < t.vertex_count; ++v) {
    const u32 n = t.valence(v);
    if (corner[v] != 0 || n == 0) {
      acc.add(in, v, 1.0);
    } else if (t.on_boundary(v)) {
      acc.add(in, v, 4.0 / 6.0);
      acc.add(in, t.boundary[2 * v], 1.0 / 6.0);
      acc.add(in, t.boundary[2 * v + 1], 1.0 / 6.0);
    } else {
      const f64 beta = loop_beta(n);
      const f64 k = 3.0 / (8.0 * beta);
      acc.add(in, v, k / (k + static_cast<f64>(n)));
      for (u32 i = t.ring_offsets[v]; i < t.ring_offsets[v + 1]; ++i)
        acc.add(in, t.ring[i], 1.0 / (k + static_cast<f64>(n)));
    }
    acc.emit(out, 0.0);
  }
}

// The cross-boundary tangent mask of a boundary vertex with k >= 2 faces under these rules: the
// left eigenvector `a v + b (p_0 + p_k) + sum sin(i pi / k) p_i` of the local subdivision matrix
// for the eigenvalue `3/8 + cos(pi / k) / 4` (docs/subsystems/geometry.md derives it).
void cross_boundary_mask(u32 k, f64& a, f64& b) noexcept {
  const f64 theta = std::numbers::pi / static_cast<f64>(k);
  const f64 c = std::cos(theta);
  const f64 s = std::sin(theta);
  const f64 cot_half = 1.0 / std::tan(0.5 * theta);
  b = (3.0 * cot_half + s * (2.0 * c - 3.0)) / ((2.0 * c - 5.0) * (2.0 * c + 1.0));
  a = b * (2.0 * c - 1.0) - s;
}

void tangent_rows(const Topology& t, const Fans& fans, const Vector<u8>& corner, const Rows& in,
                  Accumulator& acc, Rows& out_u, Rows& out_v) {
  out_u = Rows{};
  out_v = Rows{};
  for (u32 v = 0; v < t.vertex_count; ++v) {
    const u32* p = fans.ring.data() + fans.offsets[v];
    const u32 count = fans.offsets[v + 1] - fans.offsets[v];
    if (count == 0) {
      acc.emit(out_u, k_tangent_cancel);
      acc.emit(out_v, k_tangent_cancel);
      continue;
    }
    if (!t.on_boundary(v)) {
      // Loop's eigenvector masks over the counter-clockwise one-ring; a tagged interior corner
      // keeps them, because its fan still surrounds it.
      const f64 step = 2.0 * std::numbers::pi / static_cast<f64>(count);
      for (u32 i = 0; i < count; ++i)
        acc.add(in, p[i], std::cos(step * static_cast<f64>(i)));
      acc.emit(out_u, k_tangent_cancel);
      for (u32 i = 0; i < count; ++i)
        acc.add(in, p[i], std::sin(step * static_cast<f64>(i)));
      acc.emit(out_v, k_tangent_cancel);
      continue;
    }
    const u32 k = count - 1;  // faces around a boundary vertex
    acc.add(in, p[0], 1.0);
    acc.add(in, p[k], -1.0);
    acc.emit(out_u, k_tangent_cancel);
    if (corner[v] != 0) {
      // A corner has no tangent plane; its fan's own directions stand in for one.
      acc.add(in, p[0], 0.5);
      acc.add(in, p[k], 0.5);
      for (u32 i = 1; i < k; ++i)
        acc.add(in, p[i], 1.0);
      acc.add(in, v, -static_cast<f64>(k));
    } else if (k == 1) {
      acc.add(in, p[0], 1.0);
      acc.add(in, p[1], 1.0);
      acc.add(in, v, -2.0);
    } else {
      f64 a = 0.0;
      f64 b = 0.0;
      cross_boundary_mask(k, a, b);
      const f64 theta = std::numbers::pi / static_cast<f64>(k);
      acc.add(in, v, a);
      acc.add(in, p[0], b);
      acc.add(in, p[k], b);
      for (u32 i = 1; i < k; ++i)
        acc.add(in, p[i], std::sin(theta * static_cast<f64>(i)));
    }
    acc.emit(out_v, k_tangent_cancel);
  }
}

}  // namespace

bool build_loop_limit_surface(std::span<const u32> control_faces, u32 control_vertex_count,
                              const LoopSurfaceOptions& options, LoopLimitSurface& out,
                              std::string* error) {
  out = LoopLimitSurface{};
  if (options.level > k_loop_max_level)
    return fail(error, "level " + std::to_string(options.level) + " is above the maximum of " +
                           std::to_string(k_loop_max_level));
  if (control_faces.size() % 3 != 0)
    return fail(error, "the control faces are not a whole number of triangles");
  const u32 face_count = static_cast<u32>(control_faces.size() / 3);
  // Every level quadruples the triangles; the refined counts must fit the u32 indices.
  if (u64{face_count} << (2 * options.level) > u64{0xffffffffu} / 3)
    return fail(error, "the refined mesh would not fit 32-bit indices at this level");
  for (u32 f = 0; f < face_count; ++f) {
    const u32 a = control_faces[3 * f];
    const u32 b = control_faces[3 * f + 1];
    const u32 c = control_faces[3 * f + 2];
    if (a >= control_vertex_count || b >= control_vertex_count || c >= control_vertex_count)
      return fail(error, "triangle " + std::to_string(f) + " names a vertex out of range");
    if (a == b || b == c || c == a)
      return fail(error, "triangle " + std::to_string(f) + " repeats a corner");
  }
  Vector<u8> corner(control_vertex_count, u8{0});
  for (const u32 v : options.corners) {
    if (v >= control_vertex_count)
      return fail(error, "corner " + std::to_string(v) + " is out of range");
    corner[v] = 1;
  }

  Topology level_topology;
  if (!build_topology(control_vertex_count, Vector<u32>(control_faces.begin(), control_faces.end()),
                      level_topology, error))
    return false;
  {
    Fans fans;
    if (!build_fans(level_topology, fans, error)) return false;
    for (u32 v = 0; v < control_vertex_count; ++v)
      if (level_topology.on_boundary(v) && corner[v] == 0)
        out.max_boundary_faces = std::max(out.max_boundary_faces, fans.faces[v]);
  }

  out.level = options.level;
  out.control_vertex_count = control_vertex_count;
  out.control_edges = level_topology.edges;

  // Level 0: every vertex is itself, every edge its own parent, every face its own.
  Rows rows;
  for (u32 v = 0; v < control_vertex_count; ++v) {
    rows.columns.push_back(v);
    rows.values.push_back(1.0);
    rows.offsets.push_back(rows.columns.size());
  }
  out.vertex_parent.resize(control_vertex_count);
  for (u32 v = 0; v < control_vertex_count; ++v)
    out.vertex_parent[v] = LoopParent{LoopParentKind::vertex, v};
  out.face_parent.resize(face_count);
  for (u32 f = 0; f < face_count; ++f)
    out.face_parent[f] = f;
  Vector<LoopParent> edge_parent(level_topology.edge_count());
  for (u32 e = 0; e < level_topology.edge_count(); ++e)
    edge_parent[e] = LoopParent{LoopParentKind::edge, e};

  Accumulator acc(control_vertex_count);
  for (u32 step = 0; step < options.level; ++step) {
    const Topology& t = level_topology;
    Rows next_rows;
    subdivide_rows(t, corner, rows, acc, next_rows);

    // The new level's vertices: the old ones at their indices, then one per edge.
    const u32 old_count = t.vertex_count;
    for (u32 e = 0; e < t.edge_count(); ++e)
      out.vertex_parent.push_back(edge_parent[e]);
    corner.resize(old_count + t.edge_count(), u8{0});

    Vector<u32> next_faces(t.face_count() * 12);
    Vector<u32> next_face_parent(t.face_count() * 4);
    for (u32 f = 0; f < t.face_count(); ++f) {
      const u32 a = t.faces[3 * f];
      const u32 b = t.faces[3 * f + 1];
      const u32 c = t.faces[3 * f + 2];
      const u32 ab = old_count + t.face_edges[3 * f];
      const u32 bc = old_count + t.face_edges[3 * f + 1];
      const u32 ca = old_count + t.face_edges[3 * f + 2];
      const u32 child[12] = {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca};
      for (u32 i = 0; i < 12; ++i)
        next_faces[12 * f + i] = child[i];
      for (u32 i = 0; i < 4; ++i)
        next_face_parent[4 * f + i] = out.face_parent[f];
    }

    Topology next;
    if (!build_topology(old_count + t.edge_count(), std::move(next_faces), next, error))
      return false;  // unreachable for a valid control mesh; kept so a bug reports itself

    // Each new edge lies on an old edge (its half) or inside an old triangle. The children of
    // triangle f are 4f .. 4f + 3 in the order above; slot s is the edge from corner s to s + 1.
    Vector<LoopParent> next_edge_parent(next.edge_count());
    for (u32 f = 0; f < t.face_count(); ++f) {
      const LoopParent inside{LoopParentKind::face, next_face_parent[4 * f]};
      const LoopParent on_ab = edge_parent[t.face_edges[3 * f]];
      const LoopParent on_bc = edge_parent[t.face_edges[3 * f + 1]];
      const LoopParent on_ca = edge_parent[t.face_edges[3 * f + 2]];
      const LoopParent slots[12] = {on_ab, inside, on_ca, on_bc,  inside, on_ab,
                                    on_ca, inside, on_bc, inside, inside, inside};
      for (u32 i = 0; i < 12; ++i)
        next_edge_parent[next.face_edges[12 * f + i]] = slots[i];
    }

    rows = std::move(next_rows);
    out.face_parent = std::move(next_face_parent);
    edge_parent = std::move(next_edge_parent);
    level_topology = std::move(next);
  }

  Rows limit;
  limit_rows(level_topology, corner, rows, acc, limit);
  finalize(limit, control_vertex_count, 1.0, out.limit);

  if (options.tangents) {
    Fans fans;
    if (!build_fans(level_topology, fans, error)) return false;
    Rows tangent_u;
    Rows tangent_v;
    tangent_rows(level_topology, fans, corner, rows, acc, tangent_u, tangent_v);
    finalize(tangent_u, control_vertex_count, 0.0, out.tangent_u);
    finalize(tangent_v, control_vertex_count, 0.0, out.tangent_v);
  }

  out.faces = std::move(level_topology.faces);
  return true;
}

}  // namespace engine::geometry
