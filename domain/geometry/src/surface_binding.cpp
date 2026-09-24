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
constexpr f32 k_ratio_squared = k_normal_degenerate_ratio * k_normal_degenerate_ratio;
constexpr f32 k_sine_squared = k_normal_degenerate_sine * k_normal_degenerate_sine;
constexpr f32 k_interpolated_squared = k_normal_interpolated_length * k_normal_interpolated_length;

// Whether a vector is usable: finite, and its squared length above `floor` (0 for "not zero").
bool live_vector(Vec3 v, f32 floor) noexcept {
  const f32 squared = dot(v, v);
  return std::isfinite(squared) && squared > floor;
}

// The change of a unit vector as products with dc (`unit_normal_change`), taken as exactly zero —
// the reference direction kept — unless `live`. **Liveness is the caller's verdict on the state
// vector evaluated directly** (the reference plus its change, or the state's own positions), never
// on the expansion's |c + dc|: near a collapse that length is dominated by the expansion's own
// rounding (about 1e-7 of |c|), which would call an exactly collapsed triangle live. At the
// reference the direct state vector is the reference's bit for bit, so the verdict is the
// reference's validity and the change is the expansion's exact zero.
Vec3 unit_change(Vec3 c, Vec3 dc, bool live) noexcept {
  const f32 squared = dot(c, c);
  const f32 dq = dot(dc, c + c + dc);
  const f32 state_squared = squared + dq;
  const f32 length_reference = std::sqrt(squared);
  const f32 length_state = std::sqrt(std::max(state_squared, 0.0f));
  const bool usable = live && squared > 0.0f && length_state > 0.0f;
  const f32 inverse_state = usable ? 1.0f / length_state : 0.0f;
  const f32 lengths = length_reference * (length_reference + length_state);
  const f32 inverse_change = usable && lengths > 0.0f ? -(dq * inverse_state) / lengths : 0.0f;
  return usable ? dc * inverse_state + c * inverse_change : Vec3{};
}

// A unit vector, or zero when `v` is not live against `floor`.
Vec3 unit_or_zero(Vec3 v, f32 floor) noexcept {
  const f32 squared = dot(v, v);
  return std::isfinite(squared) && squared > floor ? v * (1.0f / std::sqrt(squared)) : Vec3{};
}

// The limit-interpolated rule's vertex stage for one refined vertex (surface_binding.h, "The
// limit-interpolated rule"): the unit reference normal, zero when invalid — a tangent that is zero
// or not finite, or unit tangents whose cross product's norm (a sine) is at most
// k_normal_degenerate_sine.
Vec3 limit_normal_of(Vec3 tu, Vec3 tv) noexcept {
  const Vec3 u = unit_or_zero(tu, 0.0f);
  const Vec3 v = unit_or_zero(tv, 0.0f);
  if (u == Vec3{} || v == Vec3{}) return Vec3{};
  return unit_or_zero(cross(u, v), k_sine_squared);
}

// ... and its change at a state: each unit tangent's change, the change of their cross product,
// and the normal's change, each a product with the tangents' displacement. The state is judged on
// its own tangents, tu + dtu and tv + dtv: one that has fallen to zero (at most
// k_normal_degenerate_ratio of its reference length) or unit tangents whose sine has fallen to
// k_normal_degenerate_sine make the vertex degenerate, and it keeps its reference normal.
struct NormalChange {
  Vec3 change;
  bool fell_back = false;  // the reference was valid and the state degenerate
};

NormalChange limit_normal_change(Vec3 tu, Vec3 tv, Vec3 dtu, Vec3 dtv) noexcept {
  const Vec3 u = unit_or_zero(tu, 0.0f);
  const Vec3 v = unit_or_zero(tv, 0.0f);
  if (u == Vec3{} || v == Vec3{}) return NormalChange{};
  const Vec3 a = cross(u, v);
  if (!live_vector(a, k_sine_squared)) return NormalChange{};  // invalid at the reference
  const Vec3 state_u = tu + dtu;
  const Vec3 state_v = tv + dtv;
  const bool tangents_live = live_vector(state_u, k_ratio_squared * dot(tu, tu)) &&
                             live_vector(state_v, k_ratio_squared * dot(tv, tv));
  const Vec3 state_a = cross(unit_or_zero(state_u, 0.0f), unit_or_zero(state_v, 0.0f));
  const bool live = tangents_live && live_vector(state_a, k_sine_squared);
  const Vec3 du = unit_change(tu, dtu, live);
  const Vec3 dv = unit_change(tv, dtv, live);
  const Vec3 da = cross(u, dv) + cross(du, v) + cross(du, dv);
  return NormalChange{unit_change(a, da, live), !live};
}

