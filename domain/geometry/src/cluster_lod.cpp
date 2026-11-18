#include <domain/geometry/cluster_lod.h>

#include <meshoptimizer.h>

// clusterlod.h is meshoptimizer's reference cluster LOD builder, header-only with the
// implementation compiled into this one translation unit.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#define CLUSTERLOD_IMPLEMENTATION
#include <clusterlod.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>

namespace engine::geometry {

namespace {

struct BuildContext {
  // The builder's vertices as they stand: the source's, and behind them the corners of every
  // island a level grew (geometry.md, "Loose islands"). `origin` names the source vertex each
  // stands for and is empty while nothing has grown, when a vertex is its own.
  const clodMesh* mesh = nullptr;
  const std::vector<unsigned int>* origin = nullptr;
  ClusterLodMesh* out;
  bool normal_cones = true;
  Vector<clodBounds> group_bounds;  // simplified bounds per group, in output order
  Vector<unsigned int> local_vertices;
  Vector<unsigned char> local_triangles;
  bool failed = false;
  // `build_cluster_lod_levels`: a cluster whose own error reaches this is not emitted (the caller's
  // first authored level stands there). FLT_MAX for a plain build, which no own error reaches: an
  // own error is a refined group's, and a group whose error is FLT_MAX is never refined.
  f32 cap = k_lod_terminal_error;
  u32 skipped = 0;
  // The coverage errors of an alpha-tested part (`ClusterLodOptions::mask_cover`): per group, in
  // output order, and the one the builder's loop computed for the group being emitted. Terminal
  // (no coverage) for a group that is not refined.
  bool masked = false;
  Vector<f32> group_cover;
  f32 pending_cover = k_lod_terminal_error;
};

// The attribute record the simplifier is given per source vertex: the normal, then the UV.
// Five floats and a matching stride, so `attribute_protect_mask` bit k names slot k — the same
// layout meshoptimizer's own cluster-LOD sample uses, with the UVs where it puts them.
constexpr u32 k_lod_attribute_count = 5;

u8 seam_bit(SeamRule rule) noexcept {
  switch (rule) {
    case SeamRule::protect: return static_cast<u8>(meshopt_SimplifyVertex_Protect);
    case SeamRule::lock: return static_cast<u8>(meshopt_SimplifyVertex_Lock);
    case SeamRule::none: break;
  }
  return 0;
}

// Appends `part`'s vertex ids to `to`, or `k_no_vertex_id` for each of its vertices when it carries
// none, so the merged stream stays parallel to the merged vertices. `to.vertices` must already hold
// the part's vertices. Nothing is appended when no part of the merge has ids (`wanted` false),
// which is what keeps a merge of id-less meshes exactly what it was.
void append_vertex_ids(ClusterMesh& to, const ClusterMesh& part, bool wanted) {
  if (!wanted) return;
  if (!part.vertex_ids.empty() && part.vertex_ids.size() == part.vertices.size() &&
      part.vertex_id_source != VertexIdSource::none) {
    to.vertex_ids.append(std::span<const u32>(part.vertex_ids.data(), part.vertex_ids.size()));
  } else {
    to.vertex_ids.resize(to.vertices.size(), k_no_vertex_id);
  }
}

// The same for an imported asset's tangent, second-UV and colour records (G2): a part without them
// contributes the record that says "none" (no tangent, UV1 zero, white), and a merge in which no
// part has them appends nothing.
void append_vertex_extra(ClusterMesh& to, const ClusterMesh& part, bool wanted) {
  if (!wanted) return;
  if (!part.extra.empty() && part.extra.size() == part.vertices.size()) {
    to.extra.append(std::span<const VertexExtra>(part.extra.data(), part.extra.size()));
  } else {
    to.extra.resize(to.vertices.size(), VertexExtra{});
  }
}

Vec3 position_of(const clodMesh& mesh, unsigned int v) noexcept;

int emit_group(void* context, clodGroup group, const clodCluster* clusters, size_t cluster_count) {
  auto* ctx = static_cast<BuildContext*>(context);
  ClusterLodMesh& out = *ctx->out;
  const clodMesh& pool = *ctx->mesh;
  const int group_index = static_cast<int>(ctx->group_bounds.size());
  ctx->group_bounds.push_back(group.simplified);
  ctx->group_cover.push_back(ctx->pending_cover);
  const u32 depth = group.depth >= 0 ? static_cast<u32>(group.depth) : 0;
  while (out.level_cluster_counts.size() <= depth)
    out.level_cluster_counts.push_back(0);

  for (size_t i = 0; i < cluster_count; ++i) {
    const clodCluster& cluster = clusters[i];
    if (cluster.refined >= 0 &&
        ctx->group_bounds[static_cast<u32>(cluster.refined)].error >= ctx->cap) {
      ++ctx->skipped;
      continue;
    }
    if (cluster.vertex_count > 255 || cluster.index_count / 3 > 512) {
      ctx->failed = true;
      return group_index;
    }
    ctx->local_vertices.resize(static_cast<u32>(cluster.vertex_count));
    ctx->local_triangles.resize(static_cast<u32>(cluster.index_count));
    clodLocalIndices(ctx->local_vertices.data(), ctx->local_triangles.data(), cluster.indices,
                     cluster.index_count);

    ClusterDesc desc;
    desc.vertex_offset = out.mesh.vertices.size();
    desc.triangle_offset = out.mesh.triangles.size();
    desc.vertex_count = static_cast<u32>(cluster.vertex_count);
    desc.triangle_count = static_cast<u32>(cluster.index_count / 3);
    desc.center =
        Vec3{cluster.bounds.center[0], cluster.bounds.center[1], cluster.bounds.center[2]};
    desc.radius = cluster.bounds.radius;
    // clusterlod bounds carry no cone; meshoptimizer's meshlet bounds do, on the local indices.
    const meshopt_Bounds bounds = meshopt_computeMeshletBounds(
        ctx->local_vertices.data(), ctx->local_triangles.data(), cluster.index_count / 3,
        pool.vertex_positions, pool.vertex_count, pool.vertex_positions_stride);
    desc.cone_apex = Vec3{bounds.cone_apex[0], bounds.cone_apex[1], bounds.cone_apex[2]};
    if (ctx->normal_cones) {
      auto byte = [](signed char v) { return u32{static_cast<u8>(v)}; };
      desc.cone = byte(bounds.cone_axis_s8[0]) | (byte(bounds.cone_axis_s8[1]) << 8) |
                  (byte(bounds.cone_axis_s8[2]) << 16) | (byte(bounds.cone_cutoff_s8) << 24);
    }
    for (u32 v = 0; v < desc.vertex_count; ++v) {
      const u32 source = ctx->local_vertices[v];
      out.mesh.vertices.push_back(position_of(pool, source));
      out.mesh.vertex_source.push_back(ctx->origin->empty() ? source : (*ctx->origin)[source]);
    }
    for (u32 t = 0; t < desc.triangle_count; ++t) {
      const unsigned char* tri = &ctx->local_triangles[t * 3];
      out.mesh.triangles.push_back(ClusterMesh::pack(tri[0], tri[1], tri[2]));
    }
    out.mesh.clusters.push_back(desc);

    ClusterLodDesc lod;
    if (cluster.refined >= 0) {
      const clodBounds& own = ctx->group_bounds[static_cast<u32>(cluster.refined)];
      lod.own = Vec4{own.center[0], own.center[1], own.center[2], own.radius};
      lod.own_error = own.error;
    } else {
      lod.own = Vec4{desc.center.x, desc.center.y, desc.center.z, desc.radius};
      lod.own_error = 0.0f;
    }
    lod.parent = Vec4{group.simplified.center[0], group.simplified.center[1],
                      group.simplified.center[2], group.simplified.radius};
    lod.parent_error = group.simplified.error;
    lod.level = depth;
    lod.group = static_cast<u32>(group_index);
    out.lod.push_back(lod);
    if (ctx->masked) {
      // A level 0 cluster's own transition is always acceptable; the others' is their refined
      // group's, and every cluster of this group shares its parent's.
      const f32 own =
          cluster.refined >= 0 ? ctx->group_cover[static_cast<u32>(cluster.refined)] : 0.0f;
      out.coverage.push_back(pack_lod_coverage(own, ctx->pending_cover));
    }
    ++out.level_cluster_counts[depth];
    if (depth == 0) out.leaf_triangle_count += desc.triangle_count;
  }
  return group_index;
}

// ---- heightfield levels never fold ----------------------------------------------------------
//
// `up`, normalized in double: the direction a heightfield's ground faces. Every decision below is
// made in double from the float positions, with no C library call but a correctly rounded square
// root, so it is the same decision on every toolchain (geometry.md, "The same bytes from every
// toolchain").
struct Up {
  f64 x = 0.0, y = 0.0, z = 0.0;
};

Up normalized_up(Vec3 up) noexcept {
  const f64 x = static_cast<f64>(up.x), y = static_cast<f64>(up.y), z = static_cast<f64>(up.z);
  const f64 len = std::sqrt(x * x + y * y + z * z);
  if (!(len > 0.0)) return Up{};
  return Up{x / len, y / len, z / len};
}

// A triangle's geometric normal (counter-clockwise seen from its front, unnormalized) and its
// component along `up`: twice the signed area of the triangle's shadow on the ground plane.
struct Along {
  f64 d = 0.0;   // n . up
  f64 n2 = 0.0;  // |n|^2
};

Along along_up(Vec3 a, Vec3 b, Vec3 c, const Up& up) noexcept {
  const f64 ex = static_cast<f64>(b.x) - static_cast<f64>(a.x);
  const f64 ey = static_cast<f64>(b.y) - static_cast<f64>(a.y);
  const f64 ez = static_cast<f64>(b.z) - static_cast<f64>(a.z);
  const f64 fx = static_cast<f64>(c.x) - static_cast<f64>(a.x);
  const f64 fy = static_cast<f64>(c.y) - static_cast<f64>(a.y);
  const f64 fz = static_cast<f64>(c.z) - static_cast<f64>(a.z);
  const f64 nx = ey * fz - ez * fy;
  const f64 ny = ez * fx - ex * fz;
  const f64 nz = ex * fy - ey * fx;
  return Along{nx * up.x + ny * up.y + nz * up.z, nx * nx + ny * ny + nz * nz};
}

// The census's reading (`count_lod_folds`): facing up, standing on edge (within `k_fold_facing` of
// square either way: a skirt hung straight down from a border is exactly square), or away.
enum class Facing : u8 { up, on_edge, away };

Facing facing_of(Vec3 a, Vec3 b, Vec3 c, const Up& up) noexcept {
  const Along along = along_up(a, b, c, up);
  if (along.d * along.d <= k_fold_facing * k_fold_facing * along.n2) return Facing::on_edge;
  return along.d > 0.0 ? Facing::up : Facing::away;
}

// The guard's, which is stricter: a triangle is **turned over** when its shadow's signed area is
// negative at all, so no turn is too small to be one. A heightfield's triangle has a shadow of
// positive area, and a skirt hung straight down from a border one of exactly zero (two of its
// corners share their position across `up`, so both products are of an exact zero), so nothing a
// heightfield level may hold is refused. A sliver whose shadow rounds to a hair under zero is
// refused too, which costs a collapse and never a fold.
bool turns_over(Vec3 a, Vec3 b, Vec3 c, const Up& up) noexcept {
  return along_up(a, b, c, up).d < 0.0;
}

Vec3 position_of(const clodMesh& mesh, unsigned int v) noexcept {
  const float* p = &mesh.vertex_positions[v * (mesh.vertex_positions_stride / sizeof(float))];
  return Vec3{p[0], p[1], p[2]};
}

// What a heightfield's simplification is held to: `up`, and the 16-bit grid the mesh will be drawn
// on. The grid is `quantize_positions`' over the vertices the triangles name — the box of level 0,
// which every level shares because clusterlod keeps source vertices — known before the first
// level is simplified; `snap` places a vertex on it with `quantize_positions`' arithmetic and
// `dequantize_position`'s, operation for operation, so the guard sees the triangle the
// rasterizers draw and not only the one the simplifier made. A sliver a few grid steps wide can
// turn over when its corners snap, and that is a fold too.
struct FoldGuard {
  Up up;
  bool refuse = true;
  Vec3 lo{};
  f32 scale = 1.0f;
  f32 inverse = 1.0f;
};

FoldGuard fold_guard_of(const ClusterLodOptions& options, std::span<const Vec3> positions,
                        std::span<const u32> indices) noexcept {
  FoldGuard guard;
  guard.up = normalized_up(options.up);
  guard.refuse = options.refuse_folds;
  Vec3 lo = positions[indices[0]];
  Vec3 hi = lo;
  for (const u32 index : indices) {
    const Vec3 p = positions[index];
    lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  const f32 extent = std::max(std::max(hi.x - lo.x, hi.y - lo.y), hi.z - lo.z);
  guard.lo = lo;
  guard.scale = extent > 0.0f ? extent / 65535.0f : 1.0f;
  guard.inverse = 1.0f / guard.scale;
  return guard;
}

Vec3 snap(const FoldGuard& guard, Vec3 p) noexcept {
  const f32 axis[3] = {p.x - guard.lo.x, p.y - guard.lo.y, p.z - guard.lo.z};
  f32 q[3];
  for (u32 k = 0; k < 3; ++k) {
    const f32 grid = std::round(axis[k] * guard.inverse);
    const f32 clamped = grid < 0.0f ? 0.0f : (grid > 65535.0f ? 65535.0f : grid);
    q[k] = static_cast<f32>(static_cast<u16>(static_cast<u32>(clamped)));
  }
  return Vec3{guard.lo.x + q[0] * guard.scale, guard.lo.y + q[1] * guard.scale,
              guard.lo.z + q[2] * guard.scale};
}

// A triangle folds when it is turned over as the simplifier made it or as the grid draws it.
bool folds(const FoldGuard& guard, Vec3 a, Vec3 b, Vec3 c) noexcept {
  return turns_over(a, b, c, guard.up) ||
         turns_over(snap(guard, a), snap(guard, b), snap(guard, c), guard.up);
}

bool folds(const FoldGuard& guard, const clodMesh& mesh, const unsigned int* triangle) noexcept {
  return folds(guard, position_of(mesh, triangle[0]), position_of(mesh, triangle[1]),
               position_of(mesh, triangle[2]));
}

// How many triangles of an index list fold.
size_t count_folds(const clodMesh& mesh, const std::vector<unsigned int>& indices,
                   const FoldGuard& guard) noexcept {
  size_t count = 0;
  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    if (folds(guard, mesh, &indices[i])) ++count;
  }
  return count;
}

// The regular simplification of one group, as clusterlod's `simplify` makes it: permissive, sparse,
// absolute error, and the permissive fallback upstream has for a config that is not permissive.
void simplify_regular(const clodConfig& config, const clodMesh& mesh,
                      const std::vector<unsigned int>& indices,
                      const std::vector<unsigned char>& locks, size_t target_count, float* error,
                      std::vector<unsigned int>& lod) {
  lod.resize(indices.size());
  unsigned int options = meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute |
                         (config.simplify_permissive ? meshopt_SimplifyPermissive : 0) |
                         (config.simplify_regularize ? meshopt_SimplifyRegularize : 0);

  lod.resize(meshopt_simplifyWithAttributes(
      &lod[0], &indices[0], indices.size(), mesh.vertex_positions, mesh.vertex_count,
      mesh.vertex_positions_stride, mesh.vertex_attributes, mesh.vertex_attributes_stride,
      mesh.attribute_weights, mesh.attribute_count, &locks[0], target_count, FLT_MAX, options,
      error));

  if (lod.size() > target_count && config.simplify_fallback_permissive &&
      !config.simplify_permissive) {
    lod.resize(meshopt_simplifyWithAttributes(
        &lod[0], &indices[0], indices.size(), mesh.vertex_positions, mesh.vertex_count,
        mesh.vertex_positions_stride, mesh.vertex_attributes, mesh.vertex_attributes_stride,
        mesh.attribute_weights, mesh.attribute_count, &locks[0], target_count, FLT_MAX,
        options | meshopt_SimplifyPermissive, error));
  }
}

// ---- loose islands (geometry.md, "Loose islands: removed whole, the rest grown") ---------------
//
// An **island** of a group is a connected part of its triangles: triangles joined through shared
// vertex positions (clusterlod's own position remap, so a seam does not split one). It is **free**
// when none of its vertices is locked, which is to say nothing outside the group shares a position
// with it and no caller pinned it: the whole of a leaf card, a stone of a pile, a rivet. Collapsing
// edges cannot coarsen a group of free islands (a card has no interior to collapse into), only
// removing islands can, and that is a different operation with a different error. Every decision
// here is made in double from the float positions with no library call but a correctly rounded
// square root (geometry.md, "The same bytes from every toolchain").
struct Island {
  u32 first = 0;  // its first triangle in the group's order: every sort's tie-break
  u32 triangles = 0;
  bool free = true;
  bool alive = false;  // scratch: a result still draws a triangle of it
  bool open = false;   // an edge of it belongs to one triangle only: a card, a tube without caps
  f64 area = 0.0;
  f64 centre[3] = {0.0, 0.0, 0.0};  // area-weighted; the mean of its corners when it has no area
  f64 radius = 0.0;                 // the farthest corner from `centre`
  // Its own length: the unit vector from `centre` to that farthest corner (zero for a point), and
  // the farthest a corner lies from the line through `centre` along it, which is half the
  // island's width across its length.
  f64 axis[3] = {0.0, 0.0, 0.0};
  f64 half_width = 0.0;
};

struct IslandSet {
  std::vector<unsigned int> verts;      // the group's vertices by position, sorted and distinct
  std::vector<unsigned int> island_of;  // per entry of `verts`
  std::vector<unsigned int> of_triangle;
  std::vector<Island> islands;
  u32 free_count = 0;
  f64 free_area = 0.0;

