#include "mesh_query.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace engine::tissue::query {

f64 length(D3 a) noexcept { return std::sqrt(dot(a, a)); }

D3 unit(D3 a) noexcept {
  const f64 l = length(a);
  return l > 0.0 ? a * (1.0 / l) : D3{};
}

f64 angle_deg(D3 a, D3 b) noexcept {
  return std::atan2(length(cross(a, b)), dot(a, b)) * (180.0 / std::numbers::pi);
}

namespace {

f64 component(D3 v, u32 axis) noexcept { return axis == 0 ? v.x : (axis == 1 ? v.y : v.z); }

// Ericson, Real-Time Collision Detection §5.1.5, in double, with the feature it lands on.
TriangleBvh::Nearest nearest_on_triangle(D3 p, D3 a, D3 b, D3 c) noexcept {
  TriangleBvh::Nearest n;
  const auto set = [&](f64 b0, f64 b1, f64 b2, u32 feature) {
    n.b[0] = b0;
    n.b[1] = b1;
    n.b[2] = b2;
    n.feature = feature;
    n.point = a * b0 + b * b1 + c * b2;
    return n;
  };
  const D3 ab = b - a;
  const D3 ac = c - a;
  const D3 ap = p - a;
  const f64 d1 = dot(ab, ap);
  const f64 d2 = dot(ac, ap);
  if (d1 <= 0.0 && d2 <= 0.0) return set(1.0, 0.0, 0.0, 1);
  const D3 bp = p - b;
  const f64 d3 = dot(ab, bp);
  const f64 d4 = dot(ac, bp);
  if (d3 >= 0.0 && d4 <= d3) return set(0.0, 1.0, 0.0, 2);
  const f64 vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
    const f64 v = d1 / (d1 - d3);
    return set(1.0 - v, v, 0.0, 4);
  }
  const D3 cp = p - c;
  const f64 d5 = dot(ab, cp);
  const f64 d6 = dot(ac, cp);
  if (d6 >= 0.0 && d5 <= d6) return set(0.0, 0.0, 1.0, 3);
  const f64 vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
    const f64 w = d2 / (d2 - d6);
    return set(1.0 - w, 0.0, w, 6);
  }
  const f64 va = d3 * d6 - d5 * d4;
  if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
    const f64 w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    return set(0.0, 1.0 - w, w, 5);
  }
  const f64 sum = va + vb + vc;
  if (!(sum > 0.0)) return set(1.0, 0.0, 0.0, 1);  // a degenerate triangle
  const f64 v = vb / sum;
  const f64 w = vc / sum;
  return set(1.0 - v - w, v, w, 0);
}

f64 box_distance_squared(D3 lo, D3 hi, D3 p) noexcept {
  const f64 dx = std::max({lo.x - p.x, 0.0, p.x - hi.x});
  const f64 dy = std::max({lo.y - p.y, 0.0, p.y - hi.y});
  const f64 dz = std::max({lo.z - p.z, 0.0, p.z - hi.z});
  return dx * dx + dy * dy + dz * dz;
}

// The slab test: the parameter interval of a ray inside a box, empty when t_near > t_far.
bool ray_box(D3 lo, D3 hi, D3 origin, D3 inverse, f64 t_min, f64 t_max) noexcept {
  f64 near = t_min;
  f64 far = t_max;
  for (u32 axis = 0; axis < 3; ++axis) {
    const f64 o = component(origin, axis);
    const f64 inv = component(inverse, axis);
    f64 t0 = (component(lo, axis) - o) * inv;
    f64 t1 = (component(hi, axis) - o) * inv;
    if (std::isnan(t0) || std::isnan(t1)) {  // a zero direction on this axis, exactly on a slab
      if (o < component(lo, axis) || o > component(hi, axis)) return false;
      continue;
    }
    if (t0 > t1) std::swap(t0, t1);
    near = std::max(near, t0);
    far = std::min(far, t1);
    if (near > far) return false;
  }
  return true;
}

bool boxes_overlap(D3 alo, D3 ahi, D3 blo, D3 bhi) noexcept {
  return alo.x <= bhi.x && blo.x <= ahi.x && alo.y <= bhi.y && blo.y <= ahi.y && alo.z <= bhi.z &&
         blo.z <= ahi.z;
}

// ---- Möller 1997, "A Fast Triangle-Triangle Intersection Test" --------------------------------

