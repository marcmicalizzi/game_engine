// The host-surface foundation (host.h): the weld, the BVH and the exposure terms.
#include <core/math/deterministic.h>
#include <domain/cover/host.h>

#include <algorithm>
#include <cmath>

namespace engine::cover {

namespace {

constexpr f64 k_pi = 3.14159265358979323846;
constexpr u32 k_leaf_size = 4;

P3 widen(Vec3 v) { return P3{static_cast<f64>(v.x), static_cast<f64>(v.y), static_cast<f64>(v.z)}; }
P3 min3(P3 a, P3 b) {
  return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z};
}
P3 max3(P3 a, P3 b) {
  return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z};
}
f64 axis_of(P3 p, u32 axis) { return axis == 0 ? p.x : (axis == 1 ? p.y : p.z); }

// The squared distance from `p` to the box.
f64 box_distance2(P3 p, P3 lo, P3 hi) {
  const f64 dx = p.x < lo.x ? lo.x - p.x : (p.x > hi.x ? p.x - hi.x : 0.0);
  const f64 dy = p.y < lo.y ? lo.y - p.y : (p.y > hi.y ? p.y - hi.y : 0.0);
  const f64 dz = p.z < lo.z ? lo.z - p.z : (p.z > hi.z ? p.z - hi.z : 0.0);
  return dx * dx + dy * dy + dz * dz;
}

// The closest point of triangle abc to p (Ericson, Real-Time Collision Detection, 5.1.5).
P3 closest_on_triangle(P3 p, P3 a, P3 b, P3 c) {
  const P3 ab = b - a;
  const P3 ac = c - a;
  const P3 ap = p - a;
  const f64 d1 = dot(ab, ap);
  const f64 d2 = dot(ac, ap);
  if (d1 <= 0.0 && d2 <= 0.0) return a;
  const P3 bp = p - b;
  const f64 d3 = dot(ab, bp);
  const f64 d4 = dot(ac, bp);
  if (d3 >= 0.0 && d4 <= d3) return b;
  const f64 vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
    const f64 v = d1 / (d1 - d3);
    return a + ab * v;
  }
  const P3 cp = p - c;
  const f64 d5 = dot(ab, cp);
  const f64 d6 = dot(ac, cp);
  if (d6 >= 0.0 && d5 <= d6) return c;
  const f64 vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
    const f64 w = d2 / (d2 - d6);
    return a + ac * w;
  }
  const f64 va = d3 * d6 - d5 * d4;
  if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
    const f64 w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    return b + (c - b) * w;
  }
  const f64 denom = 1.0 / (va + vb + vc);
  const f64 v = vb * denom;
  const f64 w = vc * denom;
  return a + ab * v + ac * w;
}

// Whether the segment hits triangle abc past a small epsilon (Moller and Trumbore 1997).
bool ray_hits(P3 o, P3 d, f64 length, P3 a, P3 b, P3 c) {
  const P3 e1 = b - a;
  const P3 e2 = c - a;
  const P3 pv = cross(d, e2);
  const f64 det = dot(e1, pv);
  if (det > -1e-14 && det < 1e-14) return false;
  const f64 inv = 1.0 / det;
  const P3 tv = o - a;
  const f64 u = dot(tv, pv) * inv;
  if (u < 0.0 || u > 1.0) return false;
  const P3 qv = cross(tv, e1);
  const f64 v = dot(d, qv) * inv;
  if (v < 0.0 || u + v > 1.0) return false;
  const f64 t = dot(e2, qv) * inv;
  return t > 1e-7 && t < length;
}

// Whether the segment meets the box (the slab test).
bool ray_meets_box(P3 o, P3 d, f64 length, P3 lo, P3 hi) {
  f64 t0 = 0.0;
  f64 t1 = length;
  for (u32 axis = 0; axis < 3; ++axis) {
    const f64 origin = axis_of(o, axis);
    const f64 dir = axis_of(d, axis);
    const f64 a = axis_of(lo, axis);
    const f64 b = axis_of(hi, axis);
    if (dir > -1e-300 && dir < 1e-300) {
      if (origin < a || origin > b) return false;
      continue;
    }
    f64 near = (a - origin) / dir;
    f64 far = (b - origin) / dir;
    if (near > far) std::swap(near, far);
    t0 = near > t0 ? near : t0;
    t1 = far < t1 ? far : t1;
    if (t0 > t1) return false;
  }
  return true;
}

}  // namespace

