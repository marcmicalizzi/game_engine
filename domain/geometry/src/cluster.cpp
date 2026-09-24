#include <core/containers/hash_map.h>
#include <core/containers/hash_set.h>
#include <core/hash/hash.h>
#include <domain/geometry/cluster.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <meshoptimizer.h>

namespace engine::geometry {

namespace {

u32 pack_cone_s8(const meshopt_Bounds& bounds) noexcept {
  auto byte = [](signed char v) { return u32{static_cast<u8>(v)}; };
  return byte(bounds.cone_axis_s8[0]) | (byte(bounds.cone_axis_s8[1]) << 8) |
         (byte(bounds.cone_axis_s8[2]) << 16) | (byte(bounds.cone_cutoff_s8) << 24);
}

// ---- normal cones, fit to the triangles as they are drawn ---------------------------------------
//
// meshoptimizer chooses a cluster's cone axis well, and its cutoff and apex are tight around the
// normals **its own float arithmetic** computed from the **float** positions. Neither is what the
// cull pass has to be right about. The rasterizers draw the 16-bit grid, not the floats, and on a
// dense mesh the grid turns a small triangle's normal by degrees; and a triangle's normal computed
// by different arithmetic — a GPU, another compiler, the same compiler at another baseline — is a
// different number. The failure that made this visible: with FMA contraction (GCC and clang at
// x86-64-v3), `a*b - c*d` with `a*b == c*d` is not zero but the rounding error of the product, so a
// triangle with two coincident corners (a UV sphere's pole) gets a unit "normal" pointing wherever
// the residue says. meshoptimizer put one inside its cone by luck; the validator's copy of the same
// residue pointed elsewhere, and a mesh MSVC and every v2 build accept failed on every v3 Linux
// build (docs/ci/local-linux.md, "What the first v3 runs found").
//
// So the builder refits the cone itself, in double precision, over **both** representations of
// every triangle — the float one the ray tracers build and the grid one the rasterizers read — with
// `k_cone_margin` of slack on each, which is what makes the cone robust to arithmetic nobody here
// controls instead of agreeing with one copy of it. A triangle thinner than a grid step is left
// out of both, because its facing is rounding. meshoptimizer still decides whether a cluster gets
// a cone at all and proposes an axis.

struct F64x3 {
  f64 x = 0.0;
  f64 y = 0.0;
  f64 z = 0.0;
};

F64x3 widen(Vec3 v) noexcept {
  return F64x3{static_cast<f64>(v.x), static_cast<f64>(v.y), static_cast<f64>(v.z)};
}
F64x3 sub_d(F64x3 a, F64x3 b) noexcept { return F64x3{a.x - b.x, a.y - b.y, a.z - b.z}; }
F64x3 scale_d(F64x3 a, f64 s) noexcept { return F64x3{a.x * s, a.y * s, a.z * s}; }
f64 dot_d(F64x3 a, F64x3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
f64 length_d(F64x3 a) noexcept { return std::sqrt(dot_d(a, a)); }
F64x3 cross_d(F64x3 a, F64x3 b) noexcept {
  return F64x3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// The unit normal of a triangle given as three double-precision corners, or false when it has
// none the renderer can promise: when it is **thinner than one grid step** (its height over its
// longest edge, `min_height`). The grid is the precision positions reach the rasterizers at, so a
// corner within a step of the opposite edge is on either side of it by rounding, and the
// triangle's facing is whatever quantization made it — to the rasterizers, which draw the grid,
// and to everything that computes a float normal, whose rounding residue on a unit mesh is a
// hundred times thinner still. Such a triangle is left out of the cone rather than allowed to
// widen it; it covers a pixel only when a grid step does, which is a zoom at which the whole mesh
// shows its grid anyway. This is the test the old validator lacked: it skipped only
// |n|^2 <= 1e-24, an absolute threshold that an FMA residue of 1e-11 on a unit sphere passes.
// Corners that coincide are caught before any arithmetic, because that is the case contraction
// turns into a residue: e1 == e2 makes every component `x*y - x*y`.
bool triangle_normal(F64x3 a, F64x3 b, F64x3 c, f64 min_height, F64x3& n) noexcept {
  auto same = [](F64x3 p, F64x3 q) { return p.x == q.x && p.y == q.y && p.z == q.z; };
  if (same(a, b) || same(b, c) || same(a, c)) return false;
  const F64x3 e1 = sub_d(b, a);
  const F64x3 e2 = sub_d(c, a);
  const f64 longest = std::max(std::max(length_d(e1), length_d(e2)), length_d(sub_d(c, b)));
  const F64x3 m = cross_d(e1, e2);
  const f64 l = length_d(m);
  if (!(l > min_height * longest)) return false;
  n = scale_d(m, 1.0 / l);
  return true;
}

// The grid point of a cluster-ordered vertex, as the shaders' load_position computes it, in double.
F64x3 grid_point(const ClusterMesh& mesh, u32 vertex) noexcept {
  const u16* q = &mesh.quantized[vertex * 3];
  const f64 s = static_cast<f64>(mesh.quant_scale);
  return F64x3{static_cast<f64>(mesh.quant_origin.x) + static_cast<f64>(q[0]) * s,
               static_cast<f64>(mesh.quant_origin.y) + static_cast<f64>(q[1]) * s,
               static_cast<f64>(mesh.quant_origin.z) + static_cast<f64>(q[2]) * s};
}

// Calls `visit(normal, point_on_plane, triangle, grid)` for every triangle of `cluster` that has a
// normal, once as the float triangle and once as the grid triangle. Needs the grid.
template <typename Visit>
void visit_cluster_planes(const ClusterMesh& mesh, const ClusterDesc& cluster, Visit&& visit) {
  const f64 step = static_cast<f64>(mesh.quant_scale);
  for (u32 t = 0; t < cluster.triangle_count; ++t) {
    const u32 packed = mesh.triangles[cluster.triangle_offset + t];
    u32 v[3];
    for (u32 k = 0; k < 3; ++k)
      v[k] = cluster.vertex_offset + ClusterMesh::unpack(packed, k);
    F64x3 n;
    const F64x3 f0 = widen(mesh.vertices[v[0]]);
    if (triangle_normal(f0, widen(mesh.vertices[v[1]]), widen(mesh.vertices[v[2]]), step, n))
      visit(n, f0, t, false);
    const F64x3 g0 = grid_point(mesh, v[0]);
    if (triangle_normal(g0, grid_point(mesh, v[1]), grid_point(mesh, v[2]), step, n))
      visit(n, g0, t, true);
  }
}

// The largest distance from `center` to any corner of the cluster, float or grid.
f64 cluster_reach(const ClusterMesh& mesh, const ClusterDesc& cluster, F64x3 center) noexcept {
  f64 reach = 0.0;
  for (u32 v = cluster.vertex_offset; v < cluster.vertex_offset + cluster.vertex_count; ++v) {
    reach = std::max(reach, length_d(sub_d(widen(mesh.vertices[v]), center)));
    reach = std::max(reach, length_d(sub_d(grid_point(mesh, v), center)));
  }
  return reach;
}

f64 cosine_between(F64x3 unit_a, F64x3 unit_b) noexcept {
  const f64 c = dot_d(unit_a, unit_b);
  return c < -1.0 ? -1.0 : (c > 1.0 ? 1.0 : c);
}

f64 angle_between(F64x3 unit_a, F64x3 unit_b) noexcept {
  return std::acos(cosine_between(unit_a, unit_b));
}

// meshoptimizer's own limit: a cone wider than this (cos <= 0.1, about 168 degrees across) culls
// too little to be worth a test, and its apex construction divides by the cosine.
constexpr f64 k_cone_min_cos = 0.1;

struct ConePlane {
  F64x3 normal;
  F64x3 point;
};

// Refits `cluster`'s cone: its axis, cutoff and apex. Called by quantize_positions for every
// cluster that has one, because the grid is half of what it is fit to; `planes` is scratch.
void fit_cone(const ClusterMesh& mesh, ClusterDesc& cluster, Vector<ConePlane>& planes) {
  if (decode_cone(cluster.cone).cutoff >= 1.0f) return;  // two-sided, or no useful cone
  planes.clear();
  F64x3 sum;
  visit_cluster_planes(mesh, cluster, [&](F64x3 n, F64x3 p, u32, bool) {
    planes.push_back(ConePlane{n, p});
    sum = F64x3{sum.x + n.x, sum.y + n.y, sum.z + n.z};
  });
  if (planes.empty()) {
    // Every triangle is thinner than a grid step, so the cluster draws nothing with a facing.
    // meshoptimizer's answer for the all-degenerate case is a zero axis and a zero cutoff, which
    // the CPU test reads as always backfacing and the GPU as never; never culled cannot be wrong.
    cluster.cone = k_cone_none;
    return;
  }
  // The axis: meshoptimizer's, or the mean of the normals that count, whichever needs the narrower
  // cone. meshoptimizer centres its axis on every normal its own arithmetic produced, a sliver's
  // rounding residue included, and one residue can pull a flat cluster's axis tens of degrees off
  // its only real normal: meshoptimizer's own cone for the tilted plane in cluster_tests.cpp is
  // 99/127 wide, where 3/127 holds every normal the plane really has.
  u32 candidates[2] = {cluster.cone & 0x00ffffffu, 0u};
  u32 candidate_count = 1;
  const f64 sum_length = length_d(sum);
  if (sum_length > 0.0) {
    const F64x3 mean = scale_d(sum, 1.0 / sum_length);
    candidates[candidate_count++] =
        encode_cone(
            Vec3{static_cast<f32>(mean.x), static_cast<f32>(mean.y), static_cast<f32>(mean.z)},
            1.0f) &
        0x00ffffffu;
  }
  // Every step from here to the packed cone is IEEE arithmetic and sqrt, both correctly rounded on
  // every platform, so a cone is the same bytes whatever compiled it (with contraction off, which
  // cmake/EngineFpContraction.cmake sees to for the whole tree). That is why the widest normal is
  // tracked as the smallest **cosine** rather than as an angle through std::acos, and why the
  // margin is added by the angle-sum identities over constants rather than by
  // std::cos(widest + margin): the same cone to rounding, but rounding the C library does not get
  // a say in.
  u32 axis_bytes = 0;
  F64x3 axis;
  f64 axis_length = 0.0;
  f64 nearest = -2.0;  // the widest normal's cosine to the best axis; below -1: no candidate yet
  for (u32 i = 0; i < candidate_count; ++i) {
    // k_cone_none's axis bytes are zero, so OR-ing them in reads the candidate's axis alone.
    const F64x3 q = widen(decode_cone(candidates[i] | k_cone_none).axis);
    const f64 l = length_d(q);
    if (!(l > 0.0)) continue;
    const F64x3 u = scale_d(q, 1.0 / l);
    f64 c = 1.0;
    for (const ConePlane& plane : planes)
      c = std::min(c, cosine_between(u, plane.normal));
    if (c > nearest) {
      nearest = c;
      axis = u;
      axis_length = l;
      axis_bytes = candidates[i];
    }
  }
  if (nearest < -1.0) {
    cluster.cone = k_cone_none;
    return;
  }
  const f64 margin = static_cast<f64>(k_cone_margin);
  // widest = acos(nearest) + margin, as its cosine and sine. sin(acos(c)) is sqrt((1 - c)(1 + c)),
  // which keeps its precision where 1 - c*c would cancel: near c = 1, the flat clusters.
  const f64 sin_nearest = std::sqrt((1.0 - nearest) * (1.0 + nearest));
  const f64 cos_widest = nearest * k_cone_margin_cos - sin_nearest * k_cone_margin_sin;
  const f64 sin_widest = sin_nearest * k_cone_margin_cos + nearest * k_cone_margin_sin;
  if (!(cos_widest > k_cone_min_cos)) {
    cluster.cone = k_cone_none;
    return;
  }
  // The cull pass normalizes the axis after the world transform and cluster_backfacing does not,
  // so the cutoff is scaled by the axis' own length when that is above 1: either reading then
  // implies the camera sits at least `widest` past every normal.
  const f64 cutoff = sin_widest * std::max(1.0, axis_length);
  const f64 cutoff_steps = std::ceil(cutoff * 127.0);
  if (!(cutoff_steps < 127.0)) {
    cluster.cone = k_cone_none;
    return;
  }

  // The apex: a point on the axis behind every plane by `margin` times its distance, so a normal
  // turned by up to `margin` still has it behind. With A = C - axis * t and |A - p| <= reach + t,
  //   dot(C - p, n) - t dot(axis, n) + margin (reach + t) + rounding (|C| + t) <= 0
  // for every plane, and dot(axis, n) >= cos(widest - margin) > 0.1 keeps the divisor positive.
  // `rounding` covers the floats on either side of the test: the apex stored as three f32, and a
  // grid corner the shader dequantizes in f32 rather than in double — half an ulp a component of
  // something no longer than |C| + t and |C| + reach respectively.
  const F64x3 center = widen(cluster.center);
  const f64 reach = cluster_reach(mesh, cluster, center);
  const f64 rounding = 1.0 / 4194304.0;  // 2^-22: twice what three rounded components can move it
  const f64 center_length = length_d(center);
  f64 t = 0.0;
  for (const ConePlane& plane : planes) {
    const f64 numerator = dot_d(sub_d(center, plane.point), plane.normal) + margin * reach +
                          rounding * (2.0 * center_length + reach);
    const f64 denominator = dot_d(axis, plane.normal) - margin - rounding;
    t = std::max(t, numerator / denominator);
  }
  const F64x3 apex = sub_d(center, scale_d(axis, t));
  cluster.cone_apex =
      Vec3{static_cast<f32>(apex.x), static_cast<f32>(apex.y), static_cast<f32>(apex.z)};
  cluster.cone =
      axis_bytes | (u32{static_cast<u8>(static_cast<i8>(static_cast<i32>(cutoff_steps)))} << 24);
}

// validate_clusters' half of the above: every triangle that has a normal, as floats and on the
// grid, keeps at least **half** of `k_cone_margin` inside the packed cone and in front of the
// apex. Half rather than all of it so the check says "the margin is there" without depending on
// the builder's arithmetic to the last bit — which is the property this whole section is about.
bool check_cone(const ClusterMesh& mesh, const ClusterDesc& cluster, u32 index, NormalCone cone,
                std::string* error) {
  const F64x3 axis_q = widen(cone.axis);
  const f64 axis_length = length_d(axis_q);
  const F64x3 axis = scale_d(axis_q, 1.0 / axis_length);
  // The widest a normal may sit from the axis for "the camera is inside the cone" to imply "the
  // camera is behind the triangle", under either reading of the packed axis (see fit_cone).
  const f64 limit =
      std::asin(std::min(1.0, static_cast<f64>(cone.cutoff) / std::max(1.0, axis_length)));
  const f64 half = 0.5 * static_cast<f64>(k_cone_margin);
  const F64x3 apex = widen(cluster.cone_apex);
  const char* what = nullptr;
  u32 triangle = 0;
  bool on_grid = false;
  f64 found = 0.0;
  f64 allowed = 0.0;
  visit_cluster_planes(mesh, cluster, [&](F64x3 n, F64x3 p, u32 t, bool grid) {
    if (what != nullptr) return;
    const f64 angle = angle_between(axis, n);
    const F64x3 to_apex = sub_d(apex, p);
    if (angle + half > limit) {
      what = "triangle normal outside its cluster's cone";
      found = angle;
      allowed = limit - half;
    } else if (dot_d(to_apex, n) > -half * length_d(to_apex)) {
      what = "cone apex not behind a triangle's plane";
      found = dot_d(to_apex, n);
      allowed = -half * length_d(to_apex);
    } else {
      return;
    }
    triangle = t;
    on_grid = grid;
  });
  if (what == nullptr) return true;
  if (error != nullptr) {
    char detail[160];
    std::snprintf(detail, sizeof(detail), " (cluster %u, triangle %u %s: %.9g where at most %.9g)",
                  index, triangle, on_grid ? "on the grid" : "as floats", found, allowed);
    *error = std::string(what) + detail;
  }
  return false;
}

}  // namespace

u32 encode_cone(Vec3 axis, f32 cutoff) noexcept {
  auto snorm8 = [](f32 v, bool round_up) {
    const f32 c = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    const f32 scaled = c * 127.0f;
    i32 i = round_up ? static_cast<i32>(std::ceil(scaled)) : static_cast<i32>(std::lround(scaled));
    i = i < -127 ? -127 : (i > 127 ? 127 : i);
    return u32{static_cast<u8>(static_cast<i8>(i))};
  };
  return snorm8(axis.x, false) | (snorm8(axis.y, false) << 8) | (snorm8(axis.z, false) << 16) |
         (snorm8(cutoff, true) << 24);
}

NormalCone decode_cone(u32 packed) noexcept {
  auto component = [packed](u32 byte) {
    return static_cast<f32>(static_cast<i8>((packed >> (8 * byte)) & 0xffu)) / 127.0f;
  };
  NormalCone cone;
  cone.axis = Vec3{component(0), component(1), component(2)};
  cone.cutoff = component(3);
  return cone;
}

SkinBinding make_skin_binding(const u32 joints[4], const f32 weights[4]) noexcept {
  SkinBinding out;
  f32 total = 0.0f;
  for (u32 i = 0; i < 4; ++i)
    total += weights[i] > 0.0f ? weights[i] : 0.0f;
  if (!(total > 0.0f)) {
    // No influence at all: bind rigidly to the first joint rather than leaving the vertex at
    // the origin of the palette, which is what a missing WEIGHTS_0 accessor means in practice.
    out.joints[0] = static_cast<u8>(joints[0] < k_max_skin_joints ? joints[0] : 0);
    return out;
  }
  const f32 inverse_total = 1.0f / total;
  u32 sum = 0;
  u32 largest = 0;
  f32 largest_weight = -1.0f;
  for (u32 i = 0; i < 4; ++i) {
    const f32 w = weights[i] > 0.0f ? weights[i] * inverse_total : 0.0f;
    const u32 q = static_cast<u32>(w * 255.0f + 0.5f);
    out.joints[i] = static_cast<u8>(joints[i] < k_max_skin_joints ? joints[i] : 0);
    out.weights[i] = static_cast<u8>(q > 255 ? 255 : q);
    sum += out.weights[i];
    if (w > largest_weight) {
      largest_weight = w;
      largest = i;
    }
  }
  // The rounding error lands on the biggest influence, which is where it is least visible and
  // where there is always room for it: its quantized weight is at least 64 whenever four
  // influences share the vertex, so ±3 never underflows or saturates.
  const i32 correction = 255 - static_cast<i32>(sum);
  const i32 corrected = static_cast<i32>(out.weights[largest]) + correction;
  out.weights[largest] = static_cast<u8>(corrected < 0 ? 0 : (corrected > 255 ? 255 : corrected));
  return out;
}

bool cluster_backfacing(const ClusterDesc& cluster, Vec3 camera) noexcept {
  const NormalCone cone = decode_cone(cluster.cone);
  if (cone.cutoff >= 1.0f) return false;
  const Vec3 to_apex = cluster.cone_apex - camera;
  const f32 distance = length(to_apex);
  if (distance <= 1e-12f) return false;
  return dot(to_apex * (1.0f / distance), cone.axis) >= cone.cutoff;
}

bool build_clusters(std::span<const Vec3> positions, std::span<const u32> indices,
                    const ClusterBuildOptions& options, ClusterMesh& out, std::string* error,
                    const AttributeSource& attributes) {
  out = ClusterMesh{};
  if (positions.empty() || indices.empty() || indices.size() % 3 != 0) {
    if (error != nullptr) *error = "build_clusters: need vertices and a multiple of three indices";
    return false;
  }
  if (options.max_vertices == 0 || options.max_vertices > 255 || options.max_triangles == 0 ||
      options.max_triangles > 512 || options.max_triangles % 4 != 0) {
    if (error != nullptr) {
      *error =
          "build_clusters: max_vertices must be 1..255 and max_triangles a multiple of 4 up to 512";
    }
    return false;
  }
  for (const u32 index : indices) {
    if (index >= positions.size()) {
      if (error != nullptr) *error = "build_clusters: index out of range";
      return false;
    }
  }

  const usize bound =
      meshopt_buildMeshletsBound(indices.size(), options.max_vertices, options.max_triangles);
  Vector<meshopt_Meshlet> meshlets(static_cast<u32>(bound));
  Vector<unsigned int> meshlet_vertices(static_cast<u32>(bound * options.max_vertices));
  Vector<unsigned char> meshlet_triangles(static_cast<u32>(bound * options.max_triangles * 3));
  const usize count = meshopt_buildMeshlets(
      meshlets.data(), meshlet_vertices.data(), meshlet_triangles.data(), indices.data(),
      indices.size(), &positions[0].x, positions.size(), sizeof(Vec3), options.max_vertices,
      options.max_triangles, options.cone_weight);

  out.source_vertex_count = static_cast<u32>(positions.size());
  out.source_triangle_count = static_cast<u32>(indices.size() / 3);
  out.clusters.reserve(static_cast<u32>(count));
  for (usize m = 0; m < count; ++m) {
    meshopt_Meshlet& meshlet = meshlets[static_cast<u32>(m)];
    meshopt_optimizeMeshlet(&meshlet_vertices[meshlet.vertex_offset],
                            &meshlet_triangles[meshlet.triangle_offset], meshlet.triangle_count,
                            meshlet.vertex_count);
    const meshopt_Bounds bounds = meshopt_computeMeshletBounds(
        &meshlet_vertices[meshlet.vertex_offset], &meshlet_triangles[meshlet.triangle_offset],
        meshlet.triangle_count, &positions[0].x, positions.size(), sizeof(Vec3));

    ClusterDesc desc;
    desc.vertex_offset = out.vertices.size();
    desc.triangle_offset = out.triangles.size();
    desc.vertex_count = meshlet.vertex_count;
    desc.triangle_count = meshlet.triangle_count;
    desc.center = Vec3{bounds.center[0], bounds.center[1], bounds.center[2]};
    desc.radius = bounds.radius;
    desc.cone_apex = Vec3{bounds.cone_apex[0], bounds.cone_apex[1], bounds.cone_apex[2]};
    desc.cone = options.normal_cones ? pack_cone_s8(bounds) : k_cone_none;
    for (u32 v = 0; v < meshlet.vertex_count; ++v) {
      const u32 source = meshlet_vertices[meshlet.vertex_offset + v];
      out.vertices.push_back(positions[source]);
      out.vertex_source.push_back(source);
    }
    for (u32 t = 0; t < meshlet.triangle_count; ++t) {
      const unsigned char* tri = &meshlet_triangles[meshlet.triangle_offset + t * 3];
      out.triangles.push_back(ClusterMesh::pack(tri[0], tri[1], tri[2]));
    }
    out.clusters.push_back(desc);
  }
  fill_cluster_attributes(out, positions, indices, attributes);
  quantize_positions(out);
  return true;
}

void quantize_positions(ClusterMesh& mesh) {
  mesh.quantized.clear();
  mesh.quant_origin = Vec3{};
  mesh.quant_scale = 1.0f;
  const u32 count = mesh.vertices.size();
  if (count == 0) return;
  Vec3 lo = mesh.vertices[0];
  Vec3 hi = lo;
  for (u32 i = 1; i < count; ++i) {
    const Vec3 p = mesh.vertices[i];
    lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  const f32 extent = std::max(std::max(hi.x - lo.x, hi.y - lo.y), hi.z - lo.z);
  mesh.quant_origin = lo;
  mesh.quant_scale = extent > 0.0f ? extent / 65535.0f : 1.0f;
  const f32 inverse_scale = 1.0f / mesh.quant_scale;
  mesh.quantized.reserve(count * 3 + 1);
  for (u32 i = 0; i < count; ++i) {
    const Vec3 p = mesh.vertices[i];
    const f32 axis[3] = {p.x - lo.x, p.y - lo.y, p.z - lo.z};
    for (u32 k = 0; k < 3; ++k) {
      const f32 grid = std::round(axis[k] * inverse_scale);
      const f32 clamped = grid < 0.0f ? 0.0f : (grid > 65535.0f ? 65535.0f : grid);
      mesh.quantized.push_back(static_cast<u16>(static_cast<u32>(clamped)));
    }
  }
  // One pad entry so a shader may read every triple as two whole 32-bit words.
  if ((mesh.quantized.size() & 1u) != 0) mesh.quantized.push_back(0);
  // The cones are fit to the grid as well as to the floats, so a new grid means new cones.
  Vector<ConePlane> planes;
  for (ClusterDesc& cluster : mesh.clusters)
    fit_cone(mesh, cluster, planes);
}

Vec3 dequantize_position(const ClusterMesh& mesh, u32 vertex) noexcept {
  const u64 first = u64{vertex} * 3;
  if (first + 3 > mesh.quantized.size()) return mesh.quant_origin;
  const u32 i = static_cast<u32>(first);
  return Vec3{mesh.quant_origin.x + static_cast<f32>(mesh.quantized[i]) * mesh.quant_scale,
              mesh.quant_origin.y + static_cast<f32>(mesh.quantized[i + 1]) * mesh.quant_scale,
              mesh.quant_origin.z + static_cast<f32>(mesh.quantized[i + 2]) * mesh.quant_scale};
}

bool validate_clusters(const ClusterMesh& mesh, std::span<const u32> source_indices,
                       const ClusterBuildOptions& options, std::string* error) {
  auto fail = [&](const char* what) {
    if (error != nullptr) *error = what;
    return false;
  };
  if (source_indices.size() % 3 != 0) return fail("source index count is not a multiple of three");
  if (mesh.vertices.size() != mesh.vertex_source.size())
    return fail("vertex_source does not match vertices");

  // The 16-bit grid reproduces every float position within half a step.
  if (u64{mesh.quantized.size()} < u64{mesh.vertices.size()} * 3)
    return fail("quantized positions are missing or short");
  if ((mesh.quantized.size() & 1u) != 0)
    return fail("quantized positions are not padded to an even count");
  if (!(mesh.quant_scale > 0.0f)) return fail("quantization scale is not positive");
  const f32 quant_tolerance = mesh.quant_scale * 0.5f + 1e-6f;
  for (u32 v = 0; v < mesh.vertices.size(); ++v) {
    const Vec3 p = mesh.vertices[v];
    const Vec3 q = dequantize_position(mesh, v);
    if (std::fabs(q.x - p.x) > quant_tolerance || std::fabs(q.y - p.y) > quant_tolerance ||
        std::fabs(q.z - p.z) > quant_tolerance)
      return fail("a dequantized position is more than half a grid step from the original");
  }

  // The skin binding stream: present or absent as a whole, weights that sum to exactly 255, and
  // joints inside the palette the mesh says it binds to. The fixed sum is what lets the shader
  // skip a divide, so a stream that does not hold it is a bug in whoever built it, not something
  // the GPU is asked to repair.
  if (!mesh.skin.empty()) {
    if (mesh.skin.size() != mesh.vertices.size())
      return fail("the skin binding stream is not parallel to the vertices");
    if (mesh.skin_joint_count == 0) return fail("a skinned mesh binds to no joints");
    if (mesh.skin_joint_count > k_max_skin_joints)
      return fail("a skin binds to more joints than a u8 index holds");
    for (const SkinBinding& binding : mesh.skin) {
      u32 total = 0;
      for (u32 k = 0; k < 4; ++k) {
        total += binding.weights[k];
        if (binding.weights[k] != 0 && binding.joints[k] >= mesh.skin_joint_count)
          return fail("a skin binding names a joint outside the mesh's palette");
      }
      if (total != 255) return fail("skin binding weights do not sum to 255");
    }
  } else if (mesh.skin_joint_count != 0) {
    return fail("the mesh names a joint palette but carries no skin bindings");
  }

  // The id stream: present or absent as a whole, with a source that says where it came from, and
  // one id per source vertex — every copy of a `vertex_source` entry, in every cluster, carries the
  // same one. That last is the identity contract itself: a vertex's name must not depend on which
  // cluster or which LOD level a caller happened to find it in.
  if (!mesh.vertex_ids.empty()) {
    if (mesh.vertex_ids.size() != mesh.vertices.size())
      return fail("the vertex id stream is not parallel to the vertices");
    if (mesh.vertex_id_source == VertexIdSource::none)
      return fail("the mesh carries vertex ids but does not say where they came from");
    u32 sources = 0;
    for (const u32 source : mesh.vertex_source)
      sources = source >= sources ? source + 1 : sources;
    Vector<u32> id_of_source(sources, k_no_vertex_id);
    Vector<u8> seen(sources, u8{0});
    for (u32 v = 0; v < mesh.vertices.size(); ++v) {
      const u32 source = mesh.vertex_source[v];
      if (seen[source] == 0) {
        seen[source] = 1;
        id_of_source[source] = mesh.vertex_ids[v];
      } else if (id_of_source[source] != mesh.vertex_ids[v]) {
        return fail("two cluster copies of one source vertex carry different vertex ids");
      }
    }
  } else if (mesh.vertex_id_source != VertexIdSource::none) {
    return fail("the mesh names a vertex id source but carries no ids");
  }

  if (!validate_morph_stream(mesh, error)) return false;

  // Every source triangle exactly once: compare sorted canonical corner triples.
  Vector<u64> expected;
  Vector<u64> found;
  auto key_of = [](u32 a, u32 b, u32 c) {
    u32 v[3] = {a, b, c};
    std::sort(v, v + 3);
    return (u64{v[0]} << 42) | (u64{v[1]} << 21) | u64{v[2]};
  };
  for (usize i = 0; i + 2 < source_indices.size(); i += 3) {
    expected.push_back(key_of(source_indices[i], source_indices[i + 1], source_indices[i + 2]));
  }
  u32 total_triangles = 0;
  for (u32 c = 0; c < mesh.clusters.size(); ++c) {
    const ClusterDesc& d = mesh.clusters[c];
    if (d.vertex_count == 0 || d.vertex_count > options.max_vertices)
      return fail("cluster vertex count out of limits");
    if (d.triangle_count == 0 || d.triangle_count > options.max_triangles)
      return fail("cluster triangle count out of limits");
    if (u64{d.vertex_offset} + d.vertex_count > mesh.vertices.size())
      return fail("cluster vertex range out of bounds");
    if (u64{d.triangle_offset} + d.triangle_count > mesh.triangles.size())
      return fail("cluster triangle range out of bounds");
    for (u32 v = 0; v < d.vertex_count; ++v) {
      const Vec3 p = mesh.vertices[d.vertex_offset + v];
      if (length(p - d.center) > d.radius * 1.001f + 1e-5f)
        return fail("vertex outside its cluster sphere");
    }
    const NormalCone cone = decode_cone(d.cone);
    if (cone.cutoff < -1.0f || cone.cutoff > 1.0f) return fail("cone cutoff out of range");
    const bool has_cone = cone.cutoff < 1.0f;
    if (has_cone && std::fabs(length(cone.axis) - 1.0f) > 0.02f)
      return fail("cone axis is not unit length");
    for (u32 t = 0; t < d.triangle_count; ++t) {
      const u32 packed = mesh.triangles[d.triangle_offset + t];
      u32 corners[3];
      for (u32 k = 0; k < 3; ++k) {
        corners[k] = ClusterMesh::unpack(packed, k);
        if (corners[k] >= d.vertex_count) return fail("local index out of range");
      }
      found.push_back(key_of(mesh.vertex_source[d.vertex_offset + corners[0]],
                             mesh.vertex_source[d.vertex_offset + corners[1]],
                             mesh.vertex_source[d.vertex_offset + corners[2]]));
    }
    if (has_cone && !check_cone(mesh, d, c, cone, error)) return false;
    total_triangles += d.triangle_count;
  }
  if (total_triangles != mesh.source_triangle_count)
    return fail("triangle total does not match the source");
  std::sort(expected.begin(), expected.end());
  std::sort(found.begin(), found.end());
  if (expected.size() != found.size()) return fail("triangle count mismatch");
  for (u32 i = 0; i < expected.size(); ++i) {
    if (expected[i] != found[i]) return fail("a source triangle is missing or duplicated");
  }
  return true;
}

}  // namespace engine::geometry

// ---- attributes --------------------------------------------------------------------------------

namespace engine::geometry {

u16 f32_to_f16(f32 value) noexcept {
  u32 bits = 0;
  std::memcpy(&bits, &value, 4);
  const u32 sign = (bits >> 16) & 0x8000u;
  const u32 exponent = (bits >> 23) & 0xffu;
  u32 mantissa = bits & 0x7fffffu;
  if (exponent == 0xff) return static_cast<u16>(sign | 0x7c00u | (mantissa != 0 ? 0x200u : 0u));
  const i32 e = static_cast<i32>(exponent) - 127 + 15;
  if (e >= 31) return static_cast<u16>(sign | 0x7c00u);
  if (e <= 0) {
    if (e < -10) return static_cast<u16>(sign);
    mantissa |= 0x800000u;
    const u32 shift = static_cast<u32>(14 - e);
    u32 half = mantissa >> shift;
    const u32 remainder = mantissa & ((1u << shift) - 1);
    const u32 halfway = 1u << (shift - 1);
    if (remainder > halfway || (remainder == halfway && (half & 1u) != 0)) ++half;
    return static_cast<u16>(sign | half);
  }
  u32 half = sign | (static_cast<u32>(e) << 10) | (mantissa >> 13);
  const u32 remainder = mantissa & 0x1fffu;
  if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u) != 0)) ++half;
  return static_cast<u16>(half);
}

f32 f16_to_f32(u16 half) noexcept {
  const u32 sign = (u32{half} & 0x8000u) << 16;
  const u32 exponent = (half >> 10) & 0x1fu;
  const u32 mantissa = half & 0x3ffu;
  u32 bits = 0;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      // Denormal: renormalize. After `e + 1` shifts the value is 1.m * 2^(-14 - (e + 1)), so the
      // biased exponent is 127 - 15 - e. (It was 113 - e until 2026-09-23, which decoded every
      // half denormal at twice its value; the GPU's f16tof32 never did, and the surface binding's
      // decoder test, which walks every half, is what found it.)
      u32 m = mantissa;
      i32 e = -1;
      do {
        m <<= 1;
        ++e;
      } while ((m & 0x400u) == 0);
      bits = sign | (static_cast<u32>(112 - e) << 23) | ((m & 0x3ffu) << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7f800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  }
  f32 value = 0.0f;
  std::memcpy(&value, &bits, 4);
  return value;
}

namespace {

f32 sign_or_one(f32 v) noexcept { return v < 0.0f ? -1.0f : 1.0f; }

}  // namespace

// ---- morph channels ----------------------------------------------------------------------------

namespace {

// A channel's quantization step: its own largest absolute component over 32767, so the channel
// spends its 16 bits on its own extent rather than on the mesh's. An all-zero (or empty) channel
// keeps a scale of 0, which decodes every delta to zero exactly.
f32 morph_scale_of(std::span<const Vec3> deltas) noexcept {
  f32 largest = 0.0f;
  for (const Vec3& d : deltas) {
    largest = std::max(largest, std::fabs(d.x));
    largest = std::max(largest, std::fabs(d.y));
    largest = std::max(largest, std::fabs(d.z));
  }
  return std::isfinite(largest) && largest > 0.0f ? largest / 32767.0f : 0.0f;
}

void quantize_delta(Vec3 delta, f32 scale, i16 out[3]) noexcept {
  const f32 axis[3] = {delta.x, delta.y, delta.z};
  for (u32 k = 0; k < 3; ++k) {
    if (!(scale > 0.0f) || !std::isfinite(axis[k])) {
      out[k] = 0;
      continue;
    }
    const f32 q = std::round(axis[k] / scale);
    const f32 c = q < -32767.0f ? -32767.0f : (q > 32767.0f ? 32767.0f : q);
    out[k] = static_cast<i16>(static_cast<i32>(c));
  }
}

u32 pack_i16_pair(i16 a, i16 b) noexcept {
  return u32{static_cast<u16>(a)} | (u32{static_cast<u16>(b)} << 16);
}

}  // namespace

void fill_cluster_morph(ClusterMesh& mesh, std::span<const MorphChannelSource> channels) {
  mesh.morph_channels.clear();
  mesh.morph_names.clear();
  mesh.morph_cluster_slices.clear();
  mesh.morph_slices.clear();
  mesh.morph_indices.clear();
  mesh.morph_deltas.clear();
  mesh.morph_normal_deltas.clear();
  mesh.morph_delta_count = 0;
  if (channels.empty() || mesh.clusters.empty()) return;

  // The channel records first, and **from the whole source channel**: the scale, the largest
  // displacement and the default weight are properties of the channel, so every part of a mesh
  // and every LOD level of every part quantizes channel k on the same grid and the merges are
  // pure concatenation.
  bool any_normals = false;
  mesh.morph_channels.reserve(static_cast<u32>(channels.size()));
  mesh.morph_names.reserve(static_cast<u32>(channels.size()));
  for (const MorphChannelSource& source : channels) {
    MorphChannel channel;
    channel.position_scale = morph_scale_of(source.position_deltas);
    channel.normal_scale = source.normal_deltas.size() == source.vertices.size()
                               ? morph_scale_of(source.normal_deltas)
                               : 0.0f;
    f32 largest = 0.0f;
    for (const Vec3& d : source.position_deltas) {
      const f32 l = length(d);
      if (std::isfinite(l)) largest = std::max(largest, l);
    }
    channel.max_displacement = largest;
    channel.default_weight = std::isfinite(source.default_weight) ? source.default_weight : 0.0f;
    any_normals = any_normals || channel.normal_scale > 0.0f;
    mesh.morph_channels.push_back(channel);
    mesh.morph_names.push_back(source.name);
  }

  // A CSR from source vertex to the (channel, delta) entries that touch it, so a cluster's
  // lookup costs its own vertices and the deltas they carry rather than a scan of every channel.
  const u32 source_vertices = mesh.source_vertex_count;
  Vector<u32> first(source_vertices + 1);
  for (u32 i = 0; i <= source_vertices; ++i)
    first[i] = 0;
  for (const MorphChannelSource& source : channels) {
    for (const u32 v : source.vertices) {
      if (v < source_vertices) ++first[v + 1];
    }
  }
  for (u32 i = 0; i < source_vertices; ++i)
    first[i + 1] += first[i];
  Vector<u32> entry_channel(first[source_vertices]);
  Vector<u32> entry_delta(first[source_vertices]);
  {
    Vector<u32> cursor = first;
    for (u32 c = 0; c < channels.size(); ++c) {
      const MorphChannelSource& source = channels[c];
      for (u32 i = 0; i < source.vertices.size(); ++i) {
        const u32 v = source.vertices[i];
        if (v >= source_vertices) continue;
        const u32 slot = cursor[v]++;
        entry_channel[slot] = c;
        entry_delta[slot] = i;
      }
    }
  }

  // Now one pass over the clusters, which is what puts the directory in cluster order and the
  // deltas in slice order — the layout `build_cluster_pages` needs to be able to make a page's
  // morph payload one contiguous range of each stream.
  const u32 channel_count = mesh.morph_channels.size();
  Vector<Vector<u32>> bucket_local(channel_count);  // per channel: this cluster's local vertices
  Vector<Vector<u32>> bucket_delta(channel_count);  // and the source delta each one names
  Vector<u32> dirty;
  mesh.morph_cluster_slices.reserve(mesh.clusters.size() + 1);
  mesh.morph_cluster_slices.push_back(0);
  for (u32 c = 0; c < mesh.clusters.size(); ++c) {
    const ClusterDesc& cluster = mesh.clusters[c];
    for (const u32 channel : dirty) {
      bucket_local[channel].clear();
      bucket_delta[channel].clear();
    }
    dirty.clear();
    for (u32 local = 0; local < cluster.vertex_count; ++local) {
      const u32 index = cluster.vertex_offset + local;
      if (index >= mesh.vertex_source.size()) continue;
      const u32 source = mesh.vertex_source[index];
      if (source >= source_vertices) continue;
      for (u32 e = first[source]; e < first[source + 1]; ++e) {
        const u32 channel = entry_channel[e];
        if (bucket_local[channel].empty()) dirty.push_back(channel);
        bucket_local[channel].push_back(local);
        bucket_delta[channel].push_back(entry_delta[e]);
      }
    }
    std::sort(dirty.begin(), dirty.end());
    for (const u32 channel : dirty) {
      MorphSlice slice;
      slice.channel = channel;
      slice.first_delta = mesh.morph_delta_count;
      slice.delta_count = bucket_local[channel].size();
      const MorphChannelSource& source = channels[channel];
      const MorphChannel& record = mesh.morph_channels[channel];
      for (u32 i = 0; i < slice.delta_count; ++i) {
        const u32 delta = bucket_delta[channel][i];
        mesh.morph_indices.push_back(static_cast<u8>(bucket_local[channel][i]));
        i16 p[3];
        quantize_delta(
            delta < source.position_deltas.size() ? source.position_deltas[delta] : Vec3{},
            record.position_scale, p);
        for (u32 k = 0; k < 3; ++k)
          mesh.morph_deltas.push_back(p[k]);
        if (any_normals) {
          i16 n[3];
          quantize_delta(record.normal_scale > 0.0f && delta < source.normal_deltas.size()
                             ? source.normal_deltas[delta]
                             : Vec3{},
                         record.normal_scale, n);
          for (u32 k = 0; k < 3; ++k)
            mesh.morph_normal_deltas.push_back(n[k]);
        }
      }
      mesh.morph_delta_count += slice.delta_count;
      mesh.morph_slices.push_back(slice);
    }
    mesh.morph_cluster_slices.push_back(mesh.morph_slices.size());
  }
  pad_morph_streams(mesh);
}

void append_cluster_morph(ClusterMesh& to, const ClusterMesh& from, u32 cluster, u32 channel_base,
                          bool with_normals) {
  if (cluster + 1 >= from.morph_cluster_slices.size()) return;
  const bool from_normals = from.morph_normal_deltas.size() >= from.morph_deltas.size() &&
                            !from.morph_normal_deltas.empty();
  for (u32 s = from.morph_cluster_slices[cluster]; s < from.morph_cluster_slices[cluster + 1];
       ++s) {
    const MorphSlice& slice = from.morph_slices[s];
    MorphSlice moved;
    moved.channel = slice.channel + channel_base;
    moved.first_delta = to.morph_delta_count;
    moved.delta_count = slice.delta_count;
    for (u32 i = 0; i < slice.delta_count; ++i) {
      const u32 delta = slice.first_delta + i;
      to.morph_indices.push_back(from.morph_indices[delta]);
      for (u32 k = 0; k < 3; ++k)
        to.morph_deltas.push_back(from.morph_deltas[delta * 3 + k]);
      if (with_normals) {
        for (u32 k = 0; k < 3; ++k)
          to.morph_normal_deltas.push_back(from_normals ? from.morph_normal_deltas[delta * 3 + k]
                                                        : i16{0});
      }
    }
    to.morph_delta_count += slice.delta_count;
    to.morph_slices.push_back(moved);
  }
}

void pad_morph_streams(ClusterMesh& mesh) {
  while ((mesh.morph_indices.size() & 3u) != 0)
    mesh.morph_indices.push_back(0);
  if ((mesh.morph_deltas.size() & 1u) != 0) mesh.morph_deltas.push_back(0);
  if ((mesh.morph_normal_deltas.size() & 1u) != 0) mesh.morph_normal_deltas.push_back(0);
}

bool validate_morph_stream(const ClusterMesh& mesh, std::string* error) {
  auto fail = [&](const char* what) {
    if (error != nullptr) *error = what;
    return false;
  };
  if (mesh.morph_channels.empty()) {
    if (!mesh.morph_slices.empty() || !mesh.morph_cluster_slices.empty() ||
        mesh.morph_delta_count != 0)
      return fail("the mesh carries morph data but names no channels");
    return true;
  }
  if (mesh.morph_names.size() != mesh.morph_channels.size())
    return fail("the morph channel names are not parallel to the channels");
  for (const MorphChannel& channel : mesh.morph_channels) {
    if (!std::isfinite(channel.position_scale) || channel.position_scale < 0.0f ||
        !std::isfinite(channel.normal_scale) || channel.normal_scale < 0.0f ||
        !std::isfinite(channel.max_displacement) || channel.max_displacement < 0.0f ||
        !std::isfinite(channel.default_weight))
      return fail("a morph channel's scale, displacement or default weight is not finite");
  }
  if (mesh.morph_cluster_slices.size() != u64{mesh.clusters.size()} + 1)
    return fail("the morph cluster directory does not cover every cluster");
  if (mesh.morph_cluster_slices[0] != 0 ||
      mesh.morph_cluster_slices[mesh.clusters.size()] != mesh.morph_slices.size())
    return fail("the morph cluster directory does not span the slices exactly");
  if (u64{mesh.morph_indices.size()} < mesh.morph_delta_count ||
      (mesh.morph_indices.size() & 3u) != 0)
    return fail("the morph index stream is short or not padded to a multiple of four");
  if (u64{mesh.morph_deltas.size()} < u64{mesh.morph_delta_count} * 3 ||
      (mesh.morph_deltas.size() & 1u) != 0)
    return fail("the morph delta stream is short or not padded to an even count");
  if (!mesh.morph_normal_deltas.empty() &&
      (u64{mesh.morph_normal_deltas.size()} < u64{mesh.morph_delta_count} * 3 ||
       (mesh.morph_normal_deltas.size() & 1u) != 0))
    return fail("the morph normal delta stream is short or not padded to an even count");
  u32 next_delta = 0;
  for (u32 c = 0; c < mesh.clusters.size(); ++c) {
    const u32 lo = mesh.morph_cluster_slices[c];
    const u32 hi = mesh.morph_cluster_slices[c + 1];
    if (hi < lo || hi > mesh.morph_slices.size())
      return fail("a morph cluster directory entry is out of order or out of range");
    u32 previous_channel = ~0u;
    for (u32 s = lo; s < hi; ++s) {
      const MorphSlice& slice = mesh.morph_slices[s];
      if (slice.channel >= mesh.morph_channels.size())
        return fail("a morph slice names a channel the mesh does not have");
      if (previous_channel != ~0u && slice.channel <= previous_channel)
        return fail("a cluster's morph slices are not in ascending channel order");
      previous_channel = slice.channel;
      if (slice.first_delta != next_delta)
        return fail("a morph slice's deltas are not contiguous with the slice before it");
      if (u64{slice.first_delta} + slice.delta_count > mesh.morph_delta_count)
        return fail("a morph slice runs past the end of the delta stream");
      for (u32 i = 0; i < slice.delta_count; ++i) {
        if (u32{mesh.morph_indices[slice.first_delta + i]} >= mesh.clusters[c].vertex_count)
          return fail("a morph delta names a vertex outside its cluster");
      }
      next_delta += slice.delta_count;
    }
  }
  if (next_delta != mesh.morph_delta_count)
    return fail("the morph slices do not account for every delta");
  return true;
}

bool morph_delta_at(const ClusterMesh& mesh, u32 cluster, u32 channel, u32 local,
                    Vec3& position_delta, Vec3& normal_delta) noexcept {
  position_delta = Vec3{};
  normal_delta = Vec3{};
  if (cluster + 1 >= mesh.morph_cluster_slices.size()) return false;
  if (channel >= mesh.morph_channels.size()) return false;
  const MorphChannel& record = mesh.morph_channels[channel];
  const bool have_normals = mesh.morph_normal_deltas.size() >= mesh.morph_deltas.size();
  for (u32 s = mesh.morph_cluster_slices[cluster]; s < mesh.morph_cluster_slices[cluster + 1];
       ++s) {
    const MorphSlice& slice = mesh.morph_slices[s];
    if (slice.channel != channel) continue;
    for (u32 i = 0; i < slice.delta_count; ++i) {
      const u32 delta = slice.first_delta + i;
      if (mesh.morph_indices[delta] != static_cast<u8>(local)) continue;
      const u32 w = delta * 3;
      position_delta = Vec3{static_cast<f32>(mesh.morph_deltas[w]) * record.position_scale,
                            static_cast<f32>(mesh.morph_deltas[w + 1]) * record.position_scale,
                            static_cast<f32>(mesh.morph_deltas[w + 2]) * record.position_scale};
      if (have_normals && record.normal_scale > 0.0f) {
        normal_delta =
            Vec3{static_cast<f32>(mesh.morph_normal_deltas[w]) * record.normal_scale,
                 static_cast<f32>(mesh.morph_normal_deltas[w + 1]) * record.normal_scale,
                 static_cast<f32>(mesh.morph_normal_deltas[w + 2]) * record.normal_scale};
      }
      return true;
    }
    return false;
  }
  return false;
}

f32 morph_bounds_padding(const ClusterMesh& mesh, std::span<const f32> weights) noexcept {
  f32 total = 0.0f;
  for (u32 c = 0; c < mesh.morph_channels.size() && c < weights.size(); ++c) {
    const f32 w = weights[c];
    if (!std::isfinite(w)) continue;
    total += std::fabs(w) * mesh.morph_channels[c].max_displacement;
  }
  return total;
}

u32 encode_normal_oct(Vec3 normal) noexcept {
  const f32 l1 = std::fabs(normal.x) + std::fabs(normal.y) + std::fabs(normal.z);
  Vec2 p = l1 > 0.0f ? Vec2{normal.x / l1, normal.y / l1} : Vec2{0.0f, 0.0f};
  if (normal.z < 0.0f) {
    p = Vec2{(1.0f - std::fabs(p.y)) * sign_or_one(p.x),
             (1.0f - std::fabs(p.x)) * sign_or_one(p.y)};
  }
  auto snorm = [](f32 v) {
    const f32 c = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    const i32 i = static_cast<i32>(std::lround(c * 32767.0f));
    return static_cast<u32>(static_cast<u16>(static_cast<i16>(i)));
  };
  return snorm(p.x) | (snorm(p.y) << 16);
}

Vec3 decode_normal_oct(u32 packed) noexcept {
  const f32 x = static_cast<f32>(static_cast<i16>(packed & 0xffffu)) / 32767.0f;
  const f32 y = static_cast<f32>(static_cast<i16>(packed >> 16)) / 32767.0f;
  Vec3 n{x, y, 1.0f - std::fabs(x) - std::fabs(y)};
  if (n.z < 0.0f) {
    n = Vec3{(1.0f - std::fabs(y)) * sign_or_one(x), (1.0f - std::fabs(x)) * sign_or_one(y), n.z};
  }
  return normalize(n);
}

u32 encode_half2(Vec2 v) noexcept { return u32{f32_to_f16(v.x)} | (u32{f32_to_f16(v.y)} << 16); }

Vec2 decode_half2(u32 packed) noexcept {
  return Vec2{f16_to_f32(static_cast<u16>(packed & 0xffffu)),
              f16_to_f32(static_cast<u16>(packed >> 16))};
}

void compute_vertex_normals(std::span<const Vec3> positions, std::span<const u32> indices,
                            Vector<Vec3>& out) {
  out.clear();
  out.resize(static_cast<u32>(positions.size()));
  for (Vec3& n : out)
    n = Vec3{};
  for (usize i = 0; i + 2 < indices.size(); i += 3) {
    const u32 a = indices[i];
    const u32 b = indices[i + 1];
    const u32 c = indices[i + 2];
    if (a >= positions.size() || b >= positions.size() || c >= positions.size()) continue;
    const Vec3 face =
        cross(positions[b] - positions[a], positions[c] - positions[a]);  // area-weighted
    out[a] = out[a] + face;
    out[b] = out[b] + face;
    out[c] = out[c] + face;
  }
  for (Vec3& n : out)
    n = length_squared(n) > 1e-20f ? normalize(n) : Vec3{0.0f, 1.0f, 0.0f};
}

u32 weld_vertices(Vector<Vec3>& positions, Vector<Vec3>& normals, Vector<Vec2>& uvs,
                  std::span<u32> indices, Vector<SkinBinding>* skin,
                  Vector<MorphChannelSource>* morph, Vector<u32>* vertex_ids) {
  const u32 vertex_count = positions.size();
  if (vertex_count == 0) return 0;
  const bool have_normals = normals.size() == vertex_count;
  const bool have_uvs = uvs.size() == vertex_count;
  const bool have_skin = skin != nullptr && skin->size() == vertex_count;
  const bool have_ids = vertex_ids != nullptr && vertex_ids->size() == vertex_count;
  Vector<u32> morph_keys;
  const bool have_morph = morph != nullptr && !morph->empty();
  if (have_morph) {
    morph_vertex_keys(std::span<const MorphChannelSource>(morph->data(), morph->size()),
                      vertex_count, morph_keys);
  }
  meshopt_Stream streams[6];
  usize stream_count = 0;
  streams[stream_count++] = meshopt_Stream{positions.data(), sizeof(Vec3), sizeof(Vec3)};
  if (have_normals)
    streams[stream_count++] = meshopt_Stream{normals.data(), sizeof(Vec3), sizeof(Vec3)};
  if (have_uvs) streams[stream_count++] = meshopt_Stream{uvs.data(), sizeof(Vec2), sizeof(Vec2)};
  // The binding is part of the key: two duplicates at one position with different weights are
  // different vertices, because a weld that merged them would give one of the two surfaces the
  // other's deformation.
  if (have_skin)
    streams[stream_count++] =
        meshopt_Stream{skin->data(), sizeof(SkinBinding), sizeof(SkinBinding)};
  // And the morph deltas are part of it for exactly the same reason, through the interned id.
  if (have_morph)
    streams[stream_count++] = meshopt_Stream{morph_keys.data(), sizeof(u32), sizeof(u32)};
  // And the canonical id: two duplicates the author named as two points of the base stay two
  // vertices. A derived id is a function of the position, so it never splits what the position
  // stream would have merged, and a mesh with derived ids welds exactly as it did without them.
  if (have_ids)
    streams[stream_count++] = meshopt_Stream{vertex_ids->data(), sizeof(u32), sizeof(u32)};

  Vector<unsigned int> remap(vertex_count);
  const u32 unique = static_cast<u32>(meshopt_generateVertexRemapMulti(
      remap.data(), indices.empty() ? nullptr : indices.data(),
      indices.empty() ? vertex_count : indices.size(), vertex_count, streams, stream_count));
  Vector<Vec3> welded_positions(unique);
  meshopt_remapVertexBuffer(welded_positions.data(), positions.data(), vertex_count, sizeof(Vec3),
                            remap.data());
  positions = std::move(welded_positions);
  if (have_normals) {
    Vector<Vec3> welded(unique);
    meshopt_remapVertexBuffer(welded.data(), normals.data(), vertex_count, sizeof(Vec3),
                              remap.data());
    normals = std::move(welded);
  }
  if (have_uvs) {
    Vector<Vec2> welded(unique);
    meshopt_remapVertexBuffer(welded.data(), uvs.data(), vertex_count, sizeof(Vec2), remap.data());
    uvs = std::move(welded);
  }
  if (have_skin) {
    Vector<SkinBinding> welded(unique);
    meshopt_remapVertexBuffer(welded.data(), skin->data(), vertex_count, sizeof(SkinBinding),
                              remap.data());
    *skin = std::move(welded);
  }
  if (have_ids) {
    Vector<u32> welded(unique);
    meshopt_remapVertexBuffer(welded.data(), vertex_ids->data(), vertex_count, sizeof(u32),
                              remap.data());
    *vertex_ids = std::move(welded);
  }
  if (have_morph) {
    // Every channel's vertex list is in the old numbering. Rewrite it, drop the entries whose
    // vertex the weld dropped (remap ~0u), then sort by the new index and keep one of each run —
    // exact, because two vertices only merged if `morph_vertex_keys` gave them the same id, which
    // means bit-identical deltas.
    Vector<u32> order;
    Vector<u32> vertices;
    Vector<Vec3> positions_out;
    Vector<Vec3> normals_out;
    for (MorphChannelSource& channel : *morph) {
      const u32 n = channel.vertices.size();
      const bool channel_normals = channel.normal_deltas.size() == n;
      order.clear();
      order.reserve(n);
      for (u32 i = 0; i < n; ++i) {
        const u32 v = channel.vertices[i];
        if (v >= vertex_count || remap[v] == ~0u) continue;
        order.push_back(i);
      }
      std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        const u32 ra = remap[channel.vertices[a]];
        const u32 rb = remap[channel.vertices[b]];
        return ra != rb ? ra < rb : a < b;
      });
      vertices.clear();
      positions_out.clear();
      normals_out.clear();
      u32 previous = ~0u;
      for (const u32 i : order) {
        const u32 v = remap[channel.vertices[i]];
        if (v == previous) continue;
        previous = v;
        vertices.push_back(v);
        positions_out.push_back(i < channel.position_deltas.size() ? channel.position_deltas[i]
                                                                   : Vec3{});
        if (channel_normals) normals_out.push_back(channel.normal_deltas[i]);
      }
      channel.vertices = vertices;
      channel.position_deltas = positions_out;
      channel.normal_deltas = channel_normals ? normals_out : Vector<Vec3>{};
    }
  }
  if (!indices.empty()) {
    meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
  }
  return unique;
}