// The footpoint normal's change for barycentrics (w0, w1, w2) of the triangle at `corner`: the
// transfer's `turned`, and `footpoint_normal_change`'s answer, from one piece of code.
template <bool Interpolated>
Vec3 footpoint_turn(const Vec3* reference, const Vec3* displacement, const Vec3* normal_reference,
                    const Vec3* normal_change, const u32* corner, f32 w0, f32 w1, f32 w2,
                    bool* fell_back) noexcept {
  if constexpr (Interpolated) {
    // Unit vertex normals interpolated: the state's interpolation, m + dm, judged against the
    // agreed absolute threshold; a degenerate one keeps the reference footpoint normal.
    (void)reference;
    (void)displacement;
    const Vec3 m = normal_reference[corner[0]] * w0 + normal_reference[corner[1]] * w1 +
                   normal_reference[corner[2]] * w2;
    const Vec3 dm = normal_change[corner[0]] * w0 + normal_change[corner[1]] * w1 +
                    normal_change[corner[2]] * w2;
    const bool live = live_vector(m + dm, k_interpolated_squared);
    if (fell_back != nullptr) *fell_back = !live && live_vector(m, k_interpolated_squared);
    return unit_change(m, dm, live);
  } else {
    // The facet's cross product has units, so its threshold is relative to its reference, and
    // the state facet is judged from the state's own corners, which at the reference are the
    // reference's bit for bit (r + 0 is r).
    (void)normal_reference;
    (void)normal_change;
    const Vec3 r0 = reference[corner[0]];
    const Vec3 r1 = reference[corner[1]];
    const Vec3 r2 = reference[corner[2]];
    const Vec3 d0 = displacement[corner[0]];
    const Vec3 d1 = displacement[corner[1]];
    const Vec3 d2 = displacement[corner[2]];
    const Vec3 e1 = r1 - r0;
    const Vec3 e2 = r2 - r0;
    const Vec3 g1 = d1 - d0;
    const Vec3 g2 = d2 - d0;
    const Vec3 c = cross(e1, e2);
    const Vec3 dc = cross(e1, g2) + cross(g1, e2) + cross(g1, g2);
    const Vec3 s0 = r0 + d0;
    const Vec3 state = cross((r1 + d1) - s0, (r2 + d2) - s0);
    const bool live = live_vector(state, k_ratio_squared * dot(c, c));
    if (fell_back != nullptr) *fell_back = !live && live_vector(c, 0.0f);
    return unit_change(c, dc, live);
  }
}

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

// base + step, except that a zero step leaves the base's own bits: IEEE addition turns -0 + 0
// into +0, and a mirrored mesh's midline is full of -0.
f32 add_step(f32 base, f32 step) noexcept { return step == 0.0f ? base : base + step; }

// ---- double-precision helpers for the binders ---------------------------------------------------

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