  unsigned int island_at(unsigned int canonical) const noexcept {
    const auto it = std::lower_bound(verts.begin(), verts.end(), canonical);
    if (it == verts.end() || *it != canonical) return ~0u;
    return island_of[static_cast<size_t>(it - verts.begin())];
  }
};

f64 triangle_area(const clodMesh& mesh, const unsigned int* tri) noexcept {
  const Vec3 a = position_of(mesh, tri[0]);
  const Vec3 b = position_of(mesh, tri[1]);
  const Vec3 c = position_of(mesh, tri[2]);
  const f64 ex = static_cast<f64>(b.x) - static_cast<f64>(a.x);
  const f64 ey = static_cast<f64>(b.y) - static_cast<f64>(a.y);
  const f64 ez = static_cast<f64>(b.z) - static_cast<f64>(a.z);
  const f64 fx = static_cast<f64>(c.x) - static_cast<f64>(a.x);
  const f64 fy = static_cast<f64>(c.y) - static_cast<f64>(a.y);
  const f64 fz = static_cast<f64>(c.z) - static_cast<f64>(a.z);
  const f64 nx = ey * fz - ez * fy;
  const f64 ny = ez * fx - ex * fz;
  const f64 nz = ex * fy - ey * fx;
  return 0.5 * std::sqrt(nx * nx + ny * ny + nz * nz);
}

void find_islands(const clodMesh& mesh, const std::vector<unsigned int>& indices,
                  const std::vector<unsigned char>& locks, const std::vector<unsigned int>& remap,
                  IslandSet& set) {
  set = IslandSet{};
  set.verts.reserve(indices.size());
  for (const unsigned int v : indices)
    set.verts.push_back(remap[v]);
  std::sort(set.verts.begin(), set.verts.end());
  set.verts.erase(std::unique(set.verts.begin(), set.verts.end()), set.verts.end());
  const auto local = [&](unsigned int v) {
    return static_cast<unsigned int>(
        std::lower_bound(set.verts.begin(), set.verts.end(), remap[v]) - set.verts.begin());
  };
  std::vector<unsigned int> parent(set.verts.size());
  for (size_t i = 0; i < parent.size(); ++i)
    parent[i] = static_cast<unsigned int>(i);
  const auto find = [&](unsigned int x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  };
  const size_t triangles = indices.size() / 3;
  for (size_t t = 0; t < triangles; ++t) {
    const unsigned int a = find(local(indices[t * 3]));
    const unsigned int b = find(local(indices[t * 3 + 1]));
    // The smaller root wins, so the forest is a function of the triangles and not of their order.
    if (a != b) parent[std::max(a, b)] = std::min(a, b);
    const unsigned int ab = find(local(indices[t * 3]));
    const unsigned int c = find(local(indices[t * 3 + 2]));
    if (ab != c) parent[std::max(ab, c)] = std::min(ab, c);
  }
  // Islands are numbered by their first triangle.
  std::vector<unsigned int> of_root(set.verts.size(), ~0u);
  set.of_triangle.resize(triangles);
  std::vector<f64> plain(0);
  for (size_t t = 0; t < triangles; ++t) {
    const unsigned int root = find(local(indices[t * 3]));
    if (of_root[root] == ~0u) {
      of_root[root] = static_cast<unsigned int>(set.islands.size());
      Island island;
      island.first = static_cast<u32>(t);
      set.islands.push_back(island);
      plain.insert(plain.end(), {0.0, 0.0, 0.0});
    }
    const unsigned int id = of_root[root];
    set.of_triangle[t] = id;
    Island& island = set.islands[id];
    ++island.triangles;
    const f64 area = triangle_area(mesh, &indices[t * 3]);
    island.area += area;
    for (size_t k = 0; k < 3; ++k) {
      const unsigned int v = indices[t * 3 + k];
      if ((locks[v] & meshopt_SimplifyVertex_Lock) != 0) island.free = false;
      const Vec3 p = position_of(mesh, v);
      const f64 q[3] = {static_cast<f64>(p.x), static_cast<f64>(p.y), static_cast<f64>(p.z)};
      for (size_t axis = 0; axis < 3; ++axis) {
        island.centre[axis] += area * q[axis];
        plain[id * 3 + axis] += q[axis];
      }
    }
  }
  set.island_of.resize(set.verts.size());
  for (size_t i = 0; i < set.verts.size(); ++i)
    set.island_of[i] = of_root[find(static_cast<unsigned int>(i))];
  for (size_t id = 0; id < set.islands.size(); ++id) {
    Island& island = set.islands[id];
    for (size_t axis = 0; axis < 3; ++axis) {
      island.centre[axis] = island.area > 0.0
                                ? island.centre[axis] / (3.0 * island.area)
                                : plain[id * 3 + axis] / (3.0 * static_cast<f64>(island.triangles));
    }
    if (island.free) {
      ++set.free_count;
      set.free_area += island.area;
    }
  }
  std::vector<f64> axis(set.islands.size() * 3, 0.0);
  for (size_t t = 0; t < triangles; ++t) {
    Island& island = set.islands[set.of_triangle[t]];
    for (size_t k = 0; k < 3; ++k) {
      const Vec3 p = position_of(mesh, indices[t * 3 + k]);
      const f64 dx = static_cast<f64>(p.x) - island.centre[0];
      const f64 dy = static_cast<f64>(p.y) - island.centre[1];
      const f64 dz = static_cast<f64>(p.z) - island.centre[2];
      const f64 r = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (r > island.radius) {
        island.radius = r;
        // The island's own length runs through its farthest corner: the first to reach the radius,
        // in the group's triangle order.
        axis[set.of_triangle[t] * 3] = dx;
        axis[set.of_triangle[t] * 3 + 1] = dy;
        axis[set.of_triangle[t] * 3 + 2] = dz;
      }
    }
  }
  for (size_t id = 0; id < set.islands.size(); ++id) {
    Island& island = set.islands[id];
    if (!(island.radius > 0.0)) continue;
    for (size_t k = 0; k < 3; ++k)
      island.axis[k] = axis[id * 3 + k] / island.radius;
  }
  for (size_t t = 0; t < triangles; ++t) {
    const unsigned int id = set.of_triangle[t];
    Island& island = set.islands[id];
    if (!(island.radius > 0.0)) continue;
    for (size_t k = 0; k < 3; ++k) {
      const Vec3 p = position_of(mesh, indices[t * 3 + k]);
      const f64 dx = static_cast<f64>(p.x) - island.centre[0];
      const f64 dy = static_cast<f64>(p.y) - island.centre[1];
      const f64 dz = static_cast<f64>(p.z) - island.centre[2];
      const f64 along = dx * island.axis[0] + dy * island.axis[1] + dz * island.axis[2];
      const f64 across_sq = dx * dx + dy * dy + dz * dz - along * along;
      const f64 across = across_sq > 0.0 ? std::sqrt(across_sq) : 0.0;
      if (across > island.half_width) island.half_width = across;
    }
  }
  // An island is open when an edge of it belongs to one triangle only.
  std::vector<u64> edges;
  edges.reserve(indices.size());
  for (size_t t = 0; t < triangles; ++t) {
    for (size_t k = 0; k < 3; ++k) {
      const unsigned int a = remap[indices[t * 3 + k]];
      const unsigned int b = remap[indices[t * 3 + (k + 1) % 3]];
      edges.push_back((u64{std::min(a, b)} << 32) | u64{std::max(a, b)});
    }
  }
  std::sort(edges.begin(), edges.end());
  for (size_t i = 0; i < edges.size();) {
    size_t j = i + 1;
    while (j < edges.size() && edges[j] == edges[i])
      ++j;
    if (j - i == 1) {
      const unsigned int id = set.island_at(static_cast<unsigned int>(edges[i] >> 32));
      if (id != ~0u) set.islands[id].open = true;
    }
    i = j;
  }
}

// **What the eye loses when an island goes** (geometry.md has the derivation): the most of a
// screen it can cover, as a length. Seen from any direction an island covers no more than
//
//  - its own area, and half of it when it is closed (of a closed surface at most half the area
//    faces any one way, and what faces away is behind what faces the eye), and
//  - its length times its width: it lies inside the cylinder about the line through its centre
//    and its farthest corner, `2 radius` long and `2 half_width` across, whose shadow that is to
//    within the caps.
//
// The smaller of the two is its covered area, and the side of the square of that area is the
// error a cut admits to when it no longer draws the island: under one pixel of it the island
// held less than a pixel of the picture, however it was spread. For a compact island that is its
// width, near enough (a square card: its side, where its extent is its diagonal); for a twig 30 cm
// long and 6 mm thick it is 4 cm and not 30. Rounded up into float.
f64 island_covered_area(const Island& island) noexcept {
  const f64 faces = island.open ? island.area : 0.5 * island.area;
  const f64 slab = 4.0 * island.radius * island.half_width;
  return faces < slab ? faces : slab;
}

float island_covered_size(const Island& island) noexcept {
  const f64 size = std::sqrt(island_covered_area(island));
  float e = static_cast<float>(size);
  while (static_cast<f64>(e) < size)
    e = std::nextafter(e, FLT_MAX);
  return e;
}

// Marks the islands a result still draws, and returns the area the free ones keep in it. A
// result's triangles name the group's own vertices (both simplifiers keep source vertices), so a
// corner's island is its position's.
f64 mark_alive(const clodMesh& mesh, const std::vector<unsigned int>& remap,
               const std::vector<unsigned int>& result, IslandSet& set) {
  for (Island& island : set.islands)
    island.alive = false;
  f64 free_area = 0.0;
  for (size_t i = 0; i + 2 < result.size(); i += 3) {
    const f64 area = triangle_area(mesh, &result[i]);
    bool counted = false;
    for (size_t k = 0; k < 3; ++k) {
      const unsigned int id = set.island_at(remap[result[i + k]]);
      if (id == ~0u) continue;
      Island& island = set.islands[id];
      island.alive = true;
      if (island.free && !counted) {
        free_area += area;
        counted = true;
      }
    }
  }
  return free_area;
}

// The vertices a build adds to its source's: the corners of the islands a level grew. Empty until
// the first island grows, so a mesh that never grows one is built from the caller's own arrays.
struct VertexPool {
  std::vector<float> positions;            // three a vertex
  std::vector<float> attributes;           // the simplifier's record, when it has one
  std::vector<unsigned char> vertex_lock;  // the caller's tags, when it has any
  std::vector<unsigned int> origin;        // the source vertex each vertex stands for
};

// A free island's share of the group's covered area that a regular result may lose before the
// group is coarsened by islands instead: collapsing a connected surface keeps its area to a few
// per cent, and halving a card's triangles halves the card.
constexpr f64 k_island_area_kept = 0.75;
// The largest an island may be, against its group's radius, to count as a part of an aggregate
// whose lost area is given back to it (`restore_island_area`).
constexpr f64 k_island_part_of_group = 0.25;
// Or the fewest free islands a group holds for every one of them to count as a part of an
// aggregate, whatever its size.
constexpr u32 k_aggregate_islands = 8;
// The share of its area under which a regular result has gutted an island: what is left is
// removed rather than grown back (`restore_island_area`).
constexpr f64 k_island_gutted = 0.5;

u32 morton10(u32 x, u32 y, u32 z) noexcept {
  const auto spread = [](u32 v) {
    v &= 0x3ffu;
    v = (v | (v << 16)) & 0x030000ffu;
    v = (v | (v << 8)) & 0x0300f00fu;
    v = (v | (v << 4)) & 0x030c30c3u;
    v = (v | (v << 2)) & 0x09249249u;
    return v;
  };
  return spread(x) | (spread(y) << 1) | (spread(z) << 2);
}

float rounded_up(f64 value) noexcept {
  float f = static_cast<float>(value);
  while (static_cast<f64>(f) < value)
    f = std::nextafter(f, FLT_MAX);
  return f;
}

// How one island grows about `centre`: by `scale` along `axis` and by `across` across it (both 1:
// not at all; equal: about the centre in every direction, and `axis` is not read).
struct Growth {
  f64 scale = 1.0;
  f64 across = 1.0;
  f64 centre[3] = {0.0, 0.0, 0.0};
  f64 axis[3] = {0.0, 0.0, 0.0};
};

// **The growth that gives an island `ratio` times its area and moves its corners least**
// (geometry.md, "A long island is thickened"). Grown about its centre in every direction, by the
// square root of the ratio, a corner moves up to (sqrt(ratio) - 1) of the island's `radius`;
// grown across its own length alone, by the ratio, up to (ratio - 1) of its `half_width`. A leaf
// takes the first, and a twig, a needle or a blade the second: it is thickened and not
// lengthened, which is what the published practice does with hair and ribbons. Returns how far a
// corner may have moved.
f64 grow_least(f64 ratio, f64 radius, f64 half_width, const f64 axis[3], Growth& growth) noexcept {
  const f64 every = std::sqrt(ratio);
  const f64 moved_every = (every - 1.0) * radius;
  const f64 moved_across = (ratio - 1.0) * half_width;
  if (moved_across < moved_every) {
    growth.scale = 1.0;
    growth.across = ratio;
    for (size_t k = 0; k < 3; ++k)
      growth.axis[k] = axis[k];
    return moved_across;
  }
  growth.scale = every;
  growth.across = every;
  return moved_every;
}

// Grows the islands of `triangles` whose scale is over 1: each of their corners becomes a **new
// vertex** behind the mesh's, at its position scaled about its island's centre, and `triangles`
// names the copies. A copy stands for the corner it was made from (`pool.origin` names the source
// vertex at the root of the chain), whose attribute record and tags it keeps, and two corners of
// one island at one position are one position still (`remap`). The pool takes the source's arrays
// over the first time anything grows. Returns the vertices added. `island_of` is per triangle,
// ~0u for one that belongs to no island.
u32 grow_islands(clodMesh& mesh, VertexPool& pool, std::vector<unsigned int>& triangles,
                 const std::vector<u32>& island_of, const std::vector<Growth>& growth,
                 std::vector<unsigned char>& locks, std::vector<unsigned int>& remap) {
  // The grown islands' corners, each once, in (island, vertex) order: the new vertices' order.
  struct Corner {
    u32 island;
    unsigned int vertex;
  };
  const auto corner_less = [](const Corner& a, const Corner& b) {
    return a.island != b.island ? a.island < b.island : a.vertex < b.vertex;
  };
  const auto grows = [&](u32 id) {
    return id != ~0u && (growth[id].scale > 1.0 || growth[id].across > 1.0);
  };
  std::vector<Corner> corners;
  for (size_t t = 0; t < island_of.size(); ++t) {
    if (!grows(island_of[t])) continue;
    for (size_t k = 0; k < 3; ++k)
      corners.push_back(Corner{island_of[t], triangles[t * 3 + k]});
  }
  if (corners.empty()) return 0;
  std::sort(corners.begin(), corners.end(), corner_less);
  corners.erase(std::unique(corners.begin(), corners.end(),
                            [](const Corner& a, const Corner& b) {
                              return a.island == b.island && a.vertex == b.vertex;
                            }),
                corners.end());

  const size_t stride = mesh.vertex_positions_stride / sizeof(float);
  const size_t attribute_stride = mesh.vertex_attributes_stride / sizeof(float);
  const size_t base = mesh.vertex_count;
  if (pool.origin.empty()) {
    pool.positions.assign(mesh.vertex_positions, mesh.vertex_positions + base * stride);
    if (mesh.vertex_attributes != nullptr)
      pool.attributes.assign(mesh.vertex_attributes,
                             mesh.vertex_attributes + base * attribute_stride);
    if (mesh.vertex_lock != nullptr)
      pool.vertex_lock.assign(mesh.vertex_lock, mesh.vertex_lock + base);
    pool.origin.resize(base);
    for (size_t v = 0; v < base; ++v)
      pool.origin[v] = static_cast<unsigned int>(v);
  }
  for (size_t i = 0; i < corners.size(); ++i) {
    const Corner& corner = corners[i];
    const Growth& g = growth[corner.island];
    float grown[3];
    if (g.across == g.scale) {
      for (size_t axis = 0; axis < 3; ++axis) {
        const f64 p = static_cast<f64>(pool.positions[corner.vertex * stride + axis]);
        grown[axis] = static_cast<float>(g.centre[axis] + g.scale * (p - g.centre[axis]));
      }
    } else {
      f64 d[3];
      for (size_t axis = 0; axis < 3; ++axis)
        d[axis] = static_cast<f64>(pool.positions[corner.vertex * stride + axis]) - g.centre[axis];
      const f64 along = d[0] * g.axis[0] + d[1] * g.axis[1] + d[2] * g.axis[2];
      for (size_t axis = 0; axis < 3; ++axis) {
        const f64 on_axis = along * g.axis[axis];
        grown[axis] =
            static_cast<float>(g.centre[axis] + g.scale * on_axis + g.across * (d[axis] - on_axis));
      }
    }
    // The position's class among the copies: the island's first corner at the same position.
    unsigned int canonical = static_cast<unsigned int>(base + i);
    for (size_t j = i; j > 0 && corners[j - 1].island == corner.island; --j) {
      if (remap[corners[j - 1].vertex] == remap[corner.vertex])
        canonical = static_cast<unsigned int>(base + j - 1);
    }
    pool.positions.insert(pool.positions.end(), grown, grown + 3);
    for (size_t k = 3; k < stride; ++k)
      pool.positions.push_back(pool.positions[corner.vertex * stride + k]);
    for (size_t k = 0; k < attribute_stride && mesh.vertex_attributes != nullptr; ++k)
      pool.attributes.push_back(pool.attributes[corner.vertex * attribute_stride + k]);
    if (mesh.vertex_lock != nullptr) pool.vertex_lock.push_back(pool.vertex_lock[corner.vertex]);
    pool.origin.push_back(pool.origin[corner.vertex]);
    remap.push_back(canonical);
    locks.push_back(
        static_cast<unsigned char>(locks[corner.vertex] & meshopt_SimplifyVertex_Protect));
  }
  mesh.vertex_positions = pool.positions.data();
  mesh.vertex_count = base + corners.size();
  if (mesh.vertex_attributes != nullptr) mesh.vertex_attributes = pool.attributes.data();
  if (mesh.vertex_lock != nullptr) mesh.vertex_lock = pool.vertex_lock.data();

  for (size_t t = 0; t < island_of.size(); ++t) {
    if (!grows(island_of[t])) continue;
    for (size_t k = 0; k < 3; ++k) {
      const auto it = std::lower_bound(corners.begin(), corners.end(),
                                       Corner{island_of[t], triangles[t * 3 + k]}, corner_less);
      triangles[t * 3 + k] =
          static_cast<unsigned int>(base + static_cast<size_t>(it - corners.begin()));
    }
  }
  return static_cast<u32>(corners.size());
}

// **Stochastic simplification of an aggregate** (Cook et al. 2007; geometry.md has the sources):
// the group's free islands are paired along a space-filling curve, the smaller of each pair is
// removed whole and the other grown until it covers the area of both (`grow_least`), so the
// group's covered area holds while its triangles halve. The grown island's corners are new
// vertices behind the source's, each standing for the corner it was copied from (`pool.origin`),
// whose attributes, tags and position class it keeps. The level's error is the covered size of
// the largest island removed (`island_covered_area`) or, if larger, the farthest a grown corner
// moved: what is no longer drawn where it was. `cover` is the same with each removed island
// counted for the part of its covered size the part's mask keeps (`mask_root`). Returns the
// group's triangles with the removed islands' dropped and the grown islands' renamed; islands
// that are not free pass through as they are.
std::vector<unsigned int> coarsen_by_islands(clodMesh& mesh, VertexPool& pool,
                                             const std::vector<unsigned int>& indices,
                                             std::vector<unsigned char>& locks,
                                             std::vector<unsigned int>& remap, const IslandSet& set,
                                             float stuck_threshold, f64 mask_root, float* error,
                                             float* cover, AggregateStats& stats) {
  struct Ordered {
    u32 code;
    u32 first;
    u32 island;
  };
  f64 lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
  for (const Island& island : set.islands) {
    if (!island.free) continue;
    for (size_t axis = 0; axis < 3; ++axis) {
      lo[axis] = std::min(lo[axis], island.centre[axis]);
      hi[axis] = std::max(hi[axis], island.centre[axis]);
    }
  }
  std::vector<Ordered> order;
  for (size_t id = 0; id < set.islands.size(); ++id) {
    const Island& island = set.islands[id];
    if (!island.free) continue;
    u32 q[3];
    for (size_t axis = 0; axis < 3; ++axis) {
      const f64 span = hi[axis] - lo[axis];
      const f64 t = span > 0.0 ? (island.centre[axis] - lo[axis]) / span * 1023.0 : 0.0;
      q[axis] = static_cast<u32>(t < 0.0 ? 0.0 : (t > 1023.0 ? 1023.0 : t));
    }
    order.push_back(Ordered{morton10(q[0], q[1], q[2]), island.first, static_cast<u32>(id)});
  }
  std::sort(order.begin(), order.end(), [](const Ordered& a, const Ordered& b) {
    return a.code != b.code ? a.code < b.code : a.first < b.first;
  });

  enum : unsigned char { keep = 0, removed = 1, grown = 2 };
  std::vector<unsigned char> action(set.islands.size(), keep);
  std::vector<Growth> growth(set.islands.size());
  for (size_t id = 0; id < set.islands.size(); ++id) {
    for (size_t axis = 0; axis < 3; ++axis)
      growth[id].centre[axis] = set.islands[id].centre[axis];
  }
  // `worst` is the level's geometric error and `worst_cover` its coverage error: the same, with
  // each removed island counted for the part of its covered size the part's mask keeps.
  f64 worst = 0.0, worst_cover = 0.0;
  u32 removed_count = 0, grown_count = 0;
  for (size_t i = 0; i + 1 < order.size(); i += 2) {
    const u32 a = order[i].island, b = order[i + 1].island;
    const u32 kept = set.islands[a].area >= set.islands[b].area ? a : b;
    const u32 gone = kept == a ? b : a;
    action[gone] = removed;
    ++removed_count;
    const f64 lost = std::sqrt(island_covered_area(set.islands[gone]));
    worst = std::max(worst, lost);
    worst_cover = std::max(worst_cover, mask_root * lost);
    const f64 area = set.islands[kept].area;
    if (area > 0.0 && set.islands[gone].area > 0.0) {
      const Island& island = set.islands[kept];
      action[kept] = grown;
      ++grown_count;
      const f64 moved = grow_least((area + set.islands[gone].area) / area, island.radius,
                                   island.half_width, island.axis, growth[kept]);
      worst = std::max(worst, moved);
      worst_cover = std::max(worst_cover, moved);
    }
  }
  // Too little removed to count as a level (a few large islands beside many triangles that are
  // not free): nothing is changed, and the caller goes on as upstream does.
  size_t kept_triangles = 0;
  for (size_t t = 0; t < set.of_triangle.size(); ++t)
    kept_triangles += action[set.of_triangle[t]] != removed ? 1u : 0u;
  if (removed_count == 0 ||
      static_cast<float>(kept_triangles * 3) > static_cast<float>(indices.size()) * stuck_threshold)
    return {};

  std::vector<unsigned int> result;
  std::vector<u32> island_of;
  result.reserve(indices.size());
  for (size_t t = 0; t < set.of_triangle.size(); ++t) {
    const u32 id = set.of_triangle[t];
    if (action[id] == removed) continue;
    result.insert(result.end(), &indices[t * 3], &indices[t * 3] + 3);
    island_of.push_back(id);
  }
  stats.islands_removed += removed_count;
  stats.islands_grown += grown_count;
  stats.vertices_added += grow_islands(mesh, pool, result, island_of, growth, locks, remap);
  *error = rounded_up(worst);
  *cover = rounded_up(worst_cover);
  return result;
}

// **Area kept through a regular level** (the documentation Unreal Engine gives for Nanite's
// "Preserve Area": the area simplification loses is given "to the remaining triangles by dilating
// out the open boundary edges"). Collapsing an island that has an open border can only cut its
// outline's corners, so every level leaves a leaf a little smaller, and a level may collapse a
// small one away; over a DAG's levels a crown thins by half without any one level losing a
// quarter. So after a regular result is kept, every free island with an open border is grown
// about its own centre back to the area it had, plus its share (by area) of what the group's
// islands of that kind lost whole. The result's error takes the farthest a corner moved.
// Returns the covered size of the largest island it removed for being gutted (0: none), which the
// caller's error admits to.
f64 restore_island_area(clodMesh& mesh, VertexPool& pool, const std::vector<unsigned int>& indices,
                        std::vector<unsigned int>& result, std::vector<unsigned char>& locks,
                        std::vector<unsigned int>& remap, const IslandSet& set, float* error,
                        AggregateStats& stats) {
  const size_t count = set.islands.size();
  // An island has an open border when an edge of it belongs to one triangle only.
  std::vector<u64> edges;
  edges.reserve(indices.size());
  for (size_t t = 0; t < set.of_triangle.size(); ++t) {
    for (size_t k = 0; k < 3; ++k) {
      const unsigned int a = remap[indices[t * 3 + k]];
      const unsigned int b = remap[indices[t * 3 + (k + 1) % 3]];
      edges.push_back((u64{std::min(a, b)} << 32) | u64{std::max(a, b)});
    }
  }
  std::sort(edges.begin(), edges.end());
  std::vector<unsigned char> open(count, 0);
  for (size_t i = 0; i < edges.size();) {
    size_t j = i + 1;
    while (j < edges.size() && edges[j] == edges[i])
      ++j;
    if (j - i == 1) {
      const unsigned int id = set.island_at(static_cast<unsigned int>(edges[i] >> 32));
      if (id != ~0u) open[id] = 1;
    }
    i = j;
  }

  // What the result kept of each island: its area, and the sphere of what is left.
  std::vector<u32> island_of(result.size() / 3, ~0u);
  std::vector<f64> area(count, 0.0);
  std::vector<Growth> growth(count);
  std::vector<f64> radius(count, 0.0);
  std::vector<f64> across(count, 0.0);  // what is left's half width across the island's length
  for (size_t t = 0; t < island_of.size(); ++t) {
    const unsigned int id = set.island_at(remap[result[t * 3]]);
    if (id == ~0u) continue;
    island_of[t] = id;
    const f64 a = triangle_area(mesh, &result[t * 3]);
    area[id] += a;
    for (size_t k = 0; k < 3; ++k) {
      const Vec3 p = position_of(mesh, result[t * 3 + k]);
      growth[id].centre[0] += a * static_cast<f64>(p.x);
      growth[id].centre[1] += a * static_cast<f64>(p.y);
      growth[id].centre[2] += a * static_cast<f64>(p.z);
    }
  }
  // Only a part of an aggregate grows: an island small against its group (a quarter of the
  // group's own radius at most), or any island of a group of many. A ground plane, a banner or a
  // wall with an open border is an island too, and scaling one about its centre would move
  // everything on it; a level's survivors are grown, and a crown's coarse groups hold a few dozen
  // cards a good part of the group across, which lose their area to every regular level as the
  // small ones did.
  f64 lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
  for (const unsigned int v : indices) {
    const Vec3 p = position_of(mesh, v);
    const f64 q[3] = {static_cast<f64>(p.x), static_cast<f64>(p.y), static_cast<f64>(p.z)};
    for (size_t axis = 0; axis < 3; ++axis) {
      lo[axis] = std::min(lo[axis], q[axis]);
      hi[axis] = std::max(hi[axis], q[axis]);
    }
  }
  const f64 group_radius =
      0.5 * std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) +
                      (hi[2] - lo[2]) * (hi[2] - lo[2]));
  const bool aggregate = set.free_count >= k_aggregate_islands;
  const auto part = [&](size_t id) {
    const Island& island = set.islands[id];
    return island.free && open[id] != 0 &&
           (aggregate || island.radius <= k_island_part_of_group * group_radius);
  };
  // **An island the result gutted is removed whole** (`k_island_gutted`): what is left of it is a
  // sliver, and growing a sliver back to the island's area throws its corners across the group (a
  // twig collapsed to one triangle and grown seven times over moved 70 cm in a tree 3 m tall). Its
  // triangles are dropped, its area goes to the islands that kept theirs, and the caller's error
  // admits to its covered size, as for any island removed whole.
  f64 lost = 0.0, kept_before = 0.0, worst_gone = 0.0;
  std::vector<unsigned char> gutted(count, 0);
  bool any_gutted = false;
  for (size_t id = 0; id < count; ++id) {
    const Island& island = set.islands[id];
    if (!island.free || open[id] == 0) continue;
    if (area[id] > 0.0 && part(id) && area[id] < k_island_gutted * island.area) {
      gutted[id] = 1;
      any_gutted = true;
      worst_gone = std::max(worst_gone, std::sqrt(island_covered_area(island)));
    }
    if (area[id] > 0.0 && gutted[id] == 0) {
      kept_before += island.area;
      for (size_t axis = 0; axis < 3; ++axis)
        growth[id].centre[axis] /= 3.0 * area[id];
    } else {
      lost += island.area;
    }
  }
  if (any_gutted) {
    size_t kept = 0;
    for (size_t t = 0; t < island_of.size(); ++t) {
      const u32 id = island_of[t];
      if (id != ~0u && gutted[id] != 0) continue;
      for (size_t k = 0; k < 3; ++k)
        result[kept * 3 + k] = result[t * 3 + k];
      island_of[kept++] = id;
    }
    result.resize(kept * 3);
    island_of.resize(kept);
    for (size_t id = 0; id < count; ++id)
      if (gutted[id] != 0) area[id] = 0.0;
  }
  for (size_t t = 0; t < island_of.size(); ++t) {
    const u32 id = island_of[t];
    if (id == ~0u || !(area[id] > 0.0)) continue;
    for (size_t k = 0; k < 3; ++k) {
      const Vec3 p = position_of(mesh, result[t * 3 + k]);
      const f64 dx = static_cast<f64>(p.x) - growth[id].centre[0];
      const f64 dy = static_cast<f64>(p.y) - growth[id].centre[1];
      const f64 dz = static_cast<f64>(p.z) - growth[id].centre[2];
      radius[id] = std::max(radius[id], std::sqrt(dx * dx + dy * dy + dz * dz));
      const f64* axis = set.islands[id].axis;
      const f64 along = dx * axis[0] + dy * axis[1] + dz * axis[2];
      const f64 across_sq = dx * dx + dy * dy + dz * dz - along * along;
      if (across_sq > across[id] * across[id]) across[id] = std::sqrt(across_sq);
    }
  }
  f64 worst = 0.0;
  u32 restored = 0;
  for (size_t id = 0; id < count; ++id) {
    const Island& island = set.islands[id];
    if (!part(id) || !(area[id] > 0.0)) continue;
    const f64 share = kept_before > 0.0 ? lost * island.area / kept_before : 0.0;
    const f64 ratio = (island.area + share) / area[id];
    // Under a part in a thousand (of its width) is the simplifier's own rounding, not a loss.
    if (!(ratio > 1.002)) continue;
    ++restored;
    worst = std::max(worst, grow_least(ratio, radius[id], across[id], island.axis, growth[id]));
  }
  if (restored != 0) {
    stats.islands_restored += restored;
    stats.vertices_added += grow_islands(mesh, pool, result, island_of, growth, locks, remap);
    const float moved = rounded_up(worst);
    if (moved > *error) *error = moved;
  }
  return worst_gone;
}