f64 length_of(P3 a) { return std::sqrt(dot(a, a)); }

P3 normalized(P3 a) {
  const f64 l = std::sqrt(dot(a, a));
  return l > 0.0 ? a * (1.0 / l) : P3{0.0, 1.0, 0.0};
}

P3 HostSurface::centroid(u32 triangle) const noexcept {
  return (corner(triangle, 0) + corner(triangle, 1) + corner(triangle, 2)) * (1.0 / 3.0);
}

bool make_host_surface(const assets::GeneratorHostMesh& host, HostSurface& out,
                       std::string& error) {
  out = HostSurface{};
  const u32 triangles = host.indices.size() / 3u;
  if (triangles == 0 || host.positions.empty()) {
    error = "the host has no triangles";
    return false;
  }
  for (const u32 index : host.indices) {
    if (index >= host.positions.size()) {
      error = "the host names a vertex it does not have";
      return false;
    }
  }
  out.positions.reserve(host.positions.size());
  for (const Vec3 p : host.positions)
    out.positions.push_back(widen(p));
  out.indices = host.indices;
  out.indices.resize(triangles * 3u);
  out.roughness.resize(triangles, 1.0f);
  out.holds.resize(triangles, 1u);
  for (u32 t = 0; t < triangles; ++t) {
    if (t < host.roughness.size()) out.roughness[t] = host.roughness[t];
    if (t < host.holds.size()) out.holds[t] = host.holds[t];
  }
  // The weld: one id per distinct position, numbered in the order of the sorted positions, which
  // is a total order (position, then index) and so the same on every toolchain.
  const u32 n = out.positions.size();
  Vector<u32> order;
  order.resize(n);
  for (u32 i = 0; i < n; ++i)
    order[i] = i;
  const Vector<P3>& pos = out.positions;
  std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
    if (pos[a].x != pos[b].x) return pos[a].x < pos[b].x;
    if (pos[a].y != pos[b].y) return pos[a].y < pos[b].y;
    if (pos[a].z != pos[b].z) return pos[a].z < pos[b].z;
    return a < b;
  });
  out.weld.resize(n, 0u);
  u32 id = 0;
  for (u32 i = 0; i < n; ++i) {
    if (i > 0) {
      const P3 a = pos[order[i - 1]];
      const P3 b = pos[order[i]];
      if (!(a.x == b.x && a.y == b.y && a.z == b.z)) ++id;
    }
    out.weld[order[i]] = id;
  }
  out.welded = id + 1u;
  // Faces, areas, the welded normals and the bounds.
  out.face_normal.resize(triangles);
  out.face_area.resize(triangles, 0.0);
  out.welded_normal.resize(out.welded, P3{0.0, 0.0, 0.0});
  out.lo = pos[0];
  out.hi = pos[0];
  for (const P3 p : pos) {
    out.lo = min3(out.lo, p);
    out.hi = max3(out.hi, p);
  }
  for (u32 t = 0; t < triangles; ++t) {
    const P3 c = cross(out.corner(t, 1) - out.corner(t, 0), out.corner(t, 2) - out.corner(t, 0));
    const f64 l = length_of(c);
    out.face_area[t] = 0.5 * l;
    out.face_normal[t] = l > 0.0 ? c * (1.0 / l) : P3{0.0, 1.0, 0.0};
    for (u32 k = 0; k < 3; ++k) {
      const u32 w = out.weld[out.indices[t * 3u + k]];
      out.welded_normal[w] = out.welded_normal[w] + c * 0.5;
    }
  }
  for (P3& normal : out.welded_normal)
    normal = normalized(normal);
  // The welded adjacency, as sorted unique pairs.
  Vector<u64> edges;
  edges.reserve(triangles * 6u);
  for (u32 t = 0; t < triangles; ++t) {
    for (u32 k = 0; k < 3; ++k) {
      const u32 a = out.weld[out.indices[t * 3u + k]];
      const u32 b = out.weld[out.indices[t * 3u + (k + 1u) % 3u]];
      if (a == b) continue;
      edges.push_back((static_cast<u64>(a) << 32) | b);
      edges.push_back((static_cast<u64>(b) << 32) | a);
    }
  }
  std::sort(edges.begin(), edges.end());
  out.adjacency_offset.resize(out.welded + 1u, 0u);
  u64 last = ~0ull;
  for (const u64 e : edges) {
    if (e == last) continue;
    last = e;
    out.adjacency.push_back(static_cast<u32>(e & 0xFFFFFFFFull));
    ++out.adjacency_offset[static_cast<u32>(e >> 32) + 1u];
  }
  for (u32 v = 0; v < out.welded; ++v)
    out.adjacency_offset[v + 1u] += out.adjacency_offset[v];
  return true;
}