// The footpoint normal in double, for the binders: the same definition as `footpoint_normal`,
// evaluated from the same f32 inputs, so an offset is measured along the direction the transfer
// will turn; zero when `footpoint_normal` would say it is invalid.
D3 footpoint_normal_d(NormalMode mode, const BindingSurface& surface, u32 triangle, f64 b0, f64 b1,
                      f64 b2) noexcept {
  const u32* corner = surface.faces.data() + 3 * triangle;
  if (mode == NormalMode::triangle) {
    const D3 a = d3(surface.reference[corner[0]]);
    const D3 b = d3(surface.reference[corner[1]]);
    const D3 c = d3(surface.reference[corner[2]]);
    return unit3(cross3(sub(b, a), sub(c, a)));
  }
  const Vec3 n0 = surface.normal_reference[corner[0]];
  const Vec3 n1 = surface.normal_reference[corner[1]];
  const Vec3 n2 = surface.normal_reference[corner[2]];
  if ((b0 > 0.0 && n0 == Vec3{}) || (b1 > 0.0 && n1 == Vec3{}) || (b2 > 0.0 && n2 == Vec3{}))
    return D3{};  // a corner the footpoint uses has no valid normal: none is invented
  const D3 m = add(add(scale(d3(n0), b0), scale(d3(n1), b1)), scale(d3(n2), b2));
  const f64 length = std::sqrt(dot3(m, m));
  if (!(length > static_cast<f64>(k_normal_interpolated_length))) return D3{};
  return scale(m, 1.0 / length);
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

// The region of a point of a triangle given by its barycentrics: a zero weight puts it on the edge
// opposite that corner, two zeros on the third corner.
Region region_of(f64 b0, f64 b1, f64 b2) noexcept {
  const bool z0 = b0 <= 0.0;
  const bool z1 = b1 <= 0.0;
  const bool z2 = b2 <= 0.0;
  if (z1 && z2) return Region::corner0;
  if (z0 && z2) return Region::corner1;
  if (z0 && z1) return Region::corner2;
  if (z2) return Region::edge0;  // corners 0 and 1
  if (z0) return Region::edge1;  // corners 1 and 2
  if (z1) return Region::edge2;  // corners 2 and 0
  return Region::face;
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

// The surface's boundary: its edges one triangle uses, and their vertices.
struct SurfaceBoundary {
  Vector<u64> edges;  // (min << 32) | max, sorted
  Vector<u8> vertex;  // one per surface vertex: 1 on the boundary

  bool is_edge(u32 a, u32 b) const noexcept {
    const u64 key = (u64{std::min(a, b)} << 32) | u64{std::max(a, b)};
    return std::binary_search(edges.begin(), edges.end(), key);
  }
  bool on_boundary(const u32* corner, Region region) const noexcept {
    switch (region) {
      case Region::face: return false;
      case Region::corner0: return vertex[corner[0]] != 0;
      case Region::corner1: return vertex[corner[1]] != 0;
      case Region::corner2: return vertex[corner[2]] != 0;
      case Region::edge0: return is_edge(corner[0], corner[1]);
      case Region::edge1: return is_edge(corner[1], corner[2]);
      case Region::edge2: return is_edge(corner[2], corner[0]);
    }
    return false;
  }
};

SurfaceBoundary surface_boundary(std::span<const u32> faces, u32 vertex_count) {
  SurfaceBoundary out;
  const u32 triangle_count = static_cast<u32>(faces.size() / 3);
  Vector<u64> keys;
  keys.reserve(triangle_count * 3);
  for (u32 t = 0; t < triangle_count; ++t) {
    for (u32 s = 0; s < 3; ++s) {
      const u32 a = faces[3 * t + s];
      const u32 b = faces[3 * t + (s + 1) % 3];
      keys.push_back((u64{std::min(a, b)} << 32) | u64{std::max(a, b)});
    }
  }
  std::sort(keys.begin(), keys.end());
  out.vertex.assign(vertex_count, u8{0});
  for (u32 i = 0; i < keys.size();) {
    u32 j = i + 1;
    while (j < keys.size() && keys[j] == keys[i])
      ++j;
    if (j - i == 1) {
      out.edges.push_back(keys[i]);
      out.vertex[static_cast<u32>(keys[i] >> 32)] = 1;
      out.vertex[static_cast<u32>(keys[i] & 0xffffffffu)] = 1;
    }
    i = j;
  }
  return out;
}

// What both binders check about the surface before they bind anything to it.
bool check_surface(NormalMode mode, const BindingSurface& surface, std::string* error) {
  if (static_cast<u32>(mode) >= k_normal_mode_count)
    return fail(error, "the normal mode is not one this build knows");
  if (surface.faces.size() % 3 != 0)
    return fail(error, "the surface faces are not a whole number of triangles");
  const u32 triangle_count = static_cast<u32>(surface.faces.size() / 3);
  if (triangle_count == 0) return fail(error, "the surface has no triangles");
  if (!surface_binding_can_represent(triangle_count))
    return fail(error, "the surface has " + std::to_string(triangle_count) +
                           " triangles and a binding's u16 names at most " +
                           std::to_string(k_surface_binding_max_triangles) +
                           "; bind at a lower level or split the region");
  for (const u32 index : surface.faces)
    if (index >= surface.reference.size())
      return fail(error, "a surface triangle names a vertex out of range");
  if (mode != NormalMode::triangle && surface.normal_reference.size() != surface.reference.size())
    return fail(error, std::string("the ") + normal_mode_name(mode) +
                           " normal needs one reference vertex normal per surface vertex");
  return true;
}

// One vertex's record from its footpoint (in double) and its offset; fills the report's measures.
// Returns false when the offset does not fit a half float.
struct Packed {
  SurfaceBinding record;
  f64 quantization = 0.0;
};

Packed pack(u32 triangle, f64 b0, f64 b1, f64 b2, f64 h, f32 weight, D3 q, D3 n, D3 a, D3 b, D3 c) {
  Packed out;
  SurfaceBinding& record = out.record;
  record.triangle = static_cast<u16>(triangle);
  record.barycentric[0] = to_unorm16(b0);
  record.barycentric[1] = to_unorm16(b1);
  if (u32{record.barycentric[0]} + record.barycentric[1] > 65535u)
    record.barycentric[1] = static_cast<u16>(65535u - record.barycentric[0]);
  record.normal_offset = f32_to_f16(static_cast<f32>(h));
  record.weight = to_unorm16(static_cast<f64>(weight));
  (void)b2;
  // What the record decodes to, against the exact footpoint and offset.
  const f64 q0 = static_cast<f64>(record.barycentric[0]) / 65535.0;
  const f64 q1 = static_cast<f64>(record.barycentric[1]) / 65535.0;
  const D3 decoded_q = add(add(scale(a, q0), scale(b, q1)), scale(c, 1.0 - q0 - q1));
  const f64 decoded_h = static_cast<f64>(f16_to_f32(record.normal_offset));
  const D3 difference = sub(add(decoded_q, scale(n, decoded_h)), add(q, scale(n, h)));
  out.quantization = std::sqrt(dot3(difference, difference));
  return out;
}

// ---- the transfer's vertex loop, one instantiation per mode family and terms ------------------

template <bool Interpolated, BindingTerms Terms>
void transfer(std::span<const SurfaceBinding> bindings, std::span<const Vec3> base_positions,
              const BindingSurface& surface, std::span<Vec3> out) noexcept {
  const u32* faces = surface.faces.data();
  const Vec3* reference = surface.reference.data();
  const Vec3* displacement = surface.displacement.data();
  const Vec3* normal_reference = surface.normal_reference.data();
  const Vec3* normal_change = surface.normal_change.data();
  for (usize i = 0; i < bindings.size(); ++i) {
    const SurfaceBinding b = bindings[i];
    const u32* corner = faces + 3 * u32{b.triangle};
    const f32 w0 = static_cast<f32>(b.barycentric[0]) / k_unorm16;
    const f32 w1 = static_cast<f32>(b.barycentric[1]) / k_unorm16;
    const f32 w2 = 1.0f - w0 - w1;
    // The surface's displacement at the footpoint.
    Vec3 moved{};
    if constexpr (Terms != BindingTerms::offset) {
      moved = displacement[corner[0]] * w0 + displacement[corner[1]] * w1 +
              displacement[corner[2]] * w2;
    }
    Vec3 delta = moved;
    if constexpr (Terms != BindingTerms::displacement) {
      // The footpoint normal's change, every term a product with a displacement.
      const Vec3 turned = footpoint_turn<Interpolated>(reference, displacement, normal_reference,
                                                       normal_change, corner, w0, w1, w2, nullptr);
      // A zero offset takes the normal term out entirely rather than multiplying it by zero.
      const f32 offset = half_to_f32_branchless(b.normal_offset);
      delta = offset == 0.0f ? moved : moved + turned * offset;
    }
    const Vec3 step = delta * (static_cast<f32>(b.weight) / k_unorm16);
    const Vec3 base = base_positions[i];
    const Vec3 blended{add_step(base.x, step.x), add_step(base.y, step.y),
                       add_step(base.z, step.z)};
    // A select, not a branch: weight 0 is the base's own bits, whatever `delta` holds.
    out[i] = b.weight == 0 ? base : blended;
  }
}

template <bool Interpolated>
void transfer_terms(BindingTerms terms, std::span<const SurfaceBinding> bindings,
                    std::span<const Vec3> base_positions, const BindingSurface& surface,
                    std::span<Vec3> out) noexcept {
  switch (terms) {
    case BindingTerms::full:
      transfer<Interpolated, BindingTerms::full>(bindings, base_positions, surface, out);
      return;
    case BindingTerms::displacement:
      transfer<Interpolated, BindingTerms::displacement>(bindings, base_positions, surface, out);
      return;
    case BindingTerms::offset:
      transfer<Interpolated, BindingTerms::offset>(bindings, base_positions, surface, out);
      return;
  }
}

f32 percentile(Vector<f32>& values, u32 numerator) {
  if (values.empty()) return 0.0f;
  const u32 index = static_cast<u32>((static_cast<u64>(values.size() - 1) * numerator) / 100u);
  std::nth_element(values.begin(), values.begin() + index, values.end());
  return values[index];
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

// ---- the footpoint normal ----------------------------------------------------------------------

const char* normal_mode_name(NormalMode mode) noexcept {
  switch (mode) {
    case NormalMode::triangle: return "triangle";
    case NormalMode::interpolated_vertex_area_weighted: return "interpolated-vertex-area-weighted";
    case NormalMode::limit_interpolated: return "limit-interpolated";
  }
  return "unknown";
}

bool parse_normal_mode(std::string_view name, NormalMode& out) noexcept {
  for (u32 m = 0; m < k_normal_mode_count; ++m) {
    const NormalMode mode = static_cast<NormalMode>(m);
    if (name == normal_mode_name(mode)) {
      out = mode;
      return true;
    }
  }
  return false;
}

Vec3 unit_normal(Vec3 c) noexcept {
  const f32 squared = dot(c, c);
  return squared > 0.0f && std::isfinite(squared) ? c * (1.0f / std::sqrt(squared)) : Vec3{};
}

// The change of a unit normal, written the long way round (surface_binding.h says why): with c the
// reference vector and dc its change,
//
//   dq   = dc . (2c + dc)                              (|c + dc|^2 - |c|^2)
//   dinv = -dq (1/|c + dc|) / (|c| (|c| + |c + dc|))   (1/|c + dc| - 1/|c|)
//   n(c + dc) - n(c) = dc / |c + dc| + c dinv
//
// At the reference dc is exactly zero, so each line is a product with an exact zero, and a fused
// multiply-add of anything with zero is exact: the term is zero on any compiler or GPU without
// relying on two evaluations rounding alike. No intermediate is more than quadratic in |c|, so
// nothing underflows before |c|^2 would. A state that is not live (`unit_change`) keeps the
// reference direction: the change is exactly zero.
Vec3 unit_normal_change(Vec3 c, Vec3 dc, f32 min_state_squared) noexcept {
  return unit_change(c, dc, live_vector(c, 0.0f) && live_vector(c + dc, min_state_squared));
}

void area_weighted_normal_vectors(std::span<const u32> faces, std::span<const Vec3> positions,
                                  std::span<Vec3> out) noexcept {
  ENGINE_ASSERT(out.size() == positions.size(), "one normal vector per surface vertex");
  for (Vec3& v : out)
    v = Vec3{};
  for (usize t = 0; t + 2 < faces.size(); t += 3) {
    const Vec3 p0 = positions[faces[t]];
    const Vec3 c = cross(positions[faces[t + 1]] - p0, positions[faces[t + 2]] - p0);
    out[faces[t]] += c;
    out[faces[t + 1]] += c;
    out[faces[t + 2]] += c;
  }
}

void area_weighted_normal_vector_change(std::span<const u32> faces, std::span<const Vec3> reference,
                                        std::span<const Vec3> displacement,
                                        std::span<Vec3> out) noexcept {
  ENGINE_ASSERT(out.size() == reference.size() && displacement.size() == reference.size(),
                "one change per surface vertex");
  for (Vec3& v : out)
    v = Vec3{};
  for (usize t = 0; t + 2 < faces.size(); t += 3) {
    const Vec3 r0 = reference[faces[t]];
    const Vec3 d0 = displacement[faces[t]];
    const Vec3 e1 = reference[faces[t + 1]] - r0;
    const Vec3 e2 = reference[faces[t + 2]] - r0;
    const Vec3 g1 = displacement[faces[t + 1]] - d0;
    const Vec3 g2 = displacement[faces[t + 2]] - d0;
    const Vec3 dc = cross(e1, g2) + cross(g1, e2) + cross(g1, g2);
    out[faces[t]] += dc;
    out[faces[t + 1]] += dc;
    out[faces[t + 2]] += dc;
  }
}

u32 area_weighted_reference_normals(std::span<const Vec3> vectors, std::span<Vec3> out,
                                    Vector<u32>* invalid) noexcept {
  ENGINE_ASSERT(out.size() == vectors.size(), "one normal per vector");
  u32 count = 0;
  for (usize i = 0; i < out.size(); ++i) {
    out[i] = unit_normal(vectors[i]);
    if (out[i] == Vec3{}) {
      ++count;
      if (invalid != nullptr) invalid->push_back(static_cast<u32>(i));
    }
  }
  return count;
}

void area_weighted_normal_changes(std::span<const Vec3> vectors,
                                  std::span<const Vec3> vector_change,
                                  std::span<const Vec3> state_vectors, std::span<Vec3> out,
                                  Vector<u32>* fallback) noexcept {
  ENGINE_ASSERT(out.size() == vectors.size() && vector_change.size() == vectors.size() &&
                    state_vectors.size() == vectors.size(),
                "one change and one state vector per vector");
  for (usize i = 0; i < out.size(); ++i) {
    const Vec3 a = vectors[i];
    const bool valid = live_vector(a, 0.0f);
    const bool live = valid && live_vector(state_vectors[i], k_ratio_squared * dot(a, a));
    out[i] = unit_change(a, vector_change[i], live);
    if (fallback != nullptr && valid && !live) fallback->push_back(static_cast<u32>(i));
  }
}

u32 limit_reference_normals(std::span<const Vec3> tangent_u, std::span<const Vec3> tangent_v,
                            std::span<Vec3> out, Vector<u32>* invalid) noexcept {
  ENGINE_ASSERT(tangent_u.size() == tangent_v.size() && out.size() == tangent_u.size(),
                "one normal per tangent pair");
  u32 count = 0;
  for (usize i = 0; i < out.size(); ++i) {
    out[i] = limit_normal_of(tangent_u[i], tangent_v[i]);
    if (out[i] == Vec3{}) {
      ++count;
      if (invalid != nullptr) invalid->push_back(static_cast<u32>(i));
    }
  }
  return count;
}

void limit_normal_changes(std::span<const Vec3> tangent_u, std::span<const Vec3> tangent_v,
                          std::span<const Vec3> tangent_u_change,
                          std::span<const Vec3> tangent_v_change, std::span<Vec3> out,
                          Vector<u32>* fallback) noexcept {
  ENGINE_ASSERT(tangent_u.size() == tangent_v.size() &&
                    tangent_u_change.size() == tangent_u.size() &&
                    tangent_v_change.size() == tangent_u.size() && out.size() == tangent_u.size(),
                "one change per tangent pair");
  for (usize i = 0; i < out.size(); ++i) {
    const NormalChange c =
        limit_normal_change(tangent_u[i], tangent_v[i], tangent_u_change[i], tangent_v_change[i]);
    out[i] = c.change;
    if (fallback != nullptr && c.fell_back) fallback->push_back(static_cast<u32>(i));
  }
}

bool evaluate_binding_frame(NormalMode mode, const LoopLimitSurface& surface,
                            std::span<const Vec3> reference_nodes,
                            std::span<const Vec3> state_nodes, BindingFrame& out,
                            std::string* error) {
  if (static_cast<u32>(mode) >= k_normal_mode_count)
    return fail(error, "the normal mode is not one this build knows");
  const u32 columns = surface.limit.column_count;
  if (reference_nodes.size() != columns || state_nodes.size() != columns)
    return fail(error, "the node states are not one position per control vertex of the surface");
  const u32 rows = surface.limit.rows();
  const bool tangents =
      surface.tangent_u.rows() == rows && surface.tangent_v.rows() == rows && rows > 0;
  if (mode == NormalMode::limit_interpolated && !tangents)
    return fail(
        error,
        "the limit-interpolated normal needs the surface's tangent operators; build it with "
        "LoopSurfaceOptions::tangents");
  out.reference.resize(rows);
  out.displacement.resize(rows);
  apply(surface.limit, reference_nodes, std::span<Vec3>(out.reference.data(), rows));
  Vector<Vec3> node_displacement(columns);
  const std::span<Vec3> nd(node_displacement.data(), columns);
  surface_displacement(surface.limit, reference_nodes, state_nodes, nd,
                       std::span<Vec3>(out.displacement.data(), rows));
  out.normal_reference.clear();
  out.normal_change.clear();
  out.invalid_normals.clear();
  out.fallback_normals.clear();
  if (mode == NormalMode::triangle) return true;

  out.normal_reference.resize(rows);
  out.normal_change.resize(rows);
  const std::span<Vec3> normals(out.normal_reference.data(), rows);
  const std::span<Vec3> changes(out.normal_change.data(), rows);
  if (mode == NormalMode::interpolated_vertex_area_weighted) {
    Vector<Vec3> vectors(rows);
    Vector<Vec3> change(rows);
    Vector<Vec3> state_positions(rows);
    Vector<Vec3> state_vectors(rows);
    area_weighted_normal_vectors(surface.faces, out.reference,
                                 std::span<Vec3>(vectors.data(), rows));
    area_weighted_normal_vector_change(surface.faces, out.reference, out.displacement,
                                       std::span<Vec3>(change.data(), rows));
    for (u32 i = 0; i < rows; ++i)
      state_positions[i] = out.reference[i] + out.displacement[i];
    area_weighted_normal_vectors(surface.faces, state_positions,
                                 std::span<Vec3>(state_vectors.data(), rows));
    area_weighted_reference_normals(vectors, normals, &out.invalid_normals);
    area_weighted_normal_changes(vectors, change, state_vectors, changes, &out.fallback_normals);
  } else {
    Vector<Vec3> tu(rows);
    Vector<Vec3> tv(rows);
    Vector<Vec3> dtu(rows);
    Vector<Vec3> dtv(rows);
    apply(surface.tangent_u, reference_nodes, std::span<Vec3>(tu.data(), rows));
    apply(surface.tangent_v, reference_nodes, std::span<Vec3>(tv.data(), rows));
    apply(surface.tangent_u, nd, std::span<Vec3>(dtu.data(), rows));
    apply(surface.tangent_v, nd, std::span<Vec3>(dtv.data(), rows));
    limit_reference_normals(tu, tv, normals, &out.invalid_normals);
    limit_normal_changes(tu, tv, dtu, dtv, changes, &out.fallback_normals);
  }
  return true;
}

Vec3 footpoint_normal_change(NormalMode mode, const BindingSurface& surface, u32 triangle,
                             Vec3 barycentrics, bool* fell_back) noexcept {
  const u32* corner = surface.faces.data() + 3 * triangle;
  if (mode == NormalMode::triangle)
    return footpoint_turn<false>(surface.reference.data(), surface.displacement.data(), nullptr,
                                 nullptr, corner, barycentrics.x, barycentrics.y, barycentrics.z,
                                 fell_back);
  return footpoint_turn<true>(nullptr, nullptr, surface.normal_reference.data(),
                              surface.normal_change.data(), corner, barycentrics.x, barycentrics.y,
                              barycentrics.z, fell_back);
}

Vec3 footpoint_normal(NormalMode mode, const BindingSurface& surface, u32 triangle,
                      Vec3 barycentrics) noexcept {
  const u32* corner = surface.faces.data() + 3 * triangle;
  if (mode == NormalMode::triangle) {
    const Vec3 r0 = surface.reference[corner[0]];
    return unit_normal(cross(surface.reference[corner[1]] - r0, surface.reference[corner[2]] - r0));
  }
  const Vec3 n0 = surface.normal_reference[corner[0]];
  const Vec3 n1 = surface.normal_reference[corner[1]];
  const Vec3 n2 = surface.normal_reference[corner[2]];
  if ((barycentrics.x > 0.0f && n0 == Vec3{}) || (barycentrics.y > 0.0f && n1 == Vec3{}) ||
      (barycentrics.z > 0.0f && n2 == Vec3{}))
    return Vec3{};
  const Vec3 m = n0 * barycentrics.x + n1 * barycentrics.y + n2 * barycentrics.z;
  const f32 squared = dot(m, m);
  if (!(squared > k_interpolated_squared)) return Vec3{};
  return m * (1.0f / std::sqrt(squared));
}

// ---- binding ------------------------------------------------------------------------------------

bool bind_to_surface(NormalMode mode, std::span<const Vec3> render_vertices,
                     const BindingSurface& surface, const SurfaceBindOptions& options,
                     Vector<SurfaceBinding>& out, SurfaceBindReport* report, std::string* error) {
  out.clear();
  if (report != nullptr) *report = SurfaceBindReport{};
  if (!check_surface(mode, surface, error)) return false;
  if (!options.weights.empty() && options.weights.size() != render_vertices.size())
    return fail(error, "the weights are not one per render vertex");
  for (const f32 w : options.weights)
    if (!(w >= 0.0f && w <= 1.0f)) return fail(error, "a weight is outside [0, 1]");

  const SurfaceBoundary boundary =
      surface_boundary(surface.faces, static_cast<u32>(surface.reference.size()));
  Bvh bvh;
  build_bvh(surface.reference, surface.faces, bvh);
  Vector<u32> stack;

  out.resize(static_cast<u32>(render_vertices.size()));
  for (u32 i = 0; i < render_vertices.size(); ++i) {
    const f32 weight = options.weights.empty() ? 1.0f : options.weights[i];
    out[i] = SurfaceBinding{};
    if (weight == 0.0f) continue;  // never moved, so never searched for
    const D3 p = d3(render_vertices[i]);
    const Hit hit = query_nearest(bvh, surface.reference, surface.faces, p, stack);
    const u32 t = hit.triangle;
    const u32* corner = surface.faces.data() + 3 * t;
    const D3 a = d3(surface.reference[corner[0]]);
    const D3 b = d3(surface.reference[corner[1]]);
    const D3 c = d3(surface.reference[corner[2]]);
    const Nearest& f = hit.nearest;
    const D3 q = add(add(scale(a, f.b0), scale(b, f.b1)), scale(c, f.b2));
    const D3 n = footpoint_normal_d(mode, surface, t, f.b0, f.b1, f.b2);
    const f64 h = dot3(sub(p, q), n);
    if (!(std::fabs(h) <= 65504.0))
      return fail(error, "render vertex " + std::to_string(i) +
                             " is too far from the surface for a half-float offset");
    const Packed packed = pack(t, f.b0, f.b1, f.b2, h, weight, q, n, a, b, c);
    out[i] = packed.record;

    if (report == nullptr) continue;
    if (boundary.on_boundary(corner, f.region)) report->boundary_footpoints.push_back(i);
    if (dot3(n, n) == 0.0) report->invalid_normals.push_back(i);
    const f32 magnitude = static_cast<f32>(std::fabs(h));
    if (magnitude > options.offset_limit) report->offset_exceeded.push_back(i);
    report->max_abs_offset = std::max(report->max_abs_offset, magnitude);
    report->max_quantization_error =
        std::max(report->max_quantization_error, static_cast<f32>(packed.quantization));
    const D3 tangential = sub(sub(p, q), scale(n, h));
    report->max_tangential_residual = std::max(
        report->max_tangential_residual, static_cast<f32>(std::sqrt(dot3(tangential, tangential))));
  }
  return true;
}

bool bind_from_records(NormalMode mode, std::span<const Vec3> render_vertices,
                       std::span<const AuthoredFootpoint> footpoints, const BindingSurface& surface,
                       const SurfaceBindOptions& options, Vector<SurfaceBinding>& out,
                       SurfaceBindReport* report, std::string* error) {
  out.clear();
  if (report != nullptr) *report = SurfaceBindReport{};
  if (!check_surface(mode, surface, error)) return false;
  if (footpoints.size() != render_vertices.size())
    return fail(error, "the footpoints are not one per render vertex");
  const u32 triangle_count = static_cast<u32>(surface.faces.size() / 3);
  const SurfaceBoundary boundary =
      surface_boundary(surface.faces, static_cast<u32>(surface.reference.size()));

  out.resize(static_cast<u32>(render_vertices.size()));
  for (u32 i = 0; i < render_vertices.size(); ++i) {
    const AuthoredFootpoint& fp = footpoints[i];
    const std::string who = "render vertex " + std::to_string(i);
    if (fp.triangle >= triangle_count)
      return fail(error, who + " names refined triangle " + std::to_string(fp.triangle) +
                             ", past the surface's " + std::to_string(triangle_count));
    if (!(fp.weight >= 0.0f && fp.weight <= 1.0f))
      return fail(error, who + " has a weight outside [0, 1]");
    const f64 r0 = static_cast<f64>(fp.barycentric.x);
    const f64 r1 = static_cast<f64>(fp.barycentric.y);
    const f64 r2 = static_cast<f64>(fp.barycentric.z);
    const f64 sum = r0 + r1 + r2;
    if (!std::isfinite(sum) || r0 < -1.0e-6 || r1 < -1.0e-6 || r2 < -1.0e-6 ||
        std::fabs(sum - 1.0) > 1.0e-5)
      return fail(error, who + "'s barycentrics are not a point of its triangle");
    const f64 b0 = std::max(r0, 0.0) / sum;
    const f64 b1 = std::max(r1, 0.0) / sum;
    const f64 b2 = std::max(r2, 0.0) / sum;
    const u32* corner = surface.faces.data() + 3 * fp.triangle;
    const D3 a = d3(surface.reference[corner[0]]);
    const D3 b = d3(surface.reference[corner[1]]);
    const D3 c = d3(surface.reference[corner[2]]);
    const D3 q = add(add(scale(a, b0), scale(b, b1)), scale(c, b2));
    const D3 n = footpoint_normal_d(mode, surface, fp.triangle, b0, b1, b2);
    const D3 p = d3(render_vertices[i]);
    const f64 measured = dot3(sub(p, q), n);
    const bool authored = std::isfinite(fp.offset);
    const f64 h = authored ? static_cast<f64>(fp.offset) : measured;
    if (!(std::fabs(h) <= 65504.0))
      return fail(error, who + " is too far from the surface for a half-float offset");
    const Packed packed = pack(fp.triangle, b0, b1, b2, h, fp.weight, q, n, a, b, c);
    out[i] = packed.record;

    if (report == nullptr || fp.weight == 0.0f) continue;
    if (boundary.on_boundary(corner, region_of(b0, b1, b2)))
      report->boundary_footpoints.push_back(i);
    if (dot3(n, n) == 0.0) report->invalid_normals.push_back(i);
    const f32 magnitude = static_cast<f32>(std::fabs(h));
    if (magnitude > options.offset_limit) report->offset_exceeded.push_back(i);
    report->max_abs_offset = std::max(report->max_abs_offset, magnitude);
    report->max_quantization_error =
        std::max(report->max_quantization_error, static_cast<f32>(packed.quantization));
    const D3 tangential = sub(sub(p, q), scale(n, measured));
    report->max_tangential_residual = std::max(
        report->max_tangential_residual, static_cast<f32>(std::sqrt(dot3(tangential, tangential))));
    if (authored)
      report->max_offset_disagreement =
          std::max(report->max_offset_disagreement, static_cast<f32>(std::fabs(h - measured)));
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

void check_binding_images(std::span<const SurfaceBinding> bindings,
                          std::span<const Vec3> render_positions,
                          std::span<const u32> render_triangles,
                          std::span<const Vec3> surface_positions,
                          std::span<const u32> surface_faces, bool fully_weighted,
                          BindingImageReport& out) {
  out = BindingImageReport{};
  ENGINE_ASSERT(bindings.size() == render_positions.size(), "one binding per render vertex");
  Vector<f32> deviations;
  const u32 triangle_count = static_cast<u32>(render_triangles.size() / 3);
  const auto footpoint = [&](const SurfaceBinding& b) {
    const u32* corner = surface_faces.data() + 3 * u32{b.triangle};
    const f64 q0 = static_cast<f64>(b.barycentric[0]) / 65535.0;
    const f64 q1 = static_cast<f64>(b.barycentric[1]) / 65535.0;
    return add(add(scale(d3(surface_positions[corner[0]]), q0),
                   scale(d3(surface_positions[corner[1]]), q1)),
               scale(d3(surface_positions[corner[2]]), 1.0 - q0 - q1));
  };
  for (u32 t = 0; t < triangle_count; ++t) {
    const u32 i0 = render_triangles[3 * t];
    const u32 i1 = render_triangles[3 * t + 1];
    const u32 i2 = render_triangles[3 * t + 2];
    const u16 w0 = bindings[i0].weight;
    const u16 w1 = bindings[i1].weight;
    const u16 w2 = bindings[i2].weight;
    const bool checked = fully_weighted ? (w0 == 65535 && w1 == 65535 && w2 == 65535)
                                        : (w0 != 0 && w1 != 0 && w2 != 0);
    if (!checked) continue;
    ++out.triangles;
    const D3 p0 = d3(render_positions[i0]);
    const D3 render = cross3(sub(d3(render_positions[i1]), p0), sub(d3(render_positions[i2]), p0));
    const D3 q0 = footpoint(bindings[i0]);
    const D3 image = cross3(sub(footpoint(bindings[i1]), q0), sub(footpoint(bindings[i2]), q0));
    const f64 lengths = std::sqrt(dot3(render, render) * dot3(image, image));
    const f64 cosine = lengths > 0.0 ? dot3(render, image) / lengths : -1.0;
    if (!(cosine > 0.0)) out.reversed.push_back(t);
    const f32 angle = static_cast<f32>(std::acos(std::clamp(cosine, -1.0, 1.0)) *
                                       (180.0 / 3.14159265358979323846));
    if (angle > out.deviation_max_deg || deviations.empty()) {
      out.deviation_max_deg = angle;
      out.deviation_max_triangle = t;
    }
    deviations.push_back(angle);
  }
  out.deviation_p50_deg = percentile(deviations, 50);
  out.deviation_p95_deg = percentile(deviations, 95);
}

void surface_displacement(const CsrMatrix& limit, std::span<const Vec3> reference_nodes,
                          std::span<const Vec3> state_nodes, std::span<Vec3> node_displacement,
                          std::span<Vec3> out) noexcept {
  ENGINE_ASSERT(reference_nodes.size() == state_nodes.size() &&
                    node_displacement.size() == state_nodes.size(),
                "one reference node, one state node and one scratch entry per node");
  // x - x is +0 for every finite x, and every product in the operator then has a zero factor, so
  // an unmoved cage gives a displacement of exactly zero whatever `apply` fuses.
  for (usize c = 0; c < state_nodes.size(); ++c)
    node_displacement[c] = state_nodes[c] - reference_nodes[c];
  apply(limit, node_displacement, out);
}

void apply_binding(NormalMode mode, std::span<const SurfaceBinding> bindings,
                   std::span<const Vec3> base_positions, const BindingSurface& surface,
                   std::span<Vec3> out, BindingTerms terms) noexcept {
  ENGINE_ASSERT(bindings.size() == base_positions.size() && out.size() == bindings.size(),
                "one binding per base vertex and per output");
  ENGINE_ASSERT(surface.reference.size() == surface.displacement.size(),
                "one displacement per point of the reference surface");
  if (mode == NormalMode::triangle) {
    transfer_terms<false>(terms, bindings, base_positions, surface, out);
    return;
  }
  ENGINE_ASSERT(surface.normal_reference.size() == surface.reference.size() &&
                    surface.normal_change.size() == surface.reference.size(),
                "an interpolated normal needs one reference normal and one change per vertex");
  transfer_terms<true>(terms, bindings, base_positions, surface, out);
}

}  // namespace engine::geometry