// clusterlod's optional cap on a group's error by its longest edge (off in the default config),
// as upstream's `simplify` applies it.
void limit_error_by_edges(const clodConfig& config, const clodMesh& mesh,
                          const std::vector<unsigned int>& indices, float* error) {
  if (!(config.simplify_error_edge_limit > 0)) return;
  float max_edge_sq = 0;
  for (size_t i = 0; i < indices.size(); i += 3) {
    const Vec3 va = position_of(mesh, indices[i + 0]);
    const Vec3 vb = position_of(mesh, indices[i + 1]);
    const Vec3 vc = position_of(mesh, indices[i + 2]);
    const float eab = length_squared(va - vb);
    const float eac = length_squared(va - vc);
    const float ebc = length_squared(vb - vc);
    const float emax = std::max(std::max(eab, eac), ebc);
    const float emin = std::min(std::min(eab, eac), ebc);
    max_edge_sq = std::max(max_edge_sq, std::max(emin, emax / 4));
  }
  *error = std::min(*error, std::sqrt(max_edge_sq) * config.simplify_error_edge_limit);
}

// How many times a group's regular simplification is retried with more of its vertices locked
// before the group is left unsimplified.
constexpr u32 k_fold_rounds = 8;

// clusterlod's `simplify` (demo/clusterlod.h) held to `up`. Upstream runs the regular simplifier
// and, when that gets stuck short of the target, meshoptimizer's sloppy one; this does the same,
// and refuses either result when it turns a triangle of a group that faced `up` everywhere away
// from it:
//
//  - **The regular result** that turns one over is simplified again with the corners of every
//    folded triangle locked, and every vertex of the group's triangles that touch them, so no
//    collapse can move into or out of the fold's corners; again, with the new folds' corners
//    added, up to `k_fold_rounds` times. A group that still folds is returned unsimplified, and
//    the caller ends its chain there (its clusters become terminal), which is coarse geometry
//    kept finer, never a fold kept.
//  - **The sloppy result** that turns one over is dropped for the regular one, short of the
//    target as it is: the caller keeps it when it is under the stuck threshold and ends the
//    group's chain otherwise.
//
// A group neither step refuses comes out exactly as upstream makes it, so a heightfield that never
// folds builds the same bytes with `up` set and without; the guard's test holds that.
//
// Why both: meshoptimizer's edge collapse refuses a collapse that turns a triangle by more than
// about 75 degrees **relative to the triangle as it is**, so two collapses of 60 degrees each turn
// a sloped triangle past square, and on a heightfield's coarse levels they do: every fold the
// census of the erg's grid found was made by the regular simplifier (docs/experiments/
// shadow-shards-2026-10-09.md, "2026-10-11: where the folds are made"). The sloppy simplifier has
// no facing test at all.
//
// `locks` is the build's own array: the locks added here touch only this group's vertices (a
// vertex two groups share is on their border and locked already), and `lockBoundary` clears them
// when the next level starts.
std::vector<unsigned int> simplify_heightfield(const clodConfig& config, const clodMesh& mesh,
                                               const std::vector<unsigned int>& indices,
                                               std::vector<unsigned char>& locks,
                                               size_t target_count, float* error,
                                               const FoldGuard& guard, FoldGuardStats& stats) {
  if (target_count > indices.size()) return indices;

  std::vector<unsigned int> lod;
  simplify_regular(config, mesh, indices, locks, target_count, error, lod);

  ++stats.groups;
  // A group that already faces away somewhere is not a piece of a heightfield, and nothing here
  // can say what folding means for it: it is built as upstream builds it.
  const bool held = count_folds(mesh, indices, guard) == 0;
  bool folded = held && count_folds(mesh, lod, guard) != 0;
  if (folded) {
    ++stats.regular_folded;
    if (guard.refuse) {
      ++stats.refused;
      std::vector<unsigned int> corners;
      for (u32 round = 0; folded && round < k_fold_rounds; ++round) {
        for (size_t i = 0; i + 2 < lod.size(); i += 3) {
          if (folds(guard, mesh, &lod[i])) {
            corners.push_back(lod[i]);
            corners.push_back(lod[i + 1]);
            corners.push_back(lod[i + 2]);
          }
        }
        std::sort(corners.begin(), corners.end());
        corners.erase(std::unique(corners.begin(), corners.end()), corners.end());
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
          const bool touches = std::binary_search(corners.begin(), corners.end(), indices[i]) ||
                               std::binary_search(corners.begin(), corners.end(), indices[i + 1]) ||
                               std::binary_search(corners.begin(), corners.end(), indices[i + 2]);
          if (!touches) continue;
          for (size_t k = 0; k < 3; ++k)
            locks[indices[i + k]] |= static_cast<unsigned char>(meshopt_SimplifyVertex_Lock);
        }
        ++stats.retries;
        simplify_regular(config, mesh, indices, locks, target_count, error, lod);
        folded = count_folds(mesh, lod, guard) != 0;
      }
      if (folded) {
        ++stats.unsimplified;
        return indices;
      }
    }
  }

  if (lod.size() > target_count && config.simplify_fallback_sloppy) {
    ++stats.sloppy;
    const float regular_error = *error;
    std::vector<unsigned int> sloppy;
    clod::simplifyFallback(sloppy, mesh, indices, locks, target_count, error);
    *error *= config.simplify_error_factor_sloppy;
    bool keep = true;
    if (held && count_folds(mesh, sloppy, guard) != 0) {
      ++stats.sloppy_folded;
      if (guard.refuse) {
        // The regular result, short of half as it is: the caller keeps it when it is under the
        // stuck threshold, and ends the group's chain where it is otherwise. Either way the level
        // above is coarser than this group's leaves and never folded.
        ++stats.sloppy_refused;
        *error = regular_error;
        keep = false;
      }
    }
    if (keep) lod = std::move(sloppy);
  }

  limit_error_by_edges(config, mesh, indices, error);
  return lod;
}

// What one build's simplifications of loose islands share: the switch, the vertices they add and
// what they did.
struct LooseBuild {
  bool enabled = true;
  // The square root of the share of the part its mask keeps (`ClusterLodOptions::mask_cover`); 1
  // for a part that is not alpha-tested, whose coverage errors nobody reads.
  f64 mask_root = 1.0;
  VertexPool pool;
  AggregateStats stats;
};

// clusterlod's `simplify` for every mesh that is not a heightfield, with two things upstream's
// does not have (geometry.md, "Loose islands: removed whole, the rest grown"):
//
//  - **A group of free islands is coarsened by islands** when the regular simplifier gets stuck
//    short of the target on it or thins it (its result keeps under `k_island_area_kept` of the free
//    islands' area: a card's two triangles collapsed to one), instead of by the sloppy simplifier
//    or by that result. `coarsen_by_islands` has what that does and the error it reports.
//  - **A result's error is at least the covered size of every island it removed whole**
//    (`island_covered_size`), whichever simplifier made it. The sloppy simplifier reports how far
//    its cells' kept vertices lie from the planes of the triangles it merged, which for a card
//    collapsed into one of its own corners is nothing at all; the regular one reports a collapse's
//    quadric error, which for a lone triangle is a fraction of its edge. Neither says that the
//    island is no longer drawn.
//
// `cover` is the level's **coverage error** for an alpha-tested part: the same error, with each
// island removed whole counted for the part of its covered size the mask keeps
// (`LooseBuild::mask_root`). The caller reads it only for such a part.
//
// With `LooseBuild::enabled` false, or on a group with no island of either kind, this is
// upstream's `simplify` line for line, so a mesh that is one connected surface per group builds
// the bytes `clodBuild` built.
std::vector<unsigned int> simplify_loose(const clodConfig& config, clodMesh& mesh,
                                         const std::vector<unsigned int>& indices,
                                         std::vector<unsigned char>& locks,
                                         std::vector<unsigned int>& remap, size_t target_count,
                                         float* error, float* cover, LooseBuild& build) {
  *cover = 0.0f;
  if (target_count > indices.size()) return indices;

  std::vector<unsigned int> lod;
  simplify_regular(config, mesh, indices, locks, target_count, error, lod);
  ++build.stats.groups;

  IslandSet set;
  if (build.enabled) {
    find_islands(mesh, indices, locks, remap, set);
    if (set.free_count >= 2) {
      const bool stuck = lod.size() > target_count;
      const bool thinned = mark_alive(mesh, remap, lod, set) < k_island_area_kept * set.free_area;
      if (stuck || thinned) {
        std::vector<unsigned int> coarse = coarsen_by_islands(
            mesh, build.pool, indices, locks, remap, set, config.simplify_threshold,
            build.mask_root, error, cover, build.stats);
        if (!coarse.empty()) {
          ++build.stats.by_islands;
          return coarse;
        }
      }
    }
  }

  const bool sloppy = lod.size() > target_count && config.simplify_fallback_sloppy;
  if (sloppy) {
    ++build.stats.sloppy;
    clod::simplifyFallback(lod, mesh, indices, locks, target_count, error);
    *error *= config.simplify_error_factor_sloppy;
  } else {
    ++build.stats.regular;
  }
  limit_error_by_edges(config, mesh, indices, error);
  *cover = *error;

  if (build.enabled) {
    mark_alive(mesh, remap, lod, set);
    float floor = 0.0f;
    for (const Island& island : set.islands) {
      if (!island.alive) floor = std::max(floor, island_covered_size(island));
    }
    if (floor > *error) {
      *error = floor;
      ++build.stats.errors_raised;
    }
    *cover = std::max(*cover, rounded_up(build.mask_root * static_cast<f64>(floor)));
    // A regular result's triangles each lie in one island; a sloppy one's may span two, and what
    // it merged has no area of its own to give back.
    if (!sloppy && set.free_count != 0) {
      const float before = *error;
      const f64 gone = restore_island_area(mesh, build.pool, indices, lod, locks, remap, set, error,
                                           build.stats);
      // What a corner moved to give the area back is a displacement on either scale; an island
      // removed for being gutted counts as any island removed whole.
      if (*error > before) *cover = std::max(*cover, *error);
      if (gone > 0.0) {
        *error = std::max(*error, rounded_up(gone));
        *cover = std::max(*cover, rounded_up(build.mask_root * gone));
      }
    }
  }
  return lod;
}