bool compute_intervals(f64 vv0, f64 vv1, f64 vv2, f64 d0, f64 d1, f64 d2, f64 d0d1, f64 d0d2,
                       f64& a, f64& b, f64& c, f64& x0, f64& x1) noexcept {
  if (d0d1 > 0.0) {
    a = vv2;
    b = (vv0 - vv2) * d2;
    c = (vv1 - vv2) * d2;
    x0 = d2 - d0;
    x1 = d2 - d1;
  } else if (d0d2 > 0.0) {
    a = vv1;
    b = (vv0 - vv1) * d1;
    c = (vv2 - vv1) * d1;
    x0 = d1 - d0;
    x1 = d1 - d2;
  } else if (d1 * d2 > 0.0 || d0 != 0.0) {
    a = vv0;
    b = (vv1 - vv0) * d0;
    c = (vv2 - vv0) * d0;
    x0 = d0 - d1;
    x1 = d0 - d2;
  } else if (d1 != 0.0) {
    a = vv1;
    b = (vv0 - vv1) * d1;
    c = (vv2 - vv1) * d1;
    x0 = d1 - d0;
    x1 = d1 - d2;
  } else if (d2 != 0.0) {
    a = vv2;
    b = (vv0 - vv2) * d2;
    c = (vv1 - vv2) * d2;
    x0 = d2 - d0;
    x1 = d2 - d1;
  } else {
    return false;  // coplanar
  }
  return true;
}

struct P2 {
  f64 x;
  f64 y;
};

bool edge_edge(P2 v0, P2 v1, P2 u0, P2 u1) noexcept {
  const f64 ax = v1.x - v0.x;
  const f64 ay = v1.y - v0.y;
  const f64 bx = u0.x - u1.x;
  const f64 by = u0.y - u1.y;
  const f64 cx = v0.x - u0.x;
  const f64 cy = v0.y - u0.y;
  const f64 f = ay * bx - ax * by;
  const f64 d = by * cx - bx * cy;
  if ((f > 0.0 && d >= 0.0 && d <= f) || (f < 0.0 && d <= 0.0 && d >= f)) {
    const f64 e = ax * cy - ay * cx;
    if (f > 0.0) return e >= 0.0 && e <= f;
    return e <= 0.0 && e >= f;
  }
  return false;
}

bool point_in_triangle(P2 p, P2 u0, P2 u1, P2 u2) noexcept {
  const auto side = [&](P2 s, P2 t) {
    const f64 a = t.y - s.y;
    const f64 b = -(t.x - s.x);
    const f64 c = -a * s.x - b * s.y;
    return a * p.x + b * p.y + c;
  };
  const f64 d0 = side(u0, u1);
  const f64 d1 = side(u1, u2);
  const f64 d2 = side(u2, u0);
  return d0 * d1 > 0.0 && d0 * d2 > 0.0;
}

bool coplanar(D3 n, D3 v0, D3 v1, D3 v2, D3 u0, D3 u1, D3 u2) noexcept {
  const f64 ax = std::fabs(n.x);
  const f64 ay = std::fabs(n.y);
  const f64 az = std::fabs(n.z);
  u32 i0 = 1;
  u32 i1 = 2;
  if (ax > ay) {
    if (ax > az) {
      i0 = 1;
      i1 = 2;
    } else {
      i0 = 0;
      i1 = 1;
    }
  } else {
    if (az > ay) {
      i0 = 0;
      i1 = 1;
    } else {
      i0 = 0;
      i1 = 2;
    }
  }
  const auto p = [&](D3 v) { return P2{component(v, i0), component(v, i1)}; };
  const P2 pv[3] = {p(v0), p(v1), p(v2)};
  const P2 pu[3] = {p(u0), p(u1), p(u2)};
  for (u32 i = 0; i < 3; ++i)
    for (u32 j = 0; j < 3; ++j)
      if (edge_edge(pv[i], pv[(i + 1) % 3], pu[j], pu[(j + 1) % 3])) return true;
  return point_in_triangle(pv[0], pu[0], pu[1], pu[2]) ||
         point_in_triangle(pu[0], pv[0], pv[1], pv[2]);
}

}  // namespace

// ---- the hierarchy ------------------------------------------------------------------------------

void TriangleBvh::build(std::span<const Vec3> positions, std::span<const u32> triangles) {
  positions_.resize(static_cast<u32>(positions.size()));
  for (u32 i = 0; i < positions.size(); ++i)
    positions_[i] = d3(positions[i]);
  triangles_.assign(triangles.begin(), triangles.end());
  build_nodes();
}