void HostBvh::build(const HostSurface& surface) {
  surface_ = &surface;
  nodes_.clear();
  order_.clear();
  const u32 triangles = surface.triangle_count();
  order_.resize(triangles);
  Vector<P3> centroid;
  centroid.resize(triangles);
  for (u32 t = 0; t < triangles; ++t) {
    order_[t] = t;
    centroid[t] = surface.centroid(t);
  }
  struct Task {
    u32 node;
    u32 first;
    u32 count;
  };
  Vector<Task> stack;
  nodes_.push_back(Node{});
  stack.push_back(Task{0, 0, triangles});
  while (!stack.empty()) {
    const Task task = stack.back();
    stack.pop_back();
    P3 lo = surface.corner(order_[task.first], 0);
    P3 hi = lo;
    P3 clo = centroid[order_[task.first]];
    P3 chi = clo;
    for (u32 i = task.first; i < task.first + task.count; ++i) {
      for (u32 k = 0; k < 3; ++k) {
        lo = min3(lo, surface.corner(order_[i], k));
        hi = max3(hi, surface.corner(order_[i], k));
      }
      clo = min3(clo, centroid[order_[i]]);
      chi = max3(chi, centroid[order_[i]]);
    }
    nodes_[task.node].lo = lo;
    nodes_[task.node].hi = hi;
    if (task.count <= k_leaf_size) {
      nodes_[task.node].first = task.first;
      nodes_[task.node].count = task.count;
      continue;
    }
    const P3 extent = chi - clo;
    const u32 axis =
        extent.x >= extent.y && extent.x >= extent.z ? 0u : (extent.y >= extent.z ? 1u : 2u);
    u32* begin = order_.data() + task.first;
    std::sort(begin, begin + task.count, [&](u32 a, u32 b) {
      const f64 ca = axis_of(centroid[a], axis);
      const f64 cb = axis_of(centroid[b], axis);
      if (ca != cb) return ca < cb;
      return a < b;
    });
    const u32 half = task.count / 2u;
    const u32 left = nodes_.size();
    nodes_.push_back(Node{});
    nodes_.push_back(Node{});
    nodes_[task.node].first = left;
    nodes_[task.node].count = 0;
    stack.push_back(Task{left + 1u, task.first + half, task.count - half});
    stack.push_back(Task{left, task.first, half});
  }
}

bool HostBvh::closest(P3 p, f64 max_distance, bool holding_only, HostHit& out) const {
  if (surface_ == nullptr || nodes_.empty()) return false;
  f64 best = max_distance * max_distance;
  bool found = false;
  u32 stack[64];
  u32 top = 0;
  stack[top++] = 0;
  while (top > 0) {
    const Node& node = nodes_[stack[--top]];
    if (!(box_distance2(p, node.lo, node.hi) < best)) continue;
    if (node.count > 0) {
      for (u32 i = node.first; i < node.first + node.count; ++i) {
        const u32 t = order_[i];
        if (holding_only && surface_->holds[t] == 0) continue;
        const P3 q = closest_on_triangle(p, surface_->corner(t, 0), surface_->corner(t, 1),
                                         surface_->corner(t, 2));
        const P3 d = p - q;
        const f64 d2 = dot(d, d);
        if (d2 < best || (found && d2 == best && t < out.triangle)) {
          best = d2;
          found = true;
          out.point = q;
          out.normal = surface_->face_normal[t];
          out.triangle = t;
        }
      }
      continue;
    }
    // The nearer child last, so it is visited first.
    const Node& a = nodes_[node.first];
    const Node& b = nodes_[node.first + 1u];
    const f64 da = box_distance2(p, a.lo, a.hi);
    const f64 db = box_distance2(p, b.lo, b.hi);
    if (top + 2 > 64) continue;  // depth past 64 does not happen for a median split of < 2^32
    if (da <= db) {
      stack[top++] = node.first + 1u;
      stack[top++] = node.first;
    } else {
      stack[top++] = node.first;
      stack[top++] = node.first + 1u;
    }
  }
  if (found) out.distance = std::sqrt(best);
  return found;
}