u32 morph_vertex_keys(std::span<const MorphChannelSource> channels, u32 vertex_count,
                      Vector<u32>& out) {
  out.clear();
  out.resize(vertex_count);
  for (u32 i = 0; i < vertex_count; ++i)
    out[i] = 0;
  if (channels.empty() || vertex_count == 0) return 1;

  // The tuple of one vertex, built once per vertex the sparse data mentions: the channel index
  // and the six quantized components, appended in channel order. Quantized rather than float,
  // because what matters is whether the two vertices end up with the same **stored** deltas — two
  // that differ below the channel's step are the same vertex to everything downstream.
  Vector<u32> touched;         // the vertices any channel mentions, in first-appearance order
  Vector<u32> tuple_first;     // CSR into `tuple_words`, per entry of `touched`
  Vector<u32> tuple_words;     // channel, then the six i16 packed into three u32
  Vector<u32> slot_of_vertex;  // vertex -> entry of `touched`, or ~0u
  slot_of_vertex.resize(vertex_count);
  for (u32 i = 0; i < vertex_count; ++i)
    slot_of_vertex[i] = ~0u;

  // Two passes: collect the words per vertex, then intern. The first pass appends in channel
  // order, so a vertex's words come out in ascending channel order whatever order the channels
  // mention it in — which is what makes the tuple a function of the data alone.
  Vector<Vector<u32>> per_vertex;
  for (u32 c = 0; c < channels.size(); ++c) {
    const MorphChannelSource& channel = channels[c];
    const f32 position_scale = morph_scale_of(channel.position_deltas);
    const f32 normal_scale = morph_scale_of(channel.normal_deltas);
    const u32 n = channel.vertices.size();
    for (u32 i = 0; i < n; ++i) {
      const u32 v = channel.vertices[i];
      if (v >= vertex_count) continue;
      if (slot_of_vertex[v] == ~0u) {
        slot_of_vertex[v] = touched.size();
        touched.push_back(v);
        per_vertex.push_back(Vector<u32>{});
      }
      Vector<u32>& words = per_vertex[slot_of_vertex[v]];
      const Vec3 dp = i < channel.position_deltas.size() ? channel.position_deltas[i] : Vec3{};
      const Vec3 dn = i < channel.normal_deltas.size() ? channel.normal_deltas[i] : Vec3{};
      i16 p[3];
      i16 nrm[3];
      quantize_delta(dp, position_scale, p);
      quantize_delta(dn, normal_scale, nrm);
      words.push_back(c);
      words.push_back(pack_i16_pair(p[0], p[1]));
      words.push_back(pack_i16_pair(p[2], nrm[0]));
      words.push_back(pack_i16_pair(nrm[1], nrm[2]));
    }
  }
  tuple_first.reserve(touched.size() + 1);
  tuple_first.push_back(0);
  for (u32 s = 0; s < touched.size(); ++s) {
    for (const u32 w : per_vertex[s])
      tuple_words.push_back(w);
    tuple_first.push_back(tuple_words.size());
  }

  // Intern. The map is hash -> the ids that hash there; a collision compares the words, so an id
  // means "bit-identical tuple" and not "same hash".
  HashMap<u64, Vector<u32>> by_hash;
  Vector<u32> id_slot;  // the representative entry of each id; id 0 is "no deltas" and has none
  id_slot.push_back(~0u);
  for (u32 s = 0; s < touched.size(); ++s) {
    const u32 first = tuple_first[s];
    const u32 count = tuple_first[s + 1] - first;
    const u64 h = hash_bytes(tuple_words.data() + first, usize{count} * sizeof(u32));
    Vector<u32>& bucket = by_hash[h];
    u32 id = 0;
    for (const u32 candidate : bucket) {
      const u32 other = id_slot[candidate];
      const u32 other_first = tuple_first[other];
      const u32 other_count = tuple_first[other + 1] - other_first;
      if (other_count != count) continue;
      if (std::memcmp(tuple_words.data() + first, tuple_words.data() + other_first,
                      usize{count} * sizeof(u32)) == 0) {
        id = candidate;
        break;
      }
    }
    if (id == 0) {
      id = id_slot.size();
      id_slot.push_back(s);
      bucket.push_back(id);
    }
    out[touched[s]] = id;
  }
  return id_slot.size();
}