void TriangleBvh::build(std::span<const D3> positions, std::span<const u32> triangles) {
  positions_.assign(positions.begin(), positions.end());
  triangles_.assign(triangles.begin(), triangles.end());
  build_nodes();
}

void TriangleBvh::build_nodes() {
  const u32 count = triangle_count();
  nodes_.clear();
  order_.resize(count);
  for (u32 t = 0; t < count; ++t)
    order_[t] = t;
  if (count == 0) return;
  Vector<D3> centroid(count);
  Vector<D3> lo(count);
  Vector<D3> hi(count);
  for (u32 t = 0; t < count; ++t) {
    const D3 a = corner(t, 0);
    const D3 b = corner(t, 1);
    const D3 c = corner(t, 2);
    centroid[t] = (a + b + c) * (1.0 / 3.0);
    lo[t] = D3{std::min({a.x, b.x, c.x}), std::min({a.y, b.y, c.y}), std::min({a.z, b.z, c.z})};
    hi[t] = D3{std::max({a.x, b.x, c.x}), std::max({a.y, b.y, c.y}), std::max({a.z, b.z, c.z})};
  }
  nodes_.push_back(Node{});
  struct Task {
    u32 node;
    u32 begin;
    u32 end;
  };
  Vector<Task> tasks;
  tasks.push_back(Task{0, 0, count});
  while (!tasks.empty()) {
    const Task task = tasks.back();
    tasks.pop_back();
    D3 box_lo = lo[order_[task.begin]];
    D3 box_hi = hi[order_[task.begin]];
    D3 c_lo = centroid[order_[task.begin]];
    D3 c_hi = c_lo;
    for (u32 i = task.begin; i < task.end; ++i) {
      const u32 t = order_[i];
      box_lo =
          D3{std::min(box_lo.x, lo[t].x), std::min(box_lo.y, lo[t].y), std::min(box_lo.z, lo[t].z)};
      box_hi =
          D3{std::max(box_hi.x, hi[t].x), std::max(box_hi.y, hi[t].y), std::max(box_hi.z, hi[t].z)};
      c_lo = D3{std::min(c_lo.x, centroid[t].x), std::min(c_lo.y, centroid[t].y),
                std::min(c_lo.z, centroid[t].z)};
      c_hi = D3{std::max(c_hi.x, centroid[t].x), std::max(c_hi.y, centroid[t].y),
                std::max(c_hi.z, centroid[t].z)};
    }
    nodes_[task.node].lo = box_lo;
    nodes_[task.node].hi = box_hi;
    const u32 n = task.end - task.begin;
    if (n <= 4) {
      nodes_[task.node].first = task.begin;
      nodes_[task.node].count = n;
      continue;
    }
    const D3 extent = c_hi - c_lo;
    const u32 axis =
        extent.x >= extent.y && extent.x >= extent.z ? 0u : (extent.y >= extent.z ? 1u : 2u);
    const u32 middle = task.begin + n / 2;
    std::nth_element(order_.begin() + task.begin, order_.begin() + middle,
                     order_.begin() + task.end, [&](u32 x, u32 y) {
                       const f64 cx = component(centroid[x], axis);
                       const f64 cy = component(centroid[y], axis);
                       return cx != cy ? cx < cy : x < y;
                     });
    const u32 left = nodes_.size();
    nodes_[task.node].first = left;
    nodes_[task.node].count = 0;
    nodes_.push_back(Node{});
    nodes_.push_back(Node{});
    tasks.push_back(Task{left, task.begin, middle});
    tasks.push_back(Task{left + 1, middle, task.end});
  }
}