// clusterlod's `clodBuild` (demo/clusterlod.h, MIT, Arseny Kapoulkine), line for line, with its
// `simplify` replaced: by `simplify_heightfield` above for a mesh with `ClusterLodOptions::up` set
// (`guard` not null), and by `simplify_loose` for every other. `mesh` is the builder's own, because
// a level that grows an island adds vertices to it. When meshoptimizer is updated, this is the one
// function to compare with upstream's again; the determinism pins, and the guard's test that a
// heightfield which never folds builds the same bytes with `up` set and without, are what catch a
// drift.
//
// `group_cover` is where the loop leaves each group's **coverage error** before it is output
// (geometry.md, "Two errors"; read only for an alpha-tested part): merged up the DAG exactly as
// the geometric error is, from each level's own (`simplify_loose`), so it rises along every path
// as the geometric one does, and terminal for a group that is not refined.
size_t build_lod_groups(const clodConfig& config, clodMesh& mesh, const FoldGuard* guard,
                        FoldGuardStats& stats, LooseBuild& loose, f32* group_cover,
                        void* output_context, clodOutput output_callback) {
  using namespace clod;

  std::vector<unsigned char> locks(mesh.vertex_count);

  std::vector<unsigned int> remap(mesh.vertex_count);
  meshopt_generatePositionRemap(&remap[0], mesh.vertex_positions, mesh.vertex_count,
                                mesh.vertex_positions_stride);

  if (mesh.attribute_protect_mask) {
    size_t max_attributes = mesh.vertex_attributes_stride / sizeof(float);
    for (size_t i = 0; i < mesh.vertex_count; ++i) {
      unsigned int r = remap[i];
      for (size_t j = 0; j < max_attributes; ++j)
        if (r != i && (mesh.attribute_protect_mask & (1u << j)) &&
            mesh.vertex_attributes[i * max_attributes + j] !=
                mesh.vertex_attributes[r * max_attributes + j])
          locks[i] |= meshopt_SimplifyVertex_Protect;
    }
  }

  std::vector<Cluster> clusters = clusterize(config, mesh, mesh.indices, mesh.index_count);
  for (Cluster& cluster : clusters)
    cluster.bounds = boundsCompute(mesh, cluster.indices, 0.f);

  std::vector<int> pending(clusters.size());
  for (size_t i = 0; i < clusters.size(); ++i)
    pending[i] = int(i);
  // Each cluster's coverage error, as `Cluster::bounds.error` is its geometric one.
  std::vector<float> cluster_cover(clusters.size(), 0.0f);

  int depth = 0;
  while (pending.size() > 1) {
    std::vector<std::vector<int>> groups = partition(config, mesh, clusters, pending, remap);
    pending.clear();
    lockBoundary(locks, groups, clusters, remap, mesh.vertex_lock);

    for (size_t i = 0; i < groups.size(); ++i) {
      std::vector<unsigned int> merged;
      merged.reserve(groups[i].size() * config.max_triangles * 3);
      for (size_t j = 0; j < groups[i].size(); ++j)
        merged.insert(merged.end(), clusters[groups[i][j]].indices.begin(),
                      clusters[groups[i][j]].indices.end());

      size_t target_size = size_t((merged.size() / 3) * config.simplify_ratio) * 3;
      clodBounds bounds = mergeGroups(clusters, groups[i]);
      float below = 0.f;
      for (size_t j = 0; j < groups[i].size(); ++j)
        below = std::max(below, cluster_cover[static_cast<size_t>(groups[i][j])]);

      float error = 0.f;
      float cover = 0.f;
      std::vector<unsigned int> simplified =
          guard != nullptr ? simplify_heightfield(config, mesh, merged, locks, target_size, &error,
                                                  *guard, stats)
                           : simplify_loose(config, mesh, merged, locks, remap, target_size, &error,
                                            &cover, loose);
      if (simplified.size() > merged.size() * config.simplify_threshold) {
        bounds.error = FLT_MAX;
        *group_cover = k_lod_terminal_error;
        outputGroup(config, mesh, clusters, groups[i], bounds, depth, output_context,
                    output_callback);
        continue;
      }

      bounds.error = std::max(bounds.error * config.simplify_error_merge_previous, error) +
                     error * config.simplify_error_merge_additive;
      *group_cover = std::max(below * config.simplify_error_merge_previous, cover) +
                     cover * config.simplify_error_merge_additive;
      int refined = outputGroup(config, mesh, clusters, groups[i], bounds, depth, output_context,
                                output_callback);
      for (size_t j = 0; j < groups[i].size(); ++j)
        clusters[groups[i][j]].indices = std::vector<unsigned int>();

      std::vector<Cluster> split = clusterize(config, mesh, simplified.data(), simplified.size());
      for (Cluster& cluster : split) {
        cluster.refined = refined;
        cluster.bounds = bounds;
        clusters.push_back(std::move(cluster));
        cluster_cover.push_back(*group_cover);
        pending.push_back(int(clusters.size()) - 1);
      }
    }
    depth++;
  }

  if (pending.size()) {
    const Cluster& cluster = clusters[pending[0]];
    clodBounds bounds = cluster.bounds;
    bounds.error = FLT_MAX;
    *group_cover = k_lod_terminal_error;
    outputGroup(config, mesh, clusters, pending, bounds, depth, output_context, output_callback);
  }

  return clusters.size();
}

// Whether a merge carries a skin binding stream: when **any** of its inputs has one. Both merges
// ask it, and both used to read the first input alone.
bool any_skin(std::span<const ClusterLodMesh> parts) noexcept {
  for (const ClusterLodMesh& part : parts) {
    if (!part.mesh.skin.empty() && part.mesh.skin.size() == part.mesh.vertices.size()) return true;
  }
  return false;
}

// Whether sphere `outer` holds sphere `inner` whole, with a relative slack for the float rounding
// of a centre and a radius computed in double and stored in float.
bool sphere_encloses(Vec4 outer, Vec4 inner) noexcept {
  const f64 dx = static_cast<f64>(inner.x) - static_cast<f64>(outer.x);
  const f64 dy = static_cast<f64>(inner.y) - static_cast<f64>(outer.y);
  const f64 dz = static_cast<f64>(inner.z) - static_cast<f64>(outer.z);
  const f64 reach = std::sqrt(dx * dx + dy * dy + dz * dz) + static_cast<f64>(inner.w);
  const f64 r = static_cast<f64>(outer.w);
  return reach <= r + 1e-5 * (r > 1.0 ? r : 1.0);
}

// The sphere the authored transitions carry: centred on the box of everything it has to hold
// (the vertices of every level, the survivors' spheres) and as large as the farthest of them, in
// double, rounded outward once into float. A minimal enclosing sphere would be a few per cent
// smaller and an iterative search; this one is a function of the inputs alone.
struct SphereFit {
  f64 lo[3] = {1e300, 1e300, 1e300};
  f64 hi[3] = {-1e300, -1e300, -1e300};
  Vector<Vec4> items;  // points have radius 0
  void add(Vec4 s) {
    const f64 c[3] = {static_cast<f64>(s.x), static_cast<f64>(s.y), static_cast<f64>(s.z)};
    for (u32 k = 0; k < 3; ++k) {
      if (c[k] - static_cast<f64>(s.w) < lo[k]) lo[k] = c[k] - static_cast<f64>(s.w);
      if (c[k] + static_cast<f64>(s.w) > hi[k]) hi[k] = c[k] + static_cast<f64>(s.w);
    }
    items.push_back(s);
  }
  Vec4 sphere() const {
    if (items.empty()) return Vec4{};
    const f64 c[3] = {(lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5, (lo[2] + hi[2]) * 0.5};
    const Vec4 centre{static_cast<f32>(c[0]), static_cast<f32>(c[1]), static_cast<f32>(c[2]), 0.0f};
    f64 r = 0.0;
    for (const Vec4& s : items) {
      const f64 dx = static_cast<f64>(s.x) - static_cast<f64>(centre.x);
      const f64 dy = static_cast<f64>(s.y) - static_cast<f64>(centre.y);
      const f64 dz = static_cast<f64>(s.z) - static_cast<f64>(centre.z);
      const f64 reach = std::sqrt(dx * dx + dy * dy + dz * dz) + static_cast<f64>(s.w);
      if (reach > r) r = reach;
    }
    // Outward: the float radius is never smaller than the double one.
    f32 radius = static_cast<f32>(r);
    while (static_cast<f64>(radius) < r)
      radius = std::nextafter(radius, k_lod_terminal_error);
    return Vec4{centre.x, centre.y, centre.z, radius};
  }
};

// The authored half of `build_cluster_lod_levels`, after the decimated chain was built and cut at
// `levels[0].error` (the context's cap): the survivors re-parented, each level clustered on its own
// and appended above them, and one sphere over every transition the levels make.
bool append_authored_levels(std::span<const Vec3> positions,
                            std::span<const ClusterLodLevel> levels,
                            const ClusterLodOptions& options, ClusterLodMesh& out,
                            std::string* error) {
  while (!out.level_cluster_counts.empty() && out.level_cluster_counts.back() == 0)
    out.level_cluster_counts.pop_back();
  ClusterBuildOptions build;
  build.max_vertices = options.max_vertices;
  build.max_triangles = options.max_triangles & ~3u;
  build.normal_cones = options.normal_cones;
  Vector<ClusterMesh> built(static_cast<u32>(levels.size()));
  SphereFit fit;
  for (const ClusterLodDesc& lod : out.lod)
    fit.add(lod.own);
  for (u32 k = 0; k < levels.size(); ++k) {
    if (levels[k].indices.empty()) continue;
    std::string message;
    if (!build_clusters(positions, levels[k].indices, build, built[k], &message)) {
      if (error != nullptr)
        *error = "build_cluster_lod_levels: level " + std::to_string(k + 1) +
                 " did not cluster: " + message;
      return false;
    }
    for (const u32 index : levels[k].indices) {
      const Vec3 p = positions[index];
      fit.add(Vec4{p.x, p.y, p.z, 0.0f});
    }
  }
  const Vec4 sphere = fit.sphere();
  const f32 cap = levels[0].error;
  for (ClusterLodDesc& lod : out.lod) {
    if (lod.parent_error >= cap) {
      lod.parent = sphere;
      lod.parent_error = cap;
    }
  }
  for (u32 k = 0; k < levels.size(); ++k) {
    const ClusterMesh& level = built[k];
    if (level.clusters.empty()) continue;
    const u32 vertex_base = out.mesh.vertices.size();
    const u32 triangle_base = out.mesh.triangles.size();
    out.level_cluster_counts.push_back(0);
    for (const ClusterDesc& c : level.clusters) {
      ClusterDesc desc = c;
      desc.vertex_offset += vertex_base;
      desc.triangle_offset += triangle_base;
      out.mesh.clusters.push_back(desc);
      ClusterLodDesc lod;
      lod.own = sphere;
      lod.own_error = levels[k].error;
      lod.parent = sphere;
      lod.parent_error = k + 1 < levels.size() ? levels[k + 1].error : k_lod_terminal_error;
      lod.level = out.level_cluster_counts.size() - 1;
      lod.group = out.group_count;
      out.lod.push_back(lod);
      ++out.level_cluster_counts.back();
    }
    out.mesh.vertices.append(std::span<const Vec3>(level.vertices.data(), level.vertices.size()));
    out.mesh.vertex_source.append(
        std::span<const u32>(level.vertex_source.data(), level.vertex_source.size()));
    out.mesh.triangles.append(std::span<const u32>(level.triangles.data(), level.triangles.size()));
    ++out.group_count;
  }
  return true;
}

bool build_lod(std::span<const Vec3> positions, std::span<const u32> indices,
               std::span<const ClusterLodLevel> levels, const ClusterLodOptions& options,
               ClusterLodMesh& out, std::string* error, const AttributeSource& attributes);

}  // namespace

bool build_cluster_lod(std::span<const Vec3> positions, std::span<const u32> indices,
                       const ClusterLodOptions& options, ClusterLodMesh& out, std::string* error,
                       const AttributeSource& attributes) {
  return build_lod(positions, indices, {}, options, out, error, attributes);
}

bool build_cluster_lod_levels(std::span<const Vec3> positions, std::span<const u32> indices,
                              std::span<const ClusterLodLevel> levels,
                              const ClusterLodOptions& options, ClusterLodMesh& out,
                              std::string* error, const AttributeSource& attributes) {
  out = ClusterLodMesh{};
  for (u32 k = 0; k < levels.size(); ++k) {
    const ClusterLodLevel& level = levels[k];
    const bool rising = level.error > 0.0f && level.error < k_lod_terminal_error &&
                        (k == 0 || level.error > levels[k - 1].error);
    if (!rising || level.indices.size() % 3 != 0 ||
        (level.indices.empty() && k + 1 != levels.size())) {
      if (error != nullptr)
        *error =
            "build_cluster_lod_levels: every level's error must be positive, finite and above the "
            "one below, its indices a multiple of three, and only the last may be empty";
      return false;
    }
    for (const u32 index : level.indices) {
      if (index >= positions.size()) {
        if (error != nullptr) *error = "build_cluster_lod_levels: index out of range";
        return false;
      }
    }
  }
  return build_lod(positions, indices, levels, options, out, error, attributes);
}

Vec4 authored_lod_sphere(const ClusterLodMesh& mesh, f32 from_error, Vec4 with) noexcept {
  SphereFit fit;
  for (const ClusterLodDesc& lod : mesh.lod) {
    if (lod.own_error >= from_error) fit.add(lod.own);
    if (lod.parent_error >= from_error && lod.parent_error < k_lod_terminal_error)
      fit.add(lod.parent);
  }
  if (fit.items.empty()) return Vec4{};
  if (with.w > 0.0f) fit.add(with);
  return fit.sphere();
}

bool widen_lod_spheres(ClusterLodMesh& mesh, f32 from_error, Vec4 sphere, std::string* error) {
  for (const ClusterLodDesc& lod : mesh.lod) {
    const bool own = lod.own_error >= from_error;
    const bool parent = lod.parent_error >= from_error && lod.parent_error < k_lod_terminal_error;
    if ((own && !sphere_encloses(sphere, lod.own)) ||
        (parent && !sphere_encloses(sphere, lod.parent))) {
      if (error != nullptr)
        *error = "widen_lod_spheres: the sphere does not enclose one it replaces";
      return false;
    }
  }
  for (ClusterLodDesc& lod : mesh.lod) {
    if (lod.own_error >= from_error) lod.own = sphere;
    if (lod.parent_error >= from_error && lod.parent_error < k_lod_terminal_error)
      lod.parent = sphere;
  }
  return true;
}

namespace {

// A stable counting sort of `count` items by `key` (each below `keys`): the items in key order,
// ties in item order, and where each key's run starts.
void counting_order(u32 count, u32 keys, const auto& key, Vector<u32>& order, Vector<u32>& starts) {
  starts.assign(keys + 1, 0u);
  for (u32 i = 0; i < count; ++i)
    ++starts[key(i) + 1];
  for (u32 k = 0; k < keys; ++k)
    starts[k + 1] += starts[k];
  Vector<u32> cursor(starts);
  order.assign(count, 0u);
  for (u32 i = 0; i < count; ++i)
    order[cursor[key(i)]++] = i;
}

// Appends `built`'s clusters to `out` with their offsets shifted, and their LOD records.
void append_built(const ClusterMesh& built, const ClusterLodDesc& lod, bool own_is_cluster,
                  ClusterLodMesh& out) {
  const u32 vertex_base = out.mesh.vertices.size();
  const u32 triangle_base = out.mesh.triangles.size();
  for (const ClusterDesc& c : built.clusters) {
    ClusterDesc desc = c;
    desc.vertex_offset += vertex_base;
    desc.triangle_offset += triangle_base;
    out.mesh.clusters.push_back(desc);
    ClusterLodDesc record = lod;
    if (own_is_cluster) record.own = Vec4{c.center.x, c.center.y, c.center.z, c.radius};
    out.lod.push_back(record);
  }
  out.mesh.vertices.append(std::span<const Vec3>(built.vertices.data(), built.vertices.size()));
  out.mesh.vertex_source.append(
      std::span<const u32>(built.vertex_source.data(), built.vertex_source.size()));
  out.mesh.triangles.append(std::span<const u32>(built.triangles.data(), built.triangles.size()));
}

}  // namespace

bool build_cluster_lod_hierarchy(std::span<const Vec3> positions, std::span<const u32> indices,
                                 std::span<const u32> element_of_triangle,
                                 std::span<const ClusterLodHierarchyLevel> levels,
                                 const ClusterLodOptions& options, ClusterLodMesh& out,
                                 std::string* error, const AttributeSource& attributes) {
  out = ClusterLodMesh{};
  const auto fail = [&](const char* what) {
    if (error != nullptr) *error = std::string("build_cluster_lod_hierarchy: ") + what;
    out = ClusterLodMesh{};
    return false;
  };
  const u32 depth = static_cast<u32>(levels.size());
  if (positions.empty() || indices.empty() || indices.size() % 3 != 0 || depth == 0)
    return fail("need vertices, a multiple of three indices and at least one level");
  if (element_of_triangle.size() != indices.size() / 3)
    return fail("the finest triangles need one element each");
  for (u32 k = 0; k < depth; ++k) {
    const ClusterLodHierarchyLevel& level = levels[k];
    const bool rising = level.error > 0.0f && level.error < k_lod_terminal_error &&
                        (k == 0 || level.error > levels[k - 1].error);
    if (!rising || level.indices.empty() || level.indices.size() % 3 != 0 ||
        level.element_of_triangle.size() != level.indices.size() / 3 || level.elements == 0 ||
        (k + 1 < depth && level.parent_of_element.size() != level.elements))
      return fail(
          "every level needs triangles, an element a triangle, a parent an element and "
          "an error above the one below");
    for (const u32 e : level.element_of_triangle)
      if (e >= level.elements) return fail("a triangle names an element its level does not have");
    if (k + 1 < depth) {
      for (const u32 p : level.parent_of_element)
        if (p >= levels[k + 1].elements) return fail("an element names a parent out of range");
    }
  }
  for (const u32 e : element_of_triangle)
    if (e >= levels[0].elements) return fail("a finest triangle names an element out of range");
  for (const u32 index : indices)
    if (index >= positions.size()) return fail("index out of range");
  for (const ClusterLodHierarchyLevel& level : levels)
    for (const u32 index : level.indices)
      if (index >= positions.size()) return fail("index out of range");

  // Each level's elements: their triangles (contiguous by a counting sort) and distinct vertices.
  Vector<Vector<u32>> element_triangles(depth);  // triangle ids, by element
  Vector<Vector<u32>> element_start(depth);
  Vector<Vector<u32>> element_vertices(depth);
  for (u32 k = 0; k < depth; ++k) {
    const ClusterLodHierarchyLevel& level = levels[k];
    counting_order(
        static_cast<u32>(level.element_of_triangle.size()), level.elements,
        [&](u32 t) { return level.element_of_triangle[t]; }, element_triangles[k],
        element_start[k]);
    element_vertices[k].assign(level.elements, 0u);
    Vector<u32> corners;
    for (u32 e = 0; e < level.elements; ++e) {
      corners.clear();
      for (u32 i = element_start[k][e]; i < element_start[k][e + 1]; ++i) {
        const u32 t = element_triangles[k][i];
        for (u32 c = 0; c < 3; ++c)
          corners.push_back(level.indices[t * 3 + c]);
      }
      std::sort(corners.begin(), corners.end());
      element_vertices[k][e] =
          static_cast<u32>(std::unique(corners.begin(), corners.end()) - corners.begin());
    }
  }

  // The groups, top-down: each level's elements ordered by their parents' places in the level
  // above (the top in its own order), then cut into runs of at most one cluster's worth that never
  // cross their parent's group.
  const u32 budget_vertices = options.max_vertices;
  const u32 budget_triangles = options.max_triangles & ~3u;
  Vector<Vector<u32>> order(depth);         // the level's elements, grouped
  Vector<Vector<u32>> group_start(depth);   // into `order`
  Vector<Vector<u32>> group_of(depth);      // per element
  Vector<Vector<u32>> parent_group(depth);  // per group: its parent's group in the level above
  for (u32 k = depth; k-- > 0;) {
    const ClusterLodHierarchyLevel& level = levels[k];
    Vector<u32> rank_starts;
    if (k + 1 == depth) {
      order[k].resize(level.elements);
      for (u32 e = 0; e < level.elements; ++e)
        order[k][e] = e;
    } else {
      Vector<u32> rank(levels[k + 1].elements, 0u);
      for (u32 i = 0; i < order[k + 1].size(); ++i)
        rank[order[k + 1][i]] = i;
      counting_order(
          level.elements, levels[k + 1].elements,
          [&](u32 e) { return rank[level.parent_of_element[e]]; }, order[k], rank_starts);
    }
    group_of[k].assign(level.elements, 0u);
    u32 vertices = 0, triangles = 0, above = ~0u;
    for (u32 i = 0; i < order[k].size(); ++i) {
      const u32 e = order[k][i];
      const u32 parent = k + 1 == depth ? 0u : group_of[k + 1][level.parent_of_element[e]];
      const u32 v = element_vertices[k][e];
      const u32 t = element_start[k][e + 1] - element_start[k][e];
      const bool fresh = group_start[k].empty() || parent != above ||
                         vertices + v > budget_vertices || triangles + t > budget_triangles;
      if (fresh) {
        group_start[k].push_back(i);
        parent_group[k].push_back(parent);
        vertices = 0;
        triangles = 0;
        above = parent;
      }
      vertices += v;
      triangles += t;
      group_of[k][e] = group_start[k].size() - 1;
    }
    group_start[k].push_back(order[k].size());
  }

  // The finest triangles of each first-level group.
  Vector<u32> finest_order, finest_start;
  counting_order(
      static_cast<u32>(element_of_triangle.size()), group_start[0].size() - 1,
      [&](u32 t) { return group_of[0][element_of_triangle[t]]; }, finest_order, finest_start);

  // Spheres, bottom-up: a group holds every vertex of its elements, its finest triangles' and its
  // child groups' spheres.
  Vector<Vector<Vec4>> sphere(depth);
  for (u32 k = 0; k < depth; ++k) {
    const ClusterLodHierarchyLevel& level = levels[k];
    const u32 groups = group_start[k].size() - 1;
    Vector<SphereFit> fits(groups);
    for (u32 g = 0; g < groups; ++g) {
      for (u32 i = group_start[k][g]; i < group_start[k][g + 1]; ++i) {
        const u32 e = order[k][i];
        for (u32 j = element_start[k][e]; j < element_start[k][e + 1]; ++j) {
          const u32 t = element_triangles[k][j];
          for (u32 c = 0; c < 3; ++c) {
            const Vec3 p = positions[level.indices[t * 3 + c]];
            fits[g].add(Vec4{p.x, p.y, p.z, 0.0f});
          }
        }
      }
    }
    if (k == 0) {
      for (u32 g = 0; g < groups; ++g)
        for (u32 i = finest_start[g]; i < finest_start[g + 1]; ++i)
          for (u32 c = 0; c < 3; ++c) {
            const Vec3 p = positions[indices[finest_order[i] * 3 + c]];
            fits[g].add(Vec4{p.x, p.y, p.z, 0.0f});
          }
    } else {
      for (u32 g = 0; g + 1 < group_start[k - 1].size(); ++g)
        fits[parent_group[k - 1][g]].add(sphere[k - 1][g]);
    }
    sphere[k].resize(groups);
    for (u32 g = 0; g < groups; ++g)
      sphere[k][g] = fits[g].sphere();
  }

  // The DAG's groups are the clusters that share a parent: the runs under one group of the level
  // above, and each top group on its own.
  Vector<u32> sibling_base(depth + 1, 0u);
  for (u32 k = 0; k < depth; ++k)
    sibling_base[k + 1] = sibling_base[k] + (group_start[k].size() - 1);

  ClusterBuildOptions build;
  build.max_vertices = options.max_vertices;
  build.max_triangles = budget_triangles;
  build.normal_cones = options.normal_cones;
  out.mesh.source_vertex_count = static_cast<u32>(positions.size());
  out.mesh.source_triangle_count = static_cast<u32>(indices.size() / 3);
  out.leaf_triangle_count = out.mesh.source_triangle_count;
  Vector<u32> run;
  ClusterMesh built;
  std::string message;
  // Each group is clustered over its own vertices alone: the meshlet builder's tables are as large
  // as the vertex array it is handed, and a tree's crown is thousands of groups over millions of
  // vertices — handing each the whole array made the oak's build seconds and the ancient tree's an
  // hour. The local ids map back through `vertex_source`.
  Vector<u32> local_of(static_cast<u32>(positions.size()), ~0u);
  Vector<u32> global_of;
  Vector<Vec3> local_positions;
  Vector<u32> local_run;
  const auto cluster_run = [&](std::string* why) {
    global_of.clear();
    local_positions.clear();
    local_run.clear();
    for (const u32 index : run) {
      if (local_of[index] == ~0u) {
        local_of[index] = global_of.size();
        global_of.push_back(index);
        local_positions.push_back(positions[index]);
      }
      local_run.push_back(local_of[index]);
    }
    for (const u32 index : global_of)
      local_of[index] = ~0u;
    if (!build_clusters(local_positions, local_run, build, built, why)) return false;
    for (u32& source : built.vertex_source)
      source = global_of[source];
    return true;
  };
  // The finest: own error 0, the parent its first-level group.
  out.level_cluster_counts.push_back(0);
  for (u32 g = 0; g + 1 < finest_start.size(); ++g) {
    if (finest_start[g] == finest_start[g + 1]) continue;
    run.clear();
    for (u32 i = finest_start[g]; i < finest_start[g + 1]; ++i)
      for (u32 c = 0; c < 3; ++c)
        run.push_back(indices[finest_order[i] * 3 + c]);
    if (!cluster_run(&message)) return fail("the finest triangles of a group did not cluster");
    ClusterLodDesc lod{};
    lod.own_error = 0.0f;
    lod.parent = sphere[0][g];
    lod.parent_error = levels[0].error;
    lod.level = 0;
    lod.group = sibling_base[0] + g;
    append_built(built, lod, true, out);
    out.level_cluster_counts.back() += built.clusters.size();
  }
  for (u32 k = 0; k < depth; ++k) {
    const ClusterLodHierarchyLevel& level = levels[k];
    out.level_cluster_counts.push_back(0);
    for (u32 g = 0; g + 1 < group_start[k].size(); ++g) {
      run.clear();
      for (u32 i = group_start[k][g]; i < group_start[k][g + 1]; ++i) {
        const u32 e = order[k][i];
        for (u32 j = element_start[k][e]; j < element_start[k][e + 1]; ++j)
          for (u32 c = 0; c < 3; ++c)
            run.push_back(level.indices[element_triangles[k][j] * 3 + c]);
      }
      if (!cluster_run(&message)) return fail("a level's group did not cluster");
      ClusterLodDesc lod{};
      lod.own = sphere[k][g];
      lod.own_error = level.error;
      const bool top = k + 1 == depth;
      lod.parent = top ? sphere[k][g] : sphere[k + 1][parent_group[k][g]];
      lod.parent_error = top ? k_lod_terminal_error : levels[k + 1].error;
      lod.level = k + 1;
      lod.group = top ? sibling_base[depth] + g : sibling_base[k + 1] + parent_group[k][g];
      append_built(built, lod, false, out);
      out.level_cluster_counts.back() += built.clusters.size();
    }
  }
  out.group_count = sibling_base[depth] + (group_start[depth - 1].size() - 1);
  Vector<u32> all;
  all.append(indices);
  for (const ClusterLodHierarchyLevel& level : levels)
    all.append(level.indices);
  fill_cluster_attributes(out.mesh, positions, all, attributes);
  quantize_positions(out.mesh);  // one grid over every level
  return true;
}