const char* vertex_id_source_name(VertexIdSource source) noexcept {
  switch (source) {
    case VertexIdSource::none: return "none";
    case VertexIdSource::authored: return "authored";
    case VertexIdSource::position_weld: return "position_weld";
  }
  return "unknown";
}

u32 position_weld_ids(std::span<const Vec3> positions, Vector<u32>& out) {
  out.clear();
  const u32 count = static_cast<u32>(positions.size());
  out.resize(count);
  if (count == 0) return 0;
  // One 16-byte record per vertex — the three coordinates as order-preserving integers, then the
  // vertex — so the sort touches one contiguous array rather than chasing an index into the
  // positions. The transform maps a float's bits to an unsigned integer that orders the way the
  // float does (negative values flipped whole, positive ones with the sign bit set), after folding
  // -0 into +0 so that the two zeros are one position, as they are to any comparison of values.
  // NaN has bits too, so a malformed position still sorts somewhere definite rather than making the
  // order depend on the sort's internals.
  struct Key {
    u32 x = 0;
    u32 y = 0;
    u32 z = 0;
    u32 vertex = 0;
  };
  auto ordered = [](f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    if (bits == 0x80000000u) bits = 0;
    return (bits & 0x80000000u) != 0 ? ~bits : (bits | 0x80000000u);
  };
  Vector<Key> keys(count);
  for (u32 v = 0; v < count; ++v) {
    const Vec3& p = positions[v];
    keys[v] = Key{ordered(p.x), ordered(p.y), ordered(p.z), v};
  }
  std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
    if (a.x != b.x) return a.x < b.x;
    if (a.y != b.y) return a.y < b.y;
    if (a.z != b.z) return a.z < b.z;
    return a.vertex < b.vertex;
  });
  u32 id = 0;
  for (u32 i = 0; i < count; ++i) {
    if (i > 0 &&
        (keys[i].x != keys[i - 1].x || keys[i].y != keys[i - 1].y || keys[i].z != keys[i - 1].z)) {
      ++id;
    }
    out[keys[i].vertex] = id;
  }
  return id + 1;
}