TriangleBvh::Nearest TriangleBvh::nearest(D3 p) const {
  Nearest best;
  if (nodes_.empty()) return best;
  f64 best_squared = std::numeric_limits<f64>::infinity();
  Vector<u32> stack;
  stack.push_back(0);
  while (!stack.empty()) {
    const Node& node = nodes_[stack.back()];
    stack.pop_back();
    if (box_distance_squared(node.lo, node.hi, p) > best_squared) continue;
    if (node.count > 0) {
      for (u32 i = node.first; i < node.first + node.count; ++i) {
        const u32 t = order_[i];
        Nearest candidate = nearest_on_triangle(p, corner(t, 0), corner(t, 1), corner(t, 2));
        const D3 d = p - candidate.point;
        const f64 squared = dot(d, d);
        if (squared < best_squared || (squared == best_squared && t < best.triangle)) {
          best_squared = squared;
          candidate.triangle = t;
          best = candidate;
        }
      }
      continue;
    }
    const f64 left = box_distance_squared(nodes_[node.first].lo, nodes_[node.first].hi, p);
    const f64 right = box_distance_squared(nodes_[node.first + 1].lo, nodes_[node.first + 1].hi, p);
    if (left <= right) {
      stack.push_back(node.first + 1);
      stack.push_back(node.first);
    } else {
      stack.push_back(node.first);
      stack.push_back(node.first + 1);
    }
  }
  best.distance = std::sqrt(best_squared);
  return best;
}

TriangleBvh::Hit TriangleBvh::first_hit(D3 origin, D3 direction, f64 t_min, f64 t_max) const {
  Hit best;
  if (nodes_.empty()) return best;
  const D3 inverse{1.0 / direction.x, 1.0 / direction.y, 1.0 / direction.z};
  f64 limit = t_max;
  Vector<u32> stack;
  stack.push_back(0);
  while (!stack.empty()) {
    const Node& node = nodes_[stack.back()];
    stack.pop_back();
    if (!ray_box(node.lo, node.hi, origin, inverse, t_min, limit)) continue;
    if (node.count == 0) {
      stack.push_back(node.first + 1);
      stack.push_back(node.first);
      continue;
    }
    for (u32 i = node.first; i < node.first + node.count; ++i) {
      const u32 t = order_[i];
      // Möller and Trumbore.
      const D3 a = corner(t, 0);
      const D3 e1 = corner(t, 1) - a;
      const D3 e2 = corner(t, 2) - a;
      const D3 pv = cross(direction, e2);
      const f64 det = dot(e1, pv);
      if (det == 0.0) continue;
      const f64 inv = 1.0 / det;
      const D3 tv = origin - a;
      const f64 u = dot(tv, pv) * inv;
      if (u < 0.0 || u > 1.0) continue;
      const D3 qv = cross(tv, e1);
      const f64 v = dot(direction, qv) * inv;
      if (v < 0.0 || u + v > 1.0) continue;
      const f64 hit = dot(e2, qv) * inv;
      if (!(hit > t_min) || hit > limit) continue;
      if (hit < best.t || (hit == best.t && t < best.triangle)) {
        best.t = hit;
        best.triangle = t;
        best.facing = dot(unit(cross(e1, e2)), unit(direction));
        limit = hit;
      }
    }
  }
  return best;
}

void TriangleBvh::candidate_pairs(const TriangleBvh& other, f64 distance,
                                  Vector<std::pair<u32, u32>>& out) const {
  out.clear();
  if (nodes_.empty() || other.nodes_.empty()) return;
  const f64 reach = distance * distance;
  const auto gap_squared = [](const Node& a, const Node& b) {
    const f64 dx = std::max({b.lo.x - a.hi.x, 0.0, a.lo.x - b.hi.x});
    const f64 dy = std::max({b.lo.y - a.hi.y, 0.0, a.lo.y - b.hi.y});
    const f64 dz = std::max({b.lo.z - a.hi.z, 0.0, a.lo.z - b.hi.z});
    return dx * dx + dy * dy + dz * dz;
  };
  Vector<std::pair<u32, u32>> stack;
  stack.push_back({0, 0});
  while (!stack.empty()) {
    const auto [ia, ib] = stack.back();
    stack.pop_back();
    const Node& a = nodes_[ia];
    const Node& b = other.nodes_[ib];
    if (gap_squared(a, b) > reach) continue;
    if (a.count > 0 && b.count > 0) {
      for (u32 i = a.first; i < a.first + a.count; ++i)
        for (u32 j = b.first; j < b.first + b.count; ++j)
          out.push_back({order_[i], other.order_[j]});
      continue;
    }
    if (a.count == 0) {
      stack.push_back({a.first, ib});
      stack.push_back({a.first + 1, ib});
    } else {
      stack.push_back({ia, b.first});
      stack.push_back({ia, b.first + 1});
    }
  }
  std::sort(out.begin(), out.end());
}

