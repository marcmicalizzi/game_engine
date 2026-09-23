#include <core/base/assert.h>
#include <domain/geometry/cluster.h>
#include <domain/geometry/surface_binding.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace engine::geometry {

namespace {

constexpr u32 k_none = ~0u;
constexpr f32 k_unorm16 = 65535.0f;

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

// A half float to f32 without a branch (Giesen's "magic multiply"): shift the exponent and
// mantissa into place and let one multiply by 2^112 rebias the exponent, which also turns a half
// denormal into the right normal float. Exact for every finite half; an infinity or NaN is made
// to survive by a select. Agrees with `f16_to_f32` on every finite input (the tests walk all of
// them) and is what the per-frame transfer reads the offset with.
f32 half_to_f32_branchless(u16 half) noexcept {
  const u32 magnitude = (u32{half} & 0x7fffu) << 13;
  f32 value = 0.0f;
  std::memcpy(&value, &magnitude, 4);
  value *= 0x1.0p112f;
  u32 bits = 0;
  std::memcpy(&bits, &value, 4);
  bits |= value >= 0x1.0p16f ? 0x7f800000u : 0u;  // was infinity or NaN
  bits |= (u32{half} & 0x8000u) << 16;
  std::memcpy(&value, &bits, 4);
  return value;
}

Vec3 unit_or_zero(Vec3 v) noexcept {
  const f32 length = std::sqrt(dot(v, v));
  const f32 inverse = length > 0.0f ? 1.0f / length : 0.0f;
  return v * inverse;
}

// ---- nearest point on a triangle mesh ----------------------------------------------------------

struct D3 {
  f64 x = 0.0;
  f64 y = 0.0;
  f64 z = 0.0;
};

D3 d3(Vec3 v) noexcept {
  return D3{static_cast<f64>(v.x), static_cast<f64>(v.y), static_cast<f64>(v.z)};
}
D3 sub(D3 a, D3 b) noexcept { return D3{a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 add(D3 a, D3 b) noexcept { return D3{a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 scale(D3 a, f64 s) noexcept { return D3{a.x * s, a.y * s, a.z * s}; }
f64 dot3(D3 a, D3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross3(D3 a, D3 b) noexcept {
  return D3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
D3 unit3(D3 a) noexcept {
  const f64 length = std::sqrt(dot3(a, a));
  return length > 0.0 ? scale(a, 1.0 / length) : D3{};
}

// Where on a triangle the nearest point lies: its interior, a corner, or an edge (corners s and
// s + 1 for `edge0 + s`).
enum class Region : u8 { face, corner0, corner1, corner2, edge0, edge1, edge2 };

struct Nearest {
  f64 b0 = 1.0;  // barycentric weights of corners 0, 1, 2
  f64 b1 = 0.0;
  f64 b2 = 0.0;
  Region region = Region::corner0;
};

// Ericson, Real-Time Collision Detection §5.1.5, in double: the Voronoi regions of the corners,
// then of the edges, then the interior.
Nearest nearest_on_triangle(D3 p, D3 a, D3 b, D3 c) noexcept {
  const D3 ab = sub(b, a);
  const D3 ac = sub(c, a);
  const D3 ap = sub(p, a);
  const f64 d1 = dot3(ab, ap);
  const f64 d2 = dot3(ac, ap);
  if (d1 <= 0.0 && d2 <= 0.0) return Nearest{1.0, 0.0, 0.0, Region::corner0};
  const D3 bp = sub(p, b);
  const f64 d3v = dot3(ab, bp);
  const f64 d4 = dot3(ac, bp);
  if (d3v >= 0.0 && d4 <= d3v) return Nearest{0.0, 1.0, 0.0, Region::corner1};
  const f64 vc = d1 * d4 - d3v * d2;
  if (vc <= 0.0 && d1 >= 0.0 && d3v <= 0.0) {
    const f64 v = d1 / (d1 - d3v);
    return Nearest{1.0 - v, v, 0.0, Region::edge0};
  }
  const D3 cp = sub(p, c);
  const f64 d5 = dot3(ab, cp);
  const f64 d6 = dot3(ac, cp);
  if (d6 >= 0.0 && d5 <= d6) return Nearest{0.0, 0.0, 1.0, Region::corner2};
  const f64 vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
    const f64 w = d2 / (d2 - d6);
    return Nearest{1.0 - w, 0.0, w, Region::edge2};
  }
  const f64 va = d3v * d6 - d5 * d4;
  if (va <= 0.0 && (d4 - d3v) >= 0.0 && (d5 - d6) >= 0.0) {
    const f64 w = (d4 - d3v) / ((d4 - d3v) + (d5 - d6));
    return Nearest{0.0, 1.0 - w, w, Region::edge1};
  }
  const f64 sum = va + vb + vc;
  if (!(sum > 0.0)) return Nearest{1.0, 0.0, 0.0, Region::corner0};  // a degenerate triangle
  const f64 v = vb / sum;
  const f64 w = vc / sum;
  return Nearest{1.0 - v - w, v, w, Region::face};
}

// A bounding-volume hierarchy over the surface's triangles, median split on the longest axis of
// the centroids, at most four triangles a leaf. Only the build uses it: a nearest-point query per
// render vertex, which brute force would make vertices x triangles.
struct BvhNode {
  f32 lo[3] = {0, 0, 0};
  f32 hi[3] = {0, 0, 0};
  u32 first = 0;  // a leaf's first entry of `order`, or an inner node's left child (right is +1)
  u32 count = 0;  // triangles in a leaf; 0 for an inner node
};

struct Bvh {
  Vector<BvhNode> nodes;
  Vector<u32> order;
};

constexpr u32 k_leaf_triangles = 4;

void build_bvh(std::span<const Vec3> positions, std::span<const u32> faces, Bvh& bvh) {
  const u32 triangle_count = static_cast<u32>(faces.size() / 3);
  Vector<Vec3> centroid(triangle_count);
  Vector<Vec3> lo(triangle_count);
  Vector<Vec3> hi(triangle_count);
  for (u32 t = 0; t < triangle_count; ++t) {
    const Vec3 a = positions[faces[3 * t]];
    const Vec3 b = positions[faces[3 * t + 1]];
    const Vec3 c = positions[faces[3 * t + 2]];
    centroid[t] = (a + b + c) * (1.0f / 3.0f);
    lo[t] = Vec3{std::min({a.x, b.x, c.x}), std::min({a.y, b.y, c.y}), std::min({a.z, b.z, c.z})};
    hi[t] = Vec3{std::max({a.x, b.x, c.x}), std::max({a.y, b.y, c.y}), std::max({a.z, b.z, c.z})};
  }
  bvh.order.resize(triangle_count);
  for (u32 t = 0; t < triangle_count; ++t)
    bvh.order[t] = t;
  bvh.nodes.clear();
  if (triangle_count == 0) return;
  bvh.nodes.push_back(BvhNode{});

  struct Task {
    u32 node;
    u32 begin;
    u32 end;
  };
  Vector<Task> tasks;
  tasks.push_back(Task{0, 0, triangle_count});
  while (!tasks.empty()) {
    const Task task = tasks.back();
    tasks.pop_back();
    Vec3 box_lo = lo[bvh.order[task.begin]];
    Vec3 box_hi = hi[bvh.order[task.begin]];
    Vec3 c_lo = centroid[bvh.order[task.begin]];
    Vec3 c_hi = c_lo;
    for (u32 i = task.begin; i < task.end; ++i) {
      const u32 t = bvh.order[i];
      box_lo = Vec3{std::min(box_lo.x, lo[t].x), std::min(box_lo.y, lo[t].y),
                    std::min(box_lo.z, lo[t].z)};
      box_hi = Vec3{std::max(box_hi.x, hi[t].x), std::max(box_hi.y, hi[t].y),
                    std::max(box_hi.z, hi[t].z)};
      c_lo = Vec3{std::min(c_lo.x, centroid[t].x), std::min(c_lo.y, centroid[t].y),
                  std::min(c_lo.z, centroid[t].z)};
      c_hi = Vec3{std::max(c_hi.x, centroid[t].x), std::max(c_hi.y, centroid[t].y),
                  std::max(c_hi.z, centroid[t].z)};
    }
    BvhNode& node = bvh.nodes[task.node];
    node.lo[0] = box_lo.x;
    node.lo[1] = box_lo.y;
    node.lo[2] = box_lo.z;
    node.hi[0] = box_hi.x;
    node.hi[1] = box_hi.y;
    node.hi[2] = box_hi.z;
    const u32 count = task.end - task.begin;
    if (count <= k_leaf_triangles) {
      node.first = task.begin;
      node.count = count;
      continue;
    }
    const Vec3 extent = c_hi - c_lo;
    const u32 axis =
        extent.x >= extent.y && extent.x >= extent.z ? 0u : (extent.y >= extent.z ? 1u : 2u);
    const u32 middle = task.begin + count / 2;
    // A total order (centroid, then index) so the split is a function of the input.
    std::nth_element(bvh.order.begin() + task.begin, bvh.order.begin() + middle,
                     bvh.order.begin() + task.end, [&](u32 x, u32 y) {
                       const f32 cx = centroid[x][axis];
                       const f32 cy = centroid[y][axis];
                       return cx != cy ? cx < cy : x < y;
                     });
    const u32 left = bvh.nodes.size();
    bvh.nodes[task.node].first = left;
    bvh.nodes[task.node].count = 0;
    bvh.nodes.push_back(BvhNode{});
    bvh.nodes.push_back(BvhNode{});
    tasks.push_back(Task{left, task.begin, middle});
    tasks.push_back(Task{left + 1, middle, task.end});
  }
}

f64 box_distance_squared(const BvhNode& node, D3 p) noexcept {
  const f64 dx =
      std::max({static_cast<f64>(node.lo[0]) - p.x, 0.0, p.x - static_cast<f64>(node.hi[0])});
  const f64 dy =
      std::max({static_cast<f64>(node.lo[1]) - p.y, 0.0, p.y - static_cast<f64>(node.hi[1])});
  const f64 dz =
      std::max({static_cast<f64>(node.lo[2]) - p.z, 0.0, p.z - static_cast<f64>(node.hi[2])});
  return dx * dx + dy * dy + dz * dz;
}

struct Hit {
  u32 triangle = k_none;
  f64 distance_squared = 0.0;
  Nearest nearest;
};

// The nearest triangle, ties to the lower index. A subtree is skipped only when its box is
// *strictly* farther than the best so far, so an equally near triangle of lower index is still
// found and the answer does not depend on the tree's shape.
Hit query_nearest(const Bvh& bvh, std::span<const Vec3> positions, std::span<const u32> faces, D3 p,
                  Vector<u32>& stack) {
  Hit best;
  best.distance_squared = std::numeric_limits<f64>::infinity();
  stack.clear();
  if (bvh.nodes.empty()) return best;
  stack.push_back(0);
  while (!stack.empty()) {
    const u32 index = stack.back();
    stack.pop_back();
    const BvhNode& node = bvh.nodes[index];
    if (box_distance_squared(node, p) > best.distance_squared) continue;
    if (node.count > 0) {
      for (u32 i = node.first; i < node.first + node.count; ++i) {
        const u32 t = bvh.order[i];
        const D3 a = d3(positions[faces[3 * t]]);
        const D3 b = d3(positions[faces[3 * t + 1]]);
        const D3 c = d3(positions[faces[3 * t + 2]]);
        const Nearest closest = nearest_on_triangle(p, a, b, c);
        const D3 q = add(add(scale(a, closest.b0), scale(b, closest.b1)), scale(c, closest.b2));
        const D3 d = sub(p, q);
        const f64 d2 = dot3(d, d);
        if (d2 < best.distance_squared || (d2 == best.distance_squared && t < best.triangle)) {
          best.triangle = t;
          best.distance_squared = d2;
          best.nearest = closest;
        }
      }
      continue;
    }
    // Nearer child last, so it is popped first and tightens the bound for the other.
    const f64 left = box_distance_squared(bvh.nodes[node.first], p);
    const f64 right = box_distance_squared(bvh.nodes[node.first + 1], p);
    if (left <= right) {
      stack.push_back(node.first + 1);
      stack.push_back(node.first);
    } else {
      stack.push_back(node.first);
      stack.push_back(node.first + 1);
    }
  }
  return best;
}

u16 to_unorm16(f64 value) noexcept {
  const f64 scaled = std::clamp(value, 0.0, 1.0) * 65535.0;
  return static_cast<u16>(std::lround(scaled));
}

}  // namespace

f32 binding_weight(const SurfaceBinding& binding) noexcept {
  return static_cast<f32>(binding.weight) / k_unorm16;
}

Vec3 binding_barycentrics(const SurfaceBinding& binding) noexcept {
  const f32 b0 = static_cast<f32>(binding.barycentric[0]) / k_unorm16;
  const f32 b1 = static_cast<f32>(binding.barycentric[1]) / k_unorm16;
  return Vec3{b0, b1, 1.0f - b0 - b1};
}

f32 binding_normal_offset(const SurfaceBinding& binding) noexcept {
  return half_to_f32_branchless(binding.normal_offset);
}

bool bind_to_surface(std::span<const Vec3> render_vertices, std::span<const Vec3> surface_positions,
                     std::span<const u32> surface_faces, const SurfaceBindOptions& options,
                     Vector<SurfaceBinding>& out, SurfaceBindReport* report, std::string* error) {
  out.clear();
  if (report != nullptr) *report = SurfaceBindReport{};
  if (surface_faces.size() % 3 != 0)
    return fail(error, "the surface faces are not a whole number of triangles");
  const u32 triangle_count = static_cast<u32>(surface_faces.size() / 3);
  if (triangle_count == 0) return fail(error, "the surface has no triangles");
  if (!surface_binding_can_represent(triangle_count))
    return fail(error, "the surface has " + std::to_string(triangle_count) +
                           " triangles and a binding's u16 names at most " +
                           std::to_string(k_surface_binding_max_triangles) +
                           "; bind at a lower level or split the region");
  for (const u32 index : surface_faces)
    if (index >= surface_positions.size())
      return fail(error, "a surface triangle names a vertex out of range");
  if (!options.weights.empty() && options.weights.size() != render_vertices.size())
    return fail(error, "the weights are not one per render vertex");
  for (const f32 w : options.weights)
    if (!(w >= 0.0f && w <= 1.0f)) return fail(error, "a weight is outside [0, 1]");

  // Boundary edges and vertices of the surface: an edge one triangle uses.
  Vector<u64> edge_keys;
  edge_keys.reserve(triangle_count * 3);
  for (u32 t = 0; t < triangle_count; ++t) {
    for (u32 s = 0; s < 3; ++s) {
      const u32 a = surface_faces[3 * t + s];
      const u32 b = surface_faces[3 * t + (s + 1) % 3];
      edge_keys.push_back((u64{std::min(a, b)} << 32) | u64{std::max(a, b)});
    }
  }
  std::sort(edge_keys.begin(), edge_keys.end());
  Vector<u64> boundary_edges;
  Vector<u8> boundary_vertex(static_cast<u32>(surface_positions.size()), u8{0});
  for (u32 i = 0; i < edge_keys.size();) {
    u32 j = i + 1;
    while (j < edge_keys.size() && edge_keys[j] == edge_keys[i])
      ++j;
    if (j - i == 1) {
      boundary_edges.push_back(edge_keys[i]);
      boundary_vertex[static_cast<u32>(edge_keys[i] >> 32)] = 1;
      boundary_vertex[static_cast<u32>(edge_keys[i] & 0xffffffffu)] = 1;
    }
    i = j;
  }
  const auto is_boundary_edge = [&](u32 a, u32 b) {
    const u64 key = (u64{std::min(a, b)} << 32) | u64{std::max(a, b)};
    return std::binary_search(boundary_edges.begin(), boundary_edges.end(), key);
  };

  Bvh bvh;
  build_bvh(surface_positions, surface_faces, bvh);
  Vector<u32> stack;

  out.resize(static_cast<u32>(render_vertices.size()));
  for (u32 i = 0; i < render_vertices.size(); ++i) {
    const f32 weight = options.weights.empty() ? 1.0f : options.weights[i];
    SurfaceBinding& record = out[i];
    record = SurfaceBinding{};
    if (weight == 0.0f) continue;  // never moved, so never searched for
    const D3 p = d3(render_vertices[i]);
    const Hit hit = query_nearest(bvh, surface_positions, surface_faces, p, stack);
    const u32 t = hit.triangle;
    const u32* corner = surface_faces.data() + 3 * t;
    const D3 a = d3(surface_positions[corner[0]]);
    const D3 b = d3(surface_positions[corner[1]]);
    const D3 c = d3(surface_positions[corner[2]]);
    const D3 q =
        add(add(scale(a, hit.nearest.b0), scale(b, hit.nearest.b1)), scale(c, hit.nearest.b2));
    const D3 n = unit3(cross3(sub(b, a), sub(c, a)));
    const f64 h = dot3(sub(p, q), n);
    if (!(std::fabs(h) <= 65504.0))
      return fail(error, "render vertex " + std::to_string(i) +
                             " is too far from the surface for a half-float offset");

    record.triangle = static_cast<u16>(t);
    record.barycentric[0] = to_unorm16(hit.nearest.b0);
    record.barycentric[1] = to_unorm16(hit.nearest.b1);
    if (u32{record.barycentric[0]} + record.barycentric[1] > 65535u)
      record.barycentric[1] = static_cast<u16>(65535u - record.barycentric[0]);
    record.normal_offset = f32_to_f16(static_cast<f32>(h));
    record.weight = to_unorm16(static_cast<f64>(weight));

    if (report == nullptr) continue;
    bool on_boundary = false;
    switch (hit.nearest.region) {
      case Region::face: break;
      case Region::corner0: on_boundary = boundary_vertex[corner[0]] != 0; break;
      case Region::corner1: on_boundary = boundary_vertex[corner[1]] != 0; break;
      case Region::corner2: on_boundary = boundary_vertex[corner[2]] != 0; break;
      case Region::edge0: on_boundary = is_boundary_edge(corner[0], corner[1]); break;
      case Region::edge1: on_boundary = is_boundary_edge(corner[1], corner[2]); break;
      case Region::edge2: on_boundary = is_boundary_edge(corner[2], corner[0]); break;
    }
    if (on_boundary) report->boundary_footpoints.push_back(i);
    const f32 magnitude = static_cast<f32>(std::fabs(h));
    if (magnitude > options.offset_limit) report->offset_exceeded.push_back(i);
    report->max_abs_offset = std::max(report->max_abs_offset, magnitude);
    // What the record decodes to, against the exact footpoint and offset.
    const f64 q0 = static_cast<f64>(record.barycentric[0]) / 65535.0;
    const f64 q1 = static_cast<f64>(record.barycentric[1]) / 65535.0;
    const D3 decoded_q = add(add(scale(a, q0), scale(b, q1)), scale(c, 1.0 - q0 - q1));
    const f64 decoded_h = static_cast<f64>(f16_to_f32(record.normal_offset));
    const D3 difference = sub(add(decoded_q, scale(n, decoded_h)), add(q, scale(n, h)));
    report->max_quantization_error = std::max(
        report->max_quantization_error, static_cast<f32>(std::sqrt(dot3(difference, difference))));
  }
  return true;
}

bool validate_surface_bindings(std::span<const SurfaceBinding> bindings, u32 surface_triangles,
                               std::string* error) {
  for (u32 i = 0; i < bindings.size(); ++i) {
    const SurfaceBinding& b = bindings[i];
    if (b.triangle >= surface_triangles)
      return fail(error, "binding " + std::to_string(i) + " names a triangle out of range");
    if (u32{b.barycentric[0]} + b.barycentric[1] > 65535u)
      return fail(error, "binding " + std::to_string(i) + " has barycentrics summing above one");
    if ((b.normal_offset & 0x7c00u) == 0x7c00u)
      return fail(error, "binding " + std::to_string(i) + " has a non-finite normal offset");
  }
  return true;
}

void apply_binding(std::span<const SurfaceBinding> bindings, std::span<const Vec3> base_positions,
                   std::span<const u32> surface_faces, std::span<const Vec3> surface_reference,
                   std::span<const Vec3> surface_state, std::span<Vec3> out) noexcept {
  ENGINE_ASSERT(bindings.size() == base_positions.size() && out.size() == bindings.size(),
                "one binding per base vertex and per output");
  ENGINE_ASSERT(surface_reference.size() == surface_state.size(),
                "the reference and the state are the same surface");
  const u32* faces = surface_faces.data();
  const Vec3* reference = surface_reference.data();
  const Vec3* state = surface_state.data();
  for (usize i = 0; i < bindings.size(); ++i) {
    const SurfaceBinding b = bindings[i];
    const u32* corner = faces + 3 * u32{b.triangle};
    const Vec3 r0 = reference[corner[0]];
    const Vec3 r1 = reference[corner[1]];
    const Vec3 r2 = reference[corner[2]];
    const Vec3 s0 = state[corner[0]];
    const Vec3 s1 = state[corner[1]];
    const Vec3 s2 = state[corner[2]];
    const f32 w0 = static_cast<f32>(b.barycentric[0]) / k_unorm16;
    const f32 w1 = static_cast<f32>(b.barycentric[1]) / k_unorm16;
    const f32 w2 = 1.0f - w0 - w1;
    // The surface's change at the footpoint, and the normal's change there times the offset.
    const Vec3 moved = (s0 - r0) * w0 + (s1 - r1) * w1 + (s2 - r2) * w2;
    const Vec3 turned =
        unit_or_zero(cross(s1 - s0, s2 - s0)) - unit_or_zero(cross(r1 - r0, r2 - r0));
    const Vec3 delta = moved + turned * half_to_f32_branchless(b.normal_offset);
    const Vec3 base = base_positions[i];
    const Vec3 blended = base + delta * (static_cast<f32>(b.weight) / k_unorm16);
    // A select, not a branch: weight 0 is the base's own bits, whatever `delta` holds.
    out[i] = b.weight == 0 ? base : blended;
  }
}

}  // namespace engine::geometry