namespace {

bool build_lod(std::span<const Vec3> positions, std::span<const u32> indices,
               std::span<const ClusterLodLevel> levels, const ClusterLodOptions& options,
               ClusterLodMesh& out, std::string* error, const AttributeSource& attributes) {
  out = ClusterLodMesh{};
  if (positions.empty() || indices.empty() || indices.size() % 3 != 0) {
    if (error != nullptr)
      *error = "build_cluster_lod: need vertices and a multiple of three indices";
    return false;
  }
  if (options.max_triangles < 4 || options.max_triangles > 256 || options.max_vertices == 0 ||
      options.max_vertices > 255) {
    if (error != nullptr)
      *error = "build_cluster_lod: max_triangles must be 4..256 and max_vertices 1..255";
    return false;
  }
  for (const u32 index : indices) {
    if (index >= positions.size()) {
      if (error != nullptr) *error = "build_cluster_lod: index out of range";
      return false;
    }
  }

  clodConfig config = options.ray_tracing ? clodDefaultConfigRT(options.max_triangles)
                                          : clodDefaultConfig(options.max_triangles);
  config.max_vertices = options.max_vertices;
  config.optimize_bounds = true;
  clodMesh mesh{};
  mesh.indices = indices.data();
  mesh.index_count = indices.size();
  mesh.vertex_count = positions.size();
  mesh.vertex_positions = &positions[0].x;
  mesh.vertex_positions_stride = sizeof(Vec3);

  // What the simplifier is given beyond the positions, and why it has to be given it.
  //
  // `clodDefaultConfig` runs meshoptimizer's **permissive** simplification, whose whole meaning
  // is "a collapse may cross an attribute discontinuity unless the vertex is tagged". Handed
  // positions alone — no attributes, no weights, no tags, which is what this builder did until
  // 2026-09-19 — it has neither a reason to avoid such a collapse (there is no attribute term in
  // its error metric at all) nor a rule forbidding one. On an atlas of large clean islands that
  // is invisible. On one made of thousands of small islands, which is what an AI generator and a
  // photogrammetry scan produce, a single collapse welds two unrelated islands together and every
  // triangle spanning the join interpolates its UVs across the whole atlas.
  //
  // So: the normal and the UV as weighted attributes, and a tag on every vertex that sits on a
  // discontinuity. Both arrays are indexed by *source* vertex, which is the space clusterlod's
  // indices live in.
  Vector<f32> attribute_values;
  Vector<u8> vertex_lock;
  f32 attribute_weights[k_lod_attribute_count] = {};
  Vector<Vec3> computed_normals;
  const u8 uv_bit = seam_bit(options.uv_seams);
  const u8 normal_bit = seam_bit(options.normal_seams);
  const u8 skin_bit = seam_bit(options.skin_seams);
  const u8 morph_bit = attributes.morph.empty() ? u8{0} : seam_bit(options.morph_seams);
  const bool have_ids = attributes.vertex_ids.size() == positions.size() &&
                        attributes.vertex_id_source != VertexIdSource::none;
  const u8 id_bit = have_ids ? seam_bit(options.id_seams) : u8{0};
  const bool weighted = options.normal_weight > 0.0f || options.uv_weight > 0.0f;
  const bool tagged =
      uv_bit != 0 || normal_bit != 0 || skin_bit != 0 || morph_bit != 0 || id_bit != 0;
  const u32 vertex_count = static_cast<u32>(positions.size());
  std::span<const Vec3> normals = attributes.normals;
  // Only when something will read them: with `normal_weight` at its default of zero and no
  // normal-seam rule, computing them would be a pass over the mesh nobody looks at.
  if ((options.normal_weight > 0.0f || normal_bit != 0) && normals.size() != positions.size()) {
    compute_vertex_normals(positions, indices, computed_normals);
    normals = std::span<const Vec3>(computed_normals.data(), computed_normals.size());
  }
  const bool have_uvs = attributes.uvs.size() == positions.size();
  const bool have_skin = attributes.skin.size() == positions.size();
  if (weighted) {
    attribute_values.resize(vertex_count * k_lod_attribute_count, 0.0f);
    for (u32 v = 0; v < vertex_count; ++v) {
      f32* record = &attribute_values[v * k_lod_attribute_count];
      const Vec3 n = v < normals.size() ? normals[v] : Vec3{0.0f, 1.0f, 0.0f};
      record[0] = n.x;
      record[1] = n.y;
      record[2] = n.z;
      if (have_uvs) {
        record[3] = attributes.uvs[v].x;
        record[4] = attributes.uvs[v].y;
      }
    }
    attribute_weights[0] = options.normal_weight;
    attribute_weights[1] = options.normal_weight;
    attribute_weights[2] = options.normal_weight;
    attribute_weights[3] = have_uvs ? options.uv_weight : 0.0f;
    attribute_weights[4] = have_uvs ? options.uv_weight : 0.0f;
    mesh.vertex_attributes = attribute_values.data();
    mesh.vertex_attributes_stride = sizeof(f32) * k_lod_attribute_count;
    mesh.attribute_weights = attribute_weights;
    mesh.attribute_count = k_lod_attribute_count;
  }
  if (tagged) {
    // A discontinuity is a vertex that shares a *position* with another vertex but disagrees with
    // it about an attribute — precisely the duplicates `weld_vertices` refuses to merge, since it
    // keys on position, normal, UV and binding together. meshoptimizer's own position remap is
    // what clusterlod uses for connectivity, so asking it the same question asks it in the same
    // terms. The tag goes on the duplicate rather than on the canonical vertex, which is enough:
    // the simplifier walks a vertex's whole wedge ring and protects the ring if any wedge is
    // tagged.
    Vector<unsigned int> position_remap(vertex_count);
    meshopt_generatePositionRemap(position_remap.data(), &positions[0].x, positions.size(),
                                  sizeof(Vec3));
    // The morph comparison uses the same interned id the weld keyed on, so "these two vertices
    // carry different deltas" means exactly the same thing in both places, and the walk is over
    // the sparse data rather than over channels x vertices.
    Vector<u32> morph_keys;
    if (morph_bit != 0) morph_vertex_keys(attributes.morph, vertex_count, morph_keys);
    vertex_lock.resize(vertex_count, 0u);
    for (u32 v = 0; v < vertex_count; ++v) {
      const u32 canonical = position_remap[v];
      if (canonical == v) continue;
      if (uv_bit != 0 && have_uvs &&
          (attributes.uvs[v].x != attributes.uvs[canonical].x ||
           attributes.uvs[v].y != attributes.uvs[canonical].y)) {
        vertex_lock[v] |= uv_bit;
      }
      if (normal_bit != 0 && v < normals.size() && canonical < normals.size() &&
          (normals[v].x != normals[canonical].x || normals[v].y != normals[canonical].y ||
           normals[v].z != normals[canonical].z)) {
        vertex_lock[v] |= normal_bit;
      }
      if (skin_bit != 0 && have_skin) {
        const SkinBinding& a = attributes.skin[v];
        const SkinBinding& b = attributes.skin[canonical];
        bool same = true;
        for (u32 k = 0; k < 4; ++k)
          same = same && a.joints[k] == b.joints[k] && a.weights[k] == b.weights[k];
        if (!same) vertex_lock[v] |= skin_bit;
      }
      if (morph_bit != 0 && v < morph_keys.size() && canonical < morph_keys.size() &&
          morph_keys[v] != morph_keys[canonical]) {
        vertex_lock[v] |= morph_bit;
      }
      // Two points of the base the author named apart, at one position: the weld kept them apart,
      // and so does the simplifier.
      if (id_bit != 0 && attributes.vertex_ids[v] != attributes.vertex_ids[canonical])
        vertex_lock[v] |= id_bit;
    }
    mesh.vertex_lock = vertex_lock.data();
  }

  // Vertices the caller locks (a part's border, when parts are built apart and merged): clusterlod
  // spreads a lock to every vertex at the same position, so one wedge is enough.
  if (attributes.locked.size() == positions.size()) {
    vertex_lock.resize(vertex_count, 0u);
    for (u32 v = 0; v < vertex_count; ++v) {
      if (attributes.locked[v] != 0) vertex_lock[v] |= seam_bit(SeamRule::lock);
    }
    mesh.vertex_lock = vertex_lock.data();
  }

  LooseBuild loose;
  loose.enabled = options.loose_islands;
  BuildContext ctx;
  ctx.mesh = &mesh;
  ctx.origin = &loose.pool.origin;
  ctx.out = &out;
  ctx.normal_cones = options.normal_cones;
  // Coverage errors for an alpha-tested part built with the loose-island rule and no authored
  // levels (a caller with levels brings coverage errors of its own, or none).
  ctx.masked = options.loose_islands && options.mask_cover > 0.0f && levels.empty() &&
               options.up.x == 0.0f && options.up.y == 0.0f && options.up.z == 0.0f;
  if (ctx.masked) {
    const f64 share = options.mask_cover < 1.0f ? static_cast<f64>(options.mask_cover) : 1.0;
    loose.mask_root = std::sqrt(share);
  }
  if (!levels.empty()) ctx.cap = levels[0].error;
  out.mesh.source_vertex_count = static_cast<u32>(positions.size());
  out.mesh.source_triangle_count = static_cast<u32>(indices.size() / 3);
  // One loop builds every mesh: a heightfield with its simplification held to `up`, everything
  // else with its loose islands coarsened as islands.
  const Up up = normalized_up(options.up);
  const bool heightfield = up.x != 0.0 || up.y != 0.0 || up.z != 0.0;
  FoldGuardStats fold_guard;
  const FoldGuard guard = heightfield ? fold_guard_of(options, positions, indices) : FoldGuard{};
  const size_t produced = build_lod_groups(config, mesh, heightfield ? &guard : nullptr, fold_guard,
                                           loose, &ctx.pending_cover, &ctx, &emit_group);
  if (ctx.failed) {
    if (error != nullptr) *error = "build_cluster_lod: a cluster exceeded the format limits";
    out = ClusterLodMesh{};
    return false;
  }
  if (produced != out.mesh.clusters.size() + ctx.skipped || out.mesh.clusters.empty()) {
    if (error != nullptr) *error = "build_cluster_lod: the builder produced no clusters";
    out = ClusterLodMesh{};
    return false;
  }
  out.group_count = ctx.group_bounds.size();
  out.fold_guard = fold_guard;
  out.aggregate = loose.stats;
  if (!levels.empty() && !append_authored_levels(positions, levels, options, out, error)) {
    out = ClusterLodMesh{};
    return false;
  }
  if (levels.empty() || attributes.normals.size() == positions.size()) {
    fill_cluster_attributes(out.mesh, positions, indices, attributes);
  } else {
    // Normals computed from every level's triangles, so an authored vertex no finer triangle uses
    // still gets its own surface's.
    Vector<u32> all;
    all.append(indices);
    for (const ClusterLodLevel& level : levels)
      all.append(level.indices);
    fill_cluster_attributes(out.mesh, positions, all, attributes);
  }
  quantize_positions(out.mesh);  // one grid over every level
  return true;
}

}  // namespace

f32 projected_error(Vec4 sphere, f32 error, const LodView& view) noexcept {
  if (error >= k_lod_terminal_error) return k_lod_terminal_error;
  const Vec3 center{sphere.x, sphere.y, sphere.z};
  const f32 distance = length(center - view.camera) - sphere.w;
  const f32 clamped = distance > view.znear ? distance : view.znear;
  return error / clamped * view.proj_scale;
}

bool lod_selects(const ClusterLodDesc& lod, const LodView& view) noexcept {
  const f32 own = projected_error(lod.own, lod.own_error, view);
  const f32 parent = projected_error(lod.parent, lod.parent_error, view);
  return own <= view.threshold_px && parent > view.threshold_px;
}

u32 pack_lod_coverage(f32 own, f32 parent) noexcept {
  // Past a half's range (or not a number at all) is "none": +inf, which no threshold accepts.
  const auto half = [](f32 v) -> u32 {
    if (!(v >= 0.0f) || v > 65504.0f) return 0x7C00u;
    return f32_to_f16(v);
  };
  return half(own) | (half(parent) << 16);
}

f32 lod_coverage_own(u32 packed) noexcept { return f16_to_f32(static_cast<u16>(packed & 0xFFFFu)); }

f32 lod_coverage_parent(u32 packed) noexcept { return f16_to_f32(static_cast<u16>(packed >> 16)); }

f32 measure_mask_cover(std::span<const Vec3> positions, std::span<const Vec2> uvs,
                       std::span<const u32> indices, std::span<const u8> alpha, u32 width,
                       u32 height, f32 alpha_factor, f32 alpha_cutoff, Vec2 scale,
                       Vec2 offset) noexcept {
  const bool image =
      width != 0 && height != 0 && alpha.size() >= static_cast<size_t>(width) * height;
  const auto keeps = [&](u8 texel) {
    return alpha_factor * static_cast<f32>(texel) / 255.0f >= alpha_cutoff;
  };
  f64 share = 1.0;
  if (!image) {
    share = alpha_factor >= alpha_cutoff ? 1.0 : 0.0;
  } else if (uvs.size() != positions.size()) {
    // No UVs to ask with: the share of the whole image the test keeps.
    u64 kept = 0;
    const u64 texels = static_cast<u64>(width) * height;
    for (u64 t = 0; t < texels; ++t)
      kept += keeps(alpha[static_cast<size_t>(t)]) ? 1u : 0u;
    share = static_cast<f64>(kept) / static_cast<f64>(texels);
  } else {
    constexpr u32 n = 4;
    f64 kept_area = 0.0, area_sum = 0.0;
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
      const u32 ia = indices[i], ib = indices[i + 1], ic = indices[i + 2];
      if (ia >= positions.size() || ib >= positions.size() || ic >= positions.size()) continue;
      const Vec3 a = positions[ia], b = positions[ib], c = positions[ic];
      const f64 e[3] = {static_cast<f64>(b.x) - static_cast<f64>(a.x),
                        static_cast<f64>(b.y) - static_cast<f64>(a.y),
                        static_cast<f64>(b.z) - static_cast<f64>(a.z)};
      const f64 f[3] = {static_cast<f64>(c.x) - static_cast<f64>(a.x),
                        static_cast<f64>(c.y) - static_cast<f64>(a.y),
                        static_cast<f64>(c.z) - static_cast<f64>(a.z)};
      const f64 nx = e[1] * f[2] - e[2] * f[1];
      const f64 ny = e[2] * f[0] - e[0] * f[2];
      const f64 nz = e[0] * f[1] - e[1] * f[0];
      const f64 area = 0.5 * std::sqrt(nx * nx + ny * ny + nz * nz);
      if (!(area > 0.0)) continue;
      const f64 u0 = static_cast<f64>(uvs[ia].x), v0 = static_cast<f64>(uvs[ia].y);
      const f64 du1 = static_cast<f64>(uvs[ib].x) - u0, dv1 = static_cast<f64>(uvs[ib].y) - v0;
      const f64 du2 = static_cast<f64>(uvs[ic].x) - u0, dv2 = static_cast<f64>(uvs[ic].y) - v0;
      u32 kept = 0;
      const auto sample = [&](f64 s, f64 t) {
        f64 u = (u0 + s * du1 + t * du2) * static_cast<f64>(scale.x) + static_cast<f64>(offset.x);
        f64 v = (v0 + s * dv1 + t * dv2) * static_cast<f64>(scale.y) + static_cast<f64>(offset.y);
        u -= std::floor(u);
        v -= std::floor(v);
        u32 x = static_cast<u32>(u * static_cast<f64>(width));
        u32 y = static_cast<u32>(v * static_cast<f64>(height));
        x = x < width ? x : width - 1;
        y = y < height ? y : height - 1;
        if (keeps(alpha[static_cast<size_t>(y) * width + x])) ++kept;
      };
      for (u32 p = 0; p < n; ++p) {
        for (u32 q = 0; p + q < n; ++q) {
          const f64 fp = static_cast<f64>(p), fq = static_cast<f64>(q);
          sample((fp + 1.0 / 3.0) / n, (fq + 1.0 / 3.0) / n);
          if (p + q + 1 < n) sample((fp + 2.0 / 3.0) / n, (fq + 2.0 / 3.0) / n);
        }
      }
      kept_area += area * static_cast<f64>(kept) / static_cast<f64>(n * n);
      area_sum += area;
    }
    share = area_sum > 0.0 ? kept_area / area_sum : 1.0;
  }
  f64 steps = std::floor(share * 256.0 + 0.5);
  steps = steps < 16.0 ? 16.0 : (steps > 256.0 ? 256.0 : steps);
  return static_cast<f32>(steps / 256.0);
}