u64 TriangleBvh::intersections(const TriangleBvh& other, u32 max_witnesses,
                               Vector<std::pair<u32, u32>>* witnesses) const {
  if (nodes_.empty() || other.nodes_.empty()) return 0;
  u64 count = 0;
  Vector<std::pair<u32, u32>> found;
  Vector<std::pair<u32, u32>> stack;
  stack.push_back({0, 0});
  while (!stack.empty()) {
    const auto [ia, ib] = stack.back();
    stack.pop_back();
    const Node& a = nodes_[ia];
    const Node& b = other.nodes_[ib];
    if (!boxes_overlap(a.lo, a.hi, b.lo, b.hi)) continue;
    if (a.count > 0 && b.count > 0) {
      for (u32 i = a.first; i < a.first + a.count; ++i) {
        const u32 ta = order_[i];
        for (u32 j = b.first; j < b.first + b.count; ++j) {
          const u32 tb = other.order_[j];
          if (triangles_intersect(corner(ta, 0), corner(ta, 1), corner(ta, 2), other.corner(tb, 0),
                                  other.corner(tb, 1), other.corner(tb, 2))) {
            ++count;
            found.push_back({ta, tb});
          }
        }
      }
      continue;
    }
    if (a.count == 0) {
      stack.push_back({a.first, ib});
      stack.push_back({a.first + 1, ib});
    } else {
      stack.push_back({ia, b.first});
      stack.push_back({ia, b.first + 1});
    }
  }
  if (witnesses != nullptr) {
    std::sort(found.begin(), found.end());
    for (u32 i = 0; i < found.size() && i < max_witnesses; ++i)
      witnesses->push_back(found[i]);
  }
  return count;
}

// ---- the signed surface
// ---------------------------------------------------------------------------

void SignedSurface::build(std::span<const Vec3> positions, std::span<const u32> triangles) {
  bvh_.build(positions, triangles);
  build_normals();
}

void SignedSurface::build(std::span<const D3> positions, std::span<const u32> triangles) {
  bvh_.build(positions, triangles);
  build_normals();
}

void SignedSurface::build_normals() {
  const u32 count = bvh_.triangle_count();
  const Vector<D3>& p = bvh_.positions();
  const Vector<u32>& tri = bvh_.triangles();
  face_normals_.resize(count);
  vertex_normals_.assign(p.size(), D3{});
  Vector<std::pair<u64, D3>> edges;
  for (u32 t = 0; t < count; ++t) {
    const D3 n = unit(cross(p[tri[3 * t + 1]] - p[tri[3 * t]], p[tri[3 * t + 2]] - p[tri[3 * t]]));
    face_normals_[t] = n;
    for (u32 k = 0; k < 3; ++k) {
      const u32 v = tri[3 * t + k];
      const D3 e1 = unit(p[tri[3 * t + (k + 1) % 3]] - p[v]);
      const D3 e2 = unit(p[tri[3 * t + (k + 2) % 3]] - p[v]);
      const f64 angle = std::acos(std::clamp(dot(e1, e2), -1.0, 1.0));
      vertex_normals_[v] = vertex_normals_[v] + n * angle;
      const u32 a = v;
      const u32 b = tri[3 * t + (k + 1) % 3];
      edges.push_back({(u64{std::min(a, b)} << 32) | u64{std::max(a, b)}, n});
    }
  }
  for (D3& n : vertex_normals_)
    n = unit(n);
  std::sort(edges.begin(), edges.end(),
            [](const auto& x, const auto& y) { return x.first < y.first; });
  edge_keys_.clear();
  edge_normals_.clear();
  for (u32 i = 0; i < edges.size();) {
    u32 j = i;
    D3 sum{};
    while (j < edges.size() && edges[j].first == edges[i].first) {
      sum = sum + edges[j].second;
      ++j;
    }
    edge_keys_.push_back(edges[i].first);
    edge_normals_.push_back(unit(sum));
    i = j;
  }
}