bool HostBvh::occluded(P3 origin, P3 direction, f64 length) const {
  if (surface_ == nullptr || nodes_.empty()) return false;
  u32 stack[64];
  u32 top = 0;
  stack[top++] = 0;
  while (top > 0) {
    const Node& node = nodes_[stack[--top]];
    if (!ray_meets_box(origin, direction, length, node.lo, node.hi)) continue;
    if (node.count > 0) {
      for (u32 i = node.first; i < node.first + node.count; ++i) {
        const u32 t = order_[i];
        if (ray_hits(origin, direction, length, surface_->corner(t, 0), surface_->corner(t, 1),
                     surface_->corner(t, 2)))
          return true;
      }
      continue;
    }
    if (top + 2 > 64) continue;
    stack[top++] = node.first + 1u;
    stack[top++] = node.first;
  }
  return false;
}

SiteFrame site_frame(const HostSite& site) {
  // The world's north is -z and east +x (sky.md); the host's instance is turned by `yaw_deg` about
  // +y, so a world direction w is R(-yaw) w in the host's frame.
  const f64 yaw = static_cast<f64>(site.yaw_deg) * (k_pi / 180.0);
  const f64 cy = det::cos(yaw);
  const f64 sy = det::sin(yaw);
  const auto to_host = [&](P3 w) { return P3{w.x * cy - w.z * sy, w.y, w.x * sy + w.z * cy}; };
  const f64 az = static_cast<f64>(site.light_azimuth_deg) * (k_pi / 180.0);
  const f64 el = static_cast<f64>(site.light_elevation_deg) * (k_pi / 180.0);
  const P3 light_world{det::sin(az) * det::cos(el), det::sin(el), -det::cos(az) * det::cos(el)};
  SiteFrame frame;
  frame.north = normalized(to_host(P3{0.0, 0.0, -1.0}));
  frame.light = normalized(to_host(light_world));
  return frame;
}

void host_exposure(const HostSurface& surface, const HostBvh& bvh, const HostSite& site,
                   f64 crevice_radius, u32 sky_rays, Vector<Exposure>& out) {
  const u32 triangles = surface.triangle_count();
  out.clear();
  out.resize(triangles);
  const SiteFrame frame = site_frame(site);
  // A fixed set of directions over the sphere (a Fibonacci lattice), the same for every triangle.
  Vector<P3> directions;
  const u32 rays = sky_rays < 4 ? 4u : sky_rays;
  for (u32 i = 0; i < rays; ++i) {
    const f64 y = 1.0 - (2.0 * (static_cast<f64>(i) + 0.5)) / static_cast<f64>(rays);
    const f64 r = std::sqrt(1.0 - y * y > 0.0 ? 1.0 - y * y : 0.0);
    const f64 phi = 2.39996322972865332 * static_cast<f64>(i);
    directions.push_back(P3{r * det::cos(phi), y, r * det::sin(phi)});
  }
  const f64 extent = length_of(surface.hi - surface.lo);
  const f64 sun_reach = extent * 2.0 + 1.0;
  const f64 eps = 1e-3 + 1e-5 * extent;
  const f64 ground = static_cast<f64>(site.ground_y_m);
  const f64 shade = static_cast<f64>(site.canopy_shade);
  for (u32 t = 0; t < triangles; ++t) {
    const P3 n = surface.face_normal[t];
    const P3 o = surface.centroid(t) + n * eps;
    Exposure& e = out[t];
    e.up = static_cast<f32>(n.y);
    e.north = static_cast<f32>(dot(n, frame.north));
    e.height = static_cast<f32>(o.y - ground > 0.0 ? o.y - ground : 0.0);
    e.water = site.water_distance_m;
    const f64 facing = dot(n, frame.light);
    f64 sun = 0.0;
    if (facing > 0.0 && !bvh.occluded(o, frame.light, sun_reach)) sun = facing;
    e.sun = static_cast<f32>(sun * (1.0 - shade));
    f64 open = 0.0;
    f64 total = 0.0;
    for (const P3& d : directions) {
      const f64 w = dot(n, d);
      if (w <= 0.0) continue;
      total += w;
      bool hidden = false;
      if (d.y < 0.0) {
        const f64 to_ground = (ground - o.y) / d.y;
        hidden = to_ground >= 0.0 && to_ground < crevice_radius;
      }
      if (!hidden) hidden = bvh.occluded(o, d, crevice_radius);
      if (!hidden) open += w;
    }
    e.sky = static_cast<f32>(total > 0.0 ? open / total : 1.0);
  }
}

}  // namespace engine::cover