// ---- what a cut covers (geometry.md, "Loose islands") -------------------------------------------

namespace {

void fill_cover(Vector<u8>& bitmap, const f64 a[2], const f64 b[2], const f64 c[2]) {
  const f64 top = static_cast<f64>(k_lod_cover_bitmap) - 1.0;
  const auto clamp = [top](f64 v) { return v < 0.0 ? 0.0 : (v > top ? top : v); };
  const u32 x0 = static_cast<u32>(clamp(std::floor(std::min(a[0], std::min(b[0], c[0])))));
  const u32 x1 = static_cast<u32>(clamp(std::floor(std::max(a[0], std::max(b[0], c[0])))));
  const u32 y0 = static_cast<u32>(clamp(std::floor(std::min(a[1], std::min(b[1], c[1])))));
  const u32 y1 = static_cast<u32>(clamp(std::floor(std::max(a[1], std::max(b[1], c[1])))));
  for (u32 y = y0; y <= y1; ++y) {
    for (u32 x = x0; x <= x1; ++x) {
      const f64 px = static_cast<f64>(x) + 0.5, py = static_cast<f64>(y) + 0.5;
      const f64 e0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
      const f64 e1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
      const f64 e2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
      if ((e0 >= 0.0 && e1 >= 0.0 && e2 >= 0.0) || (e0 <= 0.0 && e1 <= 0.0 && e2 <= 0.0))
        bitmap[y * k_lod_cover_bitmap + x] = 1;
    }
  }
}

}  // namespace