SignedSurface::Result SignedSurface::query(D3 p) const {
  Result out;
  const TriangleBvh::Nearest n = bvh_.nearest(p);
  if (n.triangle == k_none) return out;
  const Vector<u32>& tri = bvh_.triangles();
  D3 normal = face_normals_[n.triangle];
  if (n.feature >= 1 && n.feature <= 3) {
    normal = vertex_normals_[tri[3 * n.triangle + n.feature - 1]];
  } else if (n.feature >= 4) {
    const u32 k = n.feature - 4;
    const u32 a = tri[3 * n.triangle + k];
    const u32 b = tri[3 * n.triangle + (k + 1) % 3];
    const u64 key = (u64{std::min(a, b)} << 32) | u64{std::max(a, b)};
    const auto it = std::lower_bound(edge_keys_.begin(), edge_keys_.end(), key);
    if (it != edge_keys_.end() && *it == key)
      normal = edge_normals_[static_cast<u32>(it - edge_keys_.begin())];
  }
  const f64 side = dot(p - n.point, normal);
  out.distance = side < 0.0 ? -n.distance : n.distance;
  out.triangle = n.triangle;
  out.point = n.point;
  out.normal = normal;
  return out;
}

bool triangles_intersect(D3 v0, D3 v1, D3 v2, D3 u0, D3 u1, D3 u2) noexcept {
  const D3 n1 = cross(v1 - v0, v2 - v0);
  const f64 d1 = -dot(n1, v0);
  const f64 du0 = dot(n1, u0) + d1;
  const f64 du1 = dot(n1, u1) + d1;
  const f64 du2 = dot(n1, u2) + d1;
  const f64 du0du1 = du0 * du1;
  const f64 du0du2 = du0 * du2;
  if (du0du1 > 0.0 && du0du2 > 0.0) return false;
  const D3 n2 = cross(u1 - u0, u2 - u0);
  const f64 d2 = -dot(n2, u0);
  const f64 dv0 = dot(n2, v0) + d2;
  const f64 dv1 = dot(n2, v1) + d2;
  const f64 dv2 = dot(n2, v2) + d2;
  const f64 dv0dv1 = dv0 * dv1;
  const f64 dv0dv2 = dv0 * dv2;
  if (dv0dv1 > 0.0 && dv0dv2 > 0.0) return false;
  const D3 d = cross(n1, n2);
  u32 index = 0;
  f64 largest = std::fabs(d.x);
  if (std::fabs(d.y) > largest) {
    largest = std::fabs(d.y);
    index = 1;
  }
  if (std::fabs(d.z) > largest) index = 2;
  const f64 vp0 = component(v0, index);
  const f64 vp1 = component(v1, index);
  const f64 vp2 = component(v2, index);
  const f64 up0 = component(u0, index);
  const f64 up1 = component(u1, index);
  const f64 up2 = component(u2, index);
  f64 a = 0.0, b = 0.0, c = 0.0, x0 = 0.0, x1 = 0.0;
  if (!compute_intervals(vp0, vp1, vp2, dv0, dv1, dv2, dv0dv1, dv0dv2, a, b, c, x0, x1))
    return coplanar(n1, v0, v1, v2, u0, u1, u2);
  f64 e = 0.0, f = 0.0, g = 0.0, y0 = 0.0, y1 = 0.0;
  if (!compute_intervals(up0, up1, up2, du0, du1, du2, du0du1, du0du2, e, f, g, y0, y1))
    return coplanar(n1, v0, v1, v2, u0, u1, u2);
  const f64 xx = x0 * x1;
  const f64 yy = y0 * y1;
  const f64 xxyy = xx * yy;
  f64 tmp = a * xxyy;
  f64 i1[2] = {tmp + b * x1 * yy, tmp + c * x0 * yy};
  tmp = e * xxyy;
  f64 i2[2] = {tmp + f * xx * y1, tmp + g * xx * y0};
  if (i1[0] > i1[1]) std::swap(i1[0], i1[1]);
  if (i2[0] > i2[1]) std::swap(i2[0], i2[1]);
  return !(i1[1] < i2[0] || i2[1] < i1[0]);
}

f64 point_triangle_distance(D3 p, D3 a, D3 b, D3 c) noexcept {
  return length(p - nearest_on_triangle(p, a, b, c).point);
}