void fill_cluster_attributes(ClusterMesh& mesh, std::span<const Vec3> positions,
                             std::span<const u32> indices, const AttributeSource& attributes) {
  Vector<Vec3> computed;
  std::span<const Vec3> normals = attributes.normals;
  if (normals.size() != positions.size()) {
    compute_vertex_normals(positions, indices, computed);
    normals = std::span<const Vec3>(computed.data(), computed.size());
  }
  const bool have_uvs = attributes.uvs.size() == positions.size();
  mesh.attributes.clear();
  mesh.attributes.reserve(mesh.vertex_source.size());
  for (const u32 source : mesh.vertex_source) {
    VertexAttributes a;
    a.normal_oct =
        encode_normal_oct(source < normals.size() ? normals[source] : Vec3{0.0f, 1.0f, 0.0f});
    a.uv_half2 = encode_half2(have_uvs ? attributes.uvs[source] : Vec2{0.0f, 0.0f});
    mesh.attributes.push_back(a);
  }

  // The skin binding travels the same road as the normals and the UVs: through `vertex_source`,
  // which is what gives every LOD level its bindings, since clusterlod keeps original vertices.
  // That is also why skinning is crack-free — every copy of a source vertex, in every cluster
  // and on every level, gets bitwise the same four joints and four weights.
  // The morph channels travel the same road for the same reason, and the directory they end up
  // in is keyed by cluster rather than by vertex because that is the question the deform pass
  // asks (see `fill_cluster_morph`).
  fill_cluster_morph(mesh, attributes.morph);

  // The canonical ids travel the same road again, which is the whole of the identity contract at
  // this layer: a cluster vertex's id is its source vertex's, on every level and in every cluster.
  mesh.vertex_ids.clear();
  mesh.vertex_id_source = VertexIdSource::none;
  if (attributes.vertex_ids.size() == positions.size() && !attributes.vertex_ids.empty() &&
      attributes.vertex_id_source != VertexIdSource::none) {
    mesh.vertex_id_source = attributes.vertex_id_source;
    mesh.vertex_ids.reserve(mesh.vertex_source.size());
    for (const u32 source : mesh.vertex_source)
      mesh.vertex_ids.push_back(attributes.vertex_ids[source]);
  }

  mesh.skin.clear();
  mesh.skin_joint_count = 0;
  if (attributes.skin.size() != positions.size()) return;
  mesh.skin_joint_count = attributes.joint_count;
  mesh.skin.reserve(mesh.vertex_source.size());
  for (const u32 source : mesh.vertex_source)
    mesh.skin.push_back(attributes.skin[source]);
}

}  // namespace engine::geometry