bool measure_lod_cover(const ClusterLodMesh& mesh, std::span<const u32> cut, LodCutCover& out,
                       std::string* error) {
  out = LodCutCover{};
  const ClusterMesh& m = mesh.mesh;
  if (m.vertices.empty()) {
    if (error != nullptr) *error = "measure_lod_cover: the mesh has no vertices";
    return false;
  }
  // One frame for every cut of the mesh: the cube about the box of all its vertices, every level's.
  f64 lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
  for (const Vec3& p : m.vertices) {
    const f64 q[3] = {static_cast<f64>(p.x), static_cast<f64>(p.y), static_cast<f64>(p.z)};
    for (u32 k = 0; k < 3; ++k) {
      lo[k] = std::min(lo[k], q[k]);
      hi[k] = std::max(hi[k], q[k]);
    }
  }
  const f64 side = std::max(std::max(hi[0] - lo[0], hi[1] - lo[1]), hi[2] - lo[2]);
  const f64 to_px = side > 0.0 ? static_cast<f64>(k_lod_cover_bitmap) / side : 0.0;
  const f64 origin[3] = {(lo[0] + hi[0] - side) * 0.5, (lo[1] + hi[1] - side) * 0.5,
                         (lo[2] + hi[2] - side) * 0.5};
  Vector<u8> bitmap[3];
  for (Vector<u8>& b : bitmap)
    b.resize(k_lod_cover_bitmap * k_lod_cover_bitmap, u8{0});
  for (const u32 c : cut) {
    if (c >= m.clusters.size()) {
      if (error != nullptr) *error = "measure_lod_cover: a cluster of the cut is out of range";
      return false;
    }
    const ClusterDesc& desc = m.clusters[c];
    ++out.clusters;
    for (u32 t = 0; t < desc.triangle_count; ++t) {
      const u32 packed = m.triangles[desc.triangle_offset + t];
      f64 p[3][3];
      for (u32 k = 0; k < 3; ++k) {
        const Vec3 v = m.vertices[desc.vertex_offset + ClusterMesh::unpack(packed, k)];
        p[k][0] = static_cast<f64>(v.x);
        p[k][1] = static_cast<f64>(v.y);
        p[k][2] = static_cast<f64>(v.z);
      }
      const f64 e[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
      const f64 f[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
      const f64 n[3] = {e[1] * f[2] - e[2] * f[1], e[2] * f[0] - e[0] * f[2],
                        e[0] * f[1] - e[1] * f[0]};
      ++out.triangles;
      out.area += 0.5 * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      for (u32 axis = 0; axis < 3; ++axis) {
        const u32 u = (axis + 1) % 3, w = (axis + 2) % 3;
        f64 q[3][2];
        for (u32 k = 0; k < 3; ++k) {
          q[k][0] = (p[k][u] - origin[u]) * to_px;
          q[k][1] = (p[k][w] - origin[w]) * to_px;
        }
        fill_cover(bitmap[axis], q[0], q[1], q[2]);
      }
    }
  }
  for (u32 axis = 0; axis < 3; ++axis)
    for (const u8 b : bitmap[axis])
      out.silhouette[axis] += b;
  return true;
}

// ---- distance forms (geometry.md, "Distance forms")
// ----------------------------------------------

bool lod_acceptable_from(Vec4 sphere, f32 error, f32 coverage, const LodFormCut& cut) noexcept {
  if (!(error < k_lod_terminal_error) && !(coverage < k_lod_terminal_error)) return false;
  // The nearest any camera at least `distance` from the origin comes to the sphere's surface, in
  // f64 and with a ten-thousandth of slack, so the bound holds for the f32 arithmetic the cull pass
  // and `lod_selects` do at a camera exactly that far.
  const f64 dx = static_cast<f64>(sphere.x) - static_cast<f64>(cut.origin.x);
  const f64 dy = static_cast<f64>(sphere.y) - static_cast<f64>(cut.origin.y);
  const f64 dz = static_cast<f64>(sphere.z) - static_cast<f64>(cut.origin.z);
  const f64 offset = std::sqrt(dx * dx + dy * dy + dz * dz);
  f64 nearest = static_cast<f64>(cut.distance) - offset - static_cast<f64>(sphere.w);
  nearest = nearest > static_cast<f64>(cut.znear) ? nearest : static_cast<f64>(cut.znear);
  const f64 keep = (1.0 - static_cast<f64>(cut.fade)) * (1.0 - 1.0e-4);
  const f64 scale = static_cast<f64>(cut.proj_scale) / nearest;
  if (error < k_lod_terminal_error &&
      static_cast<f64>(error) * scale <= keep * static_cast<f64>(cut.threshold_px))
    return true;
  return cut.foliage_px > 0.0f && coverage < k_lod_terminal_error &&
         static_cast<f64>(coverage) * scale <= keep * static_cast<f64>(cut.foliage_px);
}

bool extract_lod_form(const ClusterLodMesh& mesh, std::span<const u32> coverage,
                      const LodFormCut& cut, ClusterLodMesh& out, Vector<u32>& coverage_out,
                      LodFormMap& map, std::string* error) {
  const ClusterMesh& src = mesh.mesh;
  const u32 count = src.clusters.size();
  if (mesh.lod.size() != count) {
    if (error != nullptr) *error = "the mesh's LOD descriptions are not parallel to its clusters";
    return false;
  }
  if (!coverage.empty() && coverage.size() != count) {
    if (error != nullptr) *error = "the coverage stream is not parallel to the clusters";
    return false;
  }
  if (!src.skin.empty() || !src.morph_channels.empty()) {
    if (error != nullptr)
      *error = "a skinned or morphed mesh has no distance form: its spheres are its rest pose's";
    return false;
  }
  const auto own_coverage = [&](u32 c) {
    return coverage.empty() ? k_lod_terminal_error : lod_coverage_own(coverage[c]);
  };
  const auto parent_coverage = [&](u32 c) {
    return coverage.empty() ? k_lod_terminal_error : lod_coverage_parent(coverage[c]);
  };
  map.clusters.clear();
  map.vertices.clear();
  map.frontier = 0;
  for (u32 c = 0; c < count; ++c) {
    const ClusterLodDesc& lod = mesh.lod[c];
    if (!lod_acceptable_from(lod.parent, lod.parent_error, parent_coverage(c), cut))
      map.clusters.push_back(c);
  }
  if (map.clusters.empty()) {
    if (error != nullptr) *error = "the cut keeps no cluster of the mesh";
    return false;
  }
  out = ClusterLodMesh{};
  coverage_out.clear();
  ClusterMesh& dst = out.mesh;
  const bool ids = !src.vertex_ids.empty();
  const bool sources = !src.vertex_source.empty();
  const bool extra = src.extra.size() == src.vertices.size() && !src.extra.empty();
  u32 vertices = 0;
  u32 triangles = 0;
  for (const u32 c : map.clusters) {
    vertices += src.clusters[c].vertex_count;
    triangles += src.clusters[c].triangle_count;
  }
  dst.clusters.reserve(map.clusters.size());
  dst.vertices.reserve(vertices);
  dst.attributes.reserve(vertices);
  dst.triangles.reserve(triangles);
  dst.quantized.reserve(3 * vertices + 1);
  if (sources) dst.vertex_source.reserve(vertices);
  if (ids) dst.vertex_ids.reserve(vertices);
  if (extra) dst.extra.reserve(vertices);
  map.vertices.reserve(vertices);
  out.lod.reserve(map.clusters.size());
  u32 top_level = 0;
  for (const u32 c : map.clusters) {
    const ClusterDesc& from = src.clusters[c];
    ClusterDesc cluster = from;
    cluster.vertex_offset = dst.vertices.size();
    cluster.triangle_offset = dst.triangles.size();
    for (u32 v = from.vertex_offset; v < from.vertex_offset + from.vertex_count; ++v) {
      map.vertices.push_back(v);
      dst.vertices.push_back(src.vertices[v]);
      dst.attributes.push_back(src.attributes[v]);
      for (u32 k = 0; k < 3; ++k)
        dst.quantized.push_back(src.quantized[3 * v + k]);
      if (sources) dst.vertex_source.push_back(src.vertex_source[v]);
      if (ids) dst.vertex_ids.push_back(src.vertex_ids[v]);
      if (extra) dst.extra.push_back(src.extra[v]);
    }
    for (u32 t = from.triangle_offset; t < from.triangle_offset + from.triangle_count; ++t)
      dst.triangles.push_back(src.triangles[t]);
    dst.clusters.push_back(cluster);
    ClusterLodDesc lod = mesh.lod[c];
    u32 packed = coverage.empty() ? 0u : coverage[c];
    if (lod_acceptable_from(lod.own, lod.own_error, own_coverage(c), cut)) {
      // The frontier: drawn from nearer too, as the form's own finest.
      lod.own_error = 0.0f;
      lod.level = 0;
      packed &= 0xFFFF0000u;  // own coverage +0: acceptable at any distance
      ++map.frontier;
    }
    top_level = lod.level > top_level ? lod.level : top_level;
    out.lod.push_back(lod);
    if (!coverage.empty()) coverage_out.push_back(packed);
  }
  if (dst.quantized.size() % 2 != 0) dst.quantized.push_back(0);
  dst.quant_origin = src.quant_origin;
  dst.quant_scale = src.quant_scale;
  dst.vertex_id_source = src.vertex_id_source;
  dst.source_vertex_count = src.source_vertex_count;
  dst.source_triangle_count = src.source_triangle_count;
  out.level_cluster_counts.assign(top_level + 1, 0u);
  for (const ClusterLodDesc& lod : out.lod)
    ++out.level_cluster_counts[lod.level];
  out.group_count = mesh.group_count;
  for (u32 i = 0; i < out.lod.size(); ++i) {
    if (out.lod[i].level == 0) out.leaf_triangle_count += dst.clusters[i].triangle_count;
  }
  return true;
}

namespace {

// A transition's acceptance, as `lod_selects` with coverage and cluster_cull.slang both write it:
// two comparisons and no division, so a cluster with no coverage (+inf, which projects to +inf
// and is never at or under a threshold) takes exactly the one-threshold test.
bool transition_acceptable(Vec4 sphere, f32 error, f32 coverage, const LodView& view) noexcept {
  if (projected_error(sphere, error, view) <= view.threshold_px) return true;
  if (!(coverage < k_lod_terminal_error)) return false;
  return projected_error(sphere, coverage, view) <= view.foliage_px;
}

// A projected error over its threshold, with a threshold of zero (the finest cut) making every
// positive error infinitely too coarse and a zero error always fine.
f32 over_threshold(f32 projected, f32 threshold) noexcept {
  if (!(projected > 0.0f)) return 0.0f;
  if (!(threshold > 0.0f)) return k_lod_terminal_error;
  return projected / threshold;
}

}  // namespace

bool lod_selects(const ClusterLodDesc& lod, const LodView& view, u32 coverage) noexcept {
  return lod_selects(lod, view, coverage, 1.0f, 1.0f);
}

LodView lod_view_scaled(const LodView& view, f32 scale, bool foliage_only) noexcept {
  LodView scaled = view;
  if (!foliage_only) scaled.threshold_px = view.threshold_px * scale;
  scaled.foliage_px = view.foliage_px * scale;
  return scaled;
}

bool lod_selects(const ClusterLodDesc& lod, const LodView& view, u32 coverage, f32 own_scale,
                 f32 parent_scale, bool foliage_only) noexcept {
  return transition_acceptable(lod.own, lod.own_error, lod_coverage_own(coverage),
                               lod_view_scaled(view, own_scale, foliage_only)) &&
         !transition_acceptable(lod.parent, lod.parent_error, lod_coverage_parent(coverage),
                                lod_view_scaled(view, parent_scale, foliage_only));
}

f32 lod_occluded_scale(f32 hidden, f32 k) noexcept {
  if (!(k > 0.0f)) return 1.0f;
  const f32 h = hidden > 0.0f ? (hidden < 1.0f ? hidden : 1.0f) : 0.0f;
  return 1.0f + k * h;
}

f32 lod_fade_split(Vec4 sphere, f32 error, f32 coverage, const LodView& view,
                   bool step_shift) noexcept {
  const bool fading = view.fade > 0.0f && coverage < k_lod_terminal_error;
  if (!fading) return transition_acceptable(sphere, error, coverage, view) ? 0.0f : 1.0f;
  const f32 geometric = over_threshold(projected_error(sphere, error, view), view.threshold_px);
  const f32 foliage = over_threshold(projected_error(sphere, coverage, view), view.foliage_px);
  const f32 ratio = geometric < foliage ? geometric : foliage;
  if (step_shift) return ratio <= 1.0f - view.fade ? 0.0f : 1.0f;
  const f32 split = (ratio - (1.0f - view.fade)) / (2.0f * view.fade);
  return split < 0.0f ? 0.0f : (split > 1.0f ? 1.0f : split);
}

u32 select_lod(const ClusterLodMesh& mesh, const LodView& view, std::span<const u32> coverage,
               Vector<u32>& out) {
  if (coverage.size() != mesh.lod.size()) return select_lod(mesh, view, out);
  u32 count = 0;
  for (u32 i = 0; i < mesh.lod.size(); ++i) {
    if (lod_selects(mesh.lod[i], view, coverage[i])) {
      out.push_back(i);
      ++count;
    }
  }
  return count;
}

u32 select_lod(const ClusterLodMesh& mesh, const LodView& view, Vector<u32>& out) {
  u32 count = 0;
  for (u32 i = 0; i < mesh.lod.size(); ++i) {
    if (lod_selects(mesh.lod[i], view)) {
      out.push_back(i);
      ++count;
    }
  }
  return count;
}

u32 select_lod_raw(const ClusterLodMesh& mesh, f32 threshold, Vector<u32>& out) {
  u32 count = 0;
  for (u32 i = 0; i < mesh.lod.size(); ++i) {
    const ClusterLodDesc& lod = mesh.lod[i];
    if (lod.own_error <= threshold && lod.parent_error > threshold) {
      out.push_back(i);
      ++count;
    }
  }
  return count;
}

bool merge_cluster_lod(std::span<const ClusterLodMesh> parts, ClusterLodMesh& out,
                       Vector<u32>* part_of_cluster, std::string* error) {
  out = ClusterLodMesh{};
  if (part_of_cluster != nullptr) part_of_cluster->clear();
  if (parts.empty()) {
    if (error != nullptr) *error = "merge_cluster_lod: no parts";
    return false;
  }
  u32 levels = 0;
  for (const ClusterLodMesh& part : parts) {
    if (part.mesh.clusters.empty() || part.lod.size() != part.mesh.clusters.size() ||
        part.mesh.vertex_source.size() != part.mesh.vertices.size()) {
      if (error != nullptr) *error = "merge_cluster_lod: a part is empty or inconsistent";
      return false;
    }
    levels = part.level_cluster_counts.size() > levels ? part.level_cluster_counts.size() : levels;
  }
  out.level_cluster_counts.resize(levels, 0u);

  // Streams first, remembering where each part landed.
  const u32 part_count = static_cast<u32>(parts.size());
  Vector<u32> vertex_base(part_count);
  Vector<u32> triangle_base(part_count);
  Vector<u32> group_base(part_count);
  const bool attributes = parts[0].mesh.attributes.size() == parts[0].mesh.vertices.size();
  // The parts of one mesh share one skin, so the merged palette is the widest of them; a part
  // with no bindings contributes default ones (`SkinBinding{}`: joint 0, weight 1) rather than
  // shortening the stream, exactly as it does for attributes. **Any** part with bindings makes the
  // merge skinned, not the first: a glTF mesh whose first primitive is unskinned and whose second
  // is skinned kept no stream at all before. Such a primitive follows joint 0 — the stream has one
  // record a vertex, and the default record binds rigidly to the palette's first joint, which in a
  // parent-first palette is the skin's root (docs/subsystems/geometry.md, "A scene's meshes").
  const bool skinned = any_skin(parts);
  // **The parts of one mesh share one set of morph channels**, by index: glTF requires every
  // primitive of a mesh to have the same number of targets, and the mesh's weights array drives
  // all of them, so channel k means the same expression in every part. The scales are computed
  // from the *whole* source channel (`fill_cluster_morph`), so every part quantized channel k on
  // the same grid and the merge is a concatenation with no requantization — but only if the parts
  // really do agree, so that is checked rather than assumed.
  u32 morph_channels = 0;
  const ClusterMesh* morph_template = nullptr;
  for (const ClusterLodMesh& whole : parts) {
    if (whole.mesh.morph_channels.empty()) continue;
    if (morph_template == nullptr) {
      morph_template = &whole.mesh;
      morph_channels = whole.mesh.morph_channels.size();
      continue;
    }
    if (whole.mesh.morph_channels.size() != morph_channels) {
      if (error != nullptr)
        *error = "merge_cluster_lod: the parts disagree about how many morph channels they have";
      return false;
    }
    for (u32 c = 0; c < morph_channels; ++c) {
      const MorphChannel& a = morph_template->morph_channels[c];
      const MorphChannel& b = whole.mesh.morph_channels[c];
      if (a.position_scale != b.position_scale || a.normal_scale != b.normal_scale) {
        if (error != nullptr)
          *error = "merge_cluster_lod: two parts quantized one morph channel differently";
        return false;
      }
    }
  }
  // **The parts of one mesh share one id space** — they were built over the same welded source — so
  // the ids concatenate unchanged, and the parts must agree about where they came from. A part
  // built without ids contributes `k_no_vertex_id` rather than shortening the stream, as a part
  // with no bindings contributes default ones.
  VertexIdSource id_source = VertexIdSource::none;
  for (const ClusterLodMesh& whole : parts) {
    const VertexIdSource source = whole.mesh.vertex_id_source;
    if (source == VertexIdSource::none) continue;
    if (id_source != VertexIdSource::none && source != id_source) {
      if (error != nullptr)
        *error = "merge_cluster_lod: the parts disagree about where their vertex ids came from";
      return false;
    }
    id_source = source;
  }
  out.mesh.vertex_id_source = id_source;
  // An imported asset's tangent, second-UV and colour records (G2): carried when any part has them.
  bool any_extra = false;
  for (const ClusterLodMesh& whole : parts)
    any_extra = any_extra || !whole.mesh.extra.empty();
  bool morph_normals = false;
  if (morph_template != nullptr) {
    out.mesh.morph_channels.append(std::span<const MorphChannel>(
        morph_template->morph_channels.data(), morph_template->morph_channels.size()));
    out.mesh.morph_names = morph_template->morph_names;
    for (const ClusterLodMesh& whole : parts) {
      morph_normals = morph_normals || !whole.mesh.morph_normal_deltas.empty();
      // The largest displacement of a channel is the largest over every part that carries it.
      for (u32 c = 0; c < whole.mesh.morph_channels.size() && c < morph_channels; ++c) {
        MorphChannel& into = out.mesh.morph_channels[c];
        const MorphChannel& from = whole.mesh.morph_channels[c];
        if (from.max_displacement > into.max_displacement)
          into.max_displacement = from.max_displacement;
      }
    }
  }
  for (u32 p = 0; p < part_count; ++p) {
    const ClusterMesh& part = parts[p].mesh;
    if (skinned) {
      out.mesh.skin_joint_count = part.skin_joint_count > out.mesh.skin_joint_count
                                      ? part.skin_joint_count
                                      : out.mesh.skin_joint_count;
    }
    vertex_base[p] = out.mesh.vertices.size();
    triangle_base[p] = out.mesh.triangles.size();
    group_base[p] = out.group_count;
    const u32 source_base = out.mesh.source_vertex_count;
    out.mesh.vertices.append(std::span<const Vec3>(part.vertices.data(), part.vertices.size()));
    for (const u32 source : part.vertex_source)
      out.mesh.vertex_source.push_back(source_base + source);
    if (attributes && part.attributes.size() == part.vertices.size()) {
      out.mesh.attributes.append(
          std::span<const VertexAttributes>(part.attributes.data(), part.attributes.size()));
    } else {
      out.mesh.attributes.resize(out.mesh.vertices.size(), VertexAttributes{});
    }
    if (skinned && part.skin.size() == part.vertices.size()) {
      out.mesh.skin.append(std::span<const SkinBinding>(part.skin.data(), part.skin.size()));
    } else if (skinned) {
      out.mesh.skin.resize(out.mesh.vertices.size(), SkinBinding{});
    }
    append_vertex_ids(out.mesh, part, id_source != VertexIdSource::none);
    append_vertex_extra(out.mesh, part, any_extra);
    out.mesh.triangles.append(std::span<const u32>(part.triangles.data(), part.triangles.size()));
    out.mesh.source_vertex_count += part.source_vertex_count;
    out.mesh.source_triangle_count += part.source_triangle_count;
    out.group_count += parts[p].group_count;
    out.leaf_triangle_count += parts[p].leaf_triangle_count;
    for (u32 level = 0; level < parts[p].level_cluster_counts.size(); ++level)
      out.level_cluster_counts[level] += parts[p].level_cluster_counts[level];
  }

  // Clusters: every part's leaves, then everything else, in part order. The morph directory is
  // filled in that same order, which is what keeps the delta runs in slice order.
  if (morph_template != nullptr) out.mesh.morph_cluster_slices.push_back(0);
  // The parts' coverage errors (`ClusterLodMesh::coverage`), when any part has them: a part
  // without reads none, which is the geometric rule for its clusters.
  bool any_coverage = false;
  for (u32 p = 0; p < part_count; ++p)
    any_coverage = any_coverage || (!parts[p].coverage.empty() &&
                                    parts[p].coverage.size() == parts[p].mesh.clusters.size());
  for (u32 pass = 0; pass < 2; ++pass) {
    for (u32 p = 0; p < part_count; ++p) {
      const ClusterLodMesh& part = parts[p];
      const bool part_coverage = part.coverage.size() == part.mesh.clusters.size();
      for (u32 i = 0; i < part.mesh.clusters.size(); ++i) {
        const bool leaf = part.lod[i].level == 0;
        if (leaf != (pass == 0)) continue;
        ClusterDesc desc = part.mesh.clusters[i];
        desc.vertex_offset += vertex_base[p];
        desc.triangle_offset += triangle_base[p];
        ClusterLodDesc lod = part.lod[i];
        lod.group += group_base[p];
        out.mesh.clusters.push_back(desc);
        out.lod.push_back(lod);
        if (any_coverage)
          out.coverage.push_back(part_coverage ? part.coverage[i] : k_lod_no_coverage);
        if (morph_template != nullptr) {
          append_cluster_morph(out.mesh, part.mesh, i, 0, morph_normals);
          out.mesh.morph_cluster_slices.push_back(out.mesh.morph_slices.size());
        }
        if (part_of_cluster != nullptr) part_of_cluster->push_back(p);
      }
    }
  }
  pad_morph_streams(out.mesh);
  quantize_positions(out.mesh);  // one grid over all the parts, not one per part
  return true;
}

bool merge_cluster_meshes(std::span<const ClusterLodMesh> parts, ClusterLodMesh& out,
                          Vector<ClusterMeshPart>& parts_out, std::string* error,
                          ClusterOrder order, Vector<u32>* source_of_cluster) {
  out = ClusterLodMesh{};
  parts_out.clear();
  if (source_of_cluster != nullptr) source_of_cluster->clear();
  if (parts.empty()) {
    if (error != nullptr) *error = "merge_cluster_meshes: no meshes";
    return false;
  }
  u32 levels = 0;
  for (const ClusterLodMesh& part : parts) {
    if (part.mesh.clusters.empty() || part.lod.size() != part.mesh.clusters.size() ||
        part.mesh.vertex_source.size() != part.mesh.vertices.size() ||
        part.mesh.quantized.size() < part.mesh.vertices.size() * 3) {
      if (error != nullptr) *error = "merge_cluster_meshes: a mesh is empty or inconsistent";
      return false;
    }
    levels = part.level_cluster_counts.size() > levels ? part.level_cluster_counts.size() : levels;
  }
  out.level_cluster_counts.resize(levels, 0u);
  parts_out.resize(static_cast<u32>(parts.size()));

  const bool attributes = parts[0].mesh.attributes.size() == parts[0].mesh.vertices.size();
  // Separate meshes have separate skeletons, so the merged palette width is only the bound the
  // validator needs: a mesh's own bindings index its own skin, and the renderer reaches the right
  // bone matrices through the *instance's* deform record, never through the merged mesh.
  //
  // **The stream is kept when any mesh has one**, not when the first does: a scene whose first
  // mesh is a rigid palm and whose second is a character used to merge with no stream at all, and
  // the load then refused the character ("an instance is skinned, but its mesh carries no skin
  // binding stream"). A rigid mesh's run is the default record (joint 0, weight 1), and that is
  // padding and nothing more: an instance of a rigid mesh names no joints, so its deform record (if
  // it has one at all) has no skinning stage, the pool never reads those bindings, and no pose can
  // reach its vertices.
  const bool skinned = any_skin(parts);
  // Separate meshes have separate channel sets, so the merged array is their concatenation and a
  // mesh's run is recorded on its `ClusterMeshPart` — the same split the skin palette gets, and
  // for the same reason: an instance reaches its own weights through its deform record, never
  // through the merged mesh.
  bool morph_normals = false;
  bool any_morph = false;
  for (const ClusterLodMesh& whole : parts) {
    any_morph = any_morph || !whole.mesh.morph_channels.empty();
    morph_normals = morph_normals || !whole.mesh.morph_normal_deltas.empty();
  }
  // Separate meshes have separate id spaces, so the merged stream is their concatenation and each
  // mesh says on its own `ClusterMeshPart` where its ids came from. The merged mesh's own source is
  // the first mesh's that has one — what a one-mesh merge needs, and all a whole-scene validator
  // can ask of it; a caller that wants to know what an id means asks the part.
  bool any_ids = false;
  for (const ClusterLodMesh& whole : parts) {
    if (whole.mesh.vertex_id_source == VertexIdSource::none || whole.mesh.vertex_ids.empty())
      continue;
    if (!any_ids) out.mesh.vertex_id_source = whole.mesh.vertex_id_source;
    any_ids = true;
  }
  // **A scene's merge drops an imported mesh's tangent, second-UV and colour records** (G2's
  // `extra`; review finding A3). The stream is parallel to the vertices, so carrying it here would
  // pad every vertex of every other mesh with a 12-byte "none" record: one imported prop with
  // tangents in the forest's 116 million vertices was 1.4 GB at load, for data nothing reads after
  // the merge. Each mesh's own container keeps its section 49, and the renderer's reader (G2's
  // second leg) brings a per-mesh stream. `merge_cluster_lod`, which joins the parts of one mesh,
  // still carries it.
  // **Every stream reserved at its merged size before the first append** (roadmap T129;
  // docs/experiments/memory-commit-accounting.md). The merge is the scene's whole geometry — on the
  // forest 116 million vertices, a 1.3 GB position array — and appending part by part grew each
  // array by doubling: every step copied what was there and committed a block up to twice the
  // size, whose tail nothing ever wrote. Sized once, the blocks are the bytes the merge holds and
  // nothing is copied twice.
  {
    u32 vertices = 0, triangles = 0, clusters = 0;
    for (const ClusterLodMesh& whole : parts) {
      vertices += whole.mesh.vertices.size();
      triangles += whole.mesh.triangles.size();
      clusters += whole.mesh.clusters.size();
    }
    out.mesh.vertices.reserve(vertices);
    out.mesh.vertex_source.reserve(vertices);
    if (attributes) out.mesh.attributes.reserve(vertices);
    if (skinned) out.mesh.skin.reserve(vertices);
    if (any_ids) out.mesh.vertex_ids.reserve(vertices);
    out.mesh.triangles.reserve(triangles);
    out.mesh.quantized.reserve(vertices * 3 + 1);
    out.mesh.clusters.reserve(clusters);
    out.lod.reserve(clusters);
    if (source_of_cluster != nullptr) source_of_cluster->reserve(clusters);
  }
  if (any_morph) out.mesh.morph_cluster_slices.push_back(0);
  for (u32 p = 0; p < parts.size(); ++p) {
    const ClusterLodMesh& whole = parts[p];
    const ClusterMesh& part = whole.mesh;
    if (skinned) {
      out.mesh.skin_joint_count = part.skin_joint_count > out.mesh.skin_joint_count
                                      ? part.skin_joint_count
                                      : out.mesh.skin_joint_count;
    }
    ClusterMeshPart& info = parts_out[p];
    info.first_vertex = out.mesh.vertices.size();
    info.first_cluster = out.mesh.clusters.size();
    info.cluster_count = part.clusters.size();
    info.leaf_cluster_count =
        whole.level_cluster_counts.empty() ? 0 : whole.level_cluster_counts[0];
    info.quant_origin = part.quant_origin;
    info.quant_scale = part.quant_scale;
    info.first_morph_channel = out.mesh.morph_channels.size();
    info.morph_channel_count = part.morph_channels.size();
    const u32 morph_base = info.first_morph_channel;
    out.mesh.morph_channels.append(
        std::span<const MorphChannel>(part.morph_channels.data(), part.morph_channels.size()));
    for (const std::string& name : part.morph_names)
      out.mesh.morph_names.push_back(name);
    const u32 vertex_base = info.first_vertex;
    const u32 triangle_base = out.mesh.triangles.size();
    const u32 group_base = out.group_count;
    const u32 source_base = out.mesh.source_vertex_count;
    out.mesh.vertices.append(std::span<const Vec3>(part.vertices.data(), part.vertices.size()));
    for (const u32 source : part.vertex_source)
      out.mesh.vertex_source.push_back(source_base + source);
    if (attributes && part.attributes.size() == part.vertices.size()) {
      out.mesh.attributes.append(
          std::span<const VertexAttributes>(part.attributes.data(), part.attributes.size()));
    } else {
      out.mesh.attributes.resize(out.mesh.vertices.size(), VertexAttributes{});
    }
    if (skinned && part.skin.size() == part.vertices.size()) {
      out.mesh.skin.append(std::span<const SkinBinding>(part.skin.data(), part.skin.size()));
    } else if (skinned) {
      out.mesh.skin.resize(out.mesh.vertices.size(), SkinBinding{});
    }
    info.vertex_id_source =
        part.vertex_ids.size() == part.vertices.size() && !part.vertex_ids.empty()
            ? part.vertex_id_source
            : VertexIdSource::none;
    append_vertex_ids(out.mesh, part, any_ids);
    out.mesh.triangles.append(std::span<const u32>(part.triangles.data(), part.triangles.size()));
    // Three u16 a vertex, the mesh's own padding dropped so the merged index is 3 * vertex.
    out.mesh.quantized.append(
        std::span<const u16>(part.quantized.data(), part.vertices.size() * 3));
    out.mesh.source_vertex_count += part.source_vertex_count;
    out.mesh.source_triangle_count += part.source_triangle_count;
    out.group_count += whole.group_count;
    out.leaf_triangle_count += whole.leaf_triangle_count;
    for (u32 level = 0; level < whole.level_cluster_counts.size(); ++level)
      out.level_cluster_counts[level] += whole.level_cluster_counts[level];
    // This mesh's clusters, contiguously.
    auto emit = [&](u32 i) {
      ClusterDesc desc = part.clusters[i];
      desc.vertex_offset += vertex_base;
      desc.triangle_offset += triangle_base;
      ClusterLodDesc lod = whole.lod[i];
      lod.group += group_base;
      out.mesh.clusters.push_back(desc);
      out.lod.push_back(lod);
      if (source_of_cluster != nullptr) source_of_cluster->push_back(i);
      if (any_morph) {
        append_cluster_morph(out.mesh, part, i, morph_base, morph_normals);
        out.mesh.morph_cluster_slices.push_back(out.mesh.morph_slices.size());
      }
    };
    if (order == ClusterOrder::keep) {
      // A paged mesh is already in the one order its page table describes, so the merge only
      // shifts the offsets. Its leaves are wherever the layout put them, which is at the end.
      u32 first_leaf = part.clusters.size();
      for (u32 i = 0; i < part.clusters.size(); ++i) {
        if (whole.lod[i].level == 0 && i < first_leaf) first_leaf = i;
        emit(i);
      }
      info.first_leaf_cluster = first_leaf < part.clusters.size() ? first_leaf : 0;
    } else {
      // The scene convention: this mesh's own leaves first, so a direct draw of them is one range.
      for (u32 pass = 0; pass < 2; ++pass) {
        for (u32 i = 0; i < part.clusters.size(); ++i) {
          if ((whole.lod[i].level == 0) == (pass == 0)) emit(i);
        }
      }
    }
  }
  // The shaders read the last triple as two whole 32-bit words.
  if (out.mesh.quantized.size() % 2 != 0) out.mesh.quantized.push_back(0);
  pad_morph_streams(out.mesh);
  out.mesh.quant_origin = parts[0].mesh.quant_origin;
  out.mesh.quant_scale = parts[0].mesh.quant_scale;
  return true;
}

namespace {

// The closest point of triangle (a, b, c) to p, with its barycentric coordinates. Ericson,
// "Real-Time Collision Detection", the region test written out rather than solved, because the
// degenerate cases (a sliver, a point) have to answer *something* and this form does.
Vec3 closest_on_triangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c, f32& wa, f32& wb, f32& wc) noexcept {
  const Vec3 ab = b - a;
  const Vec3 ac = c - a;
  const Vec3 ap = p - a;
  const f32 d1 = dot(ab, ap);
  const f32 d2 = dot(ac, ap);
  if (d1 <= 0.0f && d2 <= 0.0f) {
    wa = 1.0f;
    wb = 0.0f;
    wc = 0.0f;
    return a;
  }
  const Vec3 bp = p - b;
  const f32 d3 = dot(ab, bp);
  const f32 d4 = dot(ac, bp);
  if (d3 >= 0.0f && d4 <= d3) {
    wa = 0.0f;
    wb = 1.0f;
    wc = 0.0f;
    return b;
  }
  const f32 vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
    const f32 denominator = d1 - d3;
    const f32 v = denominator != 0.0f ? d1 / denominator : 0.0f;
    wa = 1.0f - v;
    wb = v;
    wc = 0.0f;
    return a + ab * v;
  }
  const Vec3 cp = p - c;
  const f32 d5 = dot(ab, cp);
  const f32 d6 = dot(ac, cp);
  if (d6 >= 0.0f && d5 <= d6) {
    wa = 0.0f;
    wb = 0.0f;
    wc = 1.0f;
    return c;
  }
  const f32 vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
    const f32 denominator = d2 - d6;
    const f32 w = denominator != 0.0f ? d2 / denominator : 0.0f;
    wa = 1.0f - w;
    wb = 0.0f;
    wc = w;
    return a + ac * w;
  }
  const f32 va = d3 * d6 - d5 * d4;
  if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
    const f32 denominator = (d4 - d3) + (d5 - d6);
    const f32 w = denominator != 0.0f ? (d4 - d3) / denominator : 0.0f;
    wa = 0.0f;
    wb = 1.0f - w;
    wc = w;
    return b + (c - b) * w;
  }
  const f32 total = va + vb + vc;
  if (!(total > 0.0f)) {
    wa = 1.0f;
    wb = 0.0f;
    wc = 0.0f;
    return a;
  }
  const f32 inverse = 1.0f / total;
  wa = va * inverse;
  wb = vb * inverse;
  wc = vc * inverse;
  return a + ab * wb + ac * wc;
}

// A triangle of the cut, with the attributes the GPU would interpolate over it.
struct SampledTriangle {
  Vec3 p[3];
  Vec2 uv[3];
  Vec3 normal[3];
};

f32 percentile(Vector<f32>& values, f32 fraction) {
  if (values.empty()) return 0.0f;
  std::sort(values.begin(), values.end());
  const f32 position = fraction * static_cast<f32>(values.size() - 1);
  u32 index = static_cast<u32>(position);
  if (index >= values.size()) index = values.size() - 1;
  return values[index];
}

}  // namespace