f64 segment_distance(D3 p1, D3 q1, D3 p2, D3 q2) noexcept {
  // Ericson, Real-Time Collision Detection §5.1.9, ClosestPtSegmentSegment.
  constexpr f64 k_eps = 1e-30;
  const D3 d1 = q1 - p1;
  const D3 d2 = q2 - p2;
  const D3 r = p1 - p2;
  const f64 a = dot(d1, d1);
  const f64 e = dot(d2, d2);
  const f64 f = dot(d2, r);
  f64 s = 0.0;
  f64 t = 0.0;
  if (a <= k_eps && e <= k_eps) return length(p1 - p2);
  if (a <= k_eps) {
    t = std::clamp(f / e, 0.0, 1.0);
  } else {
    const f64 c = dot(d1, r);
    if (e <= k_eps) {
      s = std::clamp(-c / a, 0.0, 1.0);
    } else {
      const f64 b = dot(d1, d2);
      const f64 denom = a * e - b * b;
      s = denom > 0.0 ? std::clamp((b * f - c * e) / denom, 0.0, 1.0) : 0.0;
      t = (b * s + f) / e;
      if (t < 0.0) {
        t = 0.0;
        s = std::clamp(-c / a, 0.0, 1.0);
      } else if (t > 1.0) {
        t = 1.0;
        s = std::clamp((b - c) / a, 0.0, 1.0);
      }
    }
  }
  return length((p1 + d1 * s) - (p2 + d2 * t));
}

namespace {

// Whether segment pq crosses triangle abc away from its plane: the ends on opposite sides of it by
// more than `eps`, and the crossing point inside it. A segment within `eps` of the plane at both
// ends is the coplanar test's to decide.
bool segment_crosses(D3 p, D3 q, D3 a, D3 b, D3 c, f64 eps) noexcept {
  D3 n = cross(b - a, c - a);
  const f64 area2 = length(n);
  if (!(area2 > 0.0)) return false;
  n = n * (1.0 / area2);
  const f64 sp = dot(n, p - a);
  const f64 sq = dot(n, q - a);
  if ((sp > eps && sq > eps) || (sp < -eps && sq < -eps)) return false;
  if (std::fabs(sp) <= eps && std::fabs(sq) <= eps) return false;
  if (sp * sq > 0.0) return false;  // one end within eps, the other clearly on the same side
  const f64 t = std::clamp(sp / (sp - sq), 0.0, 1.0);
  const D3 x = p + (q - p) * t;
  // Inside by the signed areas of the three sub-triangles against the normal.
  const f64 wa = dot(cross(b - x, c - x), n);
  const f64 wb = dot(cross(c - x, a - x), n);
  const f64 wc = dot(cross(a - x, b - x), n);
  const f64 slack = -eps * area2;
  return wa >= slack && wb >= slack && wc >= slack;
}

}  // namespace

bool triangles_cross(D3 a0, D3 a1, D3 a2, D3 b0, D3 b1, D3 b2) noexcept {
  const D3 a[3] = {a0, a1, a2};
  const D3 b[3] = {b0, b1, b2};
  f64 size = 0.0;
  for (u32 i = 0; i < 3; ++i) {
    size = std::max(size, length(a[(i + 1) % 3] - a[i]));
    size = std::max(size, length(b[(i + 1) % 3] - b[i]));
  }
  const f64 eps = 1.0e-10 * size;
  // Nearly coplanar: every corner of each within eps of the other's plane.
  const auto planar = [&](const D3 t[3], const D3 u[3]) {
    D3 n = cross(t[1] - t[0], t[2] - t[0]);
    const f64 l = length(n);
    if (!(l > 0.0)) return true;
    n = n * (1.0 / l);
    for (u32 i = 0; i < 3; ++i)
      if (std::fabs(dot(n, u[i] - t[0])) > eps) return false;
    return true;
  };
  if (planar(a, b) && planar(b, a))
    return coplanar(cross(a1 - a0, a2 - a0), a0, a1, a2, b0, b1, b2);
  for (u32 i = 0; i < 3; ++i) {
    if (segment_crosses(a[i], a[(i + 1) % 3], b0, b1, b2, eps)) return true;
    if (segment_crosses(b[i], b[(i + 1) % 3], a0, a1, a2, eps)) return true;
  }
  return false;
}

f64 triangle_distance(D3 a0, D3 a1, D3 a2, D3 b0, D3 b1, D3 b2) noexcept {
  if (triangles_cross(a0, a1, a2, b0, b1, b2)) return 0.0;
  f64 best = std::numeric_limits<f64>::infinity();
  const D3 a[3] = {a0, a1, a2};
  const D3 b[3] = {b0, b1, b2};
  for (u32 k = 0; k < 3; ++k) {
    best = std::min(best, point_triangle_distance(a[k], b0, b1, b2));
    best = std::min(best, point_triangle_distance(b[k], a0, a1, a2));
  }
  for (u32 i = 0; i < 3; ++i)
    for (u32 j = 0; j < 3; ++j)
      best = std::min(best, segment_distance(a[i], a[(i + 1) % 3], b[j], b[(j + 1) % 3]));
  return best;
}

f64 winding_number(D3 p, std::span<const D3> positions, std::span<const u32> triangles) noexcept {
  f64 sum = 0.0;
  for (usize t = 0; t + 2 < triangles.size(); t += 3) {
    const D3 a = positions[triangles[t]] - p;
    const D3 b = positions[triangles[t + 1]] - p;
    const D3 c = positions[triangles[t + 2]] - p;
    const f64 la = length(a);
    const f64 lb = length(b);
    const f64 lc = length(c);
    const f64 numerator = dot(a, cross(b, c));
    const f64 denominator = la * lb * lc + dot(a, b) * lc + dot(b, c) * la + dot(c, a) * lb;
    sum += 2.0 * std::atan2(numerator, denominator);
  }
  return sum / (4.0 * std::numbers::pi);
}

f64 tet_volume(D3 a, D3 b, D3 c, D3 d) noexcept { return dot(b - a, cross(c - a, d - a)) / 6.0; }

f64 tet_sicn(D3 a, D3 b, D3 c, D3 d) noexcept {
  // The regular tetrahedron's edge matrix W (columns: its three edges from the first corner, unit
  // length) is upper triangular; A = E W^-1.
  static const f64 s3 = std::sqrt(3.0);
  static const f64 w[3][3] = {
      {1.0, 0.5, 0.5}, {0.0, s3 / 2.0, s3 / 6.0}, {0.0, 0.0, std::sqrt(2.0 / 3.0)}};
  static const f64 inv[3][3] = {
      {1.0 / w[0][0], -w[0][1] / (w[0][0] * w[1][1]),
       (w[0][1] * w[1][2] - w[0][2] * w[1][1]) / (w[0][0] * w[1][1] * w[2][2])},
      {0.0, 1.0 / w[1][1], -w[1][2] / (w[1][1] * w[2][2])},
      {0.0, 0.0, 1.0 / w[2][2]}};
  const D3 e[3] = {b - a, c - a, d - a};
  f64 m[3][3];  // m[row][col] = sum_k E[row][k] inv[k][col], E[row][k] = e[k][row]
  for (u32 row = 0; row < 3; ++row)
    for (u32 col = 0; col < 3; ++col) {
      f64 sum = 0.0;
      for (u32 k = 0; k < 3; ++k)
        sum += component(e[k], row) * inv[k][col];
      m[row][col] = sum;
    }
  const f64 det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                  m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                  m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  f64 frobenius = 0.0;
  for (u32 r = 0; r < 3; ++r)
    for (u32 cc = 0; cc < 3; ++cc)
      frobenius += m[r][cc] * m[r][cc];
  f64 adjugate = 0.0;
  for (u32 r = 0; r < 3; ++r)
    for (u32 cc = 0; cc < 3; ++cc) {
      const u32 r1 = (r + 1) % 3;
      const u32 r2 = (r + 2) % 3;
      const u32 c1 = (cc + 1) % 3;
      const u32 c2 = (cc + 2) % 3;
      const f64 cof = m[r1][c1] * m[r2][c2] - m[r1][c2] * m[r2][c1];
      adjugate += cof * cof;
    }
  const f64 denominator = std::sqrt(frobenius) * std::sqrt(adjugate);
  return denominator > 0.0 ? 3.0 * det / denominator : 0.0;
}

f64 swept_volume(D3 a, D3 b, D3 c, D3 a1, D3 b1, D3 c1) noexcept {
  const D3 d = (a1 - a) + (b1 - b) + (c1 - c);
  const D3 e1 = b - a;
  const D3 e2 = c - a;
  const D3 g1 = (b1 - a1) - e1;
  const D3 g2 = (c1 - a1) - e2;
  const D3 x = cross(e1, e2) + (cross(e1, g2) + cross(g1, e2)) * 0.5 + cross(g1, g2) * (1.0 / 3.0);
  return dot(d, x) / 6.0;
}

f64 enclosed_volume(std::span<const D3> positions, std::span<const u32> triangles) noexcept {
  f64 sum = 0.0;
  for (usize t = 0; t + 2 < triangles.size(); t += 3)
    sum += dot(positions[triangles[t]],
               cross(positions[triangles[t + 1]], positions[triangles[t + 2]]));
  return sum / 6.0;
}

}  // namespace engine::tissue::query