bool measure_lod_attribute_error(const ClusterLodMesh& mesh, std::span<const u32> cut,
                                 const AttributeErrorOptions& options, AttributeError& out,
                                 std::string* error) {
  out = AttributeError{};
  auto fail = [&](const char* what) {
    if (error != nullptr) *error = what;
    return false;
  };
  if (mesh.mesh.clusters.empty()) return fail("measure_lod_attribute_error: the mesh is empty");
  if (mesh.mesh.attributes.size() != mesh.mesh.vertices.size())
    return fail("measure_lod_attribute_error: the mesh carries no attributes");
  if (cut.empty()) return fail("measure_lod_attribute_error: the cut is empty");
  if (!(options.uv_texels > 0.0f))
    return fail("measure_lod_attribute_error: the atlas size must be positive");
  for (const u32 c : cut) {
    if (c >= mesh.mesh.clusters.size())
      return fail("measure_lod_attribute_error: a cut entry is out of range");
  }

  auto gather = [&mesh](const ClusterDesc& desc, u32 triangle, SampledTriangle& tri) {
    const u32 packed = mesh.mesh.triangles[desc.triangle_offset + triangle];
    for (u32 k = 0; k < 3; ++k) {
      const u32 at = desc.vertex_offset + ClusterMesh::unpack(packed, k);
      tri.p[k] = mesh.mesh.vertices[at];
      tri.uv[k] = decode_half2(mesh.mesh.attributes[at].uv_half2);
      tri.normal[k] = decode_normal_oct(mesh.mesh.attributes[at].normal_oct);
    }
  };

  // The coarse surface, flattened once.
  Vector<SampledTriangle> coarse;
  u32 coarse_count = 0;
  for (const u32 c : cut)
    coarse_count += mesh.mesh.clusters[c].triangle_count;
  if (coarse_count == 0) return fail("measure_lod_attribute_error: the cut has no triangles");
  coarse.reserve(coarse_count);
  for (const u32 c : cut) {
    const ClusterDesc& desc = mesh.mesh.clusters[c];
    for (u32 t = 0; t < desc.triangle_count; ++t) {
      SampledTriangle tri;
      gather(desc, t, tri);
      coarse.push_back(tri);
    }
  }

  // A uniform grid over the coarse triangles, in CSR form so the whole thing is three arrays and
  // no allocation per cell. About one triangle a cell, capped so a large mesh does not turn the
  // index into the cost.
  Vec3 lo = coarse[0].p[0];
  Vec3 hi = lo;
  for (const SampledTriangle& tri : coarse) {
    for (u32 k = 0; k < 3; ++k) {
      lo = Vec3{std::min(lo.x, tri.p[k].x), std::min(lo.y, tri.p[k].y), std::min(lo.z, tri.p[k].z)};
      hi = Vec3{std::max(hi.x, tri.p[k].x), std::max(hi.y, tri.p[k].y), std::max(hi.z, tri.p[k].z)};
    }
  }
  const f32 extent = std::max(std::max(hi.x - lo.x, hi.y - lo.y), std::max(hi.z - lo.z, 1e-6f));
  u32 axis_cells = static_cast<u32>(std::cbrt(static_cast<f64>(coarse.size())) + 1.0);
  if (axis_cells < 1) axis_cells = 1;
  if (axis_cells > 64) axis_cells = 64;
  const f32 cell = extent / static_cast<f32>(axis_cells);
  const f32 inverse_cell = 1.0f / cell;
  const u32 cell_count = axis_cells * axis_cells * axis_cells;
  auto cell_of = [&](f32 v, f32 origin) {
    const f32 raw = (v - origin) * inverse_cell;
    const i32 index = static_cast<i32>(std::floor(raw));
    return static_cast<u32>(
        index < 0
            ? 0
            : (index >= static_cast<i32>(axis_cells) ? static_cast<i32>(axis_cells) - 1 : index));
  };
  Vector<u32> cell_start(cell_count + 1, 0u);
  auto for_each_cell = [&](const SampledTriangle& tri, auto&& visit) {
    const f32 tlo[3] = {std::min(std::min(tri.p[0].x, tri.p[1].x), tri.p[2].x),
                        std::min(std::min(tri.p[0].y, tri.p[1].y), tri.p[2].y),
                        std::min(std::min(tri.p[0].z, tri.p[1].z), tri.p[2].z)};
    const f32 thi[3] = {std::max(std::max(tri.p[0].x, tri.p[1].x), tri.p[2].x),
                        std::max(std::max(tri.p[0].y, tri.p[1].y), tri.p[2].y),
                        std::max(std::max(tri.p[0].z, tri.p[1].z), tri.p[2].z)};
    const f32 origin[3] = {lo.x, lo.y, lo.z};
    const u32 first[3] = {cell_of(tlo[0], origin[0]), cell_of(tlo[1], origin[1]),
                          cell_of(tlo[2], origin[2])};
    const u32 last[3] = {cell_of(thi[0], origin[0]), cell_of(thi[1], origin[1]),
                         cell_of(thi[2], origin[2])};
    for (u32 z = first[2]; z <= last[2]; ++z) {
      for (u32 y = first[1]; y <= last[1]; ++y) {
        for (u32 x = first[0]; x <= last[0]; ++x)
          visit((z * axis_cells + y) * axis_cells + x);
      }
    }
  };
  for (const SampledTriangle& tri : coarse)
    for_each_cell(tri, [&](u32 index) { ++cell_start[index + 1]; });
  for (u32 i = 0; i < cell_count; ++i)
    cell_start[i + 1] += cell_start[i];
  Vector<u32> cell_items(cell_start[cell_count], 0u);
  Vector<u32> fill(cell_count, 0u);
  for (u32 t = 0; t < coarse.size(); ++t) {
    for_each_cell(coarse[t], [&](u32 index) {
      cell_items[cell_start[index] + fill[index]] = t;
      ++fill[index];
    });
  }

  // The fine surface: every level-0 triangle, or a fixed stride through them.
  Vector<u32> leaves;
  for (u32 c = 0; c < mesh.mesh.clusters.size(); ++c) {
    if (mesh.lod[c].level == 0) leaves.push_back(c);
  }
  if (leaves.empty()) return fail("measure_lod_attribute_error: the mesh has no level-0 clusters");
  u32 leaf_triangles = 0;
  for (const u32 c : leaves)
    leaf_triangles += mesh.mesh.clusters[c].triangle_count;
  u32 stride = 1;
  if (options.max_samples > 0 && leaf_triangles > options.max_samples)
    stride = (leaf_triangles + options.max_samples - 1) / options.max_samples;

  Vector<f32> uv_errors;
  Vector<f32> normal_errors;
  f64 uv_total = 0.0;
  f64 normal_total = 0.0;
  u32 walked = 0;
  for (const u32 c : leaves) {
    const ClusterDesc& desc = mesh.mesh.clusters[c];
    for (u32 t = 0; t < desc.triangle_count; ++t, ++walked) {
      if (walked % stride != 0) continue;
      SampledTriangle fine;
      gather(desc, t, fine);
      const Vec3 point = (fine.p[0] + fine.p[1] + fine.p[2]) * (1.0f / 3.0f);
      const Vec2 want_uv = (fine.uv[0] + fine.uv[1] + fine.uv[2]) * (1.0f / 3.0f);
      Vec3 want_normal = fine.normal[0] + fine.normal[1] + fine.normal[2];
      want_normal = length_squared(want_normal) > 1e-20f ? normalize(want_normal) : fine.normal[0];

      // Expanding shells of cells: stop as soon as the best distance is inside the shell already
      // searched, which is what makes this exact rather than approximate.
      const u32 base[3] = {cell_of(point.x, lo.x), cell_of(point.y, lo.y), cell_of(point.z, lo.z)};
      f32 best = 3.402823466e+38f;
      u32 best_triangle = ~u32{0};
      f32 best_w[3] = {0.0f, 0.0f, 0.0f};
      for (u32 radius = 0; radius <= axis_cells; ++radius) {
        const i32 r = static_cast<i32>(radius);
        for (i32 dz = -r; dz <= r; ++dz) {
          for (i32 dy = -r; dy <= r; ++dy) {
            for (i32 dx = -r; dx <= r; ++dx) {
              // Only the shell, not the ball: the interior was searched last round.
              if (radius > 0 && std::abs(dx) != r && std::abs(dy) != r && std::abs(dz) != r)
                continue;
              const i32 x = static_cast<i32>(base[0]) + dx;
              const i32 y = static_cast<i32>(base[1]) + dy;
              const i32 z = static_cast<i32>(base[2]) + dz;
              if (x < 0 || y < 0 || z < 0 || x >= static_cast<i32>(axis_cells) ||
                  y >= static_cast<i32>(axis_cells) || z >= static_cast<i32>(axis_cells)) {
                continue;
              }
              const u32 index =
                  (static_cast<u32>(z) * axis_cells + static_cast<u32>(y)) * axis_cells +
                  static_cast<u32>(x);
              for (u32 i = cell_start[index]; i < cell_start[index + 1]; ++i) {
                const u32 candidate = cell_items[i];
                const SampledTriangle& tri = coarse[candidate];
                f32 w[3];
                const Vec3 on =
                    closest_on_triangle(point, tri.p[0], tri.p[1], tri.p[2], w[0], w[1], w[2]);
                const f32 distance = length_squared(on - point);
                if (distance < best) {
                  best = distance;
                  best_triangle = candidate;
                  best_w[0] = w[0];
                  best_w[1] = w[1];
                  best_w[2] = w[2];
                }
              }
            }
          }
        }
        // Everything within `radius` cells of the sample's own cell has been searched, so nothing
        // outside can be nearer than `radius * cell` — which is what makes the shell walk exact
        // rather than a heuristic.
        const f32 safe = static_cast<f32>(radius) * cell;
        if (best_triangle != ~u32{0} && best <= safe * safe) break;
      }
      ++out.samples;
      if (best_triangle == ~u32{0}) {
        ++out.unmatched;
        continue;
      }
      const SampledTriangle& tri = coarse[best_triangle];
      const Vec2 got_uv = tri.uv[0] * best_w[0] + tri.uv[1] * best_w[1] + tri.uv[2] * best_w[2];
      Vec3 got_normal =
          tri.normal[0] * best_w[0] + tri.normal[1] * best_w[1] + tri.normal[2] * best_w[2];
      got_normal = length_squared(got_normal) > 1e-20f ? normalize(got_normal) : tri.normal[0];
      const Vec2 delta = got_uv - want_uv;
      const f32 uv_texels = std::sqrt(delta.x * delta.x + delta.y * delta.y) * options.uv_texels;
      const f32 cosine = std::max(-1.0f, std::min(1.0f, dot(got_normal, want_normal)));
      const f32 degrees = std::acos(cosine) * 180.0f / 3.14159265358979323846f;
      uv_errors.push_back(uv_texels);
      normal_errors.push_back(degrees);
      uv_total += static_cast<f64>(uv_texels);
      normal_total += static_cast<f64>(degrees);
      if (uv_texels > options.uv_tolerance_texels) ++out.uv_outliers;
      if (degrees > options.normal_tolerance_deg) ++out.normal_outliers;
      out.uv_max_texels = std::max(out.uv_max_texels, uv_texels);
      out.normal_max_deg = std::max(out.normal_max_deg, degrees);
    }
  }
  const u32 measured = uv_errors.size();
  if (measured > 0) {
    out.uv_mean_texels = static_cast<f32>(uv_total / static_cast<f64>(measured));
    out.normal_mean_deg = static_cast<f32>(normal_total / static_cast<f64>(measured));
    out.uv_p99_texels = percentile(uv_errors, 0.99f);
    out.normal_p99_deg = percentile(normal_errors, 0.99f);
  }
  return true;
}

bool validate_cluster_lod(const ClusterLodMesh& mesh, std::span<const u32> source_indices,
                          std::string* error) {
  auto fail = [&](const char* what) {
    if (error != nullptr) *error = what;
    return false;
  };
  if (mesh.mesh.clusters.size() != mesh.lod.size())
    return fail("lod table does not match clusters");
  if (mesh.level_cluster_counts.empty()) return fail("no levels");
  // The morph directory is over *every* cluster of the DAG, so it is checked here rather than on
  // the level-0 view below, whose clusters are a prefix of them.
  if (!validate_morph_stream(mesh.mesh, error)) return false;

  // Level 0 covers the source exactly once: reuse the plain validator on a level-0 view.
  ClusterMesh leaves;
  leaves.source_vertex_count = mesh.mesh.source_vertex_count;
  leaves.source_triangle_count = mesh.mesh.source_triangle_count;
  leaves.vertices = mesh.mesh.vertices;
  leaves.vertex_source = mesh.mesh.vertex_source;
  leaves.triangles = mesh.mesh.triangles;
  leaves.quantized = mesh.mesh.quantized;  // the whole mesh's grid, shared by every level
  leaves.quant_origin = mesh.mesh.quant_origin;
  leaves.quant_scale = mesh.mesh.quant_scale;
  leaves.skin = mesh.mesh.skin;  // every level's bindings, so validate_clusters checks them
  leaves.skin_joint_count = mesh.mesh.skin_joint_count;
  // Every level's ids, for the same reason: one id per source vertex is a property of the whole
  // DAG, and a coarse cluster's copy of a vertex is where a broken merge or page layout would show.
  leaves.vertex_ids = mesh.mesh.vertex_ids;
  leaves.vertex_id_source = mesh.mesh.vertex_id_source;
  leaves.extra = mesh.mesh.extra;  // G2: checked parallel to the vertices
  for (u32 i = 0; i < mesh.mesh.clusters.size(); ++i) {
    if (mesh.lod[i].level == 0) leaves.clusters.push_back(mesh.mesh.clusters[i]);
  }
  ClusterBuildOptions limits;
  limits.max_vertices = 255;
  limits.max_triangles = 512;
  std::string leaf_error;
  if (!validate_clusters(leaves, source_indices, limits, &leaf_error)) {
    if (error != nullptr) *error = "level 0: " + leaf_error;
    return false;
  }
  if (mesh.leaf_triangle_count != mesh.mesh.source_triangle_count)
    return fail("leaf triangle count mismatch");

  f32 max_finite_error = 0.0f;
  for (u32 i = 0; i < mesh.lod.size(); ++i) {
    const ClusterLodDesc& lod = mesh.lod[i];
    if (!(lod.own_error <= lod.parent_error)) return fail("own error exceeds parent error");
    if (lod.level == 0 && lod.own_error != 0.0f)
      return fail("level 0 cluster with nonzero own error");
    if (lod.group >= mesh.group_count) return fail("group index out of range");
    if (lod.level >= mesh.level_cluster_counts.size()) return fail("level out of range");
    if (lod.parent_error < k_lod_terminal_error && lod.parent_error > max_finite_error) {
      max_finite_error = lod.parent_error;
    }
  }

  // The raw cut is never empty and never grows with the threshold.
  Vector<u32> cut;
  u32 previous_triangles = ~u32{0};
  for (u32 step = 0; step <= 8; ++step) {
    cut.clear();
    const f32 threshold = max_finite_error * static_cast<f32>(step) / 8.0f * 1.001f;
    if (select_lod_raw(mesh, threshold, cut) == 0) return fail("empty cut");
    u32 triangles = 0;
    for (const u32 c : cut)
      triangles += mesh.mesh.clusters[c].triangle_count;
    if (triangles > previous_triangles) return fail("cut triangle count grew with the threshold");
    previous_triangles = triangles;
  }
  return true;
}

namespace {

// A triangle's shadow on the ground plane, in double, with its box.
struct Shadow {
  f64 x[3];
  f64 y[3];
  f64 lo_x, hi_x, lo_y, hi_y;
};

// Whether two shadows share area: no edge normal of either separates them by more than a rounding.
// Coordinates are taken from `a`'s first corner so the products stay the size of the triangles.
bool shadows_overlap(const Shadow& a, const Shadow& b) noexcept {
  const f64 ox = a.x[0], oy = a.y[0];
  f64 ax[3], ay[3], bx[3], by[3];
  for (u32 k = 0; k < 3; ++k) {
    ax[k] = a.x[k] - ox;
    ay[k] = a.y[k] - oy;
    bx[k] = b.x[k] - ox;
    by[k] = b.y[k] - oy;
  }
  f64 span = 0.0;
  for (u32 k = 0; k < 3; ++k) {
    span = std::max(span, std::max(std::abs(ax[k]), std::abs(ay[k])));
    span = std::max(span, std::max(std::abs(bx[k]), std::abs(by[k])));
  }
  for (u32 side = 0; side < 2; ++side) {
    const f64* ex = side == 0 ? ax : bx;
    const f64* ey = side == 0 ? ay : by;
    for (u32 e = 0; e < 3; ++e) {
      const u32 f = e == 2 ? 0 : e + 1;
      const f64 nx = -(ey[f] - ey[e]);
      const f64 ny = ex[f] - ex[e];
      const f64 tolerance = 1e-9 * span * (std::abs(nx) + std::abs(ny));
      f64 a_lo = 0.0, a_hi = 0.0, b_lo = 0.0, b_hi = 0.0;
      for (u32 k = 0; k < 3; ++k) {
        const f64 pa = ax[k] * nx + ay[k] * ny;
        const f64 pb = bx[k] * nx + by[k] * ny;
        a_lo = k == 0 ? pa : std::min(a_lo, pa);
        a_hi = k == 0 ? pa : std::max(a_hi, pa);
        b_lo = k == 0 ? pb : std::min(b_lo, pb);
        b_hi = k == 0 ? pb : std::max(b_hi, pb);
      }
      if (a_hi <= b_lo + tolerance || b_hi <= a_lo + tolerance) return false;
    }
  }
  return true;
}

}  // namespace

bool count_lod_folds(const ClusterLodMesh& mesh, Vec3 up_in, bool on_grid,
                     Vector<LodFoldLevel>& out, std::string* error) {
  out.clear();
  auto fail = [&](const char* what) {
    if (error != nullptr) *error = what;
    return false;
  };
  if (mesh.mesh.clusters.empty() || mesh.lod.size() != mesh.mesh.clusters.size())
    return fail("count_lod_folds: the mesh is empty or has no LOD");
  const Up up = normalized_up(up_in);
  if (up.x == 0.0 && up.y == 0.0 && up.z == 0.0) return fail("count_lod_folds: `up` is zero");
  if (on_grid && mesh.mesh.quantized.size() < mesh.mesh.vertices.size() * 3)
    return fail("count_lod_folds: the mesh has no 16-bit grid");

  // Two directions across `up`: the ground plane the shadows are drawn on.
  const bool x_helper = std::abs(up.x) < 0.9;
  f64 tx = x_helper ? 1.0 : 0.0, ty = x_helper ? 0.0 : 1.0, tz = 0.0;
  const f64 along = tx * up.x + ty * up.y + tz * up.z;
  tx -= along * up.x;
  ty -= along * up.y;
  tz -= along * up.z;
  const f64 tlen = std::sqrt(tx * tx + ty * ty + tz * tz);
  tx /= tlen;
  ty /= tlen;
  tz /= tlen;
  const f64 sx = up.y * tz - up.z * ty, sy = up.z * tx - up.x * tz, sz = up.x * ty - up.y * tx;

  out.resize(mesh.level_cluster_counts.size());
  Vector<Shadow> shadows;
  Vector<u8> flat;  // a triangle standing on edge: its shadow has no area
  Vector<u8> overlapping;
  for (u32 c = 0; c < mesh.mesh.clusters.size(); ++c) {
    const ClusterDesc& desc = mesh.mesh.clusters[c];
    const u32 level = mesh.lod[c].level;
    if (level >= out.size()) out.resize(level + 1);
    LodFoldLevel& counts = out[level];
    ++counts.clusters;
    counts.triangles += desc.triangle_count;
    shadows.resize(desc.triangle_count);
    flat.resize(desc.triangle_count, u8{0});
    overlapping.resize(desc.triangle_count, u8{0});
    bool folded = false;
    for (u32 t = 0; t < desc.triangle_count; ++t) {
      const u32 packed = mesh.mesh.triangles[desc.triangle_offset + t];
      Vec3 p[3];
      for (u32 k = 0; k < 3; ++k) {
        const u32 v = desc.vertex_offset + ClusterMesh::unpack(packed, k);
        p[k] = on_grid ? dequantize_position(mesh.mesh, v) : mesh.mesh.vertices[v];
      }
      const Facing facing = facing_of(p[0], p[1], p[2], up);
      if (facing == Facing::away) {
        ++counts.upside_down;
        folded = true;
      }
      flat[t] = facing == Facing::on_edge ? u8{1} : u8{0};
      Shadow& s = shadows[t];
      for (u32 k = 0; k < 3; ++k) {
        const f64 x = static_cast<f64>(p[k].x), y = static_cast<f64>(p[k].y),
                  z = static_cast<f64>(p[k].z);
        s.x[k] = x * tx + y * ty + z * tz;
        s.y[k] = x * sx + y * sy + z * sz;
      }
      s.lo_x = std::min(s.x[0], std::min(s.x[1], s.x[2]));
      s.hi_x = std::max(s.x[0], std::max(s.x[1], s.x[2]));
      s.lo_y = std::min(s.y[0], std::min(s.y[1], s.y[2]));
      s.hi_y = std::max(s.y[0], std::max(s.y[1], s.y[2]));
      overlapping[t] = 0;
    }
    for (u32 i = 0; i < desc.triangle_count; ++i) {
      if (flat[i] != 0) continue;
      for (u32 j = i + 1; j < desc.triangle_count; ++j) {
        if (flat[j] != 0) continue;
        const Shadow& a = shadows[i];
        const Shadow& b = shadows[j];
        if (a.hi_x <= b.lo_x || b.hi_x <= a.lo_x || a.hi_y <= b.lo_y || b.hi_y <= a.lo_y) continue;
        if (!shadows_overlap(a, b)) continue;
        overlapping[i] = 1;
        overlapping[j] = 1;
      }
    }
    for (u32 t = 0; t < desc.triangle_count; ++t) {
      if (overlapping[t] != 0) {
        ++counts.overlapping;
        folded = true;
      }
    }
    if (folded) ++counts.folded_clusters;
  }
  return true;
}

}  // namespace engine::geometry
